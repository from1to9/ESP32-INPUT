// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import {pathToFileURL} from "node:url";
import {writeFile} from "node:fs/promises";
import path from "node:path";
const {chromium} = await import(pathToFileURL(process.env.WEB_PLAYWRIGHT_MODULE));
const origin = process.env.WEB_ORIGIN, output = process.env.WEB_OUTPUT;
const browser = await chromium.launch({headless: true,
  ...(process.env.WEB_BROWSER_EXECUTABLE ? {executablePath: process.env.WEB_BROWSER_EXECUTABLE} : {})});
const evidence = {browser: browser.version(), cases: [], simulated: ["SDK listener sockets", "NVS", "trusted clock", "local approval", "HID send API"]};
const errors = [], commands = [];
const waitText = (page, id, text) => page.waitForFunction(({id, text}) => document.getElementById(id).textContent.includes(text), {id, text});
const fixture = (page, action) => page.request.post(`${origin}/_fixture/${action}`);
async function reports(page) { return (await (await page.request.get(`${origin}/_fixture/reports`)).json()).reports; }
async function connect(page, first) {
  if (first) {
    await page.locator("#client-name").fill("Browser test");
    await page.locator("#pair").click();
    await waitText(page, "approval", "Waiting for approval");
    await fixture(page, "approve");
    await page.locator("#check").click();
  } else {
    if (!await page.locator("#pairing").evaluate(node => node.open)) await page.locator("#pair-title").click();
    await page.locator("#connect").click();
  }
  await waitText(page, "connection", "Connected / idle");
  assert.equal(await page.locator("#acquire").isEnabled(), true);
  assert.equal(await page.locator("#mouse-controls").evaluate(node => node.disabled), true, "login must not restore a lease");
  await page.locator("#acquire").click();
  await waitText(page, "connection", "Controlling");
}
function observe(page) {
  page.setDefaultTimeout(10000);
  page.on("pageerror", error => errors.push(error.message));
  page.on("console", message => { if (message.type() === "error" && /Content Security Policy|Refused to/.test(message.text())) errors.push(message.text()); });
  page.on("request", request => {
    if (request.url() === `${origin}/api/v1/commands`) {
      const data = request.postDataJSON();
      commands.push({op: data.op, request_id: data.request_id, args: data.op === "keyboard.text.enqueue" ? {length: data.args.text.length} : data.args});
    }
  });
}
try {
  const context = await browser.newContext({ignoreHTTPSErrors: true, viewport: {width: 1120, height: 900}, colorScheme: "light"});
  const page = await context.newPage(); observe(page);
  const response = await page.goto(origin);
  assert.match(response.headers()["content-security-policy"], /script-src 'self'/);
  assert(!response.headers()["content-security-policy"].includes("unsafe-inline"));
  assert.equal(response.headers()["cross-origin-resource-policy"], "same-origin");
  await connect(page, true);
  const firstIdentity = await page.evaluate(() => JSON.parse(localStorage.getItem("mosaico.browser.identity.v1")).client_id);
  const cookies = await context.cookies();
  const sessionCookie = cookies.find(cookie => cookie.name === "__Host-control");
  assert(sessionCookie?.secure && sessionCookie.httpOnly && sessionCookie.sameSite === "Strict");
  assert.equal(await page.evaluate(() => document.cookie.includes("__Host-control=")), false);
  evidence.cases.push("browser-generated identity, actual pairing/access approval, secure HttpOnly cookie, CSRF login and explicit lease acquisition");
  assert.equal((await page.request.get(`${origin}/controller.js`, {headers: {Origin: "https://foreign.invalid"}})).status(), 403);
  assert.equal((await page.request.get(`${origin}/controller.js?extra=1`)).status(), 404);
  const noCsrf = await page.evaluate(async () => (await fetch("/api/v1/commands", {method: "POST", headers: {"Content-Type": "application/json"}, body: "{}"})).status);
  assert.equal(noCsrf, 401);
  evidence.cases.push("cross-origin static rejection, exact asset paths and real browser cookie mutation without CSRF rejected");

  await fixture(page, "reset-trace");
  await page.getByRole("button", {name: "Move right", exact: true}).click();
  await waitText(page, "result", "Sent to the HID transport");
  await page.getByRole("button", {name: "Left click", exact: true}).click();
  await waitText(page, "result", "Sent to the HID transport");
  await page.getByRole("button", {name: "Right click", exact: true}).click();
  await waitText(page, "result", "Sent to the HID transport");
  let trace = await reports(page);
  for (const bytes of [[0, 24, 0, 0], [1, 0, 0, 0], [2, 0, 0, 0], [0, 0, 0, 0]]) {
    assert(trace.some(report => report.id === 2 && JSON.stringify(report.bytes) === JSON.stringify(bytes)));
  }
  await page.locator("#pad").scrollIntoViewIfNeeded();
  const box = await page.locator("#pad").boundingBox();
  await page.mouse.move(box.x + 50, box.y + 60); await page.mouse.down();
  for (let i = 1; i <= 3; ++i) { await page.mouse.move(box.x + 50 + 24 * i, box.y + 60 + 8 * i); await page.waitForTimeout(160); }
  await page.mouse.up();
  trace = await reports(page);
  assert(trace.filter(report => report.id === 2 && report.bytes[1] > 0).length >= 3, "continuous mouse drag must survive multiple replies");
  evidence.cases.push("direction buttons, left/right down-and-neutral pairs and continuous real mouse pointer events reach the production HID engine");

  await page.locator("#keyboard-mode").click(); await waitText(page, "readiness", "keyboard");
  await page.getByRole("button", {name: "Type a", exact: true}).click(); await waitText(page, "result", "Sent to the HID transport");
  await page.getByRole("button", {name: "Enter", exact: true}).click(); await waitText(page, "result", "Sent to the HID transport");
  await page.locator("#text").fill("Hello!"); await page.locator("#send").click(); await waitText(page, "result", "6 / 6 key pairs complete");
  assert.equal(await page.locator("#text").inputValue(), "Hello!", "the composed text remains inspectable");
  trace = await reports(page);
  assert(trace.some(report => report.id === 1 && report.bytes[2] === 4));
  assert(trace.some(report => report.id === 1 && report.bytes[2] === 40));
  assert(trace.some(report => report.id === 1 && report.bytes[0] === 2 && report.bytes[2] === 11));
  await page.screenshot({path: path.join(output, "desktop-keyboard.png"), fullPage: true});
  const beforeInvalid = commands.filter(command => command.op === "keyboard.text.enqueue").length;
  for (const text of ["x".repeat(33), "ok\u4e2d", "line\nbreak"]) {
    await page.locator("#text").fill(text); await page.locator("#send").click();
    await waitText(page, "result", "Nothing was sent");
  }
  assert.equal(commands.filter(command => command.op === "keyboard.text.enqueue").length, beforeInvalid);
  await page.locator("#text").fill("Hello!");
  evidence.cases.push("on-screen letter/Enter keys and explicit US ANSI text, exact HID usages and releases, whole-input rejection before any text command");

  await fixture(page, "partial");
  await page.locator("#text").fill("ABCDE"); await page.locator("#send").click();
  await waitText(page, "result", "key pairs complete");
  await waitText(page, "connection", "Connected / idle");
  assert.match(await page.locator("#result").textContent(), /1 \/ 5 key pairs complete/);
  await page.screenshot({path: path.join(output, "partial-result.png"), fullPage: true});
  evidence.cases.push("local takeover during a production text down/up sequence preserves partial/uncertain progress and does not replay the suffix");
  await fixture(page, "allow"); await page.locator("#refresh").click();
  await page.locator("#acquire").click(); await waitText(page, "connection", "Controlling");
  await fixture(page, "drop-next");
  const beforeUnknown = commands.filter(command => command.op === "keyboard.text.enqueue").length;
  await page.locator("#text").fill("xyz"); await page.locator("#send").click(); await waitText(page, "result", "Reply unknown");
  await page.waitForTimeout(1200);
  assert.equal(commands.filter(command => command.op === "keyboard.text.enqueue").length, beforeUnknown + 1);
  assert.equal(await page.locator("#acquire").isDisabled(), true);
  await page.screenshot({path: path.join(output, "unknown-result.png"), fullPage: true});
  evidence.cases.push("lost command response after production admission stops control and sends no automatic replay");
  await page.locator("#disconnect").click(); await waitText(page, "connection", "Disconnected");
  await connect(page, false);
  const beforeTakeover = commands.filter(command => command.op === "control.acquire").length;
  await fixture(page, "takeover"); await waitText(page, "connection", "Connected / idle");
  await page.waitForTimeout(1000);
  assert.equal(commands.filter(command => command.op === "control.acquire").length, beforeTakeover, "saved approval never automatically reacquires after takeover");
  await fixture(page, "allow"); await page.locator("#refresh").click();
  await page.locator("#acquire").click(); await waitText(page, "connection", "Controlling");
  await fixture(page, "target"); await waitText(page, "connection", "Connected / idle");
  assert.match(await page.locator("#target").textContent(), /bluetooth:18/);
  assert.equal(await page.locator("#keyboard-controls").evaluate(node => node.disabled), true);
  await page.locator("#disconnect").click(); await waitText(page, "connection", "Disconnected");
  await page.reload(); await waitText(page, "approval", "Access approved");
  assert.equal(await page.locator("#connection").textContent(), "Disconnected");
  evidence.cases.push("saved browser revisit remains idle, local takeover and replacement target terminate active control");

  const mobile = await browser.newContext({ignoreHTTPSErrors: true, viewport: {width: 390, height: 844}, isMobile: true, hasTouch: true, deviceScaleFactor: 2, colorScheme: "dark"});
  const phone = await mobile.newPage(); observe(phone); await phone.goto(origin);
  await fixture(phone, "allow"); await connect(phone, true);
  assert.notEqual(await phone.evaluate(() => JSON.parse(localStorage.getItem("mosaico.browser.identity.v1")).client_id), firstIdentity);
  await phone.locator("#mouse-mode").click(); await waitText(phone, "readiness", "mouse");
  await fixture(phone, "reset-trace");
  await phone.locator("#pad").scrollIntoViewIfNeeded();
  const pad = await phone.locator("#pad").boundingBox();
  const cdp = await mobile.newCDPSession(phone);
  const touch = (type, x, y) => cdp.send("Input.dispatchTouchEvent", {type, touchPoints: type === "touchEnd" ? [] : [{x, y, id: 1}]});
  await touch("touchStart", pad.x + 40, pad.y + 50);
  for (let i = 1; i <= 3; ++i) { await touch("touchMove", pad.x + 40 + i * 20, pad.y + 50 + i * 8); await phone.waitForTimeout(160); }
  await touch("touchEnd", 0, 0);
  trace = await reports(phone);
  assert(trace.filter(report => report.id === 2 && report.bytes[1] > 0).length >= 2, "real browser touch pointer events must move repeatedly");
  assert.equal(await phone.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
  await phone.screenshot({path: path.join(output, "mobile-mouse.png"), fullPage: true});
  await phone.locator("#keyboard-mode").click(); await waitText(phone, "readiness", "keyboard");
  await phone.getByRole("button", {name: "Type z", exact: true}).tap(); await waitText(phone, "result", "Sent to the HID transport");
  await phone.screenshot({path: path.join(output, "mobile-keyboard.png"), fullPage: true});
  await phone.locator("#disconnect").click();
  evidence.cases.push("independent same-name mobile browser pairing, real CDP touch sequence, on-screen key tap, dark 390px layout without horizontal overflow");
  assert.deepEqual(errors, []);
  evidence.commands = commands;
  await writeFile(path.join(output, "browser-results.json"), JSON.stringify(evidence, null, 2) + "\n");
  console.log(`PASS: ${evidence.cases.length} real Chromium HTTPS interaction groups; HID, local approval, trusted clock, NVS and SDK sockets are boundary substitutes`);
} finally { await browser.close(); }
