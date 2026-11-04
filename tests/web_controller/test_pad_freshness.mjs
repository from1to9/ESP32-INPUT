// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import {pathToFileURL} from "node:url";
import {writeFile} from "node:fs/promises";
import path from "node:path";
const {chromium} = await import(pathToFileURL(process.env.WEB_PLAYWRIGHT_MODULE));
const origin = process.env.WEB_ORIGIN, output = process.env.WEB_OUTPUT;
const browser = await chromium.launch({headless: true,
  ...(process.env.WEB_BROWSER_EXECUTABLE ? {executablePath: process.env.WEB_BROWSER_EXECUTABLE} : {})});
const context = await browser.newContext({ignoreHTTPSErrors: true, viewport: {width: 1120, height: 900}, hasTouch: true});
const page = await context.newPage(); page.setDefaultTimeout(10000);
const commands = [], errors = [], cases = [];
page.on("pageerror", error => errors.push(error.message));
page.on("request", request => {
  if (request.url() === `${origin}/api/v1/commands`) commands.push(request.postDataJSON());
});
const moves = () => commands.filter(command => command.op === "mouse.move_relative");
const waitText = (id, text) => page.waitForFunction(({id, text}) => document.getElementById(id).textContent.includes(text), {id, text});
const fixture = action => page.request.post(`${origin}/_fixture/${action}`);
const reports = async () => (await (await page.request.get(`${origin}/_fixture/reports`)).json()).reports;
async function afterRenewal() {
  await page.waitForRequest(request => request.url().endsWith("/api/v1/commands") && request.postDataJSON().op === "control.renew");
  await page.waitForResponse(response => response.url().endsWith("/api/v1/state") && response.status() === 200);
  await page.waitForTimeout(25);
}
async function pauseState() {
  let arrived, resume;
  const paused = new Promise(resolve => { arrived = resolve; });
  const proceed = new Promise(resolve => { resume = resolve; });
  await page.route("**/api/v1/state", async route => { arrived(); await proceed; await route.continue(); }, {times: 1});
  return {paused, resume};
}
try {
  await page.goto(origin);
  await page.locator("#pair").click(); await waitText("approval", "Waiting for approval");
  await fixture("approve"); await page.locator("#check").click(); await waitText("connection", "Connected / idle");
  await page.locator("#acquire").click(); await waitText("connection", "Controlling");
  await page.locator("#pad").scrollIntoViewIfNeeded();
  const box = await page.locator("#pad").boundingBox();
  const start = async () => { await page.mouse.move(box.x + 40, box.y + 40); await page.mouse.down(); };
  for (const kind of ["released", "held-expired", "replaced"]) {
    await afterRenewal(); await fixture("reset-trace");
    const before = moves().length;
    await start(); const gate = await pauseState();
    try {
      await page.mouse.move(box.x + 67, box.y + 49); await gate.paused;
      if (kind !== "held-expired") await page.mouse.up();
      if (kind === "replaced") {
        await page.mouse.move(box.x + 100, box.y + 80); await page.mouse.down();
      } else await page.waitForTimeout(350);
      assert.equal(moves().length, before, "no move POST exists while its state preflight is paused");
    } finally { gate.resume(); }
    await afterRenewal();
    const trace = await reports();
    console.log(JSON.stringify({case: kind, move_posts: moves().length - before, hid: trace}));
    assert.equal(moves().length, before, `${kind} contact/sample must not reach a new POST after asynchronous preflight`);
    assert(!trace.some(report => report.id === 2 && (report.bytes[1] || report.bytes[2])), "unsubmitted movement must not reach HID");
    assert.equal(await page.locator("#connection").textContent(), "Controlling", "dropping an unsubmitted sample must preserve the explicit lease");
    if (kind === "held-expired" || kind === "replaced") {
      const moved = page.waitForRequest(request => request.url().endsWith("/api/v1/commands") && request.postDataJSON().op === "mouse.move_relative");
      const x = kind === "held-expired" ? box.x + 75 : box.x + 108;
      const y = kind === "held-expired" ? box.y + 52 : box.y + 83;
      await page.mouse.move(x, y); const submitted = (await moved).postDataJSON();
      assert.deepEqual(submitted.args, {dx: 8, dy: 3});
      await page.mouse.up(); await waitText("result", "Sent to the HID transport");
      assert((await reports()).some(report => report.id === 2 && report.bytes[1] === 8 && report.bytes[2] === 3));
      cases.push(`${kind}: old sample not submitted; fresh movement accepted, including a release after its POST`);
    } else cases.push("original review replay: release plus 350 ms delayed preflight produces no move POST or HID");
  }
  await afterRenewal(); await fixture("reset-trace");
  await start(); const queuedGate = await pauseState();
  const freshPost = page.waitForRequest(request => request.url().endsWith("/api/v1/commands") && request.postDataJSON().op === "mouse.move_relative");
  try {
    await page.mouse.move(box.x + 67, box.y + 49); await queuedGate.paused;
    await page.waitForTimeout(350);
    await page.mouse.move(box.x + 75, box.y + 52);
  } finally { queuedGate.resume(); }
  assert.deepEqual((await freshPost).postDataJSON().args, {dx: 8, dy: 3}, "newly queued movement must survive discarding the older in-flight sample");
  await page.mouse.up(); await waitText("result", "Sent to the HID transport");
  const queuedTrace = await reports();
  assert(queuedTrace.some(report => report.id === 2 && report.bytes[1] === 8 && report.bytes[2] === 3));
  assert(!queuedTrace.some(report => report.id === 2 && report.bytes[1] === 27 && report.bytes[2] === 9));
  cases.push("fresh same-contact movement queued during the old preflight survives without replaying the expired sample");

  await afterRenewal(); await fixture("reset-trace");
  const beforeCancel = moves().length;
  const cdp = await context.newCDPSession(page);
  const touch = (type, x = 0, y = 0) => cdp.send("Input.dispatchTouchEvent", {type,
    touchPoints: type === "touchCancel" ? [] : [{x, y, id: 1}]});
  await touch("touchStart", box.x + 40, box.y + 40); const gate = await pauseState();
  try {
    await touch("touchMove", box.x + 67, box.y + 49); await gate.paused;
    await touch("touchCancel");
  } finally { gate.resume(); }
  await afterRenewal();
  assert.equal(moves().length, beforeCancel, "cancelled touch must not be submitted after preflight");
  assert(!(await reports()).some(report => report.id === 2 && (report.bytes[1] || report.bytes[2])));
  cases.push("real Chromium touch cancellation invalidates the pending contact");

  await page.getByRole("button", {name: "Move right", exact: true}).click(); await waitText("result", "Sent to the HID transport");
  await page.getByRole("button", {name: "Left click", exact: true}).click(); await waitText("result", "Sent to the HID transport");
  const trace = await reports();
  assert(trace.some(report => report.id === 2 && report.bytes[1] === 24));
  assert(trace.some(report => report.id === 2 && report.bytes[0] === 1));
  assert(trace.some(report => report.id === 2 && report.bytes.every(byte => byte === 0)));
  cases.push("direction and discrete click semantics retain their normal typed reports and neutral release");
  assert.deepEqual(errors, []);
  await writeFile(path.join(output, "browser-results.json"), JSON.stringify({browser: browser.version(), cases,
    commands: commands.map(command => ({op: command.op, args: command.args, deadline_ms: command.deadline_ms})),
    boundaries: "Real Chromium HTTPS and production HTTP/protocol/coordinator/HID engine; SDK sockets, approval, clock, NVS and HID send are boundary substitutes"}, null, 2) + "\n");
  console.log(`PASS: ${cases.length} focused pad freshness groups; unsubmitted movement is distinct from an already submitted request`);
} finally { await browser.close(); }
