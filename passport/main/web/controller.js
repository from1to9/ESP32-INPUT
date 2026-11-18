// SPDX-License-Identifier: Apache-2.0
// The browser owns no HID state. Every action uses the device's typed input API.
const ACTIVE = new Set(["accepted", "running", "applying", "cleanup_pending"]);
const TERMINAL = new Set(["rejected", "applied", "sent", "completed", "cancelled", "failed"]);
const HEX32 = /^[0-9a-f]{32}$/;
const HEX64 = /^[0-9a-f]{64}$/;
const DECIMAL = /^(0|[1-9][0-9]{0,19})$/;
const PAD_LIFETIME_MS = 150;
const PAD_BUCKET_MS = 8;
const PAD_BATCH_MS = 30;
const CLOCK_WINDOW_MS = 2500;
const STATE_INTERVAL_MS = 900;
const THEME_KEY = "passport.browser.theme.v1";
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const hex = bytes => Array.from(bytes, byte => byte.toString(16).padStart(2, "0")).join("");
const decimal = value => typeof value === "string" && DECIMAL.test(value) && BigInt(value) <= 18446744073709551615n;
const currentGesture = gesture => !gesture || (gesture.current() && performance.now() < (gesture.expiresAt ?? Infinity));

export function validateText(text) {
  if (typeof text !== "string" || !text.length || text.length > 32) {
    throw new Error("Send 1–32 characters. Nothing was sent; the text has not been shortened.");
  }
  if (!/^[\x20-\x7e]+$/.test(text)) {
    throw new Error("Only printable US ANSI characters are supported. Remove newlines or unsupported characters. Nothing was sent.");
  }
  return text;
}

export function describeResult(result, total = null) {
  if (!result) return "Reply unknown. Control stopped locally; do not resend without checking the target.";
  if (ACTIVE.has(result.state)) return `${result.state}: awaiting the device's result.`;
  const progress = Number.isInteger(result.completed_pairs) && Number.isInteger(result.started_prefix);
  const counts = progress ? `${result.completed_pairs}${total === null ? "" : ` / ${total}`} key pairs complete; ${result.started_prefix} started.` : "";
  const uncertain = progress && Number.isInteger(result.uncertain_index) && result.uncertain_index >= 0 ?
    ` Character ${result.uncertain_index + 1} has an uncertain down/release outcome.` : "";
  if (result.state === "sent" && result.error === "ok") {
    return `${counts} Sent to the HID transport; target application outcome is unobserved.`.trim();
  }
  if (result.state === "applied" && result.error === "ok") return "Device applied the action.";
  return `${result.state || "Unknown"}: ${result.error || "unknown"}${result.reason ? ` (${result.reason})` : ""}. ${counts}${uncertain}`.trim();
}

// Unnegotiated channels use a single request/reply exchange. Negotiated
// channels match each fully formed move to its own device receipt.
class MotionChannel {
  constructor(ticket, lost) {
    const url = new URL("/api/v1/ws", location.href);
    url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
    const socket = this.socket = new WebSocket(url, ["passport.v1", ticket]);
    this.lost = lost;
    this.ready = new Promise((resolve, reject) => {
      this.reject = reject;
      this.timer = setTimeout(() => this.close(), 3500);
      socket.onopen = () => {
        if (socket.protocol !== "passport.v1") { this.close(); return; }
        clearTimeout(this.timer);
        this.reject = null;
        resolve();
      };
    });
    socket.onmessage = event => {
      try {
        const json = JSON.parse(event.data);
        if (this.window?.size) { this.window.receive(json); return; }
        if (!this.reply) { this.close(); return; }
        // As on S31, renewal is a single exclusive exchange after moves drain.
        // Passport's selected window also supplies the original command_seq.
        const pending = this.renewal;
        if (pending) {
          if (json.command_seq !== pending.seq || json.next_seq !== pending.seq + 1 ||
              !decimal(json.command_key) || json.command_key === "0" || typeof json.error !== "string") throw new Error();
          if (pending.key !== null) {
            if (json.motion_terminal !== true || json.command_key !== pending.key) throw new Error();
          } else if (json.motion_terminal) throw new Error();
          else if (json.state === "accepted" && json.error === "ok") {
            pending.key = json.command_key;
            clearTimeout(this.timer);
            this.timer = setTimeout(() => this.close(), 2200);
            pending.onAccepted(json);
            return;
          }
          if (!(json.state === "applied" && json.error === "ok") &&
              !(["rejected", "cancelled", "failed"].includes(json.state) && json.error !== "ok")) throw new Error();
          this.renewal = null;
        }
        const resolve = this.reply;
        clearTimeout(this.timer);
        this.reject = this.reply = null;
        resolve(json);
      } catch { this.close(); }
    };
    socket.onerror = socket.onclose = () => this.close();
  }
  exchange(frame, onAccepted) {
    if (this.window?.size || this.socket.readyState !== WebSocket.OPEN || this.socket.bufferedAmount || this.reject) throw new Error("Input channel unavailable.");
    return new Promise((resolve, reject) => {
      if (onAccepted) this.renewal = {seq:frame.seq, key:null, onAccepted};
      this.reply = resolve;
      this.reject = reject;
      this.timer = setTimeout(() => this.close(), 3500);
      try { this.socket.send(JSON.stringify(frame)); }
      catch { this.close(); }
    });
  }
  async configure(caps) {
    if (caps.websocket_motion_window !== true) return;
    const {motionWindowRequest, MotionWindow} = await import("/motion_window.mjs");
    const request = motionWindowRequest(caps);
    const reply = await this.exchange(request);
    if (reply?.motion_terminal !== true) throw new Error("Device cannot confirm windowed movement.");
    this.renewTerminal = reply.renew_terminal === true;
    this.window = MotionWindow.fromSelection(this.socket, request, reply, () => this.close());
  }
  async command(envelope, onAccepted) {
    const started = performance.now();
    if (envelope.op === "control.renew") {
      return {json:await this.exchange(envelope, onAccepted), started, websocket:true};
    }
    if (this.renewal) throw new Error("Renewal is in flight.");
    if (this.window) {
      const json = await this.window.send(envelope, onAccepted);
      return {json, started, websocket:true, windowed:true};
    }
    const json = await this.exchange(envelope);
    // A WebSocket message carries the protocol result with no HTTP status.
    if (!json || !decimal(json.command_key) || json.command_key === "0" ||
        json.next_seq !== envelope.seq + 1 || typeof json.error !== "string" ||
        !((TERMINAL.has(json.state) && json.error !== "ok") ||
          (["accepted", "sent"].includes(json.state) && json.error === "ok"))) {
      this.close();
      throw new Error("Movement reply was not confirmed.");
    }
    return {json, started, websocket:true};
  }
  close() {
    clearTimeout(this.timer);
    const reject = this.reject;
    this.reply = this.reject = null;
    this.socket.onopen = this.socket.onmessage = this.socket.onclose = this.socket.onerror = null;
    this.socket.close();
    reject?.(new Error("Input channel closed before its reply."));
    const unresolved = this.window?.close() ?? [];
    if (this.renewal) {
      unresolved.push({seq:this.renewal.seq, key:this.renewal.key, op:"control.renew"});
      this.renewal = null;
    }
    this.lost(this, unresolved);
    return unresolved;
  }
}

