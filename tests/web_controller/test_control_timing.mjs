// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import {writeFile} from "node:fs/promises";
import path from "node:path";
import {pathToFileURL} from "node:url";

const {chromium} = await import(pathToFileURL(process.env.WEB_PLAYWRIGHT_MODULE));
const origin = process.env.WEB_ORIGIN;
const output = process.env.WEB_OUTPUT;
const browser = await chromium.launch({headless: true,
  ...(process.env.WEB_BROWSER_EXECUTABLE ? {executablePath: process.env.WEB_BROWSER_EXECUTABLE} : {})});
const evidence = {stateReads: [], commands: [], legacyReads: []};
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const page = await browser.newPage({ignoreHTTPSErrors: true, viewport: {width: 1100, height: 920}, hasTouch: true});

try {
  let legacyState = false;
  page.setDefaultTimeout(10000);
  const commandPath = `${origin}/api/v1/commands`;
  const fullState = `${origin}/api/v1/state`;
  const protocolState = `${fullState}?view=protocol`;
  const waitText = (id, text) => page.waitForFunction(
    ({id, text}) => document.getElementById(id).textContent.includes(text), {id, text});
  const fixture = action => page.request.post(`${origin}/_fixture/${action}`);
  const reports = async () => (await (await page.request.get(`${origin}/_fixture/reports`)).json()).reports;
  page.on("request", request => {
    if (request.url() === commandPath) evidence.commands.push(request.postDataJSON().op);
  });
  page.on("response", response => {
    if (new URL(response.url()).pathname === "/api/v1/state") {
      const reads = legacyState ? evidence.legacyReads : evidence.stateReads;
      reads.push({path: response.url().slice(origin.length), status: response.status()});
    }
  });
  await page.goto(origin);
  await page.locator("#access-code").fill("12345678");
  await page.locator("#pair").click();
  await waitText("connection", "Connected / idle");
  const detail = await page.request.get(fullState);
  assert.equal(detail.status(), 429);
  assert.equal((await detail.json()).error, "busy");
  await page.locator("#acquire").click();
  await waitText("connection", "Controlling");
  for (const mode of ["keyboard", "mouse"]) {
    await page.locator(`#${mode}-mode`).click();
    await waitText("readiness", `${mode} · ready`);
    assert.equal(await page.locator(`#${mode}-mode`).getAttribute("aria-pressed"), "true");
  }
  await fixture("reset-trace");

  // Delay delivery of one browser-generated movement beyond its original
  // deadline. The unmodified production protocol decides the actual result.
  let delayed = false;
  await page.route(commandPath, async route => {
    if (!delayed && route.request().postDataJSON().op === "mouse.move_relative") {
      delayed = true;
      await delay(225);
    }
    await route.continue();
  });
  const rejected = page.waitForResponse(response => response.url() === commandPath &&
    response.request().postDataJSON().op === "mouse.move_relative");
  await page.locator("#pad").scrollIntoViewIfNeeded();
  const pad = await page.locator("#pad").boundingBox();
  await page.mouse.move(pad.x + 40, pad.y + 40);
  await page.mouse.down();
  await page.mouse.move(pad.x + 62, pad.y + 48);
  try {
    const response = await rejected;
    const {state, error} = await response.json();
    evidence.expiredMovement = {status: response.status(), state, error};
    assert.deepEqual(evidence.expiredMovement, {status: 400, state: "rejected", error: "expired"});
  } finally { await page.mouse.up(); }
  await waitText("result", "expired");
  assert(!(await reports()).some(report => report.id === 2 && (report.bytes[1] || report.bytes[2])));
  evidence.afterExpired = await page.locator("#connection").textContent();
  assert.equal(evidence.afterExpired, "Controlling", "one exhausted movement deadline must not discard a verified active lease");
  assert(evidence.stateReads.length && evidence.stateReads.every(read => read.path === "/api/v1/state?view=protocol" && read.status === 200));
  await page.unroute(commandPath);

  await afterRenewal();
  assert.equal((await fixture("hold-motion")).status(), 200);
  const accepted = page.waitForResponse(response => response.url() === commandPath &&
    response.request().postDataJSON().op === "mouse.move_relative");
  await page.getByRole("button", {name: "Move right", exact: true}).click();
  const submission = await accepted;
  assert.equal(submission.status(), 202);
  const {command_key: expiredKey} = await submission.json();
  const result = await page.waitForResponse(response => response.url() === `${origin}/api/v1/requests/${expiredKey}` &&
    response.status() === 400);
  const {state, error} = await result.json();
  assert.deepEqual({state, error}, {state: "rejected", error: "expired"});
  await waitText("result", "rejected: expired");
  const expiredAfterAcceptance = await page.locator("#connection").textContent();
  evidence.httpBeforeEffectExpiry = {admission: 202, result: 400, state, error, connection: expiredAfterAcceptance};
  assert(!(await reports()).some(report => report.id === 2 && (report.bytes[1] || report.bytes[2])));
  assert.equal(expiredAfterAcceptance, "Controlling", "a rejected move with no HID effect must not abandon the lease");

  // Exercise the old route that the loopback proxy must use with installed
  // firmware. Keep its real 429s and delay requests to model the observed reads.
  const legacy = async route => {
    await delay(32);
    await route.continue({url: fullState});
  };
  legacyState = true;
  await page.route(protocolState, legacy);
  const cdp = await page.context().newCDPSession(page);
  await page.locator("#pad").scrollIntoViewIfNeeded();
  const touchPad = await page.locator("#pad").boundingBox();
  const point = {x: touchPad.x + 45, y: touchPad.y + 60, id: 1};
  const touch = type => cdp.send("Input.dispatchTouchEvent", {
    type, touchPoints: type === "touchEnd" ? [] : [point],
  });
  const motionReports = async () => (await reports()).filter(report => report.id === 2 && report.bytes[1] > 0).length;
  await touch("touchStart");
  const duringDrag = [];
  for (let i = 0; i < 96; ++i) {
    point.x += 2;
    await touch("touchMove");
    await delay(18);
    if (i === 47 || i === 95) duringDrag.push(await motionReports());
  }
  await touch("touchEnd");
  evidence.motionReportsDuringTouch = duringDrag;
  evidence.afterTouch = {connection: await page.locator("#connection").textContent(), result: await page.locator("#result").textContent()};
  assert(duringDrag[0] >= 2 && duringDrag[1] >= duringDrag[0] + 2, JSON.stringify(duringDrag));
  assert(evidence.legacyReads.some(read => read.status === 429) && evidence.legacyReads.some(read => read.status === 200));
  assert.equal(await page.locator("#connection").textContent(), "Controlling");
  await page.unroute(protocolState, legacy);
  legacyState = false;

  async function afterRenewal() {
    const admitted = await page.waitForResponse(response => response.url() === commandPath &&
      response.request().postDataJSON().op === "control.renew" && response.status() === 202);
    const {command_key: key} = await admitted.json();
    await page.waitForResponse(response => response.url() === `${origin}/api/v1/requests/${key}` && response.status() === 200);
    await page.waitForResponse(response => response.url() === protocolState && response.status() === 200);
    await page.waitForTimeout(25);
  }
  async function sent(label, op) {
    const posted = page.waitForResponse(response => response.url() === commandPath &&
      response.request().postDataJSON().op === op);
    await page.getByRole("button", {name: label, exact: true}).click();
    const submission = await posted;
    assert.equal(submission.status(), 202);
    const {command_key: key} = await submission.json();
    const final = await page.waitForResponse(response => response.url() === `${origin}/api/v1/requests/${key}`);
    const {state, error} = await final.json();
    assert.deepEqual({state, error}, {state: "sent", error: "ok"});
  }
  await fixture("reset-trace");
  await afterRenewal();
  for (const label of ["Move up", "Move down", "Move left", "Move right"]) {
    await sent(label, "mouse.move_relative");
  }
  await afterRenewal();
  for (const label of ["Left click", "Right click"]) await sent(label, "mouse.click");
  const mouse = (await reports()).filter(report => report.id === 2).map(report => report.bytes.slice(0, 3));
  for (const expected of [[0, 0, 232], [0, 0, 24], [0, 232, 0], [0, 24, 0], [1, 0, 0], [2, 0, 0], [0, 0, 0]]) {
    assert(mouse.some(report => report.every((value, index) => value === expected[index])), JSON.stringify(mouse));
  }
  evidence.mouseReports = mouse;

  await afterRenewal();
  await page.locator("#keyboard-mode").click();
  await waitText("readiness", "keyboard · ready");
  await sent("Type a", "keyboard.key_tap");
  await page.locator("#text").fill("Q");
  await sent("Send text", "keyboard.text.enqueue");

  // Hold the next renewal's state request until Stop has closed the session.
  // A real late 401 must not overwrite the completed close receipt.
  await afterRenewal();
  let arrived, resume;
  const held = new Promise(resolve => { arrived = resolve; });
  const proceed = new Promise(resolve => { resume = resolve; });
  await page.route(protocolState, async route => {
    arrived();
    await proceed;
    await route.continue();
  }, {times: 1});
  await held;
  const staleRead = page.waitForResponse(response => response.url() === protocolState && response.status() === 401);
  try {
    await page.locator("#stop").click();
    await waitText("result", "Browser session closed.");
    await waitText("connection", "Disconnected");
  } finally { resume(); }
  await staleRead;
  await page.waitForTimeout(150);
  assert.match(await page.locator("#result").textContent(), /^Browser session closed\./);
  evidence.stop = {lateReadStatus: 401, connection: await page.locator("#connection").textContent(),
    result: await page.locator("#result").textContent()};
  console.log("PASS: delayed motion expires safely, continuous touch sends HID through legacy state reads, and Stop keeps its close receipt");
} finally {
  await page.unrouteAll({behavior: "ignoreErrors"});
  await writeFile(path.join(output, "control-timing.json"), JSON.stringify(evidence, null, 2) + "\n");
  await browser.close();
}
