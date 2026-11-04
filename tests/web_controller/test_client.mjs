// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
const source = await readFile(new URL("../../firmware/main/web/controller.js", import.meta.url), "utf8");
const {Controller, validateText, describeResult} = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);
const id = "1".repeat(32), prefix = "2".repeat(16), boot = "3".repeat(32), lease = "4".repeat(32);
function fixture() {
  const calls = [];
  const state = {boot_id: boot, now_ms: "100", epoch: "8", transport_generation: "17", state_sequence: "1", ui_revision: "4",
    target_id: "bluetooth:17", mode: "keyboard", locked: false, release_pending: false, input_ready: true, remote_allowed: true,
    lease_active: true, lease_id: lease, lease_expires_ms: "3100"};
  let fail = false, result = {state: "sent", error: "ok", completed_pairs: 3, started_prefix: 3, uncertain_index: -1};
  const client = new Controller(async (path, options) => {
    calls.push({path, options});
    if (path === "/api/v1/state") return {status: 200, json: async () => ({protocol: {...state}})};
    if (fail) throw new Error("Disconnected after command write");
    const command = JSON.parse(options.body);
    return {status: 200, json: async () => ({...result, command_key: "12", next_seq: command.seq + 1})};
  });
  client.session = {session_id: id, request_prefix: prefix, boot_id: boot, csrf: id, next_seq: 1};
  client.state = {...state}; client.controlled = true;
  client.pin = {target_id: state.target_id, transport_generation: state.transport_generation, epoch: state.epoch, lease_id: lease};
  return {client, calls, state, fail: () => { fail = true; }, result: value => { result = value; }};
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
  assert.equal(f.calls[1].options.mode, "same-origin");
  assert.equal(f.calls[1].options.redirect, "error");
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