export class Controller {
  constructor(fetcher = globalThis.fetch.bind(globalThis)) {
    this.fetcher = fetcher;
    this.channel = null;
    this.channelLost = false;
    this.runs = 0;
    this.pendingPinRead = null;
    this.unresolved = [];
    this.session = null;
    this.state = null;
    this.clockSamples = [];
    this.onInputLaneReleased = () => {};
    this.controlled = false;
    this.busy = false;
    this.renewing = false;
    this.managing = false;
    this.closing = false;
    this.uncertain = false;
    this.pin = null;
    this.last = null;
    this.lastKey = null;
    this.lastTotal = null;
    this.message = "Nothing sent.";
    this.operations = new Map();
    this.onchange = () => {};
    this.generation = 0;
  }
  get foregroundBusy() { return this.busy && !this.renewing; }
  get canOverlapMove() {
    const w = this.channel?.window, s = this.state, at = performance.now();
    // All active actions must already have written a sampled move. This also
    // excludes a pending state read, mode change, renewal or expiry readback.
    return this.controlled && !this.uncertain && !this.pendingPinRead && w?.canSend && w.size > 0 && w.size === this.runs &&
      at < this.stateReuseUntil && s.mode === "mouse" && !s.release_pending &&
      !this.renewDue(at, true);
  }
  renewDue(at = performance.now(), sampled = false) {
    const s = this.state;
    const estimate = BigInt(s.now_ms) + BigInt(Math.floor(at - this.stateAt));
    const reserve = this.pin.longLease ? 5000n : sampled && this.channel?.renewTerminal ? 900n : 1800n;
    return BigInt(s.lease_expires_ms) - this.deviceTiming(s.boot_id, at, estimate).clock < reserve;
  }
  changed() { this.onchange(); }
  halt(message) {
    this.pendingPinRead = null;
    this.controlled = false;
    this.pin = null;
    if (message) this.message = message;
    this.changed();
  }
  async request(path, method = "GET", body, {
    csrf = this.session?.csrf, keepalive = false, token = this.session?.session_token, expiryState = false,
  } = {}) {
    const abort = new AbortController();
    const timeout = setTimeout(() => abort.abort(), 3500);
    try {
      // Preserve a concrete Origin under no-referrer in Safari. API paths are
      // relative and the page's connect-src policy remains restricted to self.
      const started = performance.now();
      const response = await this.fetcher(path, {
        method, mode: "cors", credentials: "same-origin", cache: "no-store", redirect: "error",
        headers: {Accept: "application/json", ...(token ? {Authorization: `Bearer ${token}`} : {}),
          ...(expiryState ? {"X-Passport-Expired-State":"1"} : {}),
          ...(path === "/api/v1/open-session" ? {"X-Passport-Open": "1"} : {}), ...(method === "GET" ? {} : {"Content-Type": "application/json"}),
          ...(csrf && method !== "GET" ? {"X-CSRF-Token": csrf} : {})},
        ...(body === undefined ? {} : {body: JSON.stringify(body)}), signal: abort.signal, keepalive,
      });
      const json = await response.json();
      return {status: response.status, json, started};
    } finally { clearTimeout(timeout); }
  }
  async login() {
    if (this.session || this.busy || this.closing) throw new Error("Close the current session first.");
    this.busy = true; this.changed();
    try {
      const response = await this.request("/api/v1/open-session", "POST", {});
      const s = response.json;
      if (response.status !== 200 || !HEX32.test(s.session_id || "") || !HEX64.test(s.session_token || "") ||
          !HEX32.test(s.boot_id || "") || !HEX32.test(s.csrf || "") || !/^[0-9a-f]{16}$/.test(s.request_prefix || "") ||
          !Number.isSafeInteger(Number(s.next_seq)) || Number(s.next_seq) < 1) throw new Error("Session unavailable. Check Passport or close another browser session.");
      this.session = {...s, next_seq: Number(s.next_seq)};
      ++this.generation; this.uncertain = false; this.lastKey = null; this.unresolved = [];
      await this.refresh();
      const caps = await this.request("/api/v1/capabilities");
      if (caps.json.access_policy !== "open_network") throw new Error("This page requires the Passport open-network service.");
      this.protocolView = caps.json.protocol_state_view === true;
      this.operations = new Map();
      for (const offset of [10, 18, 26, 34, 62, 70, 150]) {
        const page = await this.request(`/api/v1/operations?cursor=${offset}&limit=8`);
        if (page.status !== 200 || !Array.isArray(page.json.items)) throw new Error("Operation availability is unknown.");
        for (const op of page.json.items) this.operations.set(op.name, op.available === true);
      }
      if (caps.json.websocket === true && typeof WebSocket === "function") {
        const session = this.session;
        const ticket = await this.request("/api/v1/ws-ticket", "POST");
        if (!session || session !== this.session || ticket.status !== 200 || !HEX64.test(ticket.json.ticket || ""))
          throw new Error("Input channel ticket unavailable.");
        const channel = new MotionChannel(ticket.json.ticket, (closed, unresolved) => {
          if (this.channel === closed) {
            this.channel = null;
            this.channelLost = true;
            this.unresolved = unresolved;
            this.uncertain = true;
            this.halt("Input channel lost. Reconnect explicitly before sending input.");
          }
        });
        this.channel = channel;
        await channel.ready;
        await channel.configure(caps.json);
        if (this.channel !== channel || this.session !== session) throw new Error("Session closed while opening input channel.");
      }
      this.message = "Browser connected. Take control when the host is ready.";
    } catch (error) { await this.disconnect(); throw error; }
    finally { this.busy = false; this.changed(); }
  }
  async refresh(expectedEpoch = this.pin?.epoch, idleOnly = false) {
    if (!this.session) throw new Error("Connect this browser first.");
    const session = this.session;
    const before = this.state, pin = this.pin;
    const superseded = () => idleOnly && (this.busy || this.pin !== pin || this.state !== before || this.session !== session);
    let response;
    try {
      for (const delay of [0, 20, 40, 80]) {
        if (delay) await sleep(delay);
        if (superseded()) return null;
        response = await this.request(this.protocolView ? "/api/v1/state?view=protocol" : "/api/v1/state");
        if (![429, 503].includes(response.status) || response.json.error !== "busy") break;
      }
    } catch (error) {
      if (superseded()) return null;
      this.state = null;
      this.halt("Connection unavailable. Control stopped locally; reconnect explicitly after checking the target.");
      throw error;
    }
    if (superseded()) return null;
    if (this.session !== session) throw new Error("Browser session changed while reading state.");
    return this.observeState(response.json.protocol, expectedEpoch, response.started, {status:response.status});
  }
  observeState(s, expectedEpoch, started, {status, inline = false}) {
    if ((status !== undefined && status !== 200) || !s || s.boot_id !== this.session.boot_id ||
        !["now_ms", "epoch", "transport_generation", "state_sequence", "ui_revision"].every(k => decimal(s[k])) ||
        typeof s.target_id !== "string" || !["mouse", "keyboard", "audio"].includes(s.mode) ||
        !["locked", "release_pending", "input_ready", "remote_allowed"].every(k => typeof s[k] === "boolean")) {
      if ([401, 403].includes(status) || (s && s.boot_id !== this.session.boot_id)) {
        this.clearSession();
      }
      this.state = null;
      this.halt("Device state unavailable, permission lost or boot changed. Reconnect explicitly; previous input will not resume.");
      throw new Error(this.message);
    }
    const at = performance.now();
    const lostPin = this.controlled && (s.lease_active !== true || s.lease_id !== this.pin.lease_id ||
        !decimal(s.lease_expires_ms) || BigInt(s.lease_expires_ms) <= BigInt(s.now_ms) ||
        s.target_id !== this.pin.target_id || s.transport_generation !== this.pin.transport_generation ||
        s.epoch !== expectedEpoch || s.locked || !s.remote_allowed || !s.input_ready);
    if (inline && this.controlled && !lostPin && !s.release_pending) {
      // A positive snapshot may have aged on the return path. Re-read the pin
      // before installing it if it can no longer establish a fresh lease.
      if (at - started >= STATE_INTERVAL_MS ||
          this.deviceTiming(s.boot_id, at, BigInt(s.now_ms)).clock >= BigInt(s.lease_expires_ms)) return null;
    }
    this.state = s;
    this.stateAt = at; // Device clock lower bound is anchored at receipt.
    this.stateReuseUntil = inline ? started + STATE_INTERVAL_MS : at + (this.channel?.renewTerminal ? 1500 : STATE_INTERVAL_MS);
    this.clockSamples = this.clockSamples.filter(a => a.boot === s.boot_id && at - a.at < CLOCK_WINDOW_MS).slice(-7);
    this.clockSamples.push({boot:s.boot_id, at, time:BigInt(s.now_ms), rtt:at-started});
    if (lostPin) {
      this.halt("Control ended: local takeover, target/readiness change or lease expiry. Take control again only after checking Passport.");
    }
    if (this.controlled) this.pin.epoch = s.epoch;
    this.changed();
    return s;
  }
  deviceTiming(boot, now, clock) {
    let roundTrip = Infinity;
    for (const anchor of this.clockSamples) {
      const age = now - anchor.at;
      if (anchor.boot !== boot || age >= CLOCK_WINDOW_MS) continue;
      const lower = anchor.time + BigInt(Math.floor(age)) - 2n;
      if (lower > clock) clock = lower;
      roundTrip = Math.min(roundTrip, anchor.rtt);
    }
    return {clock, roundTrip};
  }
  async send(op, args, total, gesture, overlap = false, retryRenewal = true) {
    if (!this.operations.get(op)) throw new Error("This operation is unavailable on Passport.");
    // A discarded local gesture has no protocol result or sequence consumption.
    if (!currentGesture(gesture)) return null;
    const observe = op !== "control.renew";
    const sampledMove = op === "mouse.move_relative" && gesture?.takeSample;
    // Each POST is gated by the device against the pinned lease and target.
    // Read state periodically during motion and always after an expired sample.
    const reuse = sampledMove && this.controlled && this.state && performance.now() < this.stateReuseUntil;
    if (overlap && !reuse) return null;
    let s = reuse ? this.state : await this.refresh();
    const deviceTime = at => BigInt(s.now_ms) + BigInt(Math.floor(at - this.stateAt));
    if (!currentGesture(gesture)) return null;
    if (op !== "control.acquire" && op !== "control.release" && !this.controlled) throw new Error("Take control again before sending input.");
    if (this.uncertain) throw new Error("A reply is unknown. Reconnect explicitly; no command will be replayed.");
    if (!["control.acquire", "control.release", "control.renew"].includes(op)) {
      if (!decimal(s.lease_expires_ms)) throw new Error("Input lease deadline is unavailable.");
      /* Busy input must not starve the timer that normally renews this already
       * explicit lease. Renew before a new action, never reacquire or replay. */
      if (this.renewDue(performance.now(), Boolean(sampledMove))) {
        if (overlap) return null;
        this.renewing = true;
        this.changed();
        try { await this.send("control.renew", {}); }
        finally { this.renewing = false; this.changed(); }
        s = this.state;
        if (!this.controlled) throw new Error("Input control ended before the new action.");
      }
    }
    // Keep mode selection and the initiating input in the same command lane.
    const mode = op.startsWith("mouse.") ? "mouse" : op.startsWith("keyboard.") ? "keyboard" : null;
    if (mode && s.mode !== mode) {
      const pin = this.pin;
      await this.send("mode.select", {mode}, undefined, gesture);
      s = this.state;
      for (const delay of [20, 40, 80, 160]) {
        if (!currentGesture(gesture)) return null;
        if (!this.controlled || this.pin !== pin || s.mode !== mode) throw new Error("Input mode or control changed.");
        if (!s.release_pending) break;
        await sleep(delay);
        s = await this.refresh();
      }
      if (!this.controlled || this.pin !== pin || s.mode !== mode) throw new Error("Input mode or control changed.");
    }
    if (mode && s.release_pending) throw new Error("Waiting for Passport to release input.");
    const channelRenew = op === "control.renew" && this.channel?.renewTerminal;
    const channel = sampledMove || channelRenew ? this.channel : null;
    const window = sampledMove && channel?.window;
    if (channelRenew && s.release_pending) throw new Error("Waiting for Passport to release input.");
    const seq = window ? window.nextSequence(this.session.next_seq) : this.session.next_seq;
    if (seq > 4294967295) throw new Error("Session sequence exhausted. Reconnect.");
    if (!currentGesture(gesture)) return null;
    const now = performance.now();
    let clock = deviceTime(now), sample = null;
    if (sampledMove) {
      const timing = this.deviceTiming(s.boot_id, now, clock);
      clock = timing.clock;
      let headroom = timing.roundTrip;
      if (channel?.renewTerminal) {
        // Use S31's measured admission/expiry lead only when the device actually
        // supplied a timestamp. Each bucket still has its original expiry.
        const lead = mark => mark ? Number(mark.deviceAt - clock) + Math.floor(now - mark.sentAt) + 12 : 0;
        const expiry = channel.lastExpiry;
        headroom = Math.max(0, Math.min(PAD_LIFETIME_MS - PAD_BATCH_MS,
          Math.max(lead(channel.lastAdmission), expiry?.repeated && now - expiry.sentAt < 1500 ? lead(expiry) : 0)));
      }
      sample = gesture.takeSample(now, headroom);
    }
    if (sampledMove && !sample) return null;
    const expiresAt = sample?.expiresAt ?? gesture?.expiresAt;
    const volatile = op === "mouse.move_relative" || op === "mouse.scroll";
    const lifetime = expiresAt !== undefined ? Math.min(200, Math.floor(expiresAt - now)) : volatile ? 200 : 1500;
    if (lifetime <= 0) return null;
    const envelope = {
      v: 1, boot_id: this.session.boot_id, session_id: this.session.session_id,
      request_id: this.session.request_prefix + BigInt(seq).toString(16).padStart(16, "0"), seq,
      deadline_ms: (clock + BigInt(lifetime)).toString(),
      epoch: s.epoch, transport_generation: s.transport_generation, target_id: s.target_id,
      ui_revision: s.ui_revision, op,
      args: op === "control.acquire" || op === "control.renew" ? {...args, state_sequence: s.state_sequence} : sample?.args ?? args,
      ...(op !== "control.acquire" && s.lease_id ? {lease_id: s.lease_id} : {}),
    };
    // Nothing asynchronous may intervene between this check and request/fetch.
    // Once POST starts, retain its real result/uncertainty even if contact ends.
    if (sample ? !gesture.current() || performance.now() >= sample.expiresAt : !currentGesture(gesture)) return null;
    const generation = this.generation;
    let answer, expiredSample = false, confirmedMove = false, expirySnapshot;
    if (observe) { this.lastKey = null; this.lastTotal = total ?? null; }
    let key = null;
    try {
      gesture?.posting?.(op);
      const post = channel ? await channel.command(envelope, accepted => {
        if (generation !== this.generation) return;
        this.session.next_seq = Math.max(this.session.next_seq, accepted.next_seq);
        if (decimal(accepted.now_ms)) {
          const at = performance.now();
          this.clockSamples = this.clockSamples.filter(a => a.boot === s.boot_id && at - a.at < CLOCK_WINDOW_MS).slice(-7);
          this.clockSamples.push({boot:s.boot_id, at, time:BigInt(accepted.now_ms), rtt:at-now});
          channel.lastAdmission = {sentAt:now, deviceAt:BigInt(accepted.now_ms)};
        }
        if (sampledMove) this.onInputLaneReleased();
      }) :
        await this.request("/api/v1/commands", "POST", envelope, {expiryState:Boolean(sampledMove)});
      let response = post;
      answer = response.json;
      const next = Number(answer.next_seq);
      if (!post.windowed && (!Number.isInteger(next) || (next !== seq && next !== seq + 1))) throw new Error("Missing sequence acknowledgement.");
      if (generation !== this.generation) throw new Error("Browser session changed while input was in flight.");
      this.session.next_seq = post.windowed ? Math.max(this.session.next_seq, next) : next;
      key = decimal(answer.command_key) ? answer.command_key : null;
      if (post.windowed && !this.controlled && sampledMove) {
        // A different receipt already stopped this lease. Keep its reason and
        // lookup key visible alongside the outcome of the other in-flight move.
        this.message += ` #${seq} (key ${key}): ${describeResult(answer, total)}`;
        this.changed();
        return answer;
      }
      if (observe) { this.last = answer; this.lastKey = key; }
      const until = performance.now() + 2200;
      let pollDelay = sampledMove ? 0 : 100;
      while (ACTIVE.has(answer.state)) {
        if (observe) this.message = describeResult(answer, total);
        this.changed();
        if (!key || performance.now() >= until) throw new Error("Completion not established before the input deadline.");
        if (pollDelay) await sleep(pollDelay);
        pollDelay = 100;
        response = await this.request(`/api/v1/requests/${key}`);
        if (generation !== this.generation || ![200, 202, 400].includes(response.status) ||
            response.json.command_key !== key || !response.json.state) throw new Error("Result is unavailable.");
        answer = response.json;
        if (observe) this.last = answer;
      }
      if (channelRenew && answer.error === "ok" &&
          (answer.state !== "applied" || !this.controlled || answer.lease_active !== true || answer.lease_id !== this.pin.lease_id ||
           !decimal(answer.lease_expires_ms) || BigInt(answer.lease_expires_ms) <= BigInt(s.lease_expires_ms)))
        throw new Error("The device did not confirm an active renewed lease.");
      if (post.websocket && sampledMove && answer.error === "ok" && answer.state !== "sent") throw new Error("Unexpected movement result.");
      if (sampledMove && answer.state === "sent" && answer.error === "ok") {
        if ((!response.websocket && response.status !== 200) || !key || key === "0" || (!post.windowed && (next !== seq + 1 || answer.next_seq !== seq + 1)))
          throw new Error("Movement completion was not confirmed.");
        confirmedMove = true;
        if (channel?.lastExpiry?.repeated === false) channel.lastExpiry = null;
      }
      if (volatile && answer.state === "rejected" && answer.error === "expired") {
        if ((!response.websocket && response.status !== 400) || !key || key === "0" || (!post.windowed && (next !== seq + 1 || answer.next_seq !== seq + 1)))
          throw new Error("Movement rejection was not confirmed.");
        expiredSample = true;
        if (sampledMove && channel?.renewTerminal) {
          const stamp = /^(?:protocol|submit|dequeue|input|send) now=(\d+) deadline=(\d+)$/.exec(answer.reason || "");
          if (stamp && decimal(stamp[1]) && stamp[2] === envelope.deadline_ms && BigInt(stamp[1]) >= BigInt(stamp[2]))
            channel.lastExpiry = {sentAt:now, deviceAt:BigInt(stamp[1]),
              repeated:Boolean(channel.lastExpiry && now - channel.lastExpiry.sentAt < 1500)};
        }
        if (sampledMove && response === post && Object.hasOwn(answer, "protocol")) expirySnapshot = post;
      }
    } catch (error) {
      this.uncertain = true;
      this.halt(`${describeResult(null)} ${error.message}`);
      throw error;
    }
    if (observe) this.message = op === "mode.select" && answer.state === "applied" ?
      `Passport selected ${args.mode} mode.` : describeResult(answer, total);
    if (expiredSample) {
      // Expiry is checked before the device checks the lease. Verify the
      // original pin before the command lane can accept fresh input.
      const attached = expirySnapshot && this.observeState(answer.protocol, s.epoch, expirySnapshot.started, {inline:true});
      if (!attached && window) {
        this.pendingPinRead = this.pin;
        return answer;
      }
      const confirmed = attached ?? await this.refresh(s.epoch);
      if (this.controlled && confirmed.release_pending) this.halt("Control ended while Passport was releasing input.");
      return answer;
    }
    if (channelRenew && retryRenewal && answer.state === "rejected" && answer.error === "stale") {
      // A keyed rejection has no effect. Re-read the same pin before a single new renewal.
      return this.send(op, args, total, gesture, false, false);
    }
    if (answer.error !== "ok" || !["applied", "sent", "completed"].includes(answer.state)) {
      this.halt(window ? `#${seq} (key ${key}): ${describeResult(answer, total)}` : describeResult(answer, total));
      throw new Error(this.message);
    }
    const expectedEpoch = op === "mode.select" && args.mode !== s.mode ?
      String(BigInt(s.epoch) + 1n) : this.pin?.epoch;
    if (channelRenew) this.state = {...s, lease_expires_ms:answer.lease_expires_ms};
    else if (!confirmedMove) await this.refresh(expectedEpoch);
    this.changed();
    return answer;
  }
  async run(op, args = {}, total, gesture) {
    const overlap = this.busy;
    if (overlap && !(op === "mouse.move_relative" && gesture?.takeSample && this.canOverlapMove))
      throw new Error("Wait for the current action; input is not queued for replay.");
    ++this.runs;
    this.busy = true;
    this.renewing = op === "control.renew";
    if (!gesture && !this.renewing) this.message = op === "control.acquire" ? "Requesting control…" : "Sending input…";
    this.changed();
    try { return await this.send(op, args, total, gesture, overlap); }
    catch (error) { this.halt(this.uncertain ? this.message : error.message); throw error; }
    finally {
      --this.runs;
      try {
        // All written moves must finish processing before a separate state
        // read. Multiple expiries on this pin require only one current view.
        if (!this.runs && this.pendingPinRead && this.pendingPinRead === this.pin) {
          const pin = this.pendingPinRead;
          this.pendingPinRead = null;
          const confirmed = await this.refresh(pin.epoch);
          if (this.pin === pin && confirmed.release_pending) this.halt("Control ended while Passport was releasing input.");
        }
      } finally {
        this.busy = this.runs > 0; this.renewing = false; this.changed();
        this.onInputLaneReleased();
      }
    }
  }
  async acquire() {
    if (!this.session || this.uncertain) throw new Error("Connect a fresh browser session first.");
    if (!this.operations.get("control.acquire")) throw new Error("Input control is unavailable.");
    await this.run("control.acquire", {producer: "script"});
    const s = this.state;
    if (this.uncertain || !s?.lease_active || !HEX32.test(s.lease_id || "") ||
        !decimal(s.lease_expires_ms) || BigInt(s.lease_expires_ms) <= BigInt(s.now_ms)) throw new Error("Device has not established an active input lease.");
    this.pin = {target_id: s.target_id, transport_generation: s.transport_generation, epoch: s.epoch, lease_id: s.lease_id,
      longLease: BigInt(s.lease_expires_ms) - BigInt(s.now_ms) > 5000n};
    this.controlled = true;
    this.message = "Control acquired. Input is sent only from this page's explicit controls.";
    this.changed();
  }
  async manage(op, args, revision, { release = false, timeout = 45000, update = () => {} } = {}) {
    if (!this.operations.get(op)) throw new Error("This operation is unavailable on Passport.");
    if (this.busy || !this.session || this.uncertain) throw new Error("A current browser session and an idle command lane are required.");
    if (!decimal(revision)) throw new Error("The device’s resource revision is unavailable. Refresh the lists.");
    const session = this.session;
    this.busy = this.managing = true;
    this.changed();
    try {
      if (release && this.controlled) {
        await this.send("control.release", {});
        this.halt();
      }
      const state = await this.refresh();
      if (this.session !== session || state.locked || !decimal(state.management_generation)) {
        throw new Error("Device management is unavailable. Check Passport and refresh.");
      }
      const seq = session.next_seq;
      if (seq > 4294967295) throw new Error("Session sequence exhausted. Reconnect.");
      const envelope = {
        v: 1, boot_id: session.boot_id, session_id: session.session_id,
        request_id: session.request_prefix + BigInt(seq).toString(16).padStart(16, "0"), seq,
        deadline_ms: (BigInt(state.now_ms) + 1500n).toString(),
        management_generation: state.management_generation, resource_revision: revision, op, args,
      };
      try {
        const {json: reply} = await this.request("/api/v1/commands", "POST", envelope);
        const next = Number(reply.next_seq);
        if (this.session !== session || (next !== seq && next !== seq + 1)) throw new Error();
        session.next_seq = next;
        let result = reply;
        const stopAt = performance.now() + timeout;
        for (;;) {
          if (this.session !== session || (!ACTIVE.has(result.state) && !TERMINAL.has(result.state))) throw new Error();
          update(result);
          if (TERMINAL.has(result.state)) return result;
          if (!decimal(reply.command_key) || performance.now() >= stopAt) throw new Error();
          await sleep(450);
          if (this.session !== session) throw new Error();
          const observation = await this.request(`/api/v1/requests/${reply.command_key}`);
          if (this.session !== session) throw new Error();
          if ([429, 503].includes(observation.status) && observation.json.error === "busy") continue;
          result = observation.json;
        }
      } catch {
        if (this.session !== session) throw new Error("The browser session closed before the result arrived.");
        this.uncertain = true;
        this.halt("Device result unknown. Reconnect and inspect Passport before taking another action.");
        throw new Error(this.message);
      }
    } finally {
      if (!this.session || this.session === session) {
        this.busy = this.managing = false;
        this.changed();
      }
    }
  }
  async text(value) { validateText(value); return this.run("keyboard.text.enqueue", {text: value}, value.length); }
  async checkResult() {
    const session = this.session;
    const keyed = this.unresolved.filter(move => move.key);
    if (session && keyed.length) {
      const observations = [];
      for (const move of keyed) {
        const {status, json} = await this.request(`/api/v1/requests/${move.key}`);
        if (session !== this.session) return;
        const completed = move.op === "control.renew" ? "applied" : "sent";
        if (json?.command_key !== move.key || json.command_seq !== move.seq || typeof json.error !== "string" ||
            !((status === 200 && json.state === completed && json.error === "ok") ||
              (status === 400 && ["rejected", "cancelled", "failed"].includes(json.state) && json.error !== "ok"))) {
          observations.push(`#${move.seq}: outcome unconfirmed.`);
          continue;
        }
        this.unresolved = this.unresolved.filter(other => other !== move);
        observations.push(`#${move.seq}: ${describeResult(json)}`);
      }
      this.message = observations.join(" ");
      this.changed();
      return;
    }
    if (!this.lastKey || !this.session) throw new Error("No known command key is available. The unknown action cannot be inferred from current target state.");
    const response = await this.request(`/api/v1/requests/${this.lastKey}`);
    if (![200, 202, 400].includes(response.status) || !response.json.state) throw new Error("Result no longer available; outcome remains unknown.");
    this.last = response.json;
    this.message = describeResult(this.last, this.lastTotal);
    this.changed();
  }
  clearSession() {
    this.protocolView = false;
    ++this.generation;
    this.session = null;
    const channel = this.channel;
    this.channel = null;
    this.channelLost = false;
    if (channel) this.unresolved = channel.close();
  }
  async disconnect(keepalive = false) {
    const session = this.session;
    this.clearSession();
    this.state = null;
    this.halt("Control stopped locally. Session close is being requested; if the reply is lost, the input lease will expire at its device deadline.");
    if (session) {
      this.closing = true; this.changed();
      try {
        const response = await this.request("/api/v1/sessions/current", "DELETE", undefined, {csrf:session.csrf, keepalive, token:session.session_token});
        if (response.status !== 200) throw new Error();
        if (!this.uncertain) this.message = "Browser session closed. Connect again explicitly; no input lease will resume.";
      } catch { this.message = "Session close unconfirmed. Control is stopped locally; the lease expires at its device deadline."; }
      finally { this.closing = false; this.changed(); }
    }
  }
}

