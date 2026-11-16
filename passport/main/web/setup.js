'use strict';
const $ = id => document.getElementById(id);
const security = ['Open network', 'WPA2 Personal', 'Unsupported security'];
let state, selected = null, selectedRevision = -1, requestId = 0, sending = false, failures = 0, loading = false;
let listKey = '', scanWaiting = false, scanUntil = 0, scanFrom = -1;
let theme;
try { theme = localStorage.getItem('passport-theme'); } catch {}
if (!['dark', 'light'].includes(theme)) theme = matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
function applyTheme() {
  document.documentElement.dataset.theme = theme;
  $('theme').textContent = theme === 'dark' ? 'Light theme' : 'Dark theme';
}
applyTheme();
$('theme').addEventListener('click', () => { theme = theme === 'dark' ? 'light' : 'dark'; applyTheme(); try { localStorage.setItem('passport-theme', theme); } catch {} });
function ssid(hex) {
  if (!hex) return 'Hidden network';
  try {
    const bytes = Uint8Array.from(hex.match(/../g), x => parseInt(x, 16));
    const name = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
    return /[\u0000-\u001f\u007f-\u009f\u200b-\u200f\u202a-\u202e\u2060-\u206f\ufeff]/u.test(name) ? `Hex: ${hex}` : name;
  } catch { return `Hex: ${hex}`; }
}
function notice(text = '', problem = false) {
  $('notice').hidden = !text; $('notice').textContent = text; $('notice').classList.toggle('problem', problem);
}
function error(text = '') { $('form-error').hidden = !text; $('form-error').textContent = text; $('password').setAttribute('aria-invalid', String(Boolean(text))); }
function allowed() { return Boolean(state?.open && state.token && state.storageKnown && !state.pending && !sending && !requestId && failures === 0); }
function clearSelection() { selected = null; $('password').value = ''; error(); }
function choose(index) {
  clearSelection(); selected = index; selectedRevision = state.scanRevision;
  render();
}
function renderList() {
  const key = JSON.stringify([state.scanRevision, state.scanKnown, state.rows, selected, allowed()]);
  if (key === listKey) return;
  listKey = key;
  const list = $('network-list'); list.replaceChildren();
  if (!state.scanKnown || !state.rows.length) {
    const empty = document.createElement('p'); empty.className = 'empty';
    empty.textContent = state.scanning ? 'Looking for nearby 2.4 GHz networks…' : state.open ? 'No networks listed. Use Scan again to search.' : 'Open setup on Passport to scan nearby networks.';
    list.append(empty);
  }
  state.rows.forEach((row, index) => {
    const button = document.createElement('button'); button.className = 'network'; button.type = 'button';
    button.setAttribute('aria-pressed', String(index === selected));
    button.disabled = !allowed() || state.scanning || !row.ssidHex || row.security === 2;
    const mark = document.createElement('span'); mark.className = 'network-mark'; mark.textContent = '⌁'; mark.setAttribute('aria-hidden', 'true');
    const main = document.createElement('span'); main.className = 'network-main';
    const name = document.createElement('span'); name.className = 'network-name'; name.textContent = ssid(row.ssidHex);
    const meta = document.createElement('span'); meta.className = 'network-meta';
    meta.textContent = `${security[row.security]} · ${row.rssi} dBm${row.profileId ? ' · Saved' : ''}${row.current ? ' · Connected' : ''}`;
    const check = document.createElement('span'); check.className = 'network-check'; check.textContent = index === selected ? '✓' : ''; check.setAttribute('aria-hidden', 'true');
    main.append(name, meta); button.append(mark, main, check); button.addEventListener('click', () => choose(index)); list.append(button);
  });
}
function render() {
  if (!state) return;
  if (selectedRevision !== state.scanRevision || !state.scanKnown) clearSelection();
  if (scanWaiting && (state.scanRevision !== scanFrom || Date.now() >= scanUntil)) scanWaiting = false;
  document.querySelector('.session').classList.toggle('active', state.open && failures === 0);
  $('session-title').textContent = failures ? 'Passport connection interrupted' : state.open ? 'Setup is open on Passport' : 'Setup window is closed';
  $('session-detail').textContent = state.open ? 'Every network change needs approval on the device.' : 'On Passport: Wi-Fi → Use AP → Open setup. Unlock the device first.';
  $('countdown').textContent = state.open ? `${state.seconds}s LEFT` : 'LOCAL AP';
  $('scan').disabled = !allowed() || state.scanning || scanWaiting;
  $('scan').textContent = state.scanning || scanWaiting ? 'Scanning…' : 'Scan again';
  $('scan-detail').textContent = `${state.scanning ? 'Scanning' : `${state.rows.length} networks`} · 2.4 GHz · Open or WPA2`;
  $('scan-note').textContent = state.truncated ? 'Showing up to 16 scan results. Move closer and scan again if your network is missing.' : 'Hidden networks and unsupported security cannot be configured here.';
  renderList();
  const row = selected !== null ? state.rows[selected] : null;
  $('selection-empty').hidden = Boolean((row && state.open) || requestId || state.pending);
  $('connect-form').hidden = !row || Boolean(requestId || state.pending) || !state.open;
  $('request-card').hidden = !(requestId || state.pending);
  if (row) {
    $('selected-name').textContent = ssid(row.ssidHex); $('selected-security').textContent = security[row.security]; $('selected-hex').textContent = row.ssidHex;
    $('password-group').hidden = row.security === 0; $('open-note').hidden = row.security !== 0;
    const full = state.profileCount >= 4 && !row.profileId;
    $('password').disabled = !allowed(); $('submit').disabled = !allowed() || state.scanning || full;
    if (full) error('Four profiles are already saved. Forget a profile on Passport first.');
  }
  if (requestId || state.pending) {
    const matching = requestId && state.requestId === requestId;
    let title = 'Another request needs confirmation', detail = 'Use Passport to approve or cancel that request before submitting another.';
    if (requestId) {
      title = 'Request queued'; detail = 'Waiting for Passport to validate this request. Nothing is saved yet.';
      if (matching) {
        title = ['Request queued', 'Confirm on Passport', 'Joining your network', 'Connected and saved', 'Connection did not complete', 'Request cancelled'][state.requestState];
        detail = state.requestResult;
      } else if (state.lastSubmissionId === requestId && !state.submissionAccepted) { title = 'Request rejected'; detail = 'The setup session or network list changed. Open setup on Passport and refresh the page.'; }
      else if (!state.open) { title = 'Setup closed'; detail = 'Check Passport for this request’s outcome. A saved profile has not been confirmed here.'; }
      if (failures) { title = 'Check Passport to finish'; detail = 'The AP connection ended or changed. Check the device for the join result and LAN address. This page cannot confirm a save.'; }
    }
    $('request-title').textContent = title; $('request-detail').textContent = detail;
    $('request-number').textContent = `REQUEST ${requestId || state.requestId}`;
    $('restart').hidden = !requestId || !((matching && state.requestState >= 4) || (state.lastSubmissionId === requestId && !state.submissionAccepted));
  }
  if (!state.storageKnown) notice('Profile storage is unavailable. Restart Passport before changing saved networks.', true);
}
async function api(path, data) {
  const response = await fetch(`/api/setup/${path}`, { method: data ? 'POST' : 'GET', cache: 'no-store', credentials: 'omit', signal: AbortSignal.timeout(4500), ...(data ? { headers: { 'Content-Type': 'application/json', 'X-Passport-Setup': state.token }, body: JSON.stringify(data) } : {}) });
  const value = await response.json();
  if (!response.ok) throw new Error(value.error || 'Passport could not process this request.');
  return value;
}
async function refresh() {
  if (loading || document.hidden) return;
  loading = true;
  try {
    state = await api('state');
    if (failures) notice(); failures = 0; render();
  } catch (err) {
    ++failures;
    notice(requestId ? 'Connection changed. Check Passport for this request’s result and LAN address. No request will be sent again automatically.' : `Cannot reach setup. Stay on the Passport AP and reopen setup on the device if needed. ${err.message}`, true);
    $('scan').disabled = $('submit').disabled = true;
    render();
  } finally { loading = false; }
}
$('scan').addEventListener('click', async () => {
  if (!allowed()) return;
  scanWaiting = true; scanFrom = state.scanRevision; scanUntil = Date.now() + 2000; render(); notice();
  try { await api('scan', { setupId: state.setupId }); }
  catch (err) { scanWaiting = false; notice(err.message, true); }
  await refresh();
});
$('show-password').addEventListener('click', () => {
  const show = $('password').type === 'password'; $('password').type = show ? 'text' : 'password';
  $('show-password').textContent = show ? 'Hide' : 'Show'; $('show-password').setAttribute('aria-pressed', String(show));
});
$('restart').addEventListener('click', () => location.reload());
$('connect-form').addEventListener('submit', async event => {
  event.preventDefault();
  if (!allowed() || selected === null || selectedRevision !== state.scanRevision) return;
  const row = state.rows[selected];
  const password = row.security === 0 ? '' : $('password').value;
  if (row.security === 1 && !/^[\x20-\x7E]{8,63}$/.test(password)) { error('Enter 8–63 ASCII characters for a WPA2 password.'); $('password').focus(); return; }
  error(); notice(); sending = true; render();
  try {
    const reply = await api('submit', { setupId: state.setupId, scanRevision: selectedRevision, row: selected, password });
    if (!reply.queued || !Number.isInteger(reply.requestId) || !reply.requestId) throw new Error('No request receipt was returned. Check Passport before trying again.');
    requestId = reply.requestId;
  } catch (err) {
    // A lost response can occur after enqueue. Never retry a password submission.
    notice(`${err.message} Check Passport before submitting again.`, true);
  } finally { $('password').value = ''; sending = false; render(); await refresh(); }
});
document.addEventListener('visibilitychange', () => { if (!document.hidden) refresh(); });
refresh(); setInterval(refresh, 1000);
