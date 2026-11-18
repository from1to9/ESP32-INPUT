// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import {MotionWindow, motionWindowRequest} from "../../components/control_protocol/web/motion_window.mjs";

globalThis.WebSocket = {OPEN: 1};
const legacy = {websocket_motion_window: true};
const frame = {v: 1, type: "motion_window"};
assert.deepEqual(motionWindowRequest(legacy), frame);
assert.equal(motionWindowRequest({}, 3), null);
assert.equal(motionWindowRequest(legacy, 1), null);
assert.throws(() => motionWindowRequest({...legacy, websocket_motion_window_max: 0}));
for (const width of [2, 3, 4]) {
  const capabilities = {...legacy, websocket_motion_window_max: width};
  assert.deepEqual(motionWindowRequest(capabilities), {...frame, motion_window: width});
}
const orderedRequest = motionWindowRequest({...legacy, websocket_motion_window_max: 2, websocket_motion_ordered: true}, 2, true);
assert.deepEqual(orderedRequest, {...frame, motion_window: 2, motion_ordered: true});
assert.throws(() => MotionWindow.fromSelection(null, orderedRequest, {motion_window: 2}));
assert.throws(() => MotionWindow.fromSelection(null, frame, {motion_window: 3}));

function fixture(request) {
  const sent = [];
  const socket = {readyState: 1, bufferedAmount: 0, send: data => sent.push(JSON.parse(data))};
  let unresolved;
  const window = MotionWindow.fromSelection(socket, request, request, () => {
    socket.readyState = 3;
    unresolved = window.close();
  });
  const send = seq => window.send({op: "mouse.move_relative", seq});
  const reply = (seq, key, state, fields) => window.receive({command_seq: seq, command_key: key,
    state, error: "ok", next_seq: seq + 1, ...fields});
  return {window, send, reply, sent, get unresolved() { return unresolved; }};
}

{
  const f = fixture({...frame, motion_window: 3});
  const first = f.send(17);
  assert.throws(() => f.send(18), /unavailable/);
  f.reply(17, "25", "accepted");
  const second = f.send(18);
  f.reply(18, "26", "accepted");
  const third = f.send(19);
  f.reply(19, "27", "accepted");
  assert.throws(() => f.send(20), /unavailable/);
  f.reply(19, "27", "sent", {motion_terminal: true});
  f.reply(17, "25", "sent", {motion_terminal: true, next_seq: 20});
  f.reply(18, "26", "rejected", {error: "expired", motion_terminal: true, next_seq: 20});
  assert.deepEqual((await Promise.all([first, second, third])).map(x => x.state), ["sent", "rejected", "sent"]);
  assert.equal(f.window.size, 0);
}
for (const fault of [false, true]) {
  const f = fixture(orderedRequest);
  const first = f.send(f.window.nextSequence(7));
  const second = f.send(f.window.nextSequence(7));
  assert.throws(() => f.send(9), /unavailable/);
  assert.deepEqual(f.sent.map(x => x.seq), [7, 8]);
  if (fault) {
    f.reply(7, "56", "accepted");
    f.reply(8, "56", "sent");
    await Promise.all([assert.rejects(first), assert.rejects(second)]);
    assert.deepEqual(f.unresolved, [{seq: 7, key: "56"}, {seq: 8, key: null}]);
  } else {
    f.reply(7, "56", "sent");
    assert(!f.window.canSend, "keep the completed key while the overlapping first reply is missing");
    f.reply(8, "57", "rejected", {error: "expired"});
    assert.equal((await first).state, "sent");
    assert.equal((await second).error, "expired");
    assert.equal(f.window.nextSequence(9), 9);
    assert(f.window.canSend);
  }
}
for (const ordered of [false, true]) {
  const f = fixture(ordered ? orderedRequest : {...frame, motion_window: 3});
  const first = f.send(7);
  if (!ordered) f.reply(7, "51", "accepted");
  const second = f.send(8);
  f.reply(7, "51", "sent", ordered ? {} : {motion_terminal: true});
  assert.equal((await first).state, "sent");
  f.reply(8, "51", ordered ? "sent" : "accepted");
  await assert.rejects(second, /reply unavailable/);
  assert.deepEqual(f.unresolved, [{seq: 8, key: null}]);
}
console.log("Motion window negotiation, admission, ordered receipts and loss checks passed.");