function themeControl(select) {
  const media = matchMedia('(prefers-color-scheme: light)');
  let choice = 'system';
  try { choice = localStorage.getItem(THEME_KEY) || 'system'; } catch { /* Theme remains usable without storage. */ }
  if (!['system', 'dark', 'light'].includes(choice)) choice = 'system';
  select.value = choice;
  const apply = () => { document.documentElement.dataset.theme = choice === 'system' ? media.matches ? 'light' : 'dark' : choice; };
  select.onchange = () => {
    choice = select.value; apply();
    try { localStorage.setItem(THEME_KEY, choice); }
    catch { document.getElementById('result').textContent = 'Theme applied for this page. Browser storage is unavailable.'; }
  };
  media.addEventListener('change', apply); apply();
}
// Modifiers apply to one explicit key tap. No host key is held between taps.
function mountKeyboard(keys, extra, available, sendKey) {
  let modifiers = 0;
  const toggles = [], printable = [];
  function update() {
    for (const [button, mask] of toggles) button.setAttribute("aria-pressed", String(Boolean(modifiers & mask)));
    for (const [button, plain, shifted] of printable) {
      const value = modifiers & 2 ? shifted : plain;
      button.textContent = value.toUpperCase();
      button.setAttribute("aria-label", `Type ${value}`);
    }
  }
  const row = (parent, style = "") => {
    const el = document.createElement("div");
    el.className = "key-row " + style; parent.append(el); return el;
  };
  function button(parent, label, css, action) {
    const el = document.createElement("button");
    el.textContent = label; el.className = css; el.dataset.op = "keyboard.key_tap";
    el.onclick = () => { if (available()) action(); };
    parent.append(el); return el;
  }
  function tap(usage) {
    const mask = modifiers;
    modifiers = 0; update(); sendKey(usage, mask);
  }
  function modifier(parent, label, mask, css = "") {
    toggles.push([button(parent, label, "special " + css, () => { modifiers ^= mask; update(); }), mask]);
  }
  function characters(parent, plain, shifted, firstUsage, alphabet = false) {
    Array.from(plain).forEach((ch, i) => {
      const usage = alphabet ? ch.charCodeAt(0) - 93 : firstUsage + i;
      printable.push([button(parent, ch, "", () => tap(usage)), ch, shifted[i]]);
    });
  }
  const special = (parent, label, usage, css = "") => button(parent, label, "special " + css, () => tap(usage));
  characters(row(keys), "1234567890", "!@#$%^&*()", 30);
  for (const [letters, css] of [["qwertyuiop", ""], ["asdfghjkl", "home"]]) {
    characters(row(keys, css), letters, letters.toUpperCase(), 0, true);
  }
  const lower = row(keys);
  modifier(lower, "Shift", 2, "wide");
  characters(lower, "zxcvbnm", "ZXCVBNM", 0, true);
  special(lower, "⌫", 42, "wide").setAttribute("aria-label", "Backspace");
  const bottom = row(keys);
  modifier(bottom, "Ctrl", 1); modifier(bottom, "Alt", 4); modifier(bottom, "Meta", 8);
  special(bottom, "Space", 44, "space"); special(bottom, "Enter", 40, "wide");
  const symbols = row(extra);
  characters(symbols, "-=[]", "_+{}", 45);
  characters(symbols, "\\", "|", 49);
  characters(symbols, ";'`,./", ':"~<>?', 51);
  for (const list of [[["Tab",43],["Esc",41],["Delete",76],["Home",74],["End",77]],
                      [["←",80],["↑",82],["↓",81],["→",79]]]) {
    const line = row(extra);
    for (const [label, usage] of list) special(line, label, usage);
  }
  update();
  return () => { if (modifiers) { modifiers = 0; update(); } };
}

