// SPDX-License-Identifier: Apache-2.0
import {MotionWindow, motionWindowRequest} from "./motion_window.mjs";

// The browser owns no HID state. Every action uses the device's typed input API.
const ACTIVE = new Set(["accepted", "running", "applying", "cleanup_pending"]);
const TERMINAL = new Set(["rejected", "applied", "sent", "completed", "cancelled", "failed"]);
const HEX32 = /^[0-9a-f]{32}$/;
const HEX64 = /^[0-9a-f]{64}$/;
const DECIMAL = /^(0|[1-9][0-9]{0,19})$/;
const IDENTITY_KEY = "mosaico.browser.identity.v1";
const THEME_KEY = "mosaico.browser.theme.v1";
const PAD_CHANNEL_BATCH_MS = 30;
const PAD_SAMPLE_LIFETIME_MS = 150;
const PAD_SAMPLE_BUCKET_MS = 8;
const PAD_TIMING_RESERVE_MS = 12;
const PAD_EXPIRY_TIMING_MS = 1500;
const PAD_CLOCK_WINDOW_MS = 2500;
const PAD_RENEW_RESERVE_MS = 900;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const hex = bytes => Array.from(bytes, byte => byte.toString(16).padStart(2, "0")).join("");
const decimal = value => typeof value === "string" && DECIMAL.test(value) && BigInt(value) <= 18446744073709551615n;

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
  return `${result.state || "Unknown"}: ${result.error || "unknown"}${result.reason ? ` (${result.reason})` : ""}. ${counts}${uncertain}`.trim();
}

function describeApproval(data) {
  const states = {pending: "Authorization pending", approved: "Access approved", rejected: "Approval rejected",
    expired: "Access expired", clock_unknown: "Device time unknown; access unavailable", unknown: "Not approved"};
  const permissions = [[1, "Mouse & keyboard"], [2, "Device management"], [4, "Firmware updates"]]
    .filter(([bit]) => (data.permissions & bit) !== 0).map(([, label]) => label).join(", ") || "No permissions";
  const duration = {"6h": "6 hours", "24h": "24 hours", permanent: "Until revoked"}[data.duration] || "Duration unavailable";
  let expires = "";
  if (decimal(data.expires_unix_seconds) && BigInt(data.expires_unix_seconds) <= 8640000000000n) {
    expires = ` · expires ${new Date(Number(data.expires_unix_seconds) * 1000).toLocaleString()}`;
  }
  return `Last checked: ${states[data.state] || "Approval unavailable"} · ${permissions} · ${duration}${expires}`;
}

class MotionChannel {
  static async open(url, ticket, lost) {
    const socket = new WebSocket(url, ["esp32-control-v1", `ticket.${ticket}`]);
    return new Promise((resolve, reject) => {
      function failed() {
        clearTimeout(timeout);
        socket.close();
        reject(new Error("Could not open the device input channel."));
      }
      const timeout = setTimeout(failed, 3500);
      socket.addEventListener("error", failed, {once: true});
      socket.addEventListener("close", failed, {once: true});
      socket.addEventListener("open", () => {
        clearTimeout(timeout);
        socket.removeEventListener("error", failed);
        socket.removeEventListener("close", failed);
        if (socket.protocol !== "esp32-control-v1") return failed();
        resolve(new MotionChannel(socket, lost));
      }, {once: true});
    });
  }
  constructor(socket, lost) {
    this.socket = socket;
    this.pending = null;
    this.moveWindow = null;
    this.motionTerminal = false;
    this.renewTerminal = false;
    socket.onmessage = event => {
      if (this.moveWindow?.size) {
        try { this.moveWindow.receive(JSON.parse(event.data)); }
        catch { this.close(); }
        return;
      }
      const pending = this.pending;
      if (!pending || pending.done) { this.close(); return; }
      try {
        const json = JSON.parse(event.data);
        if (!json || typeof json !== "object" || Array.isArray(json)) throw new Error();
        const next = (Number.isInteger(json.next_seq) || decimal(json.next_seq)) && Number(json.next_seq);
        if (pending.admitted) {
          if (json.command_key !== pending.admitted.command_key || next !== pending.sequence + 1 ||
              json.motion_terminal !== true || !TERMINAL.has(json.state) || typeof json.error !== "string") throw new Error();
        } else if (json.motion_terminal === true && (!pending.negotiating || json.state !== undefined)) {
          throw new Error();
        } else if (pending.accepted && ACTIVE.has(json.state)) {
          if (json.state !== "accepted" || json.error !== "ok" || !decimal(json.command_key) ||
              next !== pending.sequence + 1) throw new Error();
          pending.admitted = json;
          pending.accepted(json);
          return;
        } else if (pending.accepted && (!TERMINAL.has(json.state) || typeof json.error !== "string" || !decimal(json.command_key) ||
                   next !== pending.sequence + 1)) {
          throw new Error();
        }
        pending.done = true;
        pending.resolve(json);
      } catch { this.close(); }
    };
    socket.onclose = () => { this.close(); lost(this); };
  }
  async exchange(payload, onAccepted) {
    if (this.pending || this.moveWindow?.size || this.socket.readyState !== WebSocket.OPEN || this.socket.bufferedAmount) {
      throw new Error("Input channel unavailable.");
    }
    let timeout;
    try {
      return await new Promise((resolve, reject) => {
        this.pending = {resolve, reject, sequence: payload.seq,
          negotiating: ["motion_terminal", "renew_terminal", "motion_window"].includes(payload.type),
          accepted: onAccepted && (answer => {
            clearTimeout(timeout);
            timeout = setTimeout(() => this.close(), 2200);
            onAccepted(answer);
          })};
        timeout = setTimeout(() => this.close(), 3500);
        this.socket.send(JSON.stringify(payload));
      });
    } finally {
      clearTimeout(timeout);
      this.pending = null;
    }
  }
  close() {
    this.pending?.reject(new Error("Input channel reply unavailable."));
    if (this.moveWindow?.size) this.unresolvedMoves = this.moveWindow.close();
    this.socket.close();
  }
}

