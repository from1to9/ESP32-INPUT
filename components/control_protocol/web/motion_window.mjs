// SPDX-License-Identifier: Apache-2.0
// Both controllers use the same receipt matcher on their authenticated WebSocket.
export function motionWindowRequest(capabilities, preferred, allowOrdered = false) {
  if (capabilities.websocket_motion_window !== true || preferred === 1) return null;
  const maximum = capabilities.websocket_motion_window_max;
  if (maximum !== undefined && (!Number.isInteger(maximum) || maximum < 2 || maximum > 4)) {
    throw new Error("Invalid device motion window.");
  }
  const width = preferred ?? maximum ?? 2;
  if (!Number.isInteger(width) || width < 2 || width > 4) throw new Error("Invalid motion window.");
  const request = {v: 1, type: "motion_window"};
  // Older listeners accept only the original two-field negotiation frame.
  if (maximum !== undefined) request.motion_window = Math.min(width, maximum);
  if (allowOrdered && capabilities.websocket_motion_ordered === true) request.motion_ordered = true;
  return request;
}

export class MotionWindow {
  static fromSelection(socket, request, reply, onFault) {
    const width = request.motion_window ?? 2;
    const ordered = request.motion_ordered === true;
    if (reply?.motion_window !== width || (reply.motion_ordered === true) !== ordered) {
      throw new Error("Device motion window negotiation failed.");
    }
    return new MotionWindow(socket, width, ordered, onFault);
  }
  constructor(socket, width, ordered, onFault) {
    this.socket = socket;
    this.width = width;
    this.ordered = ordered;
    this.onFault = onFault;
    this.moves = new Map();
    this.retiredKeys = new Set();
    this.newestSequence = 0;
  }
  get size() { return this.moves.size; }
  get canSend() {
    return this.size + this.retiredKeys.size < this.width && this.socket.readyState === WebSocket.OPEN && !this.socket.bufferedAmount &&
      (this.ordered || [...this.moves.values()].every(move => move.key !== null));
  }
  nextSequence(confirmedNext) {
    return Math.max(confirmedNext, this.newestSequence + 1);
  }
  send(envelope, onAccepted) {
    const seq = envelope.seq;
    if (!this.canSend || !Number.isInteger(seq) || seq <= this.newestSequence || seq > 0xffffffff ||
        (this.size && seq !== this.newestSequence + 1) || envelope.op !== "mouse.move_relative") {
      throw new Error("Input channel unavailable.");
    }
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => this.onFault(), 3500);
      this.moves.set(seq, {key: null, resolve, reject, onAccepted, timer});
      this.newestSequence = seq;
      try { this.socket.send(JSON.stringify(envelope)); }
      catch { this.onFault(); }
    });
  }
  receive(reply) {
    const seq = reply?.command_seq, key = reply?.command_key, next = reply?.next_seq;
    const move = this.moves.get(seq);
    if (!move || typeof key !== "string" || !/^[1-9][0-9]{0,19}$/.test(key) || BigInt(key) > 0xffffffffffffffffn ||
        !Number.isInteger(next) || next < seq + 1 || next > this.newestSequence + 1 ||
        typeof reply.error !== "string") return this.onFault();
    if (move.key === null) {
      if (next !== seq + 1 || reply.motion_terminal || this.retiredKeys.has(key) ||
          [...this.moves.values()].some(other => other.key === key)) return this.onFault();
      if (reply.state === "accepted" && reply.error === "ok") {
        move.key = key;
        if (!this.awaitingFirstReply()) this.retiredKeys.clear();
        clearTimeout(move.timer);
        move.timer = setTimeout(() => this.onFault(), 2200);
        move.onAccepted?.(reply);
        return;
      }
      // An ordered listener may submit synchronously and reply with the real terminal.
      if (!this.ordered && reply.state !== "rejected") return this.onFault();
    } else if (move.key !== key || reply.motion_terminal !== true) {
      return this.onFault();
    }
    if (!(reply.state === "sent" && reply.error === "ok") &&
        !(["rejected", "cancelled", "failed"].includes(reply.state) && reply.error !== "ok")) return this.onFault();
    this.moves.delete(seq);
    // Keep the key until overlapping writes have identities. Each retained key
    // occupies its former slot so a delayed first reply cannot grow the set.
    if (this.awaitingFirstReply()) this.retiredKeys.add(key);
    else this.retiredKeys.clear();
    clearTimeout(move.timer);
    move.resolve(reply);
  }
  awaitingFirstReply() {
    return [...this.moves.values()].some(move => move.key === null);
  }
  close() {
    const unresolved = [];
    for (const [seq, move] of this.moves) {
      clearTimeout(move.timer);
      unresolved.push({seq, key: move.key});
      move.reject(new Error("Input channel reply unavailable."));
    }
    this.moves.clear();
    this.retiredKeys.clear();
    return unresolved;
  }
}
