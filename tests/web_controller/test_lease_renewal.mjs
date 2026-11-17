// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import {writeFile} from "node:fs/promises";
import path from "node:path";
import {pathToFileURL} from "node:url";

const {chromium} = await import(pathToFileURL(process.env.WEB_PLAYWRIGHT_MODULE));
const origin = process.env.WEB_ORIGIN, output = process.env.WEB_OUTPUT;
const browser = await chromium.launch({headless: true,
  ...(process.env.WEB_BROWSER_EXECUTABLE ? {executablePath: process.env.WEB_BROWSER_EXECUTABLE} : {})});
const commands = [], renewalResults = [], errors = [];
const snapshotSelectors = ["#mouse-mode", "#keyboard-mode", "#mouse-controls", "#keyboard-controls",
  "[data-click=left]", "#refresh", "#refresh-connections", "#result-check", "#scan"];
const commandPath = `${origin}/api/v1/commands`;
const statePath = `${origin}/api/v1/state?view=protocol`;

try {
  const page = await browser.newPage({ignoreHTTPSErrors: true, viewport: {width: 1280, height: 1000}});
  page.setDefaultTimeout(8000);
  page.on("pageerror", error => errors.push(error.message));
  page.on("request", request => {
    if (request.url() === commandPath) commands.push(request.postDataJSON().op);
  });
  const waitText = (id, text) => page.waitForFunction(
    ({id, text}) => document.getElementById(id).textContent.includes(text), {id, text});
  const mouse = page.locator("#mouse-mode");
  const keyboard = page.locator("#keyboard-mode");
  const left = page.getByRole("button", {name: "Left click", exact: true});

  await page.goto(origin);
  await page.locator("#access-code").fill("12345678");
  await page.locator("#pair").click();
  await waitText("connection", "Connected / idle");

  // A poll sent while idle can arrive after the acquisition's own state
  // fetch has already verified the new lease.
  let observeOldState, deliverOldState;
  const late = new Promise(resolve => { observeOldState = resolve; });
  const deliver = new Promise(resolve => { deliverOldState = resolve; });
  await page.route(statePath, async route => {
    const response = await route.fetch();
    observeOldState((await response.json()).protocol);
    await deliver;
    await route.fulfill({response, headers: {...response.headers(), "x-fixture-late-state": "true"}});
  }, {times: 1});
  assert.equal((await late).lease_active, undefined);
  await page.locator("#acquire").click();
  await waitText("connection", "Controlling");
  const oldState = page.waitForResponse(reply => reply.headers()["x-fixture-late-state"] === "true");
  deliverOldState();
  await (await oldState).finished();
  await page.waitForFunction(() => !document.getElementById("refresh-connections").disabled);
  await mouse.focus();

  const initial = await page.evaluate(selectors => {
    const targets = selectors.map(selector => document.querySelector(selector));
    const changes = [];
    const observer = new MutationObserver(records => {
      for (const record of records) {
        if (record.type === "attributes" && (record.oldValue !== null) === record.target.hasAttribute("disabled")) continue;
        changes.push(record.target.id || record.target.dataset.click);
      }
    });
    for (const target of targets) observer.observe(target, {attributes: true, attributeOldValue: true, attributeFilter: ["disabled"]});
    for (const id of ["connection", "result"]) observer.observe(document.getElementById(id), {childList: true});
    window.finishRenewalObservation = () => { observer.disconnect(); return changes; };
    return {lastAction: document.getElementById("result").textContent, style: targets.map(target => ({
      disabled: target.matches(":disabled"), opacity: getComputedStyle(target).opacity,
    }))};
  }, snapshotSelectors);
  assert.equal(initial.style[0].disabled, false);
  assert.equal(initial.style[1].disabled, false);
  assert.equal(initial.style[4].disabled, false);

  async function appliedRenewal() {
    const response = await page.waitForResponse(async reply => reply.url().startsWith(`${origin}/api/v1/requests/`) &&
      reply.status() === 200 && (await reply.json()).state === "applied");
    const data = await response.json();
    renewalResults.push({state: data.state, error: data.error});
    assert.equal(data.error, "ok");
  }
  for (let i = 0; i < 3; ++i) await appliedRenewal();
  assert.deepEqual(await page.evaluate(() => window.finishRenewalObservation()), []);
  assert.equal(await mouse.evaluate(node => document.activeElement === node), true);
  assert.equal(await page.locator("#result").textContent(), initial.lastAction);
  assert.deepEqual(commands, ["control.acquire", ...Array(3).fill("control.renew")]);

  // Let the device admit the renewal and hold its response so the clicks
  // exercise the already occupied browser command lane.
  let beginHold, releaseHold;
  const held = new Promise(resolve => { beginHold = resolve; });
  const release = new Promise(resolve => { releaseHold = resolve; });
  const holdReply = async route => {
    const response = await route.fetch();
    beginHold(response.status());
    await release;
    await route.fulfill({response});
  };
  await page.route(commandPath, holdReply, {times: 1});
  const nextRenewal = appliedRenewal();
  try {
    assert.equal(await held, 202);
    assert.deepEqual(await page.evaluate(selectors => selectors.map(selector => {
      const target = document.querySelector(selector);
      return {disabled: target.matches(":disabled"), opacity: getComputedStyle(target).opacity};
    }), snapshotSelectors), initial.style);
    assert.equal(await mouse.evaluate(node => document.activeElement === node), true);
    assert.equal(await left.getAttribute("aria-disabled"), "true");
    assert.equal(await keyboard.getAttribute("aria-disabled"), "true");
    await left.evaluate(button => button.click());
    await keyboard.evaluate(button => button.click());
    assert.equal(await page.locator("#result").textContent(), initial.lastAction);
    assert.deepEqual(commands, ["control.acquire", ...Array(4).fill("control.renew")]);
  } finally { releaseHold(); }
  await nextRenewal;
  await appliedRenewal();
  assert.deepEqual(commands, ["control.acquire", ...Array(5).fill("control.renew")]);

  // The next renewal is admitted by the real fixture, but the page loses
  // its reply. The old session must stop emitting input or renewal commands.
  let droppedStatus;
  await page.route(commandPath, async route => {
    droppedStatus = (await route.fetch()).status();
    await route.abort();
  }, {times: 1});
  await waitText("result", "Reply unknown");
  assert.equal(droppedStatus, 202);
  await waitText("connection", "Connected / idle");
  assert(await left.isDisabled());
  assert(await mouse.isDisabled());
  assert(await keyboard.isDisabled());
  assert(await page.locator("#acquire").isDisabled());
  await page.waitForTimeout(1000);
  assert.deepEqual(commands, ["control.acquire", ...Array(6).fill("control.renew")]);
  await page.screenshot({path: path.join(output, "renewal-reply-lost.png"), fullPage: true});

  await page.locator("#disconnect").click();
  await page.locator("#pair-title").click();
  await page.locator("#connect").click();
  await waitText("connection", "Connected / idle");
  await page.locator("#acquire").click();
  await waitText("connection", "Controlling");

  // Two independent browser identities share one lease owner. The idle
  // browser can read state and close its session without affecting it.
  const ownerState = async () => (await (await page.request.get(statePath)).json()).protocol;
  const owned = await ownerState();
  assert.equal(owned.lease_active, true);
  const mobile = await browser.newContext({ignoreHTTPSErrors: true, viewport: {width: 390, height: 844},
    hasTouch: true, isMobile: true});
  const second = await mobile.newPage();
  second.setDefaultTimeout(8000);
  await second.goto(origin);
  await second.locator("#access-code").fill("12345678");
  await second.locator("#pair").tap();
  await second.waitForFunction(() => document.getElementById("connection").textContent === "Connected / idle");
  const observerRead = await second.waitForResponse(reply => reply.url() === statePath && reply.status() === 200);
  assert.equal((await observerRead.json()).protocol.lease_id, undefined);
  await second.locator("#acquire").tap();
  await second.waitForFunction(() => document.getElementById("result").textContent.includes("rejected: owner_busy"));
  assert.equal(await second.locator("#connection").textContent(), "Connected / idle");
  assert(await second.locator("#result").evaluate(node => Boolean(node.compareDocumentPosition(
    document.getElementById("input-title")) & Node.DOCUMENT_POSITION_FOLLOWING)));
  await second.locator("#disconnect").tap();
  await second.waitForFunction(() => document.getElementById("result").textContent.includes("Browser session closed."));
  await mobile.close();
  const afterSecondBrowser = await ownerState();
  assert.equal(afterSecondBrowser.lease_id, owned.lease_id);
  assert.equal(afterSecondBrowser.lease_active, true);
  assert.equal(await page.locator("#connection").textContent(), "Controlling");

  assert.equal((await page.request.post(`${origin}/_fixture/takeover`)).status(), 200);
  const atTakeover = [...commands];
  await waitText("connection", "Connected / idle");
  assert(await mouse.isDisabled());
  assert(await left.isDisabled());
  await page.waitForTimeout(1000);
  assert.deepEqual(commands, atTakeover);
  assert.equal(commands.filter(op => op === "control.acquire").length, 2);
  assert(commands.every(op => ["control.acquire", "control.renew"].includes(op)));
  assert.deepEqual(errors, []);

  await writeFile(path.join(output, "lease-renewal.json"), JSON.stringify({
    browser: browser.version(), renewalResults, commands, droppedStatus,
    verified: ["Stable native disabled attributes, focus and live regions across three renewals",
      "Stable computed opacity and two inactive, unqueued presses during an admitted renewal",
      "Delayed idle state cannot replace a verified lease",
      "A second browser sees owner_busy near Take control and does not revoke the owner",
      "No input command queued or replayed", "Lost reply and local takeover disable input"],
  }, null, 2) + "\n");
  console.log("PASS: delayed idle reads and a competing browser preserve the owner, with no replay or unsafe renewal");
} finally {
  await browser.close();
}
