// SPDX-License-Identifier: Apache-2.0
// The browser owns no HID state. Every action uses the device's typed input API.
const ACTIVE = new Set(["accepted", "running", "applying", "cleanup_pending"]);
const HEX32 = /^[0-9a-f]{32}$/;
const HEX64 = /^[0-9a-f]{64}$/;
const DECIMAL = /^(0|[1-9][0-9]{0,19})$/;
const IDENTITY_KEY = "mosaico.browser.identity.v1";
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const hex = bytes => Array.from(bytes, byte => byte.toString(16).padStart(2, "0")).join("");
const decimal = value => typeof value === "string" && DECIMAL.test(value) && BigInt(value) <= 18446744073709551615n;
const currentPadSample = sample => !sample || (sample.current() && performance.now() < sample.expiresAt);

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
  const states = {pending: "Waiting for approval", approved: "Access approved", rejected: "Approval rejected",
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

export class Controller {
  constructor(fetcher = globalThis.fetch.bind(globalThis)) {
    this.fetcher = fetcher;
    this.session = null;
    this.state = null;
    this.controlled = false;
    this.busy = false;
    this.closing = false;
    this.uncertain = false;
    this.pin = null;
    this.last = null;
    this.lastKey = null;
    this.lastTotal = null;
    this.message = "Nothing sent.";
    this.onchange = () => {};
    this.generation = 0;
  }
  changed() { this.onchange(); }
  halt(message) {
    this.controlled = false;
    this.pin = null;
    if (message) this.message = message;
    this.changed();
  }
  async request(path, method = "GET", body, csrf = this.session?.csrf, keepalive = false) {
    const abort = new AbortController();
    const timeout = setTimeout(() => abort.abort(), 3500);
    try {
      const response = await this.fetcher(path, {
        method, mode: "same-origin", credentials: "same-origin", cache: "no-store", redirect: "error",
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
        !Array.isArray(s.scopes) || !s.scopes.includes("input.control")) {
      throw new Error("Browser login rejected or keyboard/mouse permission is unavailable. Check Mosaico's permissions.");
    }
    this.session = {...s, next_seq: Number(s.next_seq)};
    this.lastKey = null;
    ++this.generation;
    this.uncertain = false;
    await this.refresh();
    this.message = "Browser connected. Take control explicitly to send input.";
    this.changed();
  }
  async refresh(allowEpochChange = false) {
    if (!this.session) throw new Error("Connect this browser first.");
    const session = this.session;
    let response;
    try {
      for (const delay of [0, 20, 40, 80]) {
        if (delay) await sleep(delay);
        response = await this.request("/api/v1/state");
        if (![429, 503].includes(response.status) || response.json.error !== "busy") break;
      }
    } catch (error) {
      this.state = null;
      this.halt("Connection unavailable. Control stopped locally; reconnect explicitly after checking the target.");
      throw error;
    }
    if (this.session !== session) throw new Error("Browser session changed while reading state.");
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
    if (this.controlled && (!s.lease_active || s.lease_id !== this.pin.lease_id ||
        s.target_id !== this.pin.target_id || s.transport_generation !== this.pin.transport_generation ||
        (!allowEpochChange && s.epoch !== this.pin.epoch) || s.locked || !s.remote_allowed || !s.input_ready)) {
      this.halt("Control ended: local takeover, target/readiness change or lease expiry. Take control again only after checking Mosaico.");
    }
    if (this.controlled && allowEpochChange) this.pin.epoch = s.epoch;
    this.changed();
    return s;
  }
  async send(op, args, total, sample) {
    // A discarded local sample has no protocol result or sequence consumption.
    if (!currentPadSample(sample)) return null;
    const observe = op !== "control.renew";
    let s = await this.refresh();
    if (!currentPadSample(sample)) return null;
    if (op !== "control.acquire" && op !== "control.release" && !this.controlled) throw new Error("Take control again before sending input.");
    if (this.uncertain) throw new Error("A reply is unknown. Reconnect explicitly; no command will be replayed.");
    if (!["control.acquire", "control.release", "control.renew"].includes(op)) {
      if (!decimal(s.lease_expires_ms)) throw new Error("Input lease deadline is unavailable.");
      /* Busy input must not starve the timer that normally renews this already
       * explicit lease. Renew before a new action, never reacquire or replay. */
      if (BigInt(s.lease_expires_ms) - BigInt(s.now_ms) < 1800n) {
        await this.send("control.renew", {});
        s = await this.refresh();
        if (!this.controlled) throw new Error("Input control ended before the new action.");
      }
    }
    const seq = this.session.next_seq;
    if (seq > 4294967295) throw new Error("Session sequence exhausted. Reconnect.");
    const lifetime = sample ? Math.min(200, Math.floor(sample.expiresAt - performance.now())) :
      op === "mouse.move_relative" ? 200 : 1500;
    if (sample && (!sample.current() || lifetime <= 0)) return null;
    const envelope = {
      v: 1, boot_id: this.session.boot_id, session_id: this.session.session_id,
      request_id: this.session.request_prefix + BigInt(seq).toString(16).padStart(16, "0"), seq,
      deadline_ms: (BigInt(s.now_ms) + BigInt(lifetime)).toString(),
      epoch: s.epoch, transport_generation: s.transport_generation, target_id: s.target_id,
      ui_revision: s.ui_revision, op,
      args: op === "control.acquire" || op === "control.renew" ? {...args, state_sequence: s.state_sequence} : args,
      ...(op !== "control.acquire" && s.lease_id ? {lease_id: s.lease_id} : {}),
    };
    // Nothing asynchronous may intervene between this check and request/fetch.
    // Once POST starts, retain its real result/uncertainty even if contact ends.
    if (!currentPadSample(sample)) return null;
    const generation = this.generation;
    let answer;
    if (observe) { this.lastKey = null; this.lastTotal = total ?? null; }
    let key = null;
    try {
      const response = await this.request("/api/v1/commands", "POST", envelope);
      answer = response.json;
      const next = Number(answer.next_seq);
      if (!Number.isInteger(next) || (next !== seq && next !== seq + 1)) throw new Error("Missing sequence acknowledgement.");
      if (generation !== this.generation) throw new Error("Browser session changed while input was in flight.");
      this.session.next_seq = next;
      key = decimal(answer.command_key) ? answer.command_key : null;
      if (observe) { this.last = answer; this.lastKey = key; }
      const until = performance.now() + 2200;
      while (ACTIVE.has(answer.state)) {
        if (observe) this.message = describeResult(answer, total);
        this.changed();
        if (!key || performance.now() >= until) throw new Error("Completion not established before the input deadline.");
        await sleep(100);
        const result = await this.request(`/api/v1/requests/${key}`);
        if (![200, 202, 400].includes(result.status) || !result.json.state) throw new Error("Result is unavailable.");
        answer = result.json;
        if (observe) this.last = answer;
      }
    } catch (error) {
      this.uncertain = true;
      this.halt(`${describeResult(null)} ${error.message}`);
      throw error;
    }
    if (observe) this.message = describeResult(answer, total);
    if (answer.error !== "ok" || !["applied", "sent", "completed"].includes(answer.state)) {
      this.halt(describeResult(answer, total));
      throw new Error(this.message);
    }
    await this.refresh(op === "mode.select");
    this.changed();
    return answer;
  }
  async run(op, args = {}, total, sample) {
    if (this.busy) throw new Error("Wait for the current action; input is not queued for replay.");
    this.busy = true;
    this.changed();
    try { return await this.send(op, args, total, sample); }
    catch (error) { this.halt(this.uncertain ? this.message : error.message); throw error; }
    finally { this.busy = false; this.changed(); }
  }
  async acquire() {
    if (!this.session || this.uncertain) throw new Error("Connect a fresh browser session first.");
    await this.run("control.acquire", {producer: "script", lifetime_ms: 3000});
    const s = this.state;
    if (!s?.lease_active || !HEX32.test(s.lease_id || "")) throw new Error("Device has not established an active input lease.");
    this.pin = {target_id: s.target_id, transport_generation: s.transport_generation, epoch: s.epoch, lease_id: s.lease_id};
    this.controlled = true;
    this.message = "Control acquired. Input is sent only from this page's explicit controls.";
    this.changed();
  }
  async text(value) { validateText(value); return this.run("keyboard.text.enqueue", {text: value}, value.length); }
  async checkResult() {
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
async function fingerprint(identity) {
  const bytes = Uint8Array.from(identity.credential.match(/../g), value => parseInt(value, 16));
  return hex(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)));
}

function mount() {
  const $ = id => document.getElementById(id);
  const client = new Controller();
  let identity = null, pad = null, movement = null, pairingBusy = false;
  const error = value => { client.message = value.message; client.changed(); };
  const act = async fn => { try { await fn(); } catch (value) { if (!client.uncertain) error(value); } };
  function paint() {
    const s = client.state;
    $("connection").textContent = client.controlled ? "Controlling" : client.session ? s ? "Connected / idle" : "State unavailable" : "Disconnected";
    $("target").textContent = s ? `${s.target_id.startsWith("usb:") ? "USB" : s.target_id.startsWith("bluetooth:") ? "Bluetooth" : "Input"} · ${s.target_id || "unavailable"}` : "Unavailable";
    $("readiness").textContent = !s ? "Connect to read the actual input state." : s.locked ? "Device locked" :
      !s.input_ready ? "Input not ready" : s.release_pending ? "Waiting for input releases" :
      !s.remote_allowed ? "Remote input is not allowed on Mosaico" : `${s.mode} · ready${client.controlled ? " · this browser owns control" : " · no control acquired here"}`;
    const active = client.controlled && !client.uncertain && s?.input_ready && !s.release_pending && !s.locked && s.remote_allowed;
    const ready = active && !client.busy;
    $("mouse-controls").disabled = !active || s.mode !== "mouse";
    $("keyboard-controls").disabled = !active || s.mode !== "keyboard";
    $("mouse-controls").hidden = s?.mode === "keyboard";
    $("keyboard-controls").hidden = s?.mode !== "keyboard";
    document.querySelectorAll("fieldset button").forEach(button => { button.disabled = !ready; });
    for (const mode of ["mouse", "keyboard"]) {
      $(`${mode}-mode`).disabled = !ready;
      $(`${mode}-mode`).setAttribute("aria-pressed", String(s?.mode === mode));
    }
    $("acquire").disabled = !client.session || client.busy || client.uncertain || client.controlled || !s?.input_ready || s.locked || s.release_pending || !s.remote_allowed;
    $("stop").disabled = !client.session;
    $("disconnect").disabled = !client.session;
    $("refresh").disabled = !client.session || client.busy;
    $("connect").disabled = !identity || client.busy || client.closing || pairingBusy || Boolean(client.session);
    $("pair").disabled = pairingBusy || client.closing || Boolean(client.session);
    $("check").disabled = !identity || pairingBusy || client.closing || Boolean(client.session);
    $("result-check").disabled = !client.lastKey || !client.session || client.busy;
    $("result").textContent = client.message;
    if (!active || s.mode !== "mouse") { pad = null; movement = null; }
  }
  client.onchange = paint;
  async function showIdentity() {
    $("fingerprint").textContent = identity ? `SHA-256 ${await fingerprint(identity)}` : "No browser identity yet";
  }
  async function status(connect = true) {
    if (!identity) throw new Error("Pair this browser first.");
    if (pairingBusy) throw new Error("Wait for the current browser pairing request.");
    pairingBusy = true; paint();
    try {
      const response = await client.request("/api/v1/pairing/status", "POST", identity, null);
      const data = response.json;
      if (![200, 202].includes(response.status) || !HEX64.test(data.fingerprint || "") ||
          data.client_id !== identity.client_id || data.fingerprint !== await fingerprint(identity)) throw new Error("Pairing status unavailable or identity mismatch.");
      $("approval").textContent = describeApproval(data);
      if (data.state === "approved" && connect) {
        if (!data.scopes?.includes("input.control")) throw new Error("This browser has no keyboard/mouse permission. Change its permissions on Mosaico.");
        await client.login(identity);
        $("pair-title").textContent = "Browser access · approved";
        $("pairing").open = false;
      }
    } finally { pairingBusy = false; paint(); }
  }
  $("pair").onclick = () => act(async () => {
    const label = $("client-name").value.trim();
    if (!/^[\x20-\x7e]{1,32}$/.test(label)) throw new Error("Use 1–32 printable ASCII characters for this browser's name.");
    pairingBusy = true; paint();
    try {
      identity = await browserIdentity(true); await showIdentity();
      const hash = await fingerprint(identity);
      const response = await client.request("/api/v1/pairing/requests", "POST", {client_id: identity.client_id, credential_sha256: hash, label}, null);
      if (![200, 202].includes(response.status) || response.json.fingerprint !== hash) throw new Error("Pairing request was not acknowledged. Check approval; no input has been sent.");
      $("approval").textContent = "Waiting for approval on Mosaico. Compare the SHA-256 fingerprint, choose keyboard/mouse permission and duration, then Check approval.";
    } finally { pairingBusy = false; paint(); }
  });
  $("check").onclick = () => act(() => status());
  $("connect").onclick = () => act(() => status());
  $("acquire").onclick = () => act(() => client.acquire());
  $("refresh").onclick = () => act(() => client.refresh());
  $("disconnect").onclick = () => act(() => client.disconnect());
  $("stop").onclick = () => act(async () => {
    pad = movement = null;
    if (client.busy || client.uncertain) await client.disconnect();
    else { await client.run("control.release"); client.halt("Control released. Take control explicitly to send more input."); }
  });
  for (const mode of ["mouse", "keyboard"]) $(`${mode}-mode`).onclick = () => act(() => client.run("mode.select", {mode}));
  document.querySelectorAll("[data-click]").forEach(button => button.onclick = () => act(() => client.run("mouse.click", {button: button.dataset.click})));
  document.querySelectorAll("[data-move]").forEach(button => button.onclick = () => {
    const [dx, dy] = button.dataset.move.split(",").map(Number);
    act(() => client.run("mouse.move_relative", {dx, dy}));
  });
  for (const character of "abcdefghijklmnopqrstuvwxyz1234567890") {
    const button = document.createElement("button");
    button.textContent = character.toUpperCase();
    button.setAttribute("aria-label", `Type ${character}`);
    button.onclick = () => act(() => client.run("keyboard.key_tap", {usage: /[a-z]/.test(character) ? character.charCodeAt(0) - 93 : character === "0" ? 39 : Number(character) + 29}));
    $("letters").append(button);
  }
  for (const [label, usage] of [["Space", 44], ["Enter", 40], ["Backspace", 42], ["Tab", 43], ["Esc", 41], ["Delete", 76], ["Home", 74], ["End", 77], ["←", 80], ["↑", 82], ["↓", 81], ["→", 79]]) {
    const button = document.createElement("button"); button.textContent = label;
    button.onclick = () => act(() => client.run("keyboard.key_tap", {usage}));
    $("function-keys").append(button);
  }
  $("send").onclick = () => act(() => client.text($("text").value));
  $("text").oninput = () => { $("text-count").textContent = `${$("text").value.length} / 32 characters`; };
  $("result-check").onclick = () => act(() => client.checkResult());
  $("forget").onclick = () => act(async () => {
    await client.disconnect(); localStorage.removeItem(IDENTITY_KEY); identity = null;
    $("pair-title").textContent = "Connect this browser"; $("pairing").open = true;
    await showIdentity(); $("approval").textContent = "Identity removed from this browser. Revoke its saved permissions on Mosaico if needed."; paint();
  });
  $("pad").onpointerdown = event => {
    if ($("mouse-controls").disabled || (event.pointerType === "mouse" && event.button !== 0) || pad) return;
    event.preventDefault(); $("pad").setPointerCapture(event.pointerId);
    pad = {id: event.pointerId, x: event.clientX, y: event.clientY}; movement = null;
  };
  $("pad").onpointermove = event => {
    if (!pad || pad.id !== event.pointerId || $("mouse-controls").disabled) return;
    event.preventDefault();
    const now = performance.now();
    if (!movement || now - movement.at > 150) movement = {dx: 0, dy: 0, at: now, contact: pad};
    movement.dx = Math.max(-127, Math.min(127, movement.dx + event.clientX - pad.x));
    movement.dy = Math.max(-127, Math.min(127, movement.dy + event.clientY - pad.y));
    pad.x = event.clientX; pad.y = event.clientY;
  };
  const endPad = event => { if (pad?.id === event.pointerId) { pad = null; movement = null; } };
  $("pad").onpointerup = $("pad").onpointercancel = $("pad").onlostpointercapture = endPad;
  $("pad").onkeydown = event => {
    const moves = {ArrowLeft: [-24, 0], ArrowRight: [24, 0], ArrowUp: [0, -24], ArrowDown: [0, 24]};
    if (!moves[event.key] || event.repeat || $("mouse-controls").disabled) return;
    event.preventDefault(); const [dx, dy] = moves[event.key]; act(() => client.run("mouse.move_relative", {dx, dy}));
  };
  setInterval(() => {
    if (!movement || !pad || !client.controlled || client.busy) return;
    const next = movement; movement = null;
    if (performance.now() - next.at > 150) return;
    const dx = Math.trunc(next.dx), dy = Math.trunc(next.dy);
    if (dx || dy) act(() => client.run("mouse.move_relative", {dx, dy}, undefined,
      {expiresAt: next.at + 150, current: () => pad === next.contact}));
  }, 60);
  setInterval(() => {
    if (!client.session || client.busy || document.hidden) return;
    act(async () => {
      if (client.controlled) await client.run("control.renew");
      else await client.refresh();
    });
  }, 900);
  const suspend = () => { pad = movement = null; if (client.session) act(() => client.disconnect(true)); };
  document.addEventListener("visibilitychange", () => { if (document.hidden) suspend(); });
  globalThis.addEventListener("pagehide", suspend);
  globalThis.addEventListener("offline", suspend);
  act(async () => {
    if (location.protocol !== "https:" || !globalThis.isSecureContext || !crypto.subtle) throw new Error("Open Mosaico's trusted station HTTPS address. Browser identity requires a secure context.");
    identity = await browserIdentity(); await showIdentity(); paint();
    if (identity) await status(false);
  });
  paint();
}
if (typeof document !== "undefined") mount();
