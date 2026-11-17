// SPDX-License-Identifier: Apache-2.0
// The browser owns no HID state. Every action uses the device's typed input API.
const ACTIVE = new Set(["accepted", "running", "applying", "cleanup_pending"]);
const TERMINAL = new Set(["rejected", "applied", "sent", "completed", "cancelled", "failed"]);
const HEX32 = /^[0-9a-f]{32}$/;
const HEX64 = /^[0-9a-f]{64}$/;
const DECIMAL = /^(0|[1-9][0-9]{0,19})$/;
const THEME_KEY = "passport.browser.theme.v1";
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
  if (result.state === "applied" && result.error === "ok") return "Device applied the action.";
  return `${result.state || "Unknown"}: ${result.error || "unknown"}${result.reason ? ` (${result.reason})` : ""}. ${counts}${uncertain}`.trim();
}

export class Controller {
  constructor(fetcher = globalThis.fetch.bind(globalThis)) {
    this.fetcher = fetcher;
    this.session = null;
    this.state = null;
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
  changed() { this.onchange(); }
  halt(message) {
    this.controlled = false;
    this.pin = null;
    if (message) this.message = message;
    this.changed();
  }
  async request(path, method = "GET", body, csrf = this.session?.csrf, keepalive = false, token = this.session?.session_token) {
    const abort = new AbortController();
    const timeout = setTimeout(() => abort.abort(), 3500);
    try {
      // Preserve a concrete Origin under no-referrer in Safari. API paths are
      // relative and the page's connect-src policy remains restricted to self.
      const response = await this.fetcher(path, {
        method, mode: "cors", credentials: "same-origin", cache: "no-store", redirect: "error",
        headers: {Accept: "application/json", ...(token ? {Authorization: `Bearer ${token}`} : {}),
          ...(path === "/api/v1/open-session" ? {"X-Passport-Open": "1"} : {}), ...(method === "GET" ? {} : {"Content-Type": "application/json"}),
          ...(csrf && method !== "GET" ? {"X-CSRF-Token": csrf} : {})},
        ...(body === undefined ? {} : {body: JSON.stringify(body)}), signal: abort.signal, keepalive,
      });
      const json = await response.json();
      return {status: response.status, json};
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
      ++this.generation; this.uncertain = false; this.lastKey = null;
      await this.refresh();
      const caps = await this.request("/api/v1/capabilities");
      if (caps.json.access_policy !== "open_network") throw new Error("This page requires the Passport open-network service.");
      this.operations = new Map();
      for (const offset of [10, 18, 26, 34, 62, 70, 150]) {
        const page = await this.request(`/api/v1/operations?cursor=${offset}&limit=8`);
        if (page.status !== 200 || !Array.isArray(page.json.items)) throw new Error("Operation availability is unknown.");
        for (const op of page.json.items) this.operations.set(op.name, op.available === true);
      }
      this.message = "Browser connected. Take control when the host is ready.";
    } catch (error) { await this.disconnect(); throw error; }
    finally { this.busy = false; this.changed(); }
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
      this.halt("Control ended: local takeover, target/readiness change or lease expiry. Take control again only after checking Passport.");
    }
    if (this.controlled && allowEpochChange) this.pin.epoch = s.epoch;
    this.changed();
    return s;
  }
  async send(op, args, total, sample) {
    if (!this.operations.get(op)) throw new Error("This operation is unavailable on Passport.");
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
      ["mouse.move_relative", "mouse.scroll"].includes(op) ? 200 : 1500;
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
    this.renewing = op === "control.renew";
    if (!sample && !this.renewing) this.message = op === "control.acquire" ? "Requesting control…" : "Sending input…";
    this.changed();
    try { return await this.send(op, args, total, sample); }
    catch (error) { this.halt(this.uncertain ? this.message : error.message); throw error; }
    finally { this.busy = this.renewing = false; this.changed(); }
  }
  async acquire() {
    if (!this.session || this.uncertain) throw new Error("Connect a fresh browser session first.");
    if (!this.operations.get("control.acquire")) throw new Error("Input control is unavailable.");
    await this.run("control.acquire", {producer: "script", lifetime_ms: 3000});
    const s = this.state;
    if (!s?.lease_active || !HEX32.test(s.lease_id || "")) throw new Error("Device has not established an active input lease.");
    this.pin = {target_id: s.target_id, transport_generation: s.transport_generation, epoch: s.epoch, lease_id: s.lease_id};
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
        const response = await this.request("/api/v1/sessions/current", "DELETE", undefined, session.csrf, keepalive, session.session_token);
        if (response.status !== 200) throw new Error();
        if (!this.uncertain) this.message = "Browser session closed. Connect again explicitly; no input lease will resume.";
      } catch { this.message = "Session close unconfirmed. Control is stopped locally; the device's short lease expires independently."; }
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
function mount() {
  const $ = id => document.getElementById(id), client = new Controller();
  themeControl($('theme'));
  let section = 'mouse', pad = null, drag = null, movement = null, dragAt = 0, loading = false;
  let lists = {}, editing = null;
  const note = text => { $('management-result').textContent = text; };
  const clear = () => { pad = drag = movement = null; $('drag').setAttribute('aria-pressed', 'false'); $('pad').dataset.drag = 'false'; };
  const act = async fn => { try { await fn(); } catch (error) { if (!client.uncertain) { client.message = error.message; client.changed(); } } };
  const can = op => client.session && !client.foregroundBusy && !client.uncertain && !loading && !client.state?.locked && client.operations.get(op);
  function paint() {
    const s = client.state, active = client.controlled && !client.uncertain && s?.input_ready && !s.locked && !s.release_pending;
    const connection = client.controlled ? 'This browser is controlling' : client.session ? s ? s.locked ? 'Passport is locked' : 'Browser connected · idle' : 'State unavailable' : 'Browser disconnected';
    if ($('connection').textContent !== connection) $('connection').textContent = connection;
    $('status-dot').dataset.active = String(Boolean(client.session && s));
    $('target').textContent = s ? `${s.target_id || 'No host selected'} · ${s.locked ? 'Unlock on Passport' : s.input_ready ? 'Input ready' : 'Input not ready'}` : client.message !== 'Nothing sent.' ? client.message : 'Connect to read the host and Lock state.';
    $('connect').hidden = Boolean(client.session); $('connect').disabled = client.busy || client.closing;
    $('acquire').hidden = !client.session || client.controlled;
    $('acquire').disabled = !can('control.acquire') || !s?.input_ready || s.release_pending;
    $('stop').disabled = !client.session; $('disconnect').hidden = !client.session; $('disconnect').disabled = client.closing;
    $('mouse-controls').disabled = !active || s.mode !== 'mouse';
    $('keyboard-controls').disabled = !active || s.mode !== 'keyboard';
    // Renewal still owns the command lane; only foreground work or actual
    // control loss changes input availability in the page.
    document.querySelectorAll('[data-op]').forEach(button => { button.disabled = !active || client.foregroundBusy || !client.operations.get(button.dataset.op); });
    $('drag').disabled = !active || !client.operations.get('mouse.drag.begin');
    $('send').disabled = !active || client.foregroundBusy || !client.operations.get('keyboard.text.enqueue');
    if ($('result').textContent !== client.message) $('result').textContent = client.message;
    $('result-check').disabled = !client.session || !client.lastKey || client.foregroundBusy;
    $('readiness').textContent = !s ? 'Unlock Passport and pair from your host’s Bluetooth settings.' : s.locked ? 'Locked on Passport. Unlock with a fresh OK hold.' : !s.input_ready ? 'Waiting for the host, encryption, subscriptions and neutral release.' : `${s.mode} input ready. ${client.controlled ? 'This browser owns the lease.' : 'Take control to begin.'}`;
    const peer = lists.peers?.peers?.find(p => p.original === s?.target_id);
    $('host-name').textContent = peer?.label || s?.target_id || 'Your host awaits';
    $('owner').textContent = client.controlled ? 'This browser' : s?.remote_owner ? 'Another controller' : 'Not acquired';
    $('address').textContent = location.host;
    if (!active || s.mode !== 'mouse') clear();
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
    for (const tab of ['mouse', 'keyboard', 'connections']) $(`tab-${tab}`).setAttribute('aria-pressed', String(next === tab));
    $('input-page').hidden = next === 'connections'; $('connections-page').hidden = next !== 'connections';
    $('mouse-page').hidden = next !== 'mouse'; $('keyboard-page').hidden = next !== 'keyboard';
    if (next === 'connections') {
      if (client.controlled) await stop();
      if (client.session) await refreshLists();
    } else if (client.controlled && client.state?.mode !== next) {
      if (client.busy) await stop();
      else await client.run('mode.select', {mode: next});
    }
    paint();
  }
  document.querySelectorAll('[data-tab]').forEach(button => { button.onclick = () => act(() => selectSection(button.dataset.tab)); });
  $('open-connections').onclick = () => act(() => selectSection('connections'));
  $('connect').onclick = () => act(async () => { await client.login(); if (section === 'connections') await refreshLists(); });
  $('acquire').onclick = () => act(async () => { await client.acquire(); if (section !== 'connections' && client.state.mode !== section) await client.run('mode.select', {mode: section}); });
  $('stop').onclick = () => act(stop);
  $('disconnect').onclick = () => act(async () => { clear(); await client.disconnect(); lists = {}; renderLists(); });
  $('result-check').onclick = () => act(() => client.checkResult());
  for (const button of document.querySelectorAll('[data-click]')) {
    button.dataset.op = 'mouse.click'; button.onclick = () => act(() => client.run('mouse.click', {button: button.dataset.click}));
  }
  for (const button of document.querySelectorAll('[data-wheel]')) {
    button.dataset.op = 'mouse.scroll'; button.onclick = () => act(() => client.run('mouse.scroll', {wheel: Number(button.dataset.wheel)}));
  }
  for (const [name, usage] of [['Esc',41],['Tab',43],['Enter',40],['Backspace',42],['Space',44],['←',80],['↓',81],['→',79],['Home',74],['End',77],['↑',82],['Delete',76]]) {
    const button = document.createElement('button'); button.textContent = name; button.dataset.op = 'keyboard.key_tap';
    button.onclick = () => act(() => client.run('keyboard.key_tap', {usage})); $('keycaps').append(button);
  }
  document.querySelectorAll('[data-shortcut]').forEach(button => {
    button.dataset.op = 'keyboard.key_tap'; button.onclick = () => act(() => client.run('keyboard.key_tap', {usage: {copy:6,paste:25,undo:29}[button.dataset.shortcut], modifiers:Number($('shortcut-modifier').value)}));
  });
  $('send').onclick = () => act(() => client.text($('text').value));
  $('text').oninput = () => { $('text-count').textContent = `${$('text').value.length} / 32 characters`; };
  const point = e => ({id: e.pointerId, x: e.clientX, y: e.clientY});
  function move(e, contact) {
    if (!contact || contact.id !== e.pointerId || $('mouse-controls').disabled) return;
    e.preventDefault(); const now = performance.now();
    if (!movement || now - movement.at > 150 || movement.contact !== contact) movement = {dx: 0, dy: 0, at: now, contact};
    movement.dx = Math.max(-127, Math.min(127, movement.dx + e.clientX - contact.x));
    movement.dy = Math.max(-127, Math.min(127, movement.dy + e.clientY - contact.y));
    contact.x = e.clientX; contact.y = e.clientY;
  }
  $('pad').onpointerdown = e => {
    if ($('mouse-controls').disabled || pad || (e.pointerType === 'mouse' && e.button !== 0)) return;
    e.preventDefault(); pad = point(e); movement = null; $('pad').setPointerCapture(e.pointerId);
  };
  $('pad').onpointermove = e => move(e, pad);
  const endPad = e => {
    if (pad?.id !== e.pointerId) return;
    pad = movement = null;
    if (drag) act(finishDrag);
  };
  $('pad').onpointerup = $('pad').onpointercancel = $('pad').onlostpointercapture = endPad;
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
      await client.run('mouse.drag.begin', {button:'left',deadman_ms:1500});
      if (drag !== contact) { await stop(); return; }
      dragAt = performance.now(); $('drag').setAttribute('aria-pressed','true'); $('pad').dataset.drag = 'true';
    } catch (error) { clear(); throw error; }
  }
  $('drag').onpointerdown = e => {
    if (client.busy || drag || $('mouse-controls').disabled || (e.pointerType === 'mouse' && e.button !== 0)) return;
    e.preventDefault(); $('drag').setPointerCapture(e.pointerId); act(() => beginDrag(point(e)));
  };
  $('drag').onpointermove = e => move(e, drag);
  async function finishDrag() {
    clear();
    if (!client.session) return;
    if (client.busy || client.uncertain) await client.disconnect();
    else if (client.controlled) await client.run('mouse.drag.end',{button:'left'});
  }
  const endDrag = e => { if (drag?.id === e.pointerId) act(finishDrag); };
  $('drag').onpointerup = $('drag').onpointercancel = $('drag').onlostpointercapture = endDrag;
  $('drag').onkeydown = e => { if ([' ', 'Enter'].includes(e.key) && !e.repeat) { e.preventDefault(); act(() => beginDrag({id:'key'})); } };
  $('drag').onkeyup = e => { if ([' ', 'Enter'].includes(e.key) && drag?.id === 'key') { e.preventDefault(); act(finishDrag); } };
  setInterval(() => {
    if (!client.controlled || client.busy || document.hidden) return;
    if (drag && performance.now() - dragAt >= 400) {
      act(async () => { await client.run('mouse.drag.refresh',{button:'left',deadman_ms:1500}); dragAt = performance.now(); }); return;
    }
    if (!movement) return;
    const next = movement; movement = null;
    if (performance.now() - next.at > 150) return;
    const dx = Math.trunc(next.dx), dy = Math.trunc(next.dy);
    if (dx || dy) act(() => client.run('mouse.move_relative', {dx,dy}, undefined,
      {expiresAt:next.at+150,current:() => next.contact === pad || next.contact === drag}));
  },60);
  setInterval(() => {
    if (!client.session || client.busy || loading || document.hidden) return;
    act(async () => { if (client.controlled) await client.run('control.renew'); else await client.refresh(); });
  },900);
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
