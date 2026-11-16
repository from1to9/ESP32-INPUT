// SPDX-License-Identifier: Apache-2.0
import assert from "node:assert/strict";
import {pathToFileURL} from "node:url";
import {writeFile} from "node:fs/promises";
import path from "node:path";

const {chromium} = await import(pathToFileURL(process.env.WEB_PLAYWRIGHT_MODULE));
const origin = process.env.WEB_ORIGIN, output = process.env.WEB_OUTPUT;
const browser = await chromium.launch({headless: true,
  ...(process.env.WEB_BROWSER_EXECUTABLE ? {executablePath: process.env.WEB_BROWSER_EXECUTABLE} : {})});
const fixture = (page, action) => page.request.post(`${origin}/_fixture/${action}`);
const waitText = (page, id, text) => page.waitForFunction(({id, text}) => document.getElementById(id).textContent.includes(text), {id, text});
const artifacts = [];
const errors = [];
const envelopes = [];
function observe(page) {
  page.setDefaultTimeout(12000);
  page.on("pageerror", error => errors.push(error.message));
  page.on("request", request => {
    if (request.url() !== `${origin}/api/v1/commands`) return;
    const envelope = request.postDataJSON();
    assert(!envelope.epoch && !envelope.lease_id && !envelope.transport_generation && !envelope.target_id);
    envelopes.push({op: envelope.op, seq: envelope.seq, resource_revision: envelope.resource_revision});
  });
}
async function connect(page, first) {
  await page.goto(origin);
  if (first) {
    await page.locator("#client-name").fill("Management test");
    await page.locator("#pair").click();
    await waitText(page, "approval", "Waiting for approval");
    assert.equal((await fixture(page, "approve-management")).status(), 200);
    await page.locator("#check").click();
  } else await page.locator("#connect").click();
  await waitText(page, "connection", "Connected / idle");
  await waitText(page, "ble-link", "BLE link ready");
  await waitText(page, "wifi-profiles", "Workshop network");
  assert.equal(await page.locator("#acquire").isDisabled(), true);
}
async function themes(page, variant, columns) {
  await page.waitForFunction(() => !document.getElementById("refresh-connections").disabled);
  assert.equal(await page.locator(".dashboard").evaluate(node => getComputedStyle(node).gridTemplateColumns.split(" ").length), columns);
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth));
  assert(await page.locator("#theme").evaluate(node => node.getBoundingClientRect().height >= 44));
  assert(await page.locator("#scan").evaluate(node => node.getBoundingClientRect().height >= 44));
  for (const theme of ["light", "dark"]) {
    await page.locator("#theme").selectOption(theme);
    await page.waitForFunction(value => document.documentElement.dataset.theme === value, theme);
    assert.equal(await page.locator(":root").evaluate(node => getComputedStyle(node).colorScheme), theme);
    const file = `${variant}-${theme}.png`;
    await page.screenshot({path: path.join(output, file), fullPage: true, animations: "disabled"});
    artifacts.push(file);
  }
}
try {
  const context = await browser.newContext({
    ignoreHTTPSErrors: true, viewport: {width: 1280, height: 920}, colorScheme: "dark",
  });
  const page = await context.newPage();
  observe(page);
  await connect(page, true);
  assert.equal(await page.locator("#theme").inputValue(), "system");
  assert.equal(await page.locator(":root").evaluate(node => getComputedStyle(node).colorScheme), "dark");
  const scan = page.locator("#scan");
  await scan.click();
  await waitText(page, "wifi-scan", "Guest WLAN");
  const nearby = page.locator("#wifi-scan");
  await nearby.getByRole("button", {name: "Add label", exact: true}).click();
  await page.locator("#editor-value").fill("<b>Guest</b>");
  await page.locator("#editor-save").click();
  await waitText(page, "management-result", "Display label saved");
  await waitText(page, "wifi-scan", "<b>Guest</b>");
  assert.equal(await nearby.locator("b").count(), 0);
  await page.locator("#retained-labels").getByRole("button", {name: "Clear label", exact: true}).click();
  await waitText(page, "management-result", "Display label cleared");
  await waitText(page, "retained-labels", "No display labels saved");
  await nearby.getByRole("button", {name: "Add label", exact: true}).click();
  await page.locator("#editor-value").fill("Guest access");
  await page.locator("#editor-save").click();
  await waitText(page, "retained-labels", "Guest access");
  await nearby.getByRole("button", {name: "Save network", exact: true}).click();
  assert.equal(await page.locator("#editor-value").getAttribute("type"), "password");
  await page.locator("#editor-value").fill("fixture-passphrase");
  await page.locator("#editor-save").click();
  await waitText(page, "management-result", "Network saved");

  await page.locator("#pair-duration").selectOption("60");
  await page.getByRole("button", {name: "Pair new host", exact: true}).click();
  await waitText(page, "management-result", "Review the request on Mosaico");
  assert.equal((await fixture(page, "confirm-pairing")).status(), 200);
  await waitText(page, "management-result", "Pairing is visible");
  await waitText(page, "ble-link", "pairing open");
  await page.locator("#ble-peers").getByRole("button", {name: "Select host", exact: true}).click();
  await waitText(page, "management-result", "saving the selection");
  assert.equal((await fixture(page, "finish-target")).status(), 200);
  await waitText(page, "management-result", "did not become ready in time");
  await waitText(page, "connection", "Connected / idle");
  await themes(page, "desktop", 2);

  await page.reload();
  await page.waitForFunction(() => document.documentElement.dataset.theme === "dark");
  assert.equal(await page.locator("#theme").inputValue(), "dark");
  const saved = await context.storageState();
  await context.close();
  for (const [variant, width, height] of [["iphone", 390, 844], ["ipad", 820, 1180]]) {
    const touch = await browser.newContext({ignoreHTTPSErrors: true, viewport: {width, height},
      colorScheme: "light", isMobile: true, hasTouch: true, storageState: {cookies: [], origins: saved.origins}});
    const screen = await touch.newPage();
    observe(screen);
    await connect(screen, false);
    await screen.locator("#scan").click();
    await waitText(screen, "wifi-scan", "Guest access");
    await themes(screen, variant, 1);
    await touch.close();
  }
  assert.deepEqual(errors, []);
  assert(envelopes.some(item => item.op === "bluetooth.pairing.open"));
  assert(envelopes.some(item => item.op === "bluetooth.target.request" && item.resource_revision === "31"));
  assert(envelopes.some(item => item.op === "wifi.profile.save" && item.resource_revision === "44"));
  await writeFile(path.join(output, "management.json"), JSON.stringify({browser: browser.version(), artifacts, envelopes,
    verified: ["management-only browser login", "physical BLE approval", "target timeout while the browser remains connected",
      "Wi-Fi scan, saving the original SSID, and exact AP label set/clear", "safe text rendering", "manual theme persistence", "desktop and touch layouts"],
    boundaries: ["Loopback HTTPS and production access, HTTP, command and coordinator; Wi-Fi/label and BLE worker outcomes use host fixtures",
      "iPhone and iPad are Chromium touch viewports, without physical Safari or a connected board"]}, null, 2) + "\n");
  console.log("PASS: real Chromium management flows and six responsive theme screenshots");
} finally { await browser.close(); }