function mount() {
  const $ = id => document.getElementById(id), client = new Controller();
  themeControl($('theme'));
  let section = 'input', pad = null, drag = null, movement = null, dragAt = 0, loading = false;
  let lists = {}, editing = null;
  const note = text => { $('management-result').textContent = text; };
  const clear = () => { pad = drag = movement = null; $('drag').setAttribute('aria-pressed', 'false'); $('pad').dataset.drag = 'false'; };
  const act = async fn => { try { await fn(); } catch (error) { if (!client.uncertain) { client.message = error.message; client.changed(); } } };
  const can = op => client.session && !client.foregroundBusy && !client.uncertain && !loading && !client.state?.locked && client.operations.get(op);
  function paint() {
    const s = client.state, active = client.controlled && !client.uncertain && s?.input_ready && !s.locked && !s.release_pending;
    const connection = client.channelLost ? 'Input channel disconnected' : client.controlled ? 'This browser is controlling' : client.session ? s ? s.locked ? 'Passport is locked' : 'Browser connected · idle' : 'State unavailable' : 'Browser disconnected';
    if ($('connection').textContent !== connection) $('connection').textContent = connection;
    $('status-dot').dataset.active = String(Boolean(client.session && s && !client.channelLost));
    $('target').textContent = s ? `${s.target_id || 'No host selected'} · ${s.locked ? 'Unlock on Passport' : s.input_ready ? 'Input ready' : 'Input not ready'}` : client.message !== 'Nothing sent.' ? client.message : 'Connect to read the host and Lock state.';
    $('connect').hidden = Boolean(client.session); $('connect').disabled = client.busy || client.closing;
    $('acquire').hidden = !client.session || client.controlled;
    $('acquire').disabled = !can('control.acquire') || !s?.input_ready || s.release_pending;
    $('stop').disabled = !client.session; $('disconnect').hidden = !client.session; $('disconnect').disabled = client.closing;
    // Preserve keyboard focus on an initiating hold while the mode change releases old reports.
    const preparingDrag = drag && !drag.submitted && client.controlled && s?.release_pending;
    $('mouse-controls').disabled = !active && !preparingDrag;
    $('keyboard-controls').disabled = !active;
    // Renewal still owns the command lane; only foreground work or actual
    // control loss changes input availability in the page.
    document.querySelectorAll('[data-op]').forEach(button => { button.disabled = !active || client.foregroundBusy || !client.operations.get(button.dataset.op); });
    $('drag').disabled = (!active && !preparingDrag) || !client.operations.get('mouse.drag.begin');
    $('send').disabled = !active || client.foregroundBusy || !client.operations.get('keyboard.text.enqueue');
    const outstanding = client.unresolved.map(({seq, key}) => key ? `#${seq} (key ${key})` : `#${seq} (key unknown)`);
    const result = outstanding.length ? `${client.message} Unconfirmed commands: ${outstanding.join(", ")}.` : client.message;
    if ($('result').textContent !== result) $('result').textContent = result;
    $('result-check').disabled = !client.session || (!client.lastKey && !client.unresolved.some(move => move.key)) || client.foregroundBusy;
    $('readiness').textContent = client.channelLost ? 'Disconnect, then Connect again to take control.' : !s ? 'Unlock Passport and pair from your host’s Bluetooth settings.' : s.locked ? 'Locked on Passport. Unlock with a fresh OK hold.' : !s.input_ready ? 'Waiting for the host, encryption, subscriptions and neutral release.' : `${s.mode} input ready. ${client.controlled ? 'This browser owns the lease.' : 'Take control to begin.'}`;
    const peer = lists.peers?.peers?.find(p => p.original === s?.target_id);
    $('host-name').textContent = peer?.label || s?.target_id || 'Your host awaits';
    $('owner').textContent = client.controlled ? 'This browser' : s?.remote_owner ? 'Another controller' : 'Not acquired';
    $('address').textContent = location.host;
    if (!client.controlled) { clear(); resetModifiers(); }
    $('scan').disabled = !can('wifi.scan.start'); $('scan-cancel').disabled = !can('wifi.scan.cancel') || !lists.scan?.scanning;
    $('pair-open').disabled = !can('bluetooth.pairing.open'); $('pair-close').disabled = !can('bluetooth.pairing.close');
    $('pair-cancel').disabled = !can('bluetooth.pairing.cancel_handshake') || !lists.peers?.handshake;
    $('refresh-lists').disabled = !client.session || client.foregroundBusy || loading;
    document.querySelectorAll('[data-manage]').forEach(button => { button.disabled = !can(button.dataset.manage); });
  }
  client.onchange = paint;
  async function stop() {
    clear();
    if (!client.session) return;
    if (client.busy || client.uncertain || !client.controlled) await client.disconnect();
    else {
      try { await client.run('control.release'); client.halt('Control released. Take control explicitly to resume.'); }
      catch { await client.disconnect(); }
    }
  }
  async function selectSection(next) {
    if (drag) await finishDrag();
    clear(); section = next;
    for (const tab of ['input', 'connections']) $(`tab-${tab}`).setAttribute('aria-pressed', String(next === tab));
    $('input-page').hidden = next === 'connections'; $('connections-page').hidden = next !== 'connections';
    resetModifiers();
    if (next === 'connections') {
      if (client.controlled) await stop();
      if (client.session) await refreshLists();
    }
    paint();
  }
  document.querySelectorAll('[data-tab]').forEach(button => { button.onclick = () => act(() => selectSection(button.dataset.tab)); });
  $('open-connections').onclick = () => act(() => selectSection('connections'));
  $('connect').onclick = () => act(async () => { await client.login(); if (section === 'connections') await refreshLists(); });
  $('acquire').onclick = () => act(() => client.acquire());
  $('stop').onclick = () => act(stop);
  $('disconnect').onclick = () => act(async () => { clear(); await client.disconnect(); lists = {}; renderLists(); });
  $('result-check').onclick = () => act(() => client.checkResult());
  for (const button of document.querySelectorAll('[data-click]')) {
    button.dataset.op = 'mouse.click'; button.onclick = () => act(() => client.run('mouse.click', {button: button.dataset.click}));
  }
  for (const button of document.querySelectorAll('[data-wheel]')) {
    button.dataset.op = 'mouse.scroll'; button.onclick = () => act(() => client.run('mouse.scroll', {wheel: Number(button.dataset.wheel)}));
  }
  async function keyboard(action) {
    if (client.busy) throw new Error("Wait for the current action; input is not queued for replay.");
    const pin = client.pin;
    if (drag) await finishDrag();
    else { pad = movement = null; }
    if (!pin || client.pin !== pin) throw new Error("Control ended before keyboard input.");
    return action();
  }
  const resetModifiers = mountKeyboard($('keycaps'), $('extra-keys'),
    () => client.controlled && !client.foregroundBusy && client.operations.get('keyboard.key_tap'),
    (usage, modifiers) => act(() => keyboard(() => client.run('keyboard.key_tap', {usage, modifiers}))));
  document.querySelectorAll('[data-shortcut]').forEach(button => {
    button.dataset.op = 'keyboard.key_tap';
    button.onclick = () => act(() => keyboard(() => client.run('keyboard.key_tap', {
      usage: {copy:6,paste:25,undo:29}[button.dataset.shortcut], modifiers:Number($('shortcut-modifier').value)})));
  });
  $('send').onclick = () => act(() => {
    const text = validateText($('text').value);
    return keyboard(() => client.text(text));
  });
  $('text').oninput = () => { $('text-count').textContent = `${$('text').value.length} / 32 characters`; };
  let lastPadPost = -Infinity;
  const point = e => ({id: e.pointerId, x: e.clientX, y: e.clientY});
  function cancelOppositeFraction(bucket, axis, incoming) {
    const fraction = bucket[axis] % 1;
    if (fraction * incoming >= 0) return incoming;
    const matched = Math.sign(incoming) * Math.min(Math.abs(incoming), Math.abs(fraction));
    bucket[axis] += matched;
    return incoming - matched;
  }
  function move(e, contact) {
    if (!contact || contact.id !== e.pointerId) return;
    e.preventDefault();
    let dx = e.clientX - contact.x, dy = e.clientY - contact.y;
    contact.x = e.clientX; contact.y = e.clientY;
    // Track the contact across neutral release without accumulating disabled motion.
    if ($('mouse-controls').disabled || client.state?.release_pending || (!dx && !dy)) return;
    if (movement?.contact !== contact) movement = {contact, samples: []};
    const samples = movement.samples, at = Math.min(e.timeStamp, performance.now());
    while (samples.length && at - samples[0].at >= PAD_LIFETIME_MS) samples.shift();
    // Resolve opposing fractions at contact time, including while a POST is in flight.
    for (let i = 0; i < samples.length && (dx || dy);) {
      const prior = samples[i];
      dx = cancelOppositeFraction(prior, 'dx', dx);
      dy = cancelOppositeFraction(prior, 'dy', dy);
      if (prior.dx || prior.dy) i++;
      else samples.splice(i, 1);
    }
    if (!dx && !dy) {
      if (!samples.length) movement = null;
      return;
    }
    let bucket = samples.at(-1);
    if (!bucket || at - bucket.at >= PAD_BUCKET_MS) {
      bucket = {at, dx:0, dy:0}; samples.push(bucket);
    }
    bucket.dx += dx; bucket.dy += dy;
    movement.unread = true;
    sendMovement();
  }
  $('pad').onpointerdown = e => {
    if ($('mouse-controls').disabled || pad || (e.pointerType === 'mouse' && e.button !== 0)) return;
    e.preventDefault(); pad = point(e); movement = null; $('pad').setPointerCapture(e.pointerId);
  };
  const endPad = e => {
    if (pad?.id !== e.pointerId) return;
    pad = movement = null;
    if (drag) act(finishDrag);
  };
  // Capture loss can retarget a held pointer elsewhere in the page.
  document.addEventListener('pointermove', e => {
    if (pad?.id !== e.pointerId) return;
    if (e.pointerType === 'mouse' && !(e.buttons & 1)) endPad(e);
    else move(e, pad);
  }, {capture:true, passive:false});
  document.addEventListener('pointerup', endPad, true);
  document.addEventListener('pointercancel', endPad, true);
  $('pad').onkeydown = e => {
    const directions = {ArrowLeft:[-24,0],ArrowRight:[24,0],ArrowUp:[0,-24],ArrowDown:[0,24]};
    if (!directions[e.key] || e.repeat || $('mouse-controls').disabled) return;
    e.preventDefault(); const [dx,dy] = directions[e.key]; act(() => client.run('mouse.move_relative', {dx,dy}));
  };
  $('pad').onwheel = e => {
    if ($('mouse-controls').disabled || client.busy) return;
    e.preventDefault();
    const wheel = e.deltaY > 0 ? -3 : e.deltaY < 0 ? 3 : 0;
    const horizontal = e.deltaX > 0 ? 3 : e.deltaX < 0 ? -3 : 0;
    if (wheel || horizontal) act(() => client.run('mouse.scroll', {wheel, horizontal}));
  };
  async function beginDrag(contact) {
    if (client.busy || drag || $('mouse-controls').disabled) return;
    drag = contact;
    try {
      await client.run('mouse.drag.begin', {button:'left',deadman_ms:1500}, undefined, {
        current: () => drag === contact,
        posting: op => { if (op === 'mouse.drag.begin') contact.submitted = true; },
      });
      if (drag !== contact) return;
      dragAt = performance.now(); $('drag').setAttribute('aria-pressed','true'); $('pad').dataset.drag = 'true';
    } catch (error) { clear(); throw error; }
  }
  $('drag').onpointerdown = e => {
    if (client.busy || drag || $('mouse-controls').disabled || (e.pointerType === 'mouse' && e.button !== 0)) return;
    e.preventDefault(); $('drag').setPointerCapture(e.pointerId); act(() => beginDrag(point(e)));
  };
  $('drag').onpointermove = e => move(e, drag);
  async function finishDrag() {
    const submitted = drag?.submitted;
    clear();
    if (!submitted || !client.session) return;
    if (client.busy || client.uncertain) await client.disconnect();
    else if (client.controlled) await client.run('mouse.drag.end',{button:'left'});
  }
  const endDrag = e => { if (drag?.id === e.pointerId) act(finishDrag); };
  $('drag').onpointerup = $('drag').onpointercancel = $('drag').onlostpointercapture = endDrag;
  const keyId = e => e.code || e.key;
  $('drag').onkeydown = e => {
    if (![' ', 'Enter'].includes(e.key) || e.repeat) return;
    e.preventDefault(); act(() => beginDrag({key:keyId(e)}));
  };
  window.addEventListener('keyup', e => {
    if (drag?.key !== keyId(e)) return;
    e.preventDefault(); act(finishDrag);
  }, true);
  $('drag').onblur = () => { if (drag?.key) act(finishDrag); };
  function retainFraction(samples, axis, remaining) {
    for (let i = samples.length - 1; i >= 0; i--) {
      const delta = samples[i][axis];
      const kept = remaining * delta > 0 ? Math.sign(remaining) * Math.min(Math.abs(remaining), Math.abs(delta)) : 0;
      samples[i][axis] = kept;
      remaining -= kept;
    }
  }
  function sendMovement() {
    if (!client.controlled || (client.busy && !client.canOverlapMove) || document.hidden) return;
    if (drag?.submitted && performance.now() - dragAt >= 400) return;
    if (!movement?.unread || performance.now() - lastPadPost < PAD_BATCH_MS) return;
    if (performance.now() >= movement.samples.at(-1).at + PAD_LIFETIME_MS) { movement = null; return; }
    const pending = movement;
    act(() => client.run('mouse.move_relative', {}, undefined, {
      current: () => movement === pending && (pending.contact === pad || pending.contact === drag),
      get expiresAt() { return pending.samples.at(-1).at + PAD_LIFETIME_MS; },
      posting(op) { if (op === 'mouse.move_relative') lastPadPost = performance.now(); },
      takeSample(now, reserve) {
        const headroom = Math.min(reserve, PAD_LIFETIME_MS);
        pending.unread = false;
        const {samples} = pending;
        const keep = client.channel?.renewTerminal ? 0 : 1;
        while (samples.length > keep && now + headroom >= samples[0].at + PAD_LIFETIME_MS) samples.shift();
        if (!samples.length) return null;
        // A single fresh delta can still arrive sooner than the estimate.
        if (now >= samples[0].at + PAD_LIFETIME_MS) { samples.length = 0; return null; }
        const expiresAt = samples[0].at + PAD_LIFETIME_MS;
        let dx = 0, dy = 0;
        for (const bucket of samples) { dx += bucket.dx; dy += bucket.dy; }
        const x = Math.trunc(dx), y = Math.trunc(dy);
        if (x === dx && y === dy) samples.length = 0;
        else {
          // Consume whole units from older contributions first. Any fractional
          // remainder stays in its original signed bucket with its own expiry.
          retainFraction(samples, 'dx', dx - x);
          retainFraction(samples, 'dy', dy - y);
          pending.samples = samples.filter(bucket => bucket.dx || bucket.dy);
        }
        if (!x && !y) return null; // Wait for a new event on this contact.
        const axis = value => Math.max(-127, Math.min(127, value));
        return {args:{dx:axis(x), dy:axis(y)}, expiresAt};
      },
    }));
  }
  client.onInputLaneReleased = () => {
    if (movement?.unread) queueMicrotask(sendMovement);
  };
  setInterval(() => {
    if (!client.controlled || (client.busy && !client.canOverlapMove) || document.hidden) return;
    if (drag?.submitted && performance.now() - dragAt >= 400) {
      if (client.busy) return;
      act(async () => { await client.run('mouse.drag.refresh',{button:'left',deadman_ms:1500}); dragAt = performance.now(); });
    } else sendMovement();
  },PAD_BATCH_MS);
  setInterval(() => {
    if (!client.session || client.busy || loading || document.hidden) return;
    if (client.controlled && (!client.pin.longLease || client.renewDue())) act(() => client.run('control.renew'));
    else if (!client.controlled || (!movement?.unread && performance.now() - lastPadPost >= PAD_LIFETIME_MS &&
        performance.now() - client.stateAt >= STATE_INTERVAL_MS))
      act(() => client.refresh(undefined, true));
  },STATE_INTERVAL_MS);
  const suspend = () => { clear(); if (client.session) act(() => client.disconnect(true)); };
  document.addEventListener('visibilitychange', () => { if (document.hidden) suspend(); });
  for (const name of ['pagehide','offline','blur']) globalThis.addEventListener(name, suspend);

  async function get(path) {
    const response = await client.request(path);
    if (response.status !== 200) throw new Error(`List unavailable (${response.json.error || response.status}).`);
    return response.json;
  }
  async function paged(path, key, revision) {
    const first = await get(path), rows = [...(first[key] || [])];
    let cursor = first.next_cursor, previous = 0;
    while (cursor) {
      if (!Number.isInteger(cursor) || cursor <= previous || cursor > 24) throw new Error('Invalid list cursor.');
      previous = cursor; const next = await get(`${path}?cursor=${cursor}`);
      if (next[revision] !== first[revision] || next.known !== first.known) throw new Error('List changed while loading. Refresh again.');
      rows.push(...(next[key] || [])); cursor = next.next_cursor;
    }
    return {...first,[key]:rows};
  }
  async function refreshLists(announce = true) {
    if (!client.session || loading || client.busy) return;
    const session = client.session; loading = true; paint(); let failed = false;
    if (announce) note('Reading current hosts, networks and labels…');
    for (const [name, load] of [['peers', () => get('/api/v1/bluetooth/peers')], ['profiles', () => get('/api/v1/wifi/profiles')],
      ['scan', () => paged('/api/v1/wifi/scans/current','records','generation')], ['labels', () => paged('/api/v1/display-names','labels','revision')]]) {
      try { lists[name] = await load(); } catch { lists[name] = null; failed = true; }
    }
    if (client.session !== session) lists = {};
    loading = false; renderLists(); paint();
    if (failed) note('Some lists are unavailable. Refresh to read their current state.');
    else if (announce) note(client.session ? 'Connection lists refreshed. Changes use the exact identity shown below.' : 'Browser disconnected. Connect to refresh these lists.');
  }
  async function manage(op,args,revision,release=false) {
    if (client.busy) { note('Wait for the current action and try again.'); return; }
    const session = client.session;
    clear(); note('Waiting for Passport…');
    try {
      const result = await client.manage(op,args,revision,{release,timeout:65000,update:r => { if (client.session === session) note(describeResult(r)); }});
      if (client.session !== session) return;
      const messages = {scan_started:'Scan started. Refresh to read completed results.',scan_cancelled:'Scan cancelled.',label_write_confirmed:'Display label change saved.',selected_host_ready:'Selected Bluetooth host is ready.',bluetooth_change_observed:'Bluetooth change confirmed.',label_storage_uncertain:'Label save is uncertain. Inspect the device and storage before retrying.',unexpected_host_rejected:'A different host tried to connect and was rejected.',host_or_window_timeout:'The host did not become ready before the window ended.',release_outcome_unknown:'The old host’s input release is unconfirmed. Check that host before continuing.'};
      note(messages[result.reason] || describeResult(result));
    } catch (e) { if (client.session === session) note(e.message); }
    if (client.session === session && session && !client.uncertain) await refreshLists(false);
  }
  const ssid = row => {
    try { return new TextDecoder('utf-8',{fatal:true}).decode(Uint8Array.from((row.ssid_hex || '').match(/../g) || [],v => parseInt(v,16))) || 'Hidden network'; }
    catch { return `SSID hex: ${row.ssid_hex}`; }
  };
  function empty(node,text) { node.replaceChildren(); const p=document.createElement('p'); p.className='hint'; p.textContent=text; node.append(p); }
  function card(node,title,detail) {
    const box=document.createElement('article'), h=document.createElement('h4'), p=document.createElement('p'), actions=document.createElement('div');
    box.className='device'; h.textContent=title; p.textContent=detail; actions.className='actions'; box.append(h,p,actions); node.append(box); return actions;
  }
  function button(actions,text,op,fn) {
    const b=document.createElement('button'); b.textContent=text; b.dataset.manage=op; b.onclick=() => act(fn); actions.append(b);
  }
  function labelButtons(actions,kind,row,original,ownerRevision,extra={}) {
    if (!lists.labels?.known || row.labelable === false || row.label_known === false) return;
    const args={kind,id:row.id,...(ownerRevision ? {owner_revision:ownerRevision} : extra)};
    button(actions,row.label ? 'Edit label' : 'Add label','display_name.set',async () => {
      editing={args,revision:lists.labels.revision}; $('editor-original').textContent=`${original}\n${row.id}`;
      $('label-value').value=row.label || ''; $('editor-error').textContent=''; $('editor').showModal(); $('label-value').focus();
    });
    const retained=lists.labels.labels.find(x=>x.kind===kind && x.id===row.id);
    if (retained) button(actions,'Clear label','display_name.clear',()=>manage('display_name.clear',{kind,id:row.id},lists.labels.revision));
  }
  function renderLists() {
    const peers=lists.peers, profiles=lists.profiles, scans=lists.scan, labels=lists.labels;
    $('ble-state').textContent=peers ? `${peers.connected?'Connected':'Disconnected'} · ${peers.ready?'Input ready':'Input not ready'}${peers.pairing?` · Pairing ${peers.window_seconds}s`:''} · ${peers.message}` : 'Bluetooth state unavailable';
    empty($('ble-peers'), !peers?.known ? 'Saved hosts unavailable.' : !peers.peers.length ? 'No saved hosts. Add device to pair your first host.' : '');
    for (const row of peers?.known ? peers.peers : []) {
      const actions=card($('ble-peers'),row.label || row.original,`${row.original} · ${row.current?'Connected host':'Saved host'} · ${row.id}`);
      if (row.labelable) button(actions,'Connect host','bluetooth.target.request',()=>manage('bluetooth.target.request',{peer_id:row.id,generation:peers.generation,window_seconds:60},peers.revision,true));
      if (row.current) button(actions,'Disconnect host','bluetooth.disconnect',()=>manage('bluetooth.disconnect',{peer_id:row.id,generation:peers.generation},peers.revision,true));
      labelButtons(actions,'hid_bond',row,row.original,peers.revision);
    }
    $('wifi-state').textContent=profiles ? `${profiles.ap_active?'Passport AP':profiles.sta_ip || 'LAN offline'} · ${profiles.message}` : 'Wi-Fi state unavailable';
    empty($('wifi-profiles'),!profiles?.known?'Saved networks unavailable.':!profiles.profiles.length?'No saved networks. Configure a network through the physical setup window.':'');
    for (const row of profiles?.known ? profiles.profiles : []) {
      const original=ssid(row), actions=card($('wifi-profiles'),row.label || original,`${original} · ${row.current?'Connected':'Saved'} · SSID ${row.ssid_hex} · Profile ${row.id}`);
      labelButtons(actions,'wifi_profile',row,original,profiles.revision);
    }
    empty($('wifi-scan'),!scans?.known ? scans?.scanning?'Scanning… Refresh for results.':'No completed scan available.':!scans.records.length?'No networks in the completed scan.':'');
    for (const row of scans?.known ? scans.records : []) {
      const original=ssid(row), actions=card($('wifi-scan'),row.label || original,`${original} · ${row.rssi} dBm · ${['Open','WPA2','Unsupported security'][row.security]} · ${row.saved_matches?'Saved network · ':''}${row.bssid} · SSID ${row.ssid_hex}`);
      labelButtons(actions,'wifi_ap',row,original,null,{scan_generation:scans.generation,scan_index:row.index});
    }
    $('label-capacity').textContent=labels?.known ? `${labels.count} LABELS · 3 HOST / 4 SAVED / 16 AP` : 'Storage unavailable';
    empty($('labels'),!labels?.known?'Label storage unavailable.':!labels.labels.length?'No labels saved yet.':'');
    for (const row of labels?.known ? labels.labels : []) {
      const actions=card($('labels'),row.label,`${row.kind} · ${row.ssid_hex ? ssid(row)+' · ' : ''}${row.id}`);
      button(actions,'Clear label','display_name.clear',()=>manage('display_name.clear',{kind:row.kind,id:row.id},labels.revision));
    }
  }
  $('refresh-lists').onclick=()=>act(refreshLists);
  for (const [element,op] of [['pair-open','bluetooth.pairing.open'],['pair-close','bluetooth.pairing.close'],['pair-cancel','bluetooth.pairing.cancel_handshake']]) {
    $(element).onclick=()=>act(async()=>{ if(!lists.peers)throw new Error('Refresh Bluetooth state first.'); await manage(op,{generation:lists.peers.generation,...(op.endsWith('open')?{window_seconds:60}:{})},lists.peers.revision,true); });
  }
  $('scan').onclick=()=>act(()=>manage('wifi.scan.start',{},String(lists.scan?.generation??0)));
  $('scan-cancel').onclick=()=>act(()=>manage('wifi.scan.cancel',{},String(lists.scan?.generation??0)));
  $('editor-cancel').onclick=()=>{editing=null;$('editor').close();};
  $('label-form').onsubmit=async e=>{
    e.preventDefault(); if(!editing)return;
    const text=$('label-value').value;
    if(!text.length || [...text].length>16 || new TextEncoder().encode(text).length>48 || /[\x00-\x1f\x7f-\x9f\u061c\u200e\u200f\u202a-\u202e\u2066-\u2069]/.test(text)) { $('editor-error').textContent='Use 1–16 supported characters, at most 48 UTF-8 bytes, without control characters.';return; }
    const target=editing;editing=null;$('editor').close();await manage('display_name.set',{...target.args,label:text},target.revision);
  };
  renderLists(); paint();
}
if (typeof document !== 'undefined') mount();
