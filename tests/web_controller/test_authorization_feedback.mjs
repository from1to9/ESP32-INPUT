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
const receipts = [];

async function checkOriginBoundary() {
  const context = await browser.newContext({ignoreHTTPSErrors: true});
  const results = [];
  for (const [source, status, error] of [[origin, 400, "invalid"], ["null", 403, "forbidden"],
    ["https://foreign.invalid", 403, "forbidden"]]) {
    const response = await context.request.post(`${origin}/api/v1/pairing/status`, {
      headers: {Origin: source, "Content-Type": "application/json"}, data: {},
    });
    assert.equal(response.status(), status);
    assert.equal((await response.json()).error, error);
    assert.equal(response.headers()["access-control-allow-origin"], undefined);
    results.push({origin: source, status, error});
  }
  await context.close();
  return results;
}

async function checkViewport(name, width, height) {
  const context = await browser.newContext({
    ignoreHTTPSErrors: true, viewport: {width, height}, isMobile: name === "narrow", hasTouch: name === "narrow",
  });
  await context.addInitScript(() => { history.scrollRestoration = "manual"; });
  const page = await context.newPage();
  const requests = [], errors = [], feedback = [];
  page.on("pageerror", error => errors.push(error.message));
  page.on("request", request => {
    const pathname = new URL(request.url()).pathname;
    if (pathname.startsWith("/api/v1/")) requests.push(pathname);
  });
  const codePath = "**/api/v1/pairing/code";
  const statusPath = "**/api/v1/pairing/status";
  const loginPath = "**/api/v1/sessions";
  const code = page.locator("#access-code");
  const pair = page.locator("#pair");
  const approval = page.locator("#approval");
  const lastAction = page.locator("#result");
  const waitText = (id, prefix) => page.waitForFunction(
    ({id, prefix}) => document.getElementById(id).textContent.startsWith(prefix), {id, prefix});
  assert.equal((await page.goto(origin)).headers()["referrer-policy"], "no-referrer");
  await pair.click();
  const initialAction = await lastAction.textContent();

  async function inline(prefix, trigger, screenshot) {
    await waitText("approval", prefix);
    assert.equal(await approval.getAttribute("role"), "status");
    const geometry = await approval.evaluate((node, trigger) => {
      const approval = node.getBoundingClientRect();
      const button = document.getElementById(trigger).getBoundingClientRect();
      return {top: approval.top, bottom: approval.bottom, buttonTop: button.top, buttonBottom: button.bottom,
        viewportHeight: window.innerHeight, open: node.closest("details").open};
    }, trigger);
    assert(geometry.open && geometry.top >= 0 && geometry.bottom <= geometry.viewportHeight, JSON.stringify(geometry));
    assert(geometry.buttonTop >= 0 && geometry.buttonBottom <= geometry.viewportHeight, JSON.stringify(geometry));
    assert.equal(await lastAction.textContent(), initialAction);
    if (screenshot) await page.screenshot({path: path.join(output, screenshot)});
    feedback.push({message: await approval.textContent(), ...geometry, ...(screenshot ? {screenshot} : {})});
  }

  await inline("Enter the 8-digit access code", "pair", `${name}-missing-code.png`);
  await page.locator("#client-name").fill(" ");
  await pair.click();
  await inline("Use 1–32 printable ASCII characters", "pair");
  assert.deepEqual(requests, []);
  await page.locator("#client-name").fill("Authorization feedback test");

  await code.fill("00000000");
  const rejected = page.waitForResponse(response => response.url() === `${origin}/api/v1/pairing/code`);
  await pair.click();
  assert.equal((await rejected).status(), 401);
  await inline("Access code rejected.", "pair");
  assert.equal(await code.inputValue(), "");

  await page.route(codePath, route => route.fulfill({status: 503, json: {error: "unavailable"}}));
  await code.fill("12345678");
  await pair.click();
  await inline("Mosaico could not accept the browser authorization request.", "pair");
  assert.equal(await code.inputValue(), "");
  await page.unroute(codePath);

  let acceptedStatus;
  await page.route(codePath, async route => {
    acceptedStatus = (await route.fetch()).status();
    await route.abort();
  });
  await code.fill("12345678");
  await pair.click();
  await inline("Authorization reply unavailable.", "pair", `${name}-missing-reply.png`);
  assert.equal(acceptedStatus, 202);
  assert.equal(await code.inputValue(), "");
  assert.deepEqual(requests, Array(3).fill("/api/v1/pairing/code"));
  await page.unroute(codePath);

  await page.route(statusPath, route => route.fulfill({status: 200, json: {state: "approved", fingerprint: "f".repeat(64)}}));
  await page.locator("#check").click();
  await inline("Pairing status unavailable or identity mismatch.", "check");
  await page.unroute(statusPath);

  await page.route(statusPath, route => route.abort());
  await page.reload();
  await waitText("approval", "Could not read browser approval.");
  assert.equal(await page.evaluate(() => window.scrollY), 0);
  assert.equal(await lastAction.textContent(), initialAction);
  await page.unroute(statusPath);

  await page.route(loginPath, route => route.fulfill({status: 503, json: {error: "unavailable"}}));
  await page.locator("#connect").click();
  await inline("Browser access approved. Could not connect.", "connect", `${name}-login-failed.png`);
  assert.equal(await page.locator("#connection").textContent(), "Disconnected");
  await page.unroute(loginPath);

  await page.locator("#connect").click();
  await waitText("connection", "Connected / idle");
  assert.equal(await page.locator("#pairing").evaluate(node => node.open), false);
  assert.deepEqual(requests.filter(url => url === "/api/v1/pairing/code"), Array(3).fill("/api/v1/pairing/code"));
  assert(!requests.includes("/api/v1/commands"));
  assert.deepEqual(errors, []);
  receipts.push({name, width, height, feedback, requests, lostReplyStatus: acceptedStatus});
  await context.close();
}

try {
  const originBoundary = await checkOriginBoundary();
  await checkViewport("desktop", 1280, 1000);
  await checkViewport("narrow", 390, 640);
  await writeFile(path.join(output, "authorization-feedback.json"),
    JSON.stringify({browser: browser.version(), originBoundary, receipts}, null, 2) + "\n");
  console.log("PASS: strict Origin boundary and visible authorization feedback in desktop and narrow Chromium");
} finally {
  await browser.close();
}
