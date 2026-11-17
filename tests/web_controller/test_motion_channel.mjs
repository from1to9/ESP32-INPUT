// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import {readFile, writeFile} from "node:fs/promises";
import path from "node:path";
import {pathToFileURL} from "node:url";

const {chromium} = await import(pathToFileURL(process.env.WEB_PLAYWRIGHT_MODULE));
const origin = process.env.WEB_ORIGIN, device = process.env.WEB_FIXTURE_ORIGIN;
const browser = await chromium.launch({headless: true,
  ...(process.env.WEB_BROWSER_EXECUTABLE ? {executablePath: process.env.WEB_BROWSER_EXECUTABLE} : {})});
const evidence = {motionCommands: [], motionResults: [], renewals: [], httpCommands: [], httpStateReads: 0, movementReports: []};
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const atMs = () => Math.round(performance.timeOrigin + performance.now());
const expectPush = process.env.WEB_EXPECT_MOTION_TERMINAL === "1";
const referenceController = process.env.WEB_TEST_REFERENCE_CONTROLLER;

try {
  const page = await browser.newPage({ignoreHTTPSErrors: true, viewport: {width: 1100, height: 920}, hasTouch: true});
  page.setDefaultTimeout(10000);
  const cdp = await page.context().newCDPSession(page);
  const waitText = (id, text) => page.waitForFunction(
    ({id, text}) => document.getElementById(id).textContent.includes(text), {id, text});
  const fixture = action => page.request.post(`${device}/_fixture/${action}`);
  const reports = async () => (await (await page.request.get(`${device}/_fixture/reports`)).json()).reports;
  const sentFrames = [];
  // Instrument the exact discard points only in this loopback test. The
  // shipping controller never collects or exposes pointer samples.
  await page.route("**/controller.js", async route => {
    const response = await route.fetch();
    let source = referenceController ? await readFile(referenceController, "utf8") : await response.text();
    if (referenceController) evidence.referenceController = referenceController;
    const observe = "globalThis.recordMotionDiscard?.";
    const contact = source.includes("if (pad !== contact) return null;") ?
      "if (pad !== contact) return null;" : "if (pad !== next.contact) return null;";
    const expiry = source.includes("if (performance.now() >= expiresAt) {") ?
      "if (performance.now() >= expiresAt) {" : "if (performance.now() >= expiresAt) return null;";
    const insertions = [
      ["    if (!movement || now - movement.at >= batch) movement =",
        `    if (movement && now - movement.at >= batch) ${observe}(batch === 150 ? "expired_before_submit" : "rollover", movement);\n`],
      ["    movement.dx = Math.max",
        "    const rawX = movement.dx + event.clientX - pad.x, rawY = movement.dy + event.clientY - pad.y;\n"],
      ["    pad.x = event.clientX;",
        "    globalThis.recordMotionClip?.(rawX - movement.dx, rawY - movement.dy);\n"],
      ["  const endPad = event => { if (pad?.id === event.pointerId) { pad = null;",
        `  const endPad = event => { if (pad?.id === event.pointerId) { ${observe}("contact_end", movement); pad = null;`],
      [`      ${contact}`,
        `      ${contact.replace("return null;", `{ ${observe}("contact_end", next); return null; }`)}`],
      [`      ${expiry}`,
        `      if (performance.now() >= expiresAt) { ${observe}("expired_before_submit", latest);${expiry.endsWith("{") ? "" : " return null; }"}`],
      ["      if (consume && latest === movement) movement = null;",
        `      if (consume && latest !== next) ${observe}("superseded_before_submit", next);\n`],
    ];
    const releaseExpiry = "      if (now >= movement.at + PAD_SAMPLE_LIFETIME_MS) {";
    if (source.includes(releaseExpiry)) insertions.push([
      releaseExpiry, `${releaseExpiry} ${observe}("expired_before_submit", movement);`,
    ]);
    for (const [needle, injected] of insertions) {
      assert.equal(source.split(needle).length, 2);
      source = source.replace(needle, injected.endsWith("\n") ? injected + needle : injected);
    }
    await route.fulfill({response, body: source});
  });
  page.on("request", request => {
    const resource = new URL(request.url()).pathname;
    if (resource === "/api/v1/state") ++evidence.httpStateReads;
    if (resource === "/api/v1/commands") evidence.httpCommands.push(request.postDataJSON().op);
  });
  page.on("websocket", socket => {
    assert.equal(socket.url(), origin.replace(/^http/, "ws") + "/api/v1/channel");
    socket.on("framesent", ({payload}) => {
      const frame = JSON.parse(payload);
      sentFrames.push({op: frame.op, seq: frame.seq, resource: frame.resource, type: frame.type, atMs: atMs()});
      if (frame.op === "mouse.move_relative") evidence.motionCommands.push({
        seq: frame.seq, deadline: frame.deadline_ms, args: frame.args, atMs: atMs(),
      });
      if (frame.op === "control.renew") evidence.renewals.push(frame.seq);
    });
    socket.on("framereceived", ({payload}) => {
      if (typeof payload !== "string") return;
      const frame = JSON.parse(payload);
      if (frame.state) evidence.motionResults.push({state: frame.state, error: frame.error, key: frame.command_key,
        nextSeq: frame.next_seq, pushed: frame.motion_terminal === true, atMs: atMs()});
    });
  });
  await page.goto(origin);
  await page.locator("#access-code").fill("12345678");
  await page.locator("#pair").click();
  await waitText("connection", "Connected / idle");
  const tls = device.startsWith("https:");
  const cookie = (await page.context().cookies()).find(cookie => cookie.name === (tls ? "__Host-control" : "control-http"));
  assert(cookie && cookie.secure === tls && cookie.httpOnly && cookie.sameSite === "Strict" && cookie.path === "/");
  evidence.secureCookieRetained = cookie.secure;
  await page.locator("#acquire").click();
  await waitText("connection", "Controlling");
  assert.equal(sentFrames.filter(frame => frame.type === "motion_terminal").length, expectPush ? 1 : 0);
  await page.locator("#pad").scrollIntoViewIfNeeded();
  const pad = await page.locator("#pad").boundingBox();
  const point = {id: 1, x: pad.x + 35, y: pad.y + 50};
  const touch = type => cdp.send("Input.dispatchTouchEvent", {type, touchPoints: type === "touchEnd" ? [] : [point]});
  await page.evaluate(() => {
    const changes = [];
    const observer = new MutationObserver(records => {
      for (const record of records) {
        if ((record.oldValue !== null) !== record.target.hasAttribute("disabled")) changes.push(record.target.id || record.target.dataset.click);
      }
    });
    for (const selector of ["#mouse-mode", "#keyboard-mode", "[data-click=left]", "[data-click=right]"]) {
      observer.observe(document.querySelector(selector), {attributes: true, attributeOldValue: true, attributeFilter: ["disabled"]});
    }
    window.finishMotionObservation = () => { observer.disconnect(); return changes; };
  });
  const initialReads = evidence.httpStateReads;
  async function waitForFreshRenewal() {
    // An inline renewal can consume the sample's entire admission window.
    const prior = evidence.renewals.at(-1) ?? 0;
    for (let i = 0; i < 100; ++i) {
      const applied = evidence.motionResults.some(reply => reply.state === "applied" &&
        evidence.renewals.some(seq => seq > prior && Number(reply.nextSeq) === seq + 1));
      if (applied) return;
      await delay(25);
    }
    assert.fail("Expected a fresh lease before injecting a command fault");
  }
  await waitForFreshRenewal();
  assert.equal((await fixture("hold-motion")).status(), 200);
  await page.mouse.move(pad.x + 35, pad.y + 50);
  await page.mouse.down();
  for (let i = 1; i <= 20 && !evidence.motionCommands.length; ++i) {
    await page.mouse.move(pad.x + 35 + i * 5, pad.y + 50);
    await delay(25);
  }
  await page.mouse.up();
  assert(evidence.motionCommands.length);
  await waitText("result", "rejected: expired");
  const rejection = evidence.motionResults.find(result => result.state === "rejected" && result.error === "expired");
  assert(rejection && evidence.motionResults.some(result => result.state === "accepted" && result.key === rejection.key));
  assert.equal(rejection.pushed, expectPush);
  assert.equal(sentFrames.some(frame => frame.resource === `/api/v1/requests/${rejection.key}`), !expectPush);
  assert.equal(await page.locator("#connection").textContent(), "Controlling");
  assert(!(await reports()).some(report => report.id === 2 && (report.bytes[1] || report.bytes[2])));
  evidence.beforeEffectExpiry = {accepted: true, rejected: true, noHid: true, controlRetained: true};

  point.x = pad.x + 35;
  const initialPoint = {x: point.x, y: point.y};
  const beforeTouch = atMs();
  await page.evaluate(() => {
    const samples = [], discarded = [], clipped = [];
    const counted = new WeakSet();
    const pad = document.getElementById("pad");
    const record = event => samples.push({x: event.clientX, y: event.clientY,
      atMs: Math.round(performance.timeOrigin + performance.now())});
    pad.addEventListener("pointermove", record, {capture: true, passive: true});
    window.recordMotionDiscard = (reason, sample) => {
      if (!sample || counted.has(sample)) return;
      counted.add(sample);
      discarded.push({reason, dx: sample.dx, dy: sample.dy, ageMs: Math.round(performance.now() - sample.at)});
    };
    window.recordMotionClip = (dx, dy) => { if (dx || dy) clipped.push({dx, dy}); };
    window.finishTouchTiming = () => {
      pad.removeEventListener("pointermove", record, true);
      delete window.recordMotionDiscard;
      delete window.recordMotionClip;
      return {samples, discarded, clipped};
    };
  });
  await touch("touchStart");
  for (let i = 0; i < 90; ++i) {
    point.x += 2;
    await touch("touchMove");
    await delay(18);
    if (i === 44 || i === 89) {
      const trace = await reports();
      evidence.movementReports.push(trace.filter(report => report.id === 2 && report.bytes[1] > 0).length);
    }
  }
  await touch("touchEnd");
  const afterTouch = atMs();
  const {samples, discarded, clipped} = await page.evaluate(() => window.finishTouchTiming());
  evidence.touchTiming = {
    startedAtMs: beforeTouch, endedAtMs: afterTouch, initialPoint, samples, discarded, clipped,
    sent: sentFrames.filter(frame => frame.atMs >= beforeTouch && frame.atMs <= afterTouch),
    received: evidence.motionResults.filter(frame => frame.atMs >= beforeTouch && frame.atMs <= afterTouch),
    hid: (await reports()).filter(report => report.id === 2 && report.atMs >= beforeTouch && report.atMs <= afterTouch &&
      (report.bytes[1] || report.bytes[2])),
  };
  const [earlier, later] = evidence.movementReports;
  assert(earlier >= 4 && later >= earlier + 4, JSON.stringify(evidence.movementReports));
  assert(evidence.motionCommands.length > 10);
  assert(evidence.motionResults.filter(result => result.state === "sent" && result.error === "ok").length >= later);
  assert(evidence.httpStateReads - initialReads < evidence.motionCommands.length);
  assert.equal(await page.locator("#connection").textContent(), "Controlling");

  await page.mouse.move(pad.x + 40, pad.y + 75);
  await page.mouse.down();
  for (let i = 1; i <= 15; ++i) {
    await page.mouse.move(pad.x + 40 + i * 4, pad.y + 75);
    await delay(24);
  }
  await page.mouse.up();
  const trace = await reports();
  evidence.movementReportsAfterMouse = trace.filter(report => report.id === 2 && report.bytes[1] > 0).length;
  assert(evidence.movementReportsAfterMouse >= later + 2);
  assert.deepEqual(await page.evaluate(() => window.finishMotionObservation()), []);

  const beforeDirection = evidence.motionCommands.length;
  const beforeDirectionResults = evidence.motionResults.length;
  const left = page.getByRole("button", {name: "Move left"});
  await left.evaluate(button => {
    button.addEventListener("click", () => {
      const others = [document.querySelector('[aria-label="Move right"]'), document.getElementById("keyboard-mode"),
        document.querySelector("[data-click=left]")];
      const peek = control => ({disabled: control.disabled, ariaDisabled: control.getAttribute("aria-disabled"),
        opacity: getComputedStyle(control).opacity});
      const controls = [button, ...others].map(peek);
      const pending = document.getElementById("result").textContent;
      others.forEach(control => control.click());
      document.getElementById("pad").dispatchEvent(new KeyboardEvent("keydown", {key: "ArrowRight"}));
      window.directionObservation = {controls, pending, afterRepeat: document.getElementById("result").textContent,
        stopDisabled: document.getElementById("stop").disabled};
    }, {once: true});
  });
  await left.click();
  evidence.directionPending = await page.evaluate(() => window.directionObservation);
  assert.deepEqual(evidence.directionPending.controls,
    Array.from({length: 4}, () => ({disabled: false, ariaDisabled: "true", opacity: "1"})));
  assert.equal(evidence.directionPending.afterRepeat, evidence.directionPending.pending);
  assert.equal(evidence.directionPending.stopDisabled, false);
  for (let i = 0; i < 100; ++i) {
    const result = evidence.motionResults.slice(beforeDirectionResults).find(item => item.state === "sent");
    if (evidence.motionCommands.length > beforeDirection && result) break;
    await delay(25);
  }
  const direction = evidence.motionCommands.slice(beforeDirection).find(command => command.args.dx === -24);
  assert(direction && direction.args.dy === 0);
  assert.equal(evidence.motionCommands.length, beforeDirection + 1);
  assert((await reports()).some(report => report.id === 2 && report.bytes[1] === 232 && !report.bytes[2]));
  assert(evidence.motionResults.slice(beforeDirectionResults).some(result => result.state === "sent" && result.error === "ok"));
  evidence.direction = {command: direction, hidDx: -24};
  await page.waitForFunction(() => !document.querySelector('[aria-label="Move right"]').hasAttribute("aria-disabled"));
  const beforeArrow = evidence.motionCommands.length;
  await page.locator("#pad").press("ArrowRight");
  await page.waitForFunction(() => !document.querySelector('[aria-label="Move right"]').hasAttribute("aria-disabled"));
  assert.equal(evidence.motionCommands.length, beforeArrow + 1);
  assert.deepEqual(evidence.motionCommands.at(-1).args, {dx: 24, dy: 0});
  assert((await reports()).some(report => report.id === 2 && report.bytes[1] === 24 && !report.bytes[2]));
  assert(!evidence.httpCommands.includes("mouse.move_relative"));
  assert(!evidence.httpCommands.includes("control.renew") && evidence.renewals.length >= 2);

  await waitForFreshRenewal();
  const sentBeforeLoss = evidence.motionCommands.length;
  const resultsBeforeLoss = evidence.motionResults.length;
  assert.equal((await fixture("drop-motion")).status(), 200);
  await page.locator("#pad").scrollIntoViewIfNeeded();
  const currentPad = await page.locator("#pad").boundingBox();
  point.x = currentPad.x + 35;
  point.y = currentPad.y + 50;
  await touch("touchStart");
  point.x += 9;
  await touch("touchMove");
  await waitText("result", "Reply unknown");
  if (expectPush) {
    const move = evidence.motionCommands[sentBeforeLoss];
    const lost = evidence.motionResults.slice(resultsBeforeLoss).filter(reply => Number(reply.nextSeq) === move.seq + 1);
    const accepted = lost.at(-1);
    assert.equal(accepted?.state, "accepted");
    await page.waitForFunction(() => !document.getElementById("result-check").disabled);
    assert(!sentFrames.some(frame => frame.resource === `/api/v1/requests/${accepted.key}`));
  }
  await touch("touchEnd");
  assert.equal(await page.locator("#connection").textContent(), "Connected / idle");
  assert(await page.locator("#mouse-mode").isDisabled());
  await delay(1000);
  assert.equal(evidence.motionCommands.length, sentBeforeLoss + 1);
  assert(!evidence.httpCommands.includes("mouse.move_relative"));
  assert.equal(evidence.httpCommands.filter(command => command === "control.acquire").length, 1);
  console.log("PASS: Chromium session cookie, WS pad and direction input, continued HID movement, steady controls, and no replay on loss");
} finally {
  await writeFile(path.join(process.env.WEB_OUTPUT, "motion-channel.json"), JSON.stringify(evidence, null, 2) + "\n");
  await browser.close();
}