export class Controller {
  constructor(fetcher = globalThis.fetch.bind(globalThis), motionURL = null) {
    this.fetcher = fetcher;
    this.motionURL = motionURL;
    this.motionChannel = null;
    this.session = null;
    this.state = null;
    this.stateAt = 0;
    this.controlled = false;
    this.busy = false;
    this.renewing = false;
    this.moving = false;
    this.managing = false;
    this.closing = false;
    this.uncertain = false;
    this.pin = null;
    this.last = null;
    this.lastKey = null;
    this.lastTotal = null;
    this.movementReadback = null;
    this.message = "Nothing sent.";
    this.onchange = () => {};
    this.onInputLaneReleased = () => {};
    this.lastPadSubmissionAt = -Infinity;
    this.padRuns = 0;
    this.padReadback = false;
    this.generation = 0;
  }
  get foregroundBusy() { return this.busy && !this.renewing && !this.moving; }
  get canCheckResult() {
    return Boolean(this.session && (this.lastKey || this.movementReadback?.session === this.session));
  }
  changed() { this.onchange(); }
  halt(message) {
    this.controlled = false;
    this.pin = null;
    const channel = this.motionChannel;
    this.motionChannel = null;
    channel?.close();
    if (this.session && channel?.unresolvedMoves?.length) {
      this.movementReadback = {session: this.session, moves: channel.unresolvedMoves};
    }
    if (message) this.message = message;
    this.changed();
  }
  async openMotionChannel(generation) {
    const capabilities = await this.request("/api/v1/capabilities");
    if (capabilities.status !== 200 || this.generation !== generation) {
      throw new Error("Could not check the device input channel.");
    }
    if (capabilities.json.websocket !== true) return;
    const reply = await this.request("/api/v1/ws-ticket", "POST");
    if (reply.status !== 200 || !HEX64.test(reply.json.ticket || "") || this.generation !== generation) {
      throw new Error("Could not authorize the device input channel.");
    }
    const channel = await MotionChannel.open(this.motionURL, reply.json.ticket, channel => {
      if (this.motionChannel === channel) this.halt("Input channel closed. Take control explicitly to resume.");
    });
    try {
      if (capabilities.json.websocket_motion_terminal === true) {
        const renewTerminal = capabilities.json.websocket_renew_terminal === true;
        const request = renewTerminal && motionWindowRequest(capabilities.json);
        const selected = await channel.exchange(request || {v: 1, type: renewTerminal ? "renew_terminal" : "motion_terminal"});
        if (selected.motion_terminal !== true || (renewTerminal && selected.renew_terminal !== true)) {
          throw new Error("Device input channel negotiation failed.");
        }
        channel.motionTerminal = true;
        channel.renewTerminal = renewTerminal;
        if (request) channel.moveWindow = MotionWindow.fromSelection(channel.socket, request, selected, () => channel.close());
      }
    } catch (error) {
      channel.close();
      throw error;
    }
    if (this.generation !== generation) {
      channel.close();
      throw new Error("Browser session changed while opening the input channel.");
    }
    this.motionChannel = channel;
  }
  async request(path, method = "GET", body, csrf = this.session?.csrf, keepalive = false) {
    const abort = new AbortController();
    const timeout = setTimeout(() => abort.abort(), 3500);
    try {
      // Safari sends Origin: null for same-origin POSTs in same-origin mode
      // under no-referrer. API paths stay relative and connect-src is 'self'.
      const response = await this.fetcher(path, {
        method, mode: "cors", credentials: "same-origin", cache: "no-store", redirect: "error",
        headers: {Accept: "application/json", ...(method === "GET" ? {} : {"Content-Type": "application/json"}),
          ...(csrf && method !== "GET" ? {"X-CSRF-Token": csrf} : {})},
        ...(body === undefined ? {} : {body: JSON.stringify(body)}), signal: abort.signal, keepalive,
      });
      const json = await response.json();
      return {status: response.status, json};
    } finally { clearTimeout(timeout); }
  }
  async login(identity) {
    this.halt();
    const challenge = await this.request("/api/v1/login-challenge");
    if (challenge.status !== 200 || !HEX32.test(challenge.json.csrf || "")) throw new Error("Login challenge unavailable.");
    const response = await this.request("/api/v1/sessions", "POST",
      {client_id: identity.client_id, credential: identity.credential}, challenge.json.csrf);
    const s = response.json;
    if (response.status !== 200 || !HEX32.test(s.session_id || "") || !HEX32.test(s.boot_id || "") ||
        !/^[0-9a-f]{16}$/.test(s.request_prefix || "") || !HEX32.test(s.csrf || "") ||
        !Number.isSafeInteger(Number(s.next_seq)) || Number(s.next_seq) < 1 ||
        !Array.isArray(s.scopes) || !s.scopes.includes("view")) {
      throw new Error("Browser login rejected. Check this browser’s approval on Mosaico.");
    }
    this.session = {...s, next_seq: Number(s.next_seq)};
    this.lastKey = null;
    this.movementReadback = null;
    ++this.generation;
    this.uncertain = false;
    await this.refresh();
    this.message = s.scopes.includes("input.control") ? "Browser connected. Take control to send input." : "Browser connected with device access.";
    this.changed();
  }
  rememberPadClock(channel, deviceAt, receivedAt) {
    const previous = channel.padClock;
    // Keep the strongest recent lower bound, or start again once it ages out.
    if (!previous || receivedAt - previous.at >= PAD_CLOCK_WINDOW_MS ||
        deviceAt - previous.deviceAt >= BigInt(Math.ceil(receivedAt - previous.at))) {
      channel.padClock = {at: receivedAt, deviceAt};
    }
  }
  async refresh(allowEpochChange = false, passive = false) {
    if (!this.session) throw new Error("Connect this browser first.");
    const session = this.session;
    const before = this.state;
    const controlled = this.controlled;
    // Passive reads can overlap acquisition or a mode change. The command
    // owns state validation during the transition and after it updates state.
    const superseded = () => passive && (this.busy || this.state !== before || this.controlled !== controlled);
    let response;
    try {
      for (const delay of [0, 20, 40, 80]) {
        if (delay) await sleep(delay);
        if (this.session !== session) throw new Error("Browser session changed while reading state.");
        if (superseded()) return null;
        response = await this.request("/api/v1/state?view=protocol");
        if (![429, 503].includes(response.status) || response.json.error !== "busy") break;
      }
    } catch (error) {
      if (superseded()) return null;
      if (this.session === session) {
        this.state = null;
        this.halt("Connection unavailable. Control stopped locally; reconnect explicitly after checking the target.");
      }
      throw error;
    }
    if (this.session !== session) throw new Error("Browser session changed while reading state.");
    if (superseded()) return null;
    const s = response.json.protocol;
    if (response.status !== 200 || !s || s.boot_id !== this.session.boot_id ||
        !["now_ms", "epoch", "transport_generation", "state_sequence", "ui_revision"].every(k => decimal(s[k])) ||
        typeof s.target_id !== "string" || !["mouse", "keyboard", "audio"].includes(s.mode) ||
        !["locked", "release_pending", "input_ready", "remote_allowed"].every(k => typeof s[k] === "boolean")) {
      if ([401, 403].includes(response.status) || (s && s.boot_id !== this.session.boot_id)) {
        this.session = null; ++this.generation;
      }
      this.state = null;
      this.halt("Device state unavailable, permission lost or boot changed. Reconnect explicitly; previous input will not resume.");
      throw new Error(this.message);
    }
    this.state = s;
    this.stateAt = performance.now();
    if (this.motionChannel?.renewTerminal) this.rememberPadClock(this.motionChannel, BigInt(s.now_ms), this.stateAt);
    if (this.controlled) {
      const differences = [];
      if (s.locked) differences.push("device locked");
      if (!s.remote_allowed) differences.push("remote input not allowed");
      if (!s.input_ready) differences.push("input not ready");
      if (s.target_id !== this.pin.target_id) differences.push("target changed");
      if (s.transport_generation !== this.pin.transport_generation) differences.push("transport generation changed");
      if (!allowEpochChange && s.epoch !== this.pin.epoch) differences.push("input epoch changed");
      if (!s.lease_active) differences.push("lease no longer active");
      else if (s.lease_id !== this.pin.lease_id) differences.push("lease ID changed");
      if (differences.length) this.halt(`Control ended: ${differences.join("; ")}. Check Mosaico before taking control again.`);
      else if (allowEpochChange) this.pin.epoch = s.epoch;
    }
    this.changed();
    return s;
  }
  async send(op, args, total, readSample, renewalRetry = true, window = null) {
    // A discarded local sample has no protocol result or sequence consumption.
    if (readSample && !readSample()) return null;
    const heldAtStart = this.controlled;
    const observe = op !== "control.renew";
    const relativeMove = op === "mouse.move_relative";
    const channel = relativeMove || op === "control.renew" ? this.motionChannel : null;
    const acquireChannel = op === "control.acquire" ? this.motionChannel : null;
    const continuing = window && this.padRuns > 1;
    if (continuing && !this.canContinuePad()) return null;
    let s = relativeMove && channel && this.state && performance.now() - this.stateAt < 1500 ?
      this.state : await this.refresh();
    // The device timestamp was already sampled when the response arrived.
    // Advancing it by local monotonic elapsed time gives a conservative clock.
    const deviceTime = at => BigInt(s.now_ms) + (channel ? BigInt(Math.floor(at - this.stateAt)) : 0n);
    if (acquireChannel && this.motionChannel !== acquireChannel) throw new Error("Input channel closed before acquisition.");
    if (readSample && !readSample()) return null;
    if (op !== "control.acquire" && op !== "control.release" && !this.controlled) {
      throw new Error(heldAtStart ? this.message : "Take control again before sending input.");
    }
    if (this.uncertain) throw new Error("A reply is unknown. Reconnect explicitly; no command will be replayed.");
    if (!["control.acquire", "control.release", "control.renew"].includes(op)) {
      if (!decimal(s.lease_expires_ms)) throw new Error("Input lease deadline is unavailable.");
      /* Busy input must not starve the timer that normally renews this already
       * explicit lease. Renew before a new action, never reacquire or replay. */
      const renewalReserve = readSample && channel?.renewTerminal ? BigInt(PAD_RENEW_RESERVE_MS) : 1800n;
      if (BigInt(s.lease_expires_ms) - deviceTime(performance.now()) < renewalReserve) {
        if (continuing) return null;
        await this.send("control.renew", {});
        s = this.state; // Successful renewal already refreshed state.
        if (!this.controlled) throw new Error("Input control ended before the new action.");
      }
    }
    const seq = window ? window.nextSequence(this.session.next_seq) : this.session.next_seq;
    if (seq > 4294967295) throw new Error("Session sequence exhausted. Reconnect.");
    // A recent admission measures the gap between our clock estimate at
    // write and the device's check. Require that headroom at the next write
    // while keeping each bucket's original deadline.
    const timing = readSample && channel?.padTiming;
    const expired = readSample && channel?.padExpiryTiming;
    const anchor = readSample && channel?.padClock;
    const motionTime = anchor && performance.now() - anchor.at < PAD_CLOCK_WINDOW_MS ? at => {
      const lower = anchor.deviceAt + BigInt(Math.floor(at - anchor.at)) - 2n;
      const current = deviceTime(at);
      return lower > current ? lower : current;
    } : deviceTime;
    const measuredLead = timing => timing ? Number(timing.deviceAt - motionTime(timing.sentAt)) + PAD_TIMING_RESERVE_MS : 0;
    // Repeated expiries indicate that admission timing alone misses too much
    // queue time. Use the measured expiry briefly to select fresher movement.
    const expiryLead = expired?.repeated && performance.now() - expired.sentAt < PAD_EXPIRY_TIMING_MS ?
      measuredLead(expired) : 0;
    const headroom = Math.max(0, Math.min(PAD_SAMPLE_LIFETIME_MS - PAD_CHANNEL_BATCH_MS,
      Math.max(measuredLead(timing), expiryLead)));
    const sample = readSample?.(true, headroom);
    if (readSample && !sample) return null;
    const sentAt = performance.now();
    const lifetime = sample ? Math.min(200, Math.floor(sample.expiresAt - sentAt)) :
      relativeMove ? 200 : 1500;
    if (lifetime <= 0) return null;
    const envelope = {
      v: 1, boot_id: this.session.boot_id, session_id: this.session.session_id,
      request_id: this.session.request_prefix + BigInt(seq).toString(16).padStart(16, "0"), seq,
      deadline_ms: ((sample ? motionTime(sentAt) : deviceTime(sentAt)) + BigInt(lifetime)).toString(),
      epoch: s.epoch, transport_generation: s.transport_generation, target_id: s.target_id,
      ui_revision: s.ui_revision, op,
      args: op === "control.acquire" || op === "control.renew" ? {...args, state_sequence: s.state_sequence} : sample?.args ?? args,
      ...(op !== "control.acquire" && s.lease_id ? {lease_id: s.lease_id} : {}),
    };
    // No await separates taking the current sample from starting the POST.
    // After POST starts, contact end does not change its result or uncertainty.
    if (sample && performance.now() >= sample.expiresAt) return null;
    const generation = this.generation;
    let answer, confirmedCommand = false;
    const latest = () => observe && (!window || (!this.uncertain && seq >= window.newestSequence));
    if (latest()) { this.lastKey = null; this.lastTotal = total ?? null; }
    let key = null;
    try {
      const pushedTerminal = relativeMove ? channel?.motionTerminal : op === "control.renew" && channel?.renewTerminal;
      const accepted = pushedTerminal ? admitted => {
        if (generation !== this.generation) throw new Error("Browser session changed while input was in flight.");
        this.session.next_seq = seq + 1;
        if (decimal(admitted.now_ms)) {
          const deviceAt = BigInt(admitted.now_ms);
          channel.padTiming = {sentAt, deviceAt};
          if (channel.renewTerminal) this.rememberPadClock(channel, deviceAt, performance.now());
        }
        if (latest()) {
          this.lastKey = admitted.command_key;
          this.last = admitted;
          if (!readSample) this.message = describeResult(admitted, total);
          this.changed();
        }
        if (window) this.onInputLaneReleased();
      } : undefined;
      let response;
      if (channel) {
        const reply = window ? window.send(envelope, accepted) : channel.exchange(envelope, accepted);
        // exchange starts the socket write before returning its promise.
        if (pushedTerminal && sample) this.lastPadSubmissionAt = performance.now();
        response = {json: await reply};
      } else {
        response = await this.request("/api/v1/commands", "POST", envelope);
      }
      answer = response.json;
      const next = Number(answer.next_seq);
      if (!Number.isInteger(next) || (window ? answer.command_seq !== seq || next < seq + 1 :
          (next !== seq && next !== seq + 1))) throw new Error("Missing sequence acknowledgement.");
      if (generation !== this.generation) throw new Error("Browser session changed while input was in flight.");
      this.session.next_seq = window ? Math.max(this.session.next_seq, next) : next;
      key = decimal(answer.command_key) ? answer.command_key : null;
      confirmedCommand = Boolean(channel || [202, 400].includes(response.status)) && key !== null &&
        (window ? answer.command_seq === seq : next === seq + 1);
      if (latest()) { this.last = answer; this.lastKey = key; }
      const until = performance.now() + 2200;
      let delay = channel ? 0 : 100;
      while (ACTIVE.has(answer.state)) {
        if (observe && !readSample) this.message = describeResult(answer, total);
        this.changed();
        if (!key || performance.now() >= until) throw new Error("Completion not established before the input deadline.");
        if (delay) await sleep(delay);
        if (generation !== this.generation) throw new Error("Browser session changed while input was in flight.");
        const resource = `/api/v1/requests/${key}`;
        const result = channel ? {json: await channel.exchange({v: 1, resource})} : await this.request(resource);
        if (generation !== this.generation) throw new Error("Browser session changed while input was in flight.");
        if (result.json.command_key !== key || !result.json.state ||
            (!channel && ![200, 202, 400].includes(result.status))) {
          throw new Error("Result is unavailable.");
        }
        answer = result.json;
        if (observe) this.last = answer;
        if (channel) delay = 20;
      }
    } catch (error) {
      if (generation === this.generation) {
        this.uncertain = true;
        this.halt(`${describeResult(null)} ${error.message}`);
      }
      throw error;
    }
    if (latest()) this.message = describeResult(answer, total);
    if (relativeMove && confirmedCommand && answer.state === "rejected" && answer.error === "expired") {
      // This movement was definitively rejected before any effect. Read back
      // the lease so a simultaneous takeover or expiry still stops input.
      if (sample && channel?.renewTerminal && answer.command_key === key &&
          (window ? answer.command_seq === seq : Number(answer.next_seq) === seq + 1)) {
        const stamp = /^(?:protocol|submit|dequeue|input|send) now=(\d+) deadline=(\d+)$/.exec(answer.reason || "");
        if (stamp && stamp[2] === envelope.deadline_ms && decimal(stamp[1]) && BigInt(stamp[1]) >= BigInt(stamp[2])) {
          const previous = channel.padExpiryTiming;
          const repeated = Boolean(previous && sentAt - previous.sentAt < PAD_EXPIRY_TIMING_MS);
          channel.padExpiryTiming = {sentAt, deviceAt: BigInt(stamp[1]), repeated};
        }
      }
      if (window) this.padReadback = true;
      else await this.refresh();
      return answer;
    }
    if (sample && answer.state === "sent" && answer.error === "ok" && channel?.padExpiryTiming?.repeated === false) {
      channel.padExpiryTiming = null;
    }
    if (renewalRetry && op === "control.renew" && confirmedCommand &&
        answer.state === "rejected" && answer.error === "stale" &&
        answer.command_key === key && Number(answer.next_seq) === seq + 1) {
      // A verified rejection has no renewal effect. Retry once with a fresh
      // authority read and a new sequence, using the same already held lease.
      return this.send(op, args, total, readSample, false);
    }
    if (answer.error !== "ok" || !["applied", "sent", "completed"].includes(answer.state)) {
      this.halt((answer.error === "stale" ? op + " — " : "") + describeResult(answer, total));
      throw new Error(this.message);
    }
    if (channel && op === "control.renew") {
      if (answer.lease_active !== true || answer.lease_id !== this.pin?.lease_id ||
          !decimal(answer.lease_expires_ms) || !decimal(s.lease_expires_ms) ||
          BigInt(answer.lease_expires_ms) <= BigInt(s.lease_expires_ms)) {
        throw new Error("The device did not confirm an active renewed lease.");
      }
      this.state = {...s, lease_expires_ms: answer.lease_expires_ms};
    } else if (!channel) {
      await this.refresh(op === "mode.select");
    }
    if (acquireChannel && this.motionChannel !== acquireChannel) throw new Error("Input channel closed during acquisition.");
    this.changed();
    return answer;
  }
  input(op, args, total, readSample) {
    const mode = op.startsWith("mouse.") ? "mouse" : "keyboard";
    return this.run(op, args, total, readSample, mode);
  }
  canContinuePad() {
    const at = performance.now() - this.stateAt;
    return this.padRuns > 0 && !this.padReadback && this.controlled && !this.uncertain &&
      this.motionChannel?.moveWindow?.size > 0 && this.motionChannel.moveWindow.canSend && this.state?.mode === "mouse" && at >= 0 && at < 1500 &&
      decimal(this.state.lease_expires_ms) &&
      BigInt(this.state.lease_expires_ms) - BigInt(this.state.now_ms) - BigInt(Math.floor(at)) >= BigInt(PAD_RENEW_RESERVE_MS);
  }
  async run(op, args = {}, total, readSample, mode = null) {
    const window = readSample && op === "mouse.move_relative" ? this.motionChannel?.moveWindow : null;
    if (this.busy && !(window && this.canContinuePad())) throw new Error("Wait for the current action; input is not queued for replay.");
    const generation = this.generation;
    if (window) ++this.padRuns;
    this.busy = true;
    this.renewing = op === "control.renew";
    this.moving = op === "mouse.move_relative";
    if (!readSample && !this.renewing) this.message = op === "control.acquire" ? "Requesting control…" : "Sending input…";
    this.changed();
    try {
      if (op === "control.acquire" && this.motionURL && !this.motionChannel) await this.openMotionChannel(generation);
      if (mode && this.controlled && this.state?.mode !== mode) {
        if (readSample && !readSample()) return null;
        await this.send("mode.select", {mode});
        if (this.generation !== generation || !this.controlled || this.state?.mode !== mode) {
          throw new Error("Input mode or control changed before the action.");
        }
      }
      return await this.send(op, args, total, readSample, true, window);
    }
    catch (error) {
      if (generation === this.generation) this.halt(this.uncertain ? this.message : error.message);
      throw error;
    }
    finally {
      if (window) --this.padRuns;
      if (!this.padRuns) {
        try {
          if (window && this.padReadback && this.motionChannel?.moveWindow === window && this.generation === generation) await this.refresh();
        } finally {
          this.padReadback = false;
          this.busy = this.renewing = this.moving = false;
          this.changed();
        }
      }
      this.onInputLaneReleased();
    }
  }
  async acquire() {
    if (!this.session || this.uncertain) throw new Error("Connect a fresh browser session first.");
    if (!this.session.scopes.includes("input.control")) throw new Error("Keyboard and mouse permission is unavailable.");
    await this.run("control.acquire", {producer: "script", lifetime_ms: 3000});
    const s = this.state;
    if (!s?.lease_active || !HEX32.test(s.lease_id || "")) throw new Error("Device has not established an active input lease.");
    this.pin = {target_id: s.target_id, transport_generation: s.transport_generation, epoch: s.epoch, lease_id: s.lease_id};
    this.controlled = true;
    this.message = "Control acquired. Input is sent only from this page's explicit controls.";
    this.changed();
  }
  async manage(op, args, revision, { release = false, timeout = 45000, update = () => {} } = {}) {
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
        throw new Error("Device management is unavailable. Check Mosaico and refresh.");
      }
      const seq = session.next_seq;
      if (seq > 4294967295) throw new Error("Session sequence exhausted. Reconnect.");
      const envelope = {
        v: 1, boot_id: session.boot_id, session_id: session.session_id,
        request_id: session.request_prefix + BigInt(seq).toString(16).padStart(16, "0"), seq,
        deadline_ms: (BigInt(state.now_ms) + 1500n).toString(),
        management_generation: state.management_generation, resource_revision: revision, op, args,
      };
      let reply;
      try {
        ({json: reply} = await this.request("/api/v1/commands", "POST", envelope));
        const next = Number(reply.next_seq);
        if (this.session !== session || (next !== seq && next !== seq + 1)) throw new Error();
        session.next_seq = next;
      } catch {
        if (this.session === session) {
          this.uncertain = true;
          this.halt();
        }
        throw new Error("Device reply unknown. Reconnect and refresh before taking another action.");
      }
      let result = reply;
      const stopAt = performance.now() + timeout;
      update(result);
      while (ACTIVE.has(result.state)) {
        if (this.session !== session) throw new Error("The browser session closed before the result arrived.");
        if (!decimal(reply.command_key) || performance.now() >= stopAt) {
          throw new Error("Result still unconfirmed. Refresh the device lists before retrying.");
        }
        await sleep(450);
        const observation = await this.request(`/api/v1/requests/${reply.command_key}`);
        if ([429, 503].includes(observation.status) && observation.json.error === "busy") continue;
        if (this.session !== session || !observation.json.state) throw new Error("The result is unavailable. Refresh before retrying.");
        result = observation.json;
        update(result);
      }
      return result;
    } finally {
      this.busy = this.managing = false;
      this.changed();
    }
  }
  async text(value) { validateText(value); return this.input("keyboard.text.enqueue", {text: value}, value.length); }
  async checkResult() {
    const readback = this.movementReadback;
    if (this.session && readback?.session === this.session) {
      const messages = await Promise.all(readback.moves.map(async move => {
        if (move.key && !TERMINAL.has(move.result?.state)) {
          try {
            const response = await this.request(`/api/v1/requests/${move.key}`);
            const result = response.json;
            if ([200, 202, 400].includes(response.status) && result.command_key === move.key &&
                result.command_seq === move.seq && (ACTIVE.has(result.state) || TERMINAL.has(result.state)) &&
                typeof result.error === "string") {
              move.result = result;
            } else move.result = null;
          } catch { move.result = null; }
        }
        return `Move ${move.seq}: ${move.result ? describeResult(move.result) : "Outcome unknown."}`;
      }));
      if (this.session !== readback.session || this.movementReadback !== readback) return;
      this.message = messages.join(" ");
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
  async disconnect(keepalive = false) {
    const session = this.session;
    ++this.generation;
    this.session = null;
    this.state = null;
    this.halt("Control stopped locally. Session close is being requested; if the reply is lost, the short input lease must expire.");
    if (session) {
      this.closing = true; this.changed();
      try {
        const response = await this.request("/api/v1/sessions/current", "DELETE", undefined, session.csrf, keepalive);
        if (response.status !== 200) throw new Error();
        if (!this.uncertain) this.message = "Browser session closed. Saved approval remains; no input lease will resume.";
      } catch { this.message = "Session close unconfirmed. Control is stopped locally; the device's short lease expires independently."; }
      finally { this.closing = false; this.changed(); }
    }
  }
}

async function browserIdentity(create = false) {
  const saved = localStorage.getItem(IDENTITY_KEY);
  if (saved) {
    const identity = JSON.parse(saved);
    if (!HEX32.test(identity.client_id || "") || !HEX64.test(identity.credential || "")) throw new Error("Stored browser identity is invalid. Forget it explicitly to replace it.");
    return identity;
  }
  if (!create) return null;
  const identity = {client_id: hex(crypto.getRandomValues(new Uint8Array(16))), credential: hex(crypto.getRandomValues(new Uint8Array(32)))};
  localStorage.setItem(IDENTITY_KEY, JSON.stringify(identity));
  return identity;
}
async function verifiedFingerprint(identity) {
  if (location.protocol !== "https:" || !crypto.subtle) return null;
  const bytes = Uint8Array.from(identity.credential.match(/../g), value => parseInt(value, 16));
  return hex(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)));
}

