// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import {writeFile} from "node:fs/promises";
import path from "node:path";
import {pathToFileURL} from "node:url";

const {chromium} = await import(pathToFileURL(process.env.WEB_PLAYWRIGHT_MODULE));
const origin = process.env.WEB_ORIGIN, device = process.env.WEB_FIXTURE_ORIGIN;
const browser = await chromium.launch({headless: true,
  ...(process.env.WEB_BROWSER_EXECUTABLE ? {executablePath: process.env.WEB_BROWSER_EXECUTABLE} : {})});
const evidence = {motionCommands: [], motionResults: [], renewals: [], httpCommands: [], httpStateReads: 0, movementReports: []};
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const atMs = () => Math.round(performance.timeOrigin + performance.now());

try {
  const page = await browser.newPage({ignoreHTTPSErrors: true, viewport: {width: 1100, height: 920}, hasTouch: true});
  page.setDefaultTimeout(10000);
  const cdp = await page.context().newCDPSession(page);
  const waitText = (id, text) => page.waitForFunction(
    ({id, text}) => document.getElementById(id).textContent.includes(text), {id, text});
  const fixture = action => page.request.post(`${device}/_fixture/${action}`);
  const reports = async () => (await (await page.request.get(`${device}/_fixture/reports`)).json()).reports;
  const sentFrames = [];
  page.on("request", request => {
    const resource = new URL(request.url()).pathname;
    if (resource === "/api/v1/state") ++evidence.httpStateReads;
    if (resource === "/api/v1/commands") evidence.httpCommands.push(request.postDataJSON().op);
  });
  page.on("websocket", socket => {
    assert.equal(socket.url(), origin.replace(/^http/, "ws") + "/api/v1/channel");
    socket.on("framesent", ({payload}) => {
      const frame = JSON.parse(payload);
      if (frame.op || frame.resource) sentFrames.push({op: frame.op, seq: frame.seq, resource: frame.resource, atMs: atMs()});
      if (frame.op === "mouse.move_relative") evidence.motionCommands.push({
        seq: frame.seq, deadline: frame.deadline_ms, args: frame.args, atMs: atMs(),
      });
      if (frame.op === "control.renew") evidence.renewals.push(frame.seq);
    });
    socket.on("framereceived", ({payload}) => {
      if (typeof payload !== "string") return;
      const frame = JSON.parse(payload);
      if (frame.state) evidence.motionResults.push({state: frame.state, error: frame.error, key: frame.command_key, atMs: atMs()});
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
  assert.equal(await page.locator("#connection").textContent(), "Controlling");
  assert(!(await reports()).some(report => report.id === 2 && (report.bytes[1] || report.bytes[2])));
  evidence.beforeEffectExpiry = {accepted: true, rejected: true, noHid: true, controlRetained: true};

  point.x = pad.x + 35;
  const beforeTouch = atMs();
  await page.evaluate(() => {
    const samples = [];
    const pad = document.getElementById("pad");
    const record = event => samples.push({x: event.clientX, atMs: Math.round(performance.timeOrigin + performance.now())});
    pad.addEventListener("pointermove", record, {capture: true, passive: true});
    window.finishTouchTiming = () => { pad.removeEventListener("pointermove", record, true); return samples; };
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
  const samples = await page.evaluate(() => window.finishTouchTiming());
  evidence.touchTiming = {
    startedAtMs: beforeTouch, endedAtMs: afterTouch, samples,
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

  const sentBeforeLoss = evidence.motionCommands.length;
  assert.equal((await fixture("drop-motion")).status(), 200);
  await page.locator("#pad").scrollIntoViewIfNeeded();
  const currentPad = await page.locator("#pad").boundingBox();
  point.x = currentPad.x + 35;
  point.y = currentPad.y + 50;
  await touch("touchStart");
  point.x += 9;
  await touch("touchMove");
  await waitText("result", "Reply unknown");
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
