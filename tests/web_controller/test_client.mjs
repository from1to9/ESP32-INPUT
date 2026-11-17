// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
const source = await readFile(new URL("../../firmware/main/web/controller.js", import.meta.url), "utf8");
const {Controller, validateText, describeResult} = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);
const id = "1".repeat(32), prefix = "2".repeat(16), boot = "3".repeat(32), lease = "4".repeat(32);
const statePath = "/api/v1/state?view=protocol";
function movementSample(args, lifetime, current = () => true) {
  const expiresAt = performance.now() + lifetime;
  return () => current() && performance.now() < expiresAt ? {args, expiresAt} : null;
}
function fixture() {
  const calls = [];
  const state = {boot_id: boot, now_ms: "100", epoch: "8", transport_generation: "17", state_sequence: "1", ui_revision: "4",
    target_id: "bluetooth:17", mode: "keyboard", locked: false, release_pending: false, input_ready: true, remote_allowed: true,
    lease_active: true, lease_id: lease, lease_expires_ms: "3100"};
  let fail = false, status = 200;
  let result = {state: "sent", error: "ok", completed_pairs: 3, started_prefix: 3, uncertain_index: -1};
  const client = new Controller(async (path, options) => {
    calls.push({path, options});
    if (path === statePath) return {status: 200, json: async () => ({protocol: {...state}})};
    if (fail) throw new Error("Disconnected after command write");
    const command = JSON.parse(options.body);
    return {status, json: async () => ({...result, command_key: "12", next_seq: command.seq + 1})};
  });
  client.session = {session_id: id, request_prefix: prefix, boot_id: boot, csrf: id, next_seq: 1};
  client.state = {...state}; client.controlled = true;
  client.pin = {target_id: state.target_id, transport_generation: state.transport_generation, epoch: state.epoch, lease_id: lease};
  return {client, calls, state, fail: () => { fail = true; }, result: (value, code = 200) => {
    result = value; status = code;
  }};
}
// Exercise the actual mounted pointer handlers and their one send timer with
// a deterministic clock and a real Controller. Socket and state replies
// stay independently controllable at the await boundaries.
function padFixture() {
  const f = fixture();
  const pointer = {setPointerCapture() {}};
  const controls = {disabled: false};
  const $ = name => name === "pad" ? pointer : controls;
  const start = source.indexOf('  $("pad").onpointerdown =');
  const end = source.indexOf('  }, PAD_CHANNEL_BATCH_MS);', start) + '  }, PAD_CHANNEL_BATCH_MS);'.length;
  const stop = source.indexOf('  $("stop").onclick =');
  const stopEnd = source.indexOf('\n  for (const mode', stop);
  assert(start >= 0 && end > start);
  const constants = source.match(/^const PAD_.*;$/gm).join("\n");
  let tick, queued;
  const mount = new Function("$", "client", "act", "setInterval",
    `${constants}\nlet pad = null, movement = null, lastMovementAt = 0;
     ${source.slice(start, end)}\n${source.slice(stop, stopEnd)}
     return () => { pad = movement = null; };`);
  const clear = mount($, f.client, run => { queued = run(); }, (run, interval) => {
    assert.equal(interval, 30);
    tick = run;
  });
  f.client.state.mode = "mouse";
  f.state.mode = "mouse";
  f.client.stateAt = 0;
  f.client.onchange = () => { if (!f.client.controlled) clear(); };
  const motions = [];
  f.client.motionChannel = {
    motionTerminal: true,
    exchange: async command => {
      const next_seq = String(command.seq + 1);
      if (command.op === "control.renew") return {state: "applied", error: "ok", next_seq, command_key: "11",
        lease_active: true, lease_id: lease, lease_expires_ms: "4100"};
      assert.equal(command.op, "mouse.move_relative");
      motions.push(command);
      return {state: "sent", error: "ok", next_seq, command_key: "12"};
    },
    close() {},
  };
  const fire = (kind, x = 0, y = 0, id = 7) => pointer["onpointer" + kind]({
    clientX: x, clientY: y, pointerId: id, pointerType: "touch", preventDefault() {},
  });
  return {...f, motions, fire, stop: () => { controls.onclick(); return queued; },
    tick: () => { queued = undefined; tick(); return queued; }};
}
function holdState(f) {
  let entered, resume;
  const paused = new Promise(resolve => { entered = resolve; });
  const proceed = new Promise(resolve => { resume = resolve; });
  const request = f.client.fetcher;
  f.client.fetcher = async (path, options) => {
    if (path === statePath) { entered(); await proceed; }
    return request(path, options);
  };
  return {paused, resume};
}
for (let code = 32; code <= 126; ++code) assert.equal(validateText(String.fromCharCode(code)), String.fromCharCode(code));
for (const text of ["", "x".repeat(33), "A\nB", "A\rB", "A\tB", "ok\u4e2d", "ok😀", "A\0B", "x\x7f"]) {
  const f = fixture();
  await assert.rejects(f.client.text(text));
  assert.equal(f.calls.length, 0, "entire text must be validated before reading or sending anything");
}
{
  const f = fixture();
  f.result({state: "sent", error: "ok", completed_pairs: 32, started_prefix: 32, uncertain_index: -1});
  await f.client.text("A!? ".repeat(8));
  const command = JSON.parse(f.calls[1].options.body);
  assert.equal(command.args.text.length, 32);
  assert.equal(command.request_id, prefix + "0000000000000001");
  assert.equal(command.epoch, "8"); assert.equal(command.lease_id, lease);
  assert.equal(f.calls[1].options.headers["X-CSRF-Token"], id);
  for (const {path, options} of f.calls) {
    assert.match(path, /^\/api\/v1\//);
    assert.equal(options.mode, "cors");
    assert.equal(options.credentials, "same-origin");
    assert.equal(options.redirect, "error");
    assert.equal(options.cache, "no-store");
  }
  assert.equal(f.client.session.next_seq, 2);
  assert.match(f.client.message, /32 \/ 32 key pairs complete/);
  const originalMessage = f.client.message, originalKey = f.client.lastKey;
  await f.client.run("control.renew");
  assert.equal(f.client.message, originalMessage, "renewal must preserve the last input result");
  assert.equal(f.client.lastKey, originalKey);
}
{
  const f = fixture(); f.fail();
  await assert.rejects(f.client.text("abc"));
  assert(f.client.uncertain && !f.client.controlled);
  assert.equal(f.calls.filter(call => call.options.method === "POST").length, 1);
  await assert.rejects(f.client.text("abc"));
  assert.equal(f.calls.filter(call => call.options.method === "POST").length, 1, "unknown commands must not replay");
}
for (const change of [s => { s.target_id = "usb:18"; }, s => { s.epoch = "9"; }, s => { s.lease_active = false; },
  s => { s.locked = true; }, s => { s.remote_allowed = false; }, s => { s.input_ready = false; }]) {
  const f = fixture(); change(f.state);
  await assert.rejects(f.client.run("keyboard.key_tap", {usage: 4}));
  assert(!f.client.controlled);
  assert.equal(f.calls.filter(call => call.options.method === "POST").length, 0);
}
for (const leaseLost of [false, true]) {
  const f = fixture();
  f.result({state: "rejected", error: "expired"}, 400);
  if (leaseLost) {
    const fetch = f.client.fetcher;
    f.client.fetcher = (url, options) => {
      if (options.method === "POST") f.state.lease_active = false;
      return fetch(url, options);
    };
  }
  const result = await f.client.run("mouse.move_relative", {dx: 8, dy: 3});
  assert.equal(result.state, "rejected");
  assert.equal(result.error, "expired");
  assert.equal(f.client.controlled, !leaseLost);
  assert.equal(f.client.lastKey, "12");
  assert.equal(f.client.session.next_seq, 2);
  assert.deepEqual(f.calls.map(call => call.options.method), ["GET", "POST", "GET"]);
}
for (const outcome of ["sent", "stale", "reply lost"]) {
  const {client, calls} = fixture();
  client.state.mode = "mouse";
  client.stateAt = performance.now();
  let submitted;
  client.motionChannel = {
    exchange: async command => {
      assert(!submitted);
      submitted = command;
      const estimatedTime = BigInt(command.deadline_ms) - 200n;
      assert(estimatedTime >= 100n &&
        estimatedTime <= 100n + BigInt(Math.floor(performance.now() - client.stateAt)));
      if (outcome === "reply lost") throw new Error(outcome);
      return outcome === "sent" ? {state: "sent", error: "ok", next_seq: "2", command_key: "12"} :
        {state: "rejected", error: "stale", next_seq: "2", command_key: "12"};
    },
    close() {},
  };
  const button = client.run("mouse.move_relative", {dx: 24, dy: 0});
  if (outcome === "sent") assert.equal((await button).state, "sent");
  else await assert.rejects(button, /stale|reply lost/);
  assert.equal(submitted.op, "mouse.move_relative");
  assert.deepEqual(submitted.args, {dx: 24, dy: 0});
  assert.equal(submitted.lease_id, lease);
  assert.equal(submitted.epoch, "8");
  assert.equal(calls.length, 0);
  assert.equal(client.controlled, outcome === "sent");
  assert.equal(client.uncertain, outcome === "reply lost");
}
for (const terminal of [
  {command_key: "12", state: "rejected", error: "expired"},
  {command_key: "13", state: "rejected", error: "expired"},
  {state: "rejected", error: "expired"},
  {command_key: "12", state: "cancelled", error: "expired"},
]) {
  const f = fixture();
  f.result({state: "accepted", error: "ok"}, 202);
  const fetch = f.client.fetcher;
  f.client.fetcher = (url, options) => {
    if (url !== "/api/v1/requests/12") return fetch(url, options);
    f.calls.push({path: url, options});
    return {status: 400, json: async () => terminal};
  };
  const known = terminal.command_key === "12";
  const recoverable = known && terminal.state === "rejected";
  const move = f.client.run("mouse.move_relative", {dx: 8, dy: 3});
  if (recoverable) assert.equal(await move, terminal);
  else await assert.rejects(move, known ? /cancelled/ : /Result is unavailable/);
  assert.equal(f.client.controlled, recoverable);
  assert.equal(f.client.uncertain, !known);
  if (!known) assert.match(f.client.message, /Reply unknown/);
  assert.equal(f.client.session.next_seq, 2);
  assert.deepEqual(f.calls.map(call => call.path), [
    statePath, "/api/v1/commands", "/api/v1/requests/12", ...(recoverable ? [statePath] : []),
  ]);
}
for (const outcome of ["matched", "wrong key", "duplicate admission", "lost terminal", "inline terminal", "early terminal"]) {
  const {client, calls} = fixture();
  const OriginalSocket = globalThis.WebSocket;
  const messages = [];
  class FakeSocket extends EventTarget {
    static OPEN = 1;
    readyState = FakeSocket.OPEN;
    bufferedAmount = 0;
    protocol = "esp32-control-v1";
    constructor() {
      super();
      queueMicrotask(() => this.dispatchEvent(new Event("open")));
    }
    send(serialized) {
      const input = JSON.parse(serialized);
      messages.push(input);
      queueMicrotask(() => {
        const reply = value => this.onmessage?.({data: JSON.stringify(value)});
        if (input.type === "motion_terminal") {
          reply({motion_terminal: true});
          return;
        }
        const admission = {command_key: "12", next_seq: 2, state: "accepted", error: "ok"};
        const terminal = {...admission, motion_terminal: true, state: "sent"};
        if (outcome === "inline terminal") {
          reply({command_key: "12", next_seq: 2, state: "sent", error: "ok"});
          return;
        }
        if (outcome !== "early terminal") reply(admission);
        if (outcome === "lost terminal") this.close();
        else if (outcome === "wrong key") reply({...terminal, command_key: "13"});
        else if (outcome === "duplicate admission") reply(admission);
        else reply(terminal);
      });
    }
    close() {
      if (this.readyState !== FakeSocket.OPEN) return;
      this.readyState = 3;
      queueMicrotask(() => this.onclose?.());
    }
  }
  globalThis.WebSocket = FakeSocket;
  try {
    client.motionURL = "ws://device.test/api/v1/channel";
    client.state.mode = "mouse";
    client.stateAt = performance.now();
    const request = client.request.bind(client);
    client.request = async (path, ...args) => {
      if (path === "/api/v1/capabilities") return {status: 200, json: {websocket: true, websocket_motion_terminal: true}};
      if (path === "/api/v1/ws-ticket") return {status: 200, json: {ticket: "0".repeat(64)}};
      return request(path, ...args);
    };
    await client.openMotionChannel(client.generation);
    const move = client.run("mouse.move_relative", {dx: 8, dy: 3});
    const success = outcome === "matched" || outcome === "inline terminal";
    if (success) assert.equal((await move).state, "sent");
    else {
      await assert.rejects(move, /reply unavailable/);
      assert.match(client.message, /Reply unknown/);
      await assert.rejects(client.run("mouse.move_relative", {dx: 8, dy: 3}));
    }
    assert.equal(client.uncertain, !success);
    assert.equal(client.controlled, success);
    assert.equal(client.lastKey, outcome === "early terminal" ? null : "12");
    assert.deepEqual(messages.map(item => item.op || item.type), ["motion_terminal", "mouse.move_relative"]);
    assert(!calls.some(call => call.path.startsWith("/api/v1/commands") || call.path.startsWith("/api/v1/requests/")));
  } finally {
    client.motionChannel?.close();
    globalThis.WebSocket = OriginalSocket;
  }
}
{
  const f = fixture(); f.state.now_ms = "2500";
  await f.client.run("keyboard.key_tap", {usage: 4});
  assert.deepEqual(f.calls.filter(call => call.options.method === "POST").map(call => JSON.parse(call.options.body).op),
    ["control.renew", "keyboard.key_tap"], "continuous input must renew its explicit lease before the next action");
}
{
  const f = fixture();
  f.result({state: "cancelled", error: "stale", completed_pairs: 2, started_prefix: 3, uncertain_index: 2});
  await assert.rejects(f.client.text("ABCDE"));
  assert.match(f.client.message, /2 \/ 5 key pairs complete/);
  assert.match(f.client.message, /Character 3 has an uncertain/);
  assert(!f.client.controlled);
}
assert.match(describeResult(null), /unknown/);
// The same local sample survives every awaited preflight/renewal read, but may
// never become a new command after its contact ends or original deadline passes.
for (const stage of [1, 2, 3]) {
  const f = fixture();
  if (stage > 1) f.state.now_ms = "2500";
  let active = true, reads = 0, arrived, resume;
  const paused = new Promise(resolve => { arrived = resolve; });
  const proceed = new Promise(resolve => { resume = resolve; });
  const fetcher = f.client.fetcher;
  f.client.fetcher = async (url, options) => {
    if (url === statePath && ++reads === stage) { arrived(); await proceed; }
    return fetcher(url, options);
  };
  const beforeMessage = f.client.message;
  const moving = f.client.run("mouse.move_relative", {}, undefined,
    movementSample({dx: 27, dy: 9}, 10000, () => active));
  await paused; active = false; resume();
  assert.equal(await moving, null, "contact invalidation must discard an unsubmitted sample after any asynchronous stage");
  const posts = f.calls.filter(call => call.options.method === "POST").map(call => JSON.parse(call.options.body));
  assert(!posts.some(command => command.op === "mouse.move_relative"));
  assert.equal(f.client.session.next_seq, stage === 1 ? 1 : 2, "only an actual renewal may consume sequence");
  assert(f.client.controlled && !f.client.uncertain && !f.client.busy);
  assert.equal(f.client.message, beforeMessage);
}
{
  const f = fixture(); let arrived, resume;
  const paused = new Promise(resolve => { arrived = resolve; });
  const proceed = new Promise(resolve => { resume = resolve; });
  const fetcher = f.client.fetcher;
  f.client.fetcher = async (url, options) => {
    if (url === statePath) { arrived(); await proceed; }
    return fetcher(url, options);
  };
  const moving = f.client.run("mouse.move_relative", {}, undefined, movementSample({dx: 27, dy: 9}, 30));
  await paused; await new Promise(resolve => setTimeout(resolve, 60)); resume();
  assert.equal(await moving, null);
  assert(!f.calls.some(call => call.options.method === "POST"));
  assert(f.client.controlled && !f.client.uncertain);
}
{
  const f = fixture();
  await f.client.run("mouse.move_relative", {}, undefined, movementSample({dx: 8, dy: 3}, 150));
  const command = JSON.parse(f.calls.find(call => call.options.method === "POST").options.body);
  assert(BigInt(command.deadline_ms) > 100n && BigInt(command.deadline_ms) <= 250n,
    "the wire deadline may use only the sample's original remaining freshness");
}
for (const lostReply of [false, true]) {
  const f = fixture(); let active = true, submitted, finish;
  const posted = new Promise(resolve => { submitted = resolve; });
  const reply = new Promise((resolve, reject) => { finish = lostReply ? reject : resolve; });
  const fetcher = f.client.fetcher;
  f.client.fetcher = async (url, options) => {
    if (options.method === "POST") {
      const response = await fetcher(url, options); submitted(); await reply; return response;
    }
    return fetcher(url, options);
  };
  const moving = f.client.run("mouse.move_relative", {}, undefined, movementSample({dx: 8, dy: 3}, 150, () => active));
  await posted; active = false;
  if (lostReply) {
    finish(new Error("Reply lost after submission"));
    await assert.rejects(moving);
    assert(f.client.uncertain && !f.client.controlled);
  } else { finish(); assert.equal((await moving).state, "sent"); }
  assert.equal(f.calls.filter(call => call.options.method === "POST").length, 1);
}
{
  const realPerformance = globalThis.performance;
  let now = 2000;
  globalThis.performance = {now: () => now};
  try {
    const f = padFixture(), held = holdState(f);
    f.fire("down");
    f.fire("move", 10, 7);
    now = 2010;
    const first = f.tick();
    await held.paused;
    now = 2040;
    f.fire("move", 80, -7);
    now = 2050;
    f.fire("move", 72, 4);
    held.resume();
    await first;
    assert.deepEqual(f.motions[0].args, {dx: 72, dy: 4}, "preserve the whole fresh aggregate across a state read");
    assert.equal(f.motions[0].deadline_ms, "200", "use the oldest sample's remaining freshness");
    assert.equal(f.tick(), undefined);
    assert.equal(f.motions.length, 1);

    let sent, finish;
    const sending = new Promise(resolve => { sent = resolve; });
    const result = new Promise(resolve => { finish = resolve; });
    const exchange = f.client.motionChannel.exchange;
    f.client.motionChannel.exchange = async command => {
      const response = await exchange(command);
      sent(); await result;
      return response;
    };
    now = 2060;
    f.fire("move", 90, 8);
    const inFlight = f.tick();
    await sending;
    now = 2110;
    f.fire("move", 120, -3);
    now = 2130;
    f.fire("move", 109, 11);
    finish();
    await inFlight;
    await f.tick();
    assert.deepEqual(f.motions.slice(1).map(move => move.args), [{dx: 18, dy: 4}, {dx: 19, dy: 3}]);
    assert.equal(f.tick(), undefined, "a submitted snapshot is detached exactly once");

    for (const boundary of [2150, 2151]) {
      now = 2000;
      const f = padFixture(), held = holdState(f);
      f.fire("down");
      f.fire("move", 80, -30);
      const waiting = f.tick();
      await held.paused;
      now = 2149;
      f.fire("move", 85, -25);
      now = boundary;
      held.resume();
      assert.equal(await waiting, null);
      assert.deepEqual(f.motions, [], "new samples cannot renew an older aggregate at its deadline");
      f.fire("move", 97, -38);
      await f.tick();
      assert.deepEqual(f.motions[0].args, {dx: 12, dy: -13});
    }

    now = 2000;
    const renew = padFixture();
    renew.state.lease_expires_ms = "1800";
    let renewing, finishRenew;
    const renewalStarted = new Promise(resolve => { renewing = resolve; });
    const renewed = new Promise(resolve => { finishRenew = resolve; });
    const normal = renew.client.motionChannel.exchange;
    renew.client.motionChannel.exchange = async command => {
      if (command.op === "control.renew") { renewing(); await renewed; }
      return normal(command);
    };
    renew.fire("down");
    renew.fire("move", 120, 90);
    const awaitingRenew = renew.tick();
    await renewalStarted;
    now = 2150;
    renew.fire("move", 300, -80);
    now = 2160;
    renew.fire("move", 280, -75);
    finishRenew();
    await awaitingRenew;
    assert.deepEqual(renew.motions[0].args, {dx: 107, dy: -122},
      "deadline equality starts fresh, preserving two-axis bounds and reversal");
    assert.equal(renew.motions[0].seq, 2);
    assert.equal(renew.motions[0].deadline_ms, "400");

    for (const ending of ["up", "cancel", "takeover"]) {
      now = 2000;
      const f = padFixture(), held = holdState(f);
      f.fire("down"); f.fire("move", 70, 90);
      const waiting = f.tick();
      await held.paused;
      if (ending === "takeover") f.state.lease_active = false;
      else {
        f.fire(ending);
        f.fire("down", 5, 3, 8); f.fire("move", 17, 19, 8);
      }
      held.resume();
      assert.equal(await waiting, null);
      assert.deepEqual(f.motions, []);
      await f.tick();
      assert.deepEqual(f.motions.map(move => move.args), ending === "takeover" ? [] : [{dx: 12, dy: 16}]);
    }
    now = 2000;
    const stopped = padFixture(), waitingState = holdState(stopped);
    const fetcher = stopped.client.fetcher;
    stopped.client.fetcher = (path, options) => options.method === "DELETE" ?
      Promise.resolve({status: 200, json: async () => ({})}) : fetcher(path, options);
    stopped.fire("down"); stopped.fire("move", 26, -12);
    const waitingMove = stopped.tick();
    await waitingState.paused;
    stopped.fire("move", 54, 18);
    await stopped.stop();
    waitingState.resume();
    await assert.rejects(waitingMove, /session changed/i);
    assert.equal(stopped.tick(), undefined);
    assert.deepEqual(stopped.motions, []);
  } finally {
    globalThis.performance = realPerformance;
  }
}
{
  const f = fixture();
  let finish;
  f.client.fetcher = () => new Promise(resolve => { finish = resolve; });
  const reading = f.client.refresh();
  f.client.session = null;
  finish({status: 200, json: async () => ({protocol: f.state})});
  await assert.rejects(reading, /session changed/);
  assert.equal(f.client.session, null, "late reads must never restore a closed session");
}
console.log("PASS: whole-text US ANSI validation, typed single-submit envelopes, cookie/CSRF options, retained progress, no replay after lost replies and target/owner/readiness invalidation");