const managementMessages = {
  physical_approval_required: "Review the request on Mosaico and confirm it there.",
  pairing_preparing: "Releasing the current host and preparing pairing.",
  pairing_preference_cleared_starting: "The selected preference was cleared. Waiting for pairing advertising.",
  pairing_advertising: "Pairing is visible. Select Mosaico from the new host’s Bluetooth settings.",
  pairing_preference_cleared_advertising_failed: "The selected preference was cleared. Pairing advertising failed.",
  pairing_policy_outcome_unknown: "Pairing did not open. The preference write may still finish. Refresh before retrying.",
  target_preparing: "Releasing the current host and saving the selection.",
  target_saved_waiting: "Selection saved. Connect from the selected host’s Bluetooth settings.",
  target_wrong_host_rejected: "A different host was rejected. Still waiting for the selected host.",
  target_ble_ready: "The selected host’s BLE link is ready.",
  target_saved_link_timeout: "Selection saved. The selected host did not become ready in time.",
  target_saved_link_failed: "Selection saved. The connection attempt failed.",
  target_policy_outcome_unknown: "The attempt ended. The preference write may still finish. Refresh before retrying.",
  label_saved: "Display label saved.",
  label_cleared: "Display label cleared.",
  label_storage_uncertain: "The label write is unconfirmed. Refresh the catalog before retrying.",
  label_identity_or_revision_changed: "The item changed. Refresh before saving a label.",
  label_capacity_reached: "The saved label limit was reached. Clear a label first.",
};

