// Actual Chromium rendering of the embedded production assets. This loopback
// server substitutes Wi-Fi/HTTP observations; it does not qualify a board or iOS.
import assert from 'node:assert/strict';
import http from 'node:http';
import { readFile, mkdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import path from 'node:path';
const require = createRequire(import.meta.url);
const { chromium } = require(process.env.PASSPORT_PLAYWRIGHT || 'playwright');
const root = fileURLToPath(new URL('../../..', import.meta.url));
const out = process.env.PASSPORT_WEB_OUTPUT || path.join(root, '.agent/local/passport-setup/web');
const capacityOnly = process.argv.includes('--capacity-only');
await mkdir(out, { recursive: true });
const token = '0123456789abcdef0123456789abcdef';
const hex = text => Buffer.from(text).toString('hex').toUpperCase();
function initial() {
  return { open: true, setupId: 17, seconds: 108, token, storageKnown: true, profileCount: 1, scanning: false, scanKnown: true, truncated: false, scanRevision: 4, pending: false, requestId: 0, requestState: 0, requestResult: '', lastSubmissionId: 0, submissionAccepted: false, result: 'Scan complete', staIp: '', apIp: '192.168.4.1', rows: [
    { ssidHex: hex('Meridian Studio'), security: 1, rssi: -42, profileId: 1, current: false },
    { ssidHex: hex('Library Guest'), security: 0, rssi: -58, profileId: 0, current: false },
    { ssidHex: hex('Workshop Upstairs'), security: 1, rssi: -67, profileId: 0, current: false },
    { ssidHex: hex('Office Enterprise'), security: 2, rssi: -72, profileId: 0, current: false },
  ] };
}
let fixture = initial(), requests = [], unavailable = false;
const assets = new Map(await Promise.all(['html','css','js'].map(async ext => [ext, await readFile(path.join(root, `passport/main/web/setup.${ext}`))])));
const server = http.createServer(async (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  res.setHeader('Content-Security-Policy', "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
  if (req.url === '/api/setup/state') { res.writeHead(unavailable ? 503 : 200, {'Content-Type':'application/json'}); res.end(JSON.stringify(unavailable ? {error:'Fixture AP disconnected'} : fixture)); return; }
  if (req.method === 'POST' && ['/api/setup/scan','/api/setup/submit'].includes(req.url)) {
    let body = ''; for await (const chunk of req) body += chunk;
    const data = JSON.parse(body); requests.push({ path:req.url, data, token:req.headers['x-passport-setup'] });
    assert.equal(req.headers['x-passport-setup'], token);
    if (req.url.endsWith('scan')) { fixture.scanRevision++; res.writeHead(202,{'Content-Type':'application/json'}); res.end('{"queued":true,"requestId":0}'); return; }
    fixture.pending = true; fixture.requestId = fixture.lastSubmissionId = 123456; fixture.submissionAccepted = true; fixture.requestState = 1; fixture.requestResult = 'Confirm this request on Passport';
    res.writeHead(202,{'Content-Type':'application/json'}); res.end('{"queued":true,"requestId":123456}'); return;
  }
  const ext = req.url === '/setup' ? 'html' : req.url === '/setup.css' ? 'css' : req.url === '/setup.js' ? 'js' : '';
  if (!ext) { res.writeHead(404);res.end();return; }
  res.setHeader('Content-Type', {html:'text/html',css:'text/css',js:'text/javascript'}[ext]); res.end(assets.get(ext));
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const url = `http://127.0.0.1:${server.address().port}/setup`;
const browser = await chromium.launch({headless:true, executablePath:process.env.PASSPORT_CHROMIUM});
const errors = [];
async function open(context, changes = {}) {
  fixture = { ...initial(), ...changes }; unavailable = false;
  const page = await context.newPage();
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(url); await page.getByText('Setup is open on Passport',{exact:true}).waitFor();
  return page;
}
async function noOverflow(page) { assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)); }
try {
  const capacityContext = await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true});
  const capacity = await open(capacityContext, {profileCount:4});
  await capacity.getByRole('button',{name:/Library Guest/}).click();
  assert(await capacity.locator('#submit').isDisabled());
  await capacity.getByText('Four profiles are already saved. Forget a profile on Passport first.').waitFor();
  assert.equal(requests.length,0);
  await capacity.getByRole('button',{name:/Meridian Studio/}).click();
  assert(!await capacity.locator('#submit').isDisabled());
  assert(!await capacity.locator('#form-error').isVisible());
  await capacity.locator('#password').fill('updated123');
  await capacity.locator('#submit').click();
  await capacity.getByText('Confirm on Passport',{exact:true}).waitFor();
  assert.equal(requests.length,1);
  assert.deepEqual(requests[0].data,{setupId:17,scanRevision:4,row:0,password:'updated123'});
  await capacityContext.close(); requests=[];
  console.log('PASS: full-capacity page rejects a new row, admits exact saved-row credential update, and clears the prior capacity error.');
  if (!capacityOnly) {
  for (const [name, viewport, mobile] of [['desktop',{width:1440,height:1100},false],['iphone',{width:390,height:844},true],['ipad',{width:1024,height:1366},true]]) {
    for (const colorScheme of ['dark','light']) {
      const context = await browser.newContext({ viewport, colorScheme, isMobile:mobile, hasTouch:mobile, deviceScaleFactor:1 });
      const page = await open(context);
      await page.getByRole('button',{name:/Meridian Studio/}).click();
      await page.locator('#password').fill('synthetic-passphrase');
      assert.equal(await page.locator('html').getAttribute('data-theme'),colorScheme);
      await noOverflow(page);
      await page.screenshot({path:path.join(out,`${name}-${colorScheme}.png`),fullPage:true});
      await context.close();
    }
  }
  const context = await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true,colorScheme:'light'});
  const page = await open(context);
  await page.getByRole('button',{name:/Meridian Studio/}).click();
  await page.locator('#password').fill('short'); await page.locator('#submit').click();
  await page.getByText('Enter 8–63 ASCII characters for a WPA2 password.').waitFor(); assert.equal(requests.length,0);
  await page.locator('#password').fill('fixture123'); await page.locator('#show-password').click(); assert.equal(await page.locator('#password').getAttribute('type'),'text');
  await page.locator('#submit').click(); await page.getByText('Confirm on Passport',{exact:true}).waitFor();
  assert.equal(requests.length,1); assert.deepEqual(requests[0].data,{setupId:17,scanRevision:4,row:0,password:'fixture123'});
  assert.equal(await page.locator('#password').inputValue(),''); assert.equal(await page.locator('#request-number').textContent(),'REQUEST 123456');
  fixture.requestState=3; fixture.requestId=999; fixture.requestResult='LAN connected; profile saved';
  await page.getByText('Request queued',{exact:true}).waitFor(); // Another receipt must not claim our success.
  fixture.requestId=123456; fixture.requestState=2; fixture.requestResult='Joining; save is not confirmed yet';
  await page.getByText('Joining your network',{exact:true}).waitFor();
  unavailable=true; await page.getByText('Check Passport to finish',{exact:true}).waitFor();
  assert.equal(requests.length,1); assert(!await page.getByText('Connected and saved',{exact:true}).isVisible());
  unavailable=false; fixture.pending=false; fixture.open=false; fixture.token=''; fixture.requestState=4; fixture.requestResult='Profile save uncertain; restart required';
  await page.getByText('Connection did not complete',{exact:true}).waitFor();
  await page.getByText('Profile save uncertain; restart required',{exact:true}).waitFor();
  await page.screenshot({path:path.join(out,'iphone-storage-uncertain.png'),fullPage:true});
  fixture.requestState=3; fixture.requestResult='LAN connected; profile saved';
  await page.getByText('Connected and saved',{exact:true}).waitFor();
  await page.close();
  const again = await open(context);
  await again.getByRole('button',{name:/Library Guest/}).click(); assert(!await again.locator('#password').isVisible());
  await again.locator('#submit').click(); await again.getByText('Confirm on Passport',{exact:true}).waitFor(); assert.equal(requests.at(-1).data.password,'');
  await again.close();
  const edge = await open(context);
  fixture.rows[0].ssidHex=hex('<img src=x onerror=alert(1)>'); fixture.scanRevision++;
  await edge.getByRole('button',{name:/<img src=x/}).waitFor(); assert.equal(await edge.locator('img').count(),0);
  await edge.getByRole('button',{name:/<img src=x/}).click();
  fixture.scanRevision++; await edge.locator('#selection-empty').waitFor({state:'visible'});
  await edge.locator('#scan').click(); await edge.locator('#scan').waitFor({state:'visible'});
  await edge.locator('#theme').click(); const theme=await edge.locator('html').getAttribute('data-theme');
  await edge.reload(); await edge.getByText('Setup is open on Passport',{exact:true}).waitFor(); assert.equal(await edge.locator('html').getAttribute('data-theme'),theme);
  fixture.open=false;fixture.token=''; await edge.getByText('Setup window is closed',{exact:true}).waitFor();
  assert(await edge.locator('#scan').isDisabled()); assert(!await edge.locator('#connect-form').isVisible());
  await noOverflow(edge); await edge.screenshot({path:path.join(out,'iphone-closed.png'),fullPage:true});
  assert.deepEqual(await edge.evaluate(() => Object.keys(localStorage)),['passport-theme']);
  await context.close(); assert.deepEqual(errors,[]);
  console.log('PASS: six responsive theme renders; password/open submit, exact receipt, uncertainty, no retry, stale scan, literal SSID, closed window and persistent theme. Chromium viewport simulation only.');
  }
  assert.deepEqual(errors,[]);
} finally { await browser.close(); await new Promise(resolve=>server.close(resolve)); }