function themeControl(select) {
  const choices = ["system", "light", "dark"];
  let value = "system";
  try {
    const saved = localStorage.getItem(THEME_KEY);
    if (choices.includes(saved)) value = saved;
  } catch { /* System theme remains available without storage. */ }
  const apply = () => {
    if (select.value === "system") delete document.documentElement.dataset.theme;
    else document.documentElement.dataset.theme = select.value;
  };
  select.value = value;
  apply();
  select.onchange = () => {
    apply();
    try { localStorage.setItem(THEME_KEY, select.value); } catch { /* Keep this tab's choice. */ }
  };
}

function mountManagement(client, $, clearInput) {
  const model = {operations: new Map(), peers: null, profiles: null, labels: null, scan: null};
  let loading = false;
  let seenSession = null;
  let disclosure = 8;
  const has = scope => client.session?.scopes.includes(scope) === true;
  const supports = op => model.operations.get(op) === true;
  const node = (tag, title, css) => {
    const result = document.createElement(tag);
    if (title) result.textContent = title;
    if (css) result.className = css;
    return result;
  };
  const note = (message, section) => {
    const status = $("management-result");
    if (section) section.children[0].after(status);
    status.textContent = message;
    status.hidden = !message;
  };
  const blocked = () => !client.session || !client.state || client.state.locked || client.uncertain || client.foregroundBusy || loading;
  const controls = () => {
    $("refresh-connections").disabled = !client.session || loading || client.foregroundBusy;
    $("scan").disabled = blocked() || !has("wifi.configure") || !supports("wifi.scan.start");
    $("pair-duration").disabled = blocked() || !supports("bluetooth.pairing.open");
    document.querySelectorAll("[data-manage]").forEach(button => {
      button.disabled = blocked() || button.dataset.available !== "yes";
    });
  };
  const button = (label, act, available, css) => {
    const result = node("button", label, css);
    result.type = "button";
    result.dataset.manage = "";
    result.dataset.available = available ? "yes" : "no";
    result.onclick = act;
    return result;
  };
  const empty = title => node("p", title, "hint");
  const item = (title, detail, identity) => {
    const row = node("article", "", "item");
    row.append(node("strong", title));
    if (detail) row.append(node("p", detail, "hint"));
    if (identity) row.append(node("p", identity, "identity"));
    return row;
  };
  const rowActions = (row, actions) => {
    if (!actions.length) return;
    const group = node("div", "", "actions");
    group.append(...actions);
    row.append(group);
  };
  async function get(path) {
    for (const delay of [0, 60, 140]) {
      if (delay) await sleep(delay);
      const reply = await client.request(path);
      if (reply.status === 200) return reply.json;
      if (![429, 503].includes(reply.status) || reply.json.error !== "busy") break;
    }
    throw new Error("The device list is unavailable.");
  }
  async function pageList(path, key, cursorName, revisionKeys, bound) {
    const first = await get(path(0));
    const rows = Array.isArray(first[key]) ? [...first[key]] : [];
    let cursor = first[cursorName];
    while (Number.isInteger(cursor) && cursor > 0 && cursor <= bound && rows.length < bound) {
      const next = await get(path(cursor));
      if (revisionKeys.some(name => next[name] !== first[name]) || next[cursorName] === cursor || !Array.isArray(next[key])) {
        throw new Error("The list changed while it was loading.");
      }
      rows.push(...next[key]);
      cursor = next[cursorName];
    }
    return {...first, [key]: Array.isArray(first[key]) ? rows : first[key]};
  }
  const peers = () => pageList(i => `/api/v1/bluetooth/peers?cursor=${i}`, "peers", "next_cursor",
    ["ble_revision", "classic_revision", "target_revision"], 64);
  const labels = () => pageList(i => `/api/v1/display-names?cursor=${i}`, "labels", "next_cursor", ["revision"], 88);
  const scans = () => pageList(i => `/api/v1/wifi/scans/${i}`, "records", "next_index", ["generation"], 32);
  async function refresh({ scan = model.scan !== null, announce = false } = {}) {
    if (!client.session || loading) return;
    const session = client.session;
    loading = true;
    controls();
    let unavailable = false;
    try {
      const ops = new Map();
      // These are the catalog pages for the existing Bluetooth, Wi-Fi and
      // display-label operations. Availability is still returned by the device.
      for (const offset of [62, 71, 141, 151]) {
        try {
          const page = await get(`/api/v1/operations?cursor=${offset}&limit=8`);
          for (const row of page.items || []) ops.set(row.name, row.available === true);
        } catch { unavailable = true; }
      }
      model.operations = ops;
      for (const [key, load] of [["peers", peers], ["profiles", () => get("/api/v1/wifi/profiles")], ["labels", labels],
        ...(scan ? [["scan", scans]] : [])]) {
        try { model[key] = await load(); }
        catch { model[key] = null; unavailable = true; }
      }
    } finally {
      loading = false;
      if (client.session !== session) reset();
      else {
        render();
        if (announce) note(unavailable ? "Some lists are unavailable. Refresh when Mosaico is ready." : "Connection lists refreshed.");
      }
    }
  }
  async function command(op, args, revision, {
    release = false, timeout, success = "Device change confirmed.", scan = model.scan !== null
  } = {}) {
    const active = document.activeElement?.closest("#connections section");
    const fallback = $(op.startsWith("bluetooth.") ? "bluetooth-title" : op.startsWith("wifi.") ? "wifi-title" : "labels-title");
    const section = active || fallback.closest("section");
    if (client.busy) { note("Wait for the current action and try again.", section); return; }
    if (release) clearInput();
    note("Sending request…", section);
    try {
      const result = await client.manage(op, args, revision, {release, timeout, update: current => {
        if (ACTIVE.has(current.state)) note(managementMessages[current.reason] || "Waiting for Mosaico.");
      }});
      const applied = result.error === "ok" && ["applied", "completed"].includes(result.state);
      note(managementMessages[result.reason] || (applied ? success : `Change not confirmed (${result.error || result.state || "unknown"}).`));
    } catch (error) { note(error.message); }
    if (client.session && !client.uncertain) await refresh({scan});
  }
  function editor(title, detail, label, value, secret, save) {
    const dialog = $("editor"), input = $("editor-value");
    $("editor-title").textContent = title;
    $("editor-detail").textContent = detail;
    $("editor-label").textContent = label;
    $("editor-error").textContent = "";
    input.type = secret ? "password" : "text";
    input.autocomplete = secret ? "new-password" : "off";
    input.maxLength = secret ? 63 : 48;
    input.value = value || "";
    $("editor-form").onsubmit = event => {
      event.preventDefault();
      const value = input.value;
      const allowed = secret ? /^[\x20-\x7e]{8,63}$/.test(value) :
        value && [...value].length <= 16 && new TextEncoder().encode(value).length <= 48 &&
        !/[\u0000-\u001f\u007f-\u009f\u061c\u200e\u200f\u202a-\u202e\u2066-\u2069]/u.test(value);
      if (!allowed) {
        $("editor-error").textContent = secret ? "Enter an 8–63 character Wi-Fi passphrase." : "Enter up to 16 characters and 48 UTF-8 bytes.";
        return;
      }
      dialog.close();
      save(value);
    };
    $("editor-cancel").onclick = () => dialog.close();
    dialog.onclose = () => { input.value = ""; $("editor-form").onsubmit = null; };
    dialog.showModal();
    input.focus();
  }
  function labelActions(kind, id, title, label, owner, labelable = true) {
    const catalog = model.labels;
    const scope = ["hid_bond", "classic_bond"].includes(kind) ? "connections.manage" : "wifi.configure";
    if (!id || !has(scope) || !catalog?.known) return [];
    const actions = [];
    if (labelable && owner) {
      actions.push(button(label ? "Edit label" : "Add label", () => editor("Display label", title, "Label", label, false,
        value => command("display_name.set", {kind, id, label: value, ...owner}, catalog.revision)), supports("display_name.set")));
    }
    if (label && catalog.labels.some(row => row.kind === kind && row.id === id)) {
      actions.push(button("Clear label", () => command("display_name.clear", {kind, id}, catalog.revision), supports("display_name.clear")));
    }
    return actions;
  }
  function drawBluetooth() {
    const data = model.peers;
    const connection = $("ble-link"), actions = $("bluetooth-actions");
    const inputs = $("ble-peers"), classic = $("classic-peers");
    actions.replaceChildren();
    inputs.replaceChildren();
    classic.replaceChildren();
    if (!data) {
      connection.textContent = client.session ? "Bluetooth state unavailable." : "Connect this browser to read Bluetooth state.";
      inputs.append(empty("Saved input hosts unavailable."));
      classic.append(empty("Classic audio hosts unavailable."));
      return;
    }
    const link = !data.ble_initialized || data.ble_failed ? "BLE unavailable" : data.ble_ready ? "BLE link ready" :
      data.pairing_handshake ? "Pairing handshake in progress" : data.connected ? "BLE connected · preparing input" :
      data.reconnect_paused ? "Host reconnect paused" : "No BLE link";
    connection.textContent = `${link} · ${data.input_route === "usb" ? "USB" : "Bluetooth"} input route` +
      (data.pairing_open ? ` · pairing open (${Math.ceil(data.pairing_remaining_ms / 1000)}s at last refresh)` : "");
    const generation = data.ble_generation;
    const permission = has("connections.manage");
    if (permission && data.ble_initialized && !data.ble_failed) {
      actions.append(
        button("Pair new host", () => command("bluetooth.pairing.open", {generation, window_seconds: Number($("pair-duration").value)}, generation,
          {release: data.input_route === "ble_hid", timeout: 70000}), supports("bluetooth.pairing.open")),
        button("Close pairing window", () => command("bluetooth.pairing.close", {generation}, generation,
          {success: "Pairing window closed."}), data.pairing_open && supports("bluetooth.pairing.close")),
        button("Cancel handshake", () => command("bluetooth.pairing.cancel_handshake", {generation}, generation,
          {success: "Pairing handshake cancelled."}), data.pairing_handshake && supports("bluetooth.pairing.cancel_handshake")),
        button("Disconnect BLE host", () => command("bluetooth.disconnect", {generation}, generation,
          {release: data.input_route === "ble_hid", success: "BLE host disconnected. Its automatic reconnect is paused."}),
          data.admitted && supports("bluetooth.disconnect")),
      );
    }
    for (const host of data.peers || []) {
      const audio = host.transport === "classic_audio";
      const original = host.name || "Original host name unavailable";
      const state = audio ? [host.hfp_current && "Mic connected", host.a2dp_current && "Speaker connected"].filter(Boolean).join(" · ") :
        [data.current_match_known && host.current && "Current", host.selected && "Selected"].filter(Boolean).join(" · ");
      const title = host.label || host.name || (audio ? "Saved audio host" : "Saved input host");
      const row = item(title, [original, state].filter(Boolean).join(" · "), host.peer_id || "Identity unavailable");
      const choices = labelActions(audio ? "classic_bond" : "hid_bond", audio ? host.display_id : host.peer_id, title, host.label,
        {owner_revision: audio ? data.classic_revision : data.ble_revision}, host.labelable);
      if (audio && !host.labelable) row.append(empty("Display labels unavailable for this audio host."));
      if (!audio && permission && host.selectable) choices.unshift(button(host.selected ? "Reconnect selected host" : "Select host",
        () => command("bluetooth.target.request", {peer_id: host.peer_id, generation}, data.ble_revision,
          {release: data.input_route === "ble_hid", timeout: 100000}), supports("bluetooth.target.request")));
      rowActions(row, choices);
      (audio ? classic : inputs).append(row);
    }
    if (!inputs.children.length) inputs.append(empty(data.ble_known ? "No saved input hosts." : "Saved input hosts unavailable."));
    if (!classic.children.length) classic.append(empty(data.classic_known ? "No saved audio hosts." : "Classic audio hosts unavailable."));
  }
  function drawWifi() {
    const profiles = model.profiles, scan = model.scan;
    const saved = $("wifi-profiles"), nearby = $("wifi-scan");
    saved.replaceChildren();
    nearby.replaceChildren();
    $("wifi-status").textContent = !client.session ? "Connect this browser to read saved networks." :
      profiles?.uncertain ? "Saved network storage is uncertain." : !profiles?.known ? "Saved networks unavailable." :
      `${profiles.profiles.length} of ${profiles.capacity} saved${supports("wifi.scan.start") ? "" : " · live scanning unavailable"}`;
    const allow = has("wifi.configure") && profiles?.known && !profiles.uncertain && !profiles.operation_pending;
    for (const [index, entry] of (profiles?.profiles || []).entries()) {
      const row = item(entry.label || entry.ssid, `SSID: ${entry.ssid} · ${entry.current ? "Current" : entry.enabled ? "Saved" : "Disabled"} · priority ${entry.priority}`, entry.id);
      const choices = labelActions("wifi_profile", entry.id, entry.ssid, entry.label, {owner_revision: profiles.revision}, entry.labelable);
      if (allow) {
        choices.unshift(button("Connect", () => command("wifi.profile.join", {profile_id: entry.id}, profiles.revision,
          {release: true, success: "Saved network connected."}), entry.enabled && !entry.current && supports("wifi.profile.join")));
        if (index > 0) choices.push(button("Move to first", () => command("wifi.profiles.reorder",
          {profile_ids: [entry.id, ...profiles.profiles.filter(peer => peer.id !== entry.id).map(peer => peer.id)]},
          profiles.revision, {success: "Network priority updated."}), supports("wifi.profiles.reorder")));
        choices.push(button("Forget", () => {
          if (globalThis.confirm(`Forget ${entry.ssid} on Mosaico?`))
            command("wifi.profile.forget", {profile_id: entry.id}, profiles.revision, {success: "Saved network forgotten."});
        }, supports("wifi.profile.forget")));
      }
      rowActions(row, choices);
      saved.append(row);
    }
    if (!saved.children.length) saved.append(empty(profiles?.known && !profiles.uncertain ? "No saved networks." : "Saved network list unavailable."));
    $("scan-panel").hidden = !scan;
    if (!scan) return;
    const security = ["Open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA3", "WPA2/WPA3", "Enterprise", "OWE"];
    for (const entry of scan.records || []) {
      const row = item(entry.label || entry.ssid, `SSID: ${entry.ssid} · ${security[entry.security] || "Unknown security"} · ${entry.rssi_dbm} dBm`,
        entry.bssid?.match(/.{1,2}/g)?.join(":"));
      const choices = labelActions("wifi_ap", entry.display_id, entry.ssid, entry.label,
        {scan_generation: scan.generation, scan_index: entry.index}, entry.labelable);
      if (allow && entry.join_availability === 0 && !entry.remembered) choices.unshift(button("Save network",
        () => editor("Save Wi-Fi network", entry.ssid, "Wi-Fi passphrase", "", true,
          password => command("wifi.profile.save", {ssid: entry.ssid, password, keep_password: false, enabled: true, join: false},
            profiles.revision, {success: "Network saved. Choose Connect when ready."})),
        profiles.profiles.length < profiles.capacity && supports("wifi.profile.save")));
      rowActions(row, choices);
      nearby.append(row);
    }
    if (!nearby.children.length) nearby.append(empty(scan.state === 7 ? "Scan expired. Start a fresh scan." :
      scan.state === 4 ? "No nearby networks were observed." : "No completed scan is available."));
  }
  function drawLabels() {
    const catalog = model.labels, list = $("retained-labels");
    list.replaceChildren();
    $("label-capacity").textContent = !client.session ? "Saved labels appear after connecting." :
      catalog?.known ? `${catalog.ap_count} of ${catalog.ap_capacity} exact AP labels used${catalog.cleanup_pending ? " · cleanup unconfirmed" : ""}` :
      catalog?.cleanup_pending ? "Label cleanup and storage are unconfirmed." : "Display label storage unavailable.";
    if (!catalog?.known) return;
    const kinds = {hid_bond: "Bluetooth input", classic_bond: "Classic audio", wifi_profile: "Saved Wi-Fi", wifi_ap: "Scanned AP"};
    for (const label of catalog.labels.slice(0, disclosure)) {
      const row = item(label.label, [kinds[label.kind], label.original_ssid && `SSID: ${label.original_ssid}`].filter(Boolean).join(" · "), label.id);
      rowActions(row, labelActions(label.kind, label.id, label.label, label.label, null));
      list.append(row);
    }
    if (catalog.labels.length > disclosure) {
      const more = node("button", "Show more labels", "quiet");
      more.onclick = () => { disclosure += 8; drawLabels(); controls(); };
      list.append(more);
    }
    if (!list.children.length) list.append(empty("No display labels saved."));
  }
  function render() { drawBluetooth(); drawWifi(); drawLabels(); controls(); }
  function reset() {
    model.operations.clear();
    model.peers = model.profiles = model.labels = model.scan = null;
    note("");
    render();
  }
  $("refresh-connections").onclick = () => { if (!client.busy) return refresh({announce: true}); };
  $("scan").onclick = () => command("wifi.scan.start", {}, String(model.scan?.generation || 0),
    {scan: true, success: "Scan complete."});
  setInterval(async () => {
    if (blocked() || client.busy || document.hidden || !(model.peers?.pairing_open || model.peers?.pairing_handshake)) return;
    const session = client.session;
    try {
      const observed = await peers();
      if (client.session === session && !loading && !client.busy) {
        model.peers = observed;
        drawBluetooth();
        controls();
      }
    } catch { /* Keep the last confirmed list until an explicit refresh. */ }
  }, 3500);
  render();
  return {
    paint() {
      if (seenSession !== client.session) {
        seenSession = client.session;
        reset();
        if (seenSession && (has("connections.manage") || has("wifi.configure"))) void refresh();
      }
      controls();
    },
  };
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
    el.textContent = label; el.className = css;
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
  const $ = id => document.getElementById(id);
  const motionURL = ["https:", "http:"].includes(location.protocol) ?
    `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/api/v1/channel` : null;
  const client = new Controller(undefined, motionURL);
  let identity = null, pad = null, movement = null, pairingBusy = false;
  let lastMovementAt = 0;
  themeControl($("theme"));
  const management = mountManagement(client, $, () => { pad = movement = null; });
  const error = value => { client.message = value.message; client.changed(); };
  const act = async fn => {
    const generation = client.generation;
    try { await fn(); }
    catch (value) { if (generation === client.generation && !client.uncertain) error(value); }
  };
  const inputAction = fn => { if (!client.busy) return act(fn); };
  async function authorize(fn, revealError = true) {
    if (pairingBusy || client.closing) return;
    pairingBusy = true;
    paint();
    try {
      await fn();
    } catch (value) {
      $("approval").textContent = value.message;
      if (revealError) {
        $("pairing").open = true;
        $("approval").scrollIntoView({block: "nearest"});
      }
    } finally {
      pairingBusy = false;
      paint();
    }
  }
  function paint() {
    const s = client.state;
    const inputPermission = client.session?.scopes.includes("input.control");
    const connection = client.controlled ? "Controlling" : client.session ? s ? "Connected / idle" : "State unavailable" : "Disconnected";
    if ($("connection").textContent !== connection) $("connection").textContent = connection;
    $("connection").dataset.state = client.controlled ? "controlling" : client.session && s ? "online" : "offline";
    $("target").textContent = s ? `${s.target_id.startsWith("usb:") ? "USB" : s.target_id.startsWith("bluetooth:") ? "Bluetooth" : "Input"} · ${s.target_id || "unavailable"}` : "Unavailable";
    $("readiness").textContent = !s ? "Connect to view target." : !inputPermission ?
      "Keyboard and mouse permission required." : s.locked ? "Device locked" :
      !s.input_ready ? "Input not ready" : s.release_pending ? "Waiting for input releases" :
      !s.remote_allowed ? "Remote input is not allowed on Mosaico" : `${s.mode} · ready${client.controlled ? " · this browser owns control" : " · no control acquired here"}`;
    const active = client.controlled && !client.uncertain && s?.input_ready && !s.release_pending && !s.locked && s.remote_allowed;
    // Keep input controls steady during renewal and movement, and expose
    // their temporary unavailability without disabling focus.
    const ready = active && !client.foregroundBusy;
    const paintButton = (button, available) => {
      if (button.disabled === available) button.disabled = !available;
      if (available && client.busy) {
        if (!button.hasAttribute("aria-disabled")) button.setAttribute("aria-disabled", "true");
      } else {
        button.removeAttribute("aria-disabled");
      }
    };
    $("mouse-controls").disabled = $("keyboard-controls").disabled = !active;
    for (const button of inputButtons) paintButton(button, ready);
    $("acquire").disabled = !inputPermission || client.busy || client.uncertain || client.controlled || !s?.input_ready || s.locked || s.release_pending || !s.remote_allowed;
    $("stop").disabled = !inputPermission || (client.managing && !client.controlled);
    $("disconnect").disabled = !client.session;
    $("refresh").disabled = !client.session || client.foregroundBusy;
    $("connect").disabled = !identity || client.busy || client.closing || pairingBusy || Boolean(client.session);
    $("pair").disabled = pairingBusy || client.closing || Boolean(client.session);
    $("check").disabled = !identity || pairingBusy || client.closing || Boolean(client.session);
    // A fresh move has no result key until admission. Keep focus and appearance
    // steady while the busy guard prevents checking the preceding move.
    paintButton($("result-check"), !client.foregroundBusy && (client.canCheckResult || (ready && client.moving)));
    if ($("result").textContent !== client.message) $("result").textContent = client.message;
    if (!active) { pad = movement = null; resetModifiers(); }
    management.paint();
  }
  client.onchange = paint;
  async function showIdentity() {
    const fingerprint = identity && await verifiedFingerprint(identity);
    $("fingerprint").textContent = fingerprint ? `SHA-256 ${fingerprint}` :
      identity ? `Browser ID ${identity.client_id}` : "No browser identity yet";
  }
  async function status() {
    if (!identity) throw new Error("Pair this browser first.");
    let response;
    try {
      response = await client.request("/api/v1/pairing/status", "POST", identity, null);
    } catch {
      throw new Error("Could not read browser approval. Check the connection and try again.");
    }
    const data = response.json;
    const fingerprint = await verifiedFingerprint(identity);
    if (![200, 202].includes(response.status) || !HEX64.test(data?.fingerprint || "") ||
        data.client_id !== identity.client_id || (fingerprint && data.fingerprint !== fingerprint)) {
      throw new Error("Pairing status unavailable or identity mismatch.");
    }
    $("approval").textContent = describeApproval(data);
    if (data.state === "approved") {
      try {
        await client.login(identity);
      } catch {
        throw new Error(client.session ? "Browser access approved. Device state unavailable. Try Refresh." :
          "Browser access approved. Could not connect. Try Connect.");
      }
      $("pair-title").textContent = "Browser access · approved";
      $("pairing").open = false;
    }
    return data.state;
  }
  $("pair").onclick = () => authorize(async () => {
    const label = $("client-name").value.trim();
    if (!/^[\x20-\x7e]{1,32}$/.test(label)) throw new Error("Use 1–32 printable ASCII characters for this browser's name.");
    const code = $("access-code").value;
    if (!/^[0-9]{8}$/.test(code)) throw new Error("Enter the 8-digit access code from Mosaico settings.");
    const duration = $("access-duration").value;
    let response;
    try {
      identity = await browserIdentity(true); await showIdentity();
      try {
        response = await client.request("/api/v1/pairing/code", "POST", {...identity, label, code, duration}, null);
      } catch {
        throw new Error("Authorization reply unavailable. Check approval before submitting the code again.");
      }
    } finally { $("access-code").value = ""; }
    if (response.status !== 202) throw new Error(response.status === 401 ? "Access code rejected." :
      response.status === 429 ? "Pairing is busy. Wait a minute before trying again." : "Mosaico could not accept the browser authorization request.");
    $("approval").textContent = "Saving browser authorization…";
    for (let attempt = 0; attempt < 12; ++attempt) {
      await sleep(250);
      if (await status() !== "pending") break;
    }
  });
  $("check").onclick = () => authorize(status);
  $("connect").onclick = () => authorize(status);
  $("acquire").onclick = () => act(() => client.acquire());
  $("refresh").onclick = () => { if (!client.busy) return act(() => client.refresh(false, true)); };
  $("disconnect").onclick = () => act(() => client.disconnect());
  $("stop").onclick = () => act(async () => {
    pad = movement = null;
    if (client.busy || client.uncertain) await client.disconnect();
    else { await client.run("control.release"); client.halt("Control released. Take control explicitly to send more input."); }
  });
  document.querySelectorAll("[data-click]").forEach(button => button.onclick = () => inputAction(() => client.input("mouse.click", {button: button.dataset.click})));
  document.querySelectorAll("[data-move]").forEach(button => button.onclick = () => {
    const [dx, dy] = button.dataset.move.split(",").map(Number);
    inputAction(() => client.input("mouse.move_relative", {dx, dy}));
  });
  const resetModifiers = mountKeyboard($("keys"), $("extra-keys"),
    () => !client.busy && !$("keyboard-controls").disabled,
    (usage, modifiers) => inputAction(() => {
      pad = movement = null;
      return client.input("keyboard.key_tap", {usage, modifiers});
    }));
  const inputButtons = document.querySelectorAll("#mouse-controls button, #keyboard-controls button");
  $("send").onclick = () => inputAction(() => { pad = movement = null; return client.text($("text").value); });
  $("text").oninput = () => { $("text-count").textContent = `${$("text").value.length} / 32 characters`; };
  $("result-check").onclick = () => { if (!client.busy) return act(() => client.checkResult()); };
  $("forget").onclick = () => act(async () => {
    await client.disconnect(); localStorage.removeItem(IDENTITY_KEY); identity = null;
    $("pair-title").textContent = "Connect this browser"; $("pairing").open = true;
    await showIdentity(); $("approval").textContent = "Identity removed from this browser. Revoke its saved permissions on Mosaico if needed."; paint();
  });
  $("pad").onpointerdown = event => {
    if ($("mouse-controls").disabled || (event.pointerType === "mouse" && event.button !== 0) || pad) return;
    event.preventDefault(); $("pad").setPointerCapture(event.pointerId);
    pad = {id: event.pointerId, x: event.clientX, y: event.clientY, samples: []}; movement = null;
  };
  const endPad = event => { if (pad?.id === event.pointerId) { pad = null; movement = null; } };
  // Capture normally keeps events on the pad. Follow the original pointer
  // across the page even if the browser retargets it before the finger lifts.
  document.addEventListener("pointermove", event => {
    if (!pad || pad.id !== event.pointerId || $("mouse-controls").disabled) return;
    if (event.pointerType === "mouse" && !(event.buttons & 1)) { endPad(event); return; }
    event.preventDefault();
    const now = performance.now();
    if (client.motionChannel?.motionTerminal) {
      trimPadSamples(pad.samples, now);
      const dx = Math.round(event.clientX) - Math.round(pad.x);
      const dy = Math.round(event.clientY) - Math.round(pad.y);
      pad.x = event.clientX; pad.y = event.clientY;
      if (!dx && !dy) return;
      // The first delta supplies the bucket's deadline. Short buckets bound
      // storage while allowing later movement to outlive the oldest bucket.
      const bucketMs = client.motionChannel.renewTerminal ? PAD_SAMPLE_BUCKET_MS : PAD_SAMPLE_LIFETIME_MS;
      let bucket = pad.samples[pad.samples.length - 1];
      if (!bucket || now - bucket.at >= bucketMs) {
        bucket = {at: now, dx: 0, dy: 0};
        pad.samples.push(bucket);
      }
      bucket.dx += dx; bucket.dy += dy;
      return;
    }
    const batch = client.motionChannel ?
      PAD_CHANNEL_BATCH_MS : PAD_SAMPLE_LIFETIME_MS;
    if (!movement || now - movement.at >= batch) movement = {dx: 0, dy: 0, at: now, contact: pad};
    movement.dx = Math.max(-127, Math.min(127, movement.dx + event.clientX - pad.x));
    movement.dy = Math.max(-127, Math.min(127, movement.dy + event.clientY - pad.y));
    pad.x = event.clientX; pad.y = event.clientY;
  }, {capture: true, passive: false});
  document.addEventListener("pointerup", endPad, true);
  document.addEventListener("pointercancel", endPad, true);
  globalThis.addEventListener("blur", () => { pad = movement = null; });
  $("pad").onkeydown = event => {
    const moves = {ArrowLeft: [-24, 0], ArrowRight: [24, 0], ArrowUp: [0, -24], ArrowDown: [0, 24]};
    if (!moves[event.key] || event.repeat || $("mouse-controls").disabled) return;
    event.preventDefault(); const [dx, dy] = moves[event.key]; inputAction(() => client.input("mouse.move_relative", {dx, dy}));
  };
  function trimPadSamples(samples, now, headroom = 0) {
    while (samples.length && now + headroom >= samples[0].at + PAD_SAMPLE_LIFETIME_MS) samples.shift();
  }
  function readPadSample(contact, consume = false, headroom = 0) {
    if (pad !== contact) return null;
    const {samples} = contact;
    trimPadSamples(samples, performance.now(), headroom);
    if (!samples.length) return null;
    let dx = 0, dy = 0;
    for (const sample of samples) { dx += sample.dx; dy += sample.dy; }
    if (!dx && !dy) return null;
    const expiresAt = samples[0].at + PAD_SAMPLE_LIFETIME_MS;
    if (consume) samples.length = 0;
    return {args: {dx: Math.max(-127, Math.min(127, dx)), dy: Math.max(-127, Math.min(127, dy))}, expiresAt};
  }
  function sendPadMovement() {
    if (!pad || !client.controlled || (client.busy && !client.canContinuePad())) return;
    const now = performance.now();
    const contact = pad;
    if (client.motionChannel?.motionTerminal) {
      if (now - client.lastPadSubmissionAt < PAD_CHANNEL_BATCH_MS || !readPadSample(contact)) return;
      act(() => client.input("mouse.move_relative", {}, undefined, (consume, headroom) => readPadSample(contact, consume, headroom)));
      return;
    }
    if (!movement || (!client.motionChannel && now - lastMovementAt < 60)) return;
    lastMovementAt = now;
    const next = movement;
    movement = null;
    act(() => client.input("mouse.move_relative", {}, undefined, consume => {
      if (pad !== contact) return null;
      const latest = movement?.contact === contact ? movement : next;
      const expiresAt = latest.at + PAD_SAMPLE_LIFETIME_MS;
      if (performance.now() >= expiresAt) return null;
      const dx = Math.trunc(latest.dx), dy = Math.trunc(latest.dy);
      if (consume && latest === movement) movement = null;
      return dx || dy ? {args: {dx, dy}, expiresAt} : null;
    }));
  }
  client.onInputLaneReleased = () => {
    if (client.motionChannel?.motionTerminal && pad?.samples.length) queueMicrotask(sendPadMovement);
  };
  setInterval(sendPadMovement, PAD_CHANNEL_BATCH_MS);
  setInterval(() => {
    if (!client.session || client.busy || document.hidden) return;
    act(async () => {
      if (client.controlled) await client.run("control.renew");
      else await client.refresh(false, true);
    });
  }, 900);
  const suspend = () => { pad = movement = null; if (client.session) act(() => client.disconnect(true)); };
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) return;
    // A pairing or target request may send the user to the host's Bluetooth
    // settings. Its session can remain live once browser input has released.
    if (client.managing && !client.controlled) { pad = movement = null; return; }
    suspend();
  });
  globalThis.addEventListener("pagehide", suspend);
  globalThis.addEventListener("offline", suspend);
  authorize(async () => {
    if (!["http:", "https:"].includes(location.protocol) || !crypto.getRandomValues) throw new Error("Open Mosaico’s address over Wi-Fi or its control hotspot.");
    identity = await browserIdentity(); await showIdentity(); paint();
    if (identity) await status();
  }, false);
  paint();
}
if (typeof document !== "undefined") mount();
