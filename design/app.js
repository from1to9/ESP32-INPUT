// SPDX-License-Identifier: Apache-2.0
// Interaction demonstrator only. No network, Bluetooth or microphone access.
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const labels = { mouse: 'Mouse', keyboard: 'Keyboard', audio: 'Audio' };
  const groups = ['ABCDEF', 'GHIJKL', 'MNOPQR', 'STUVWX', 'YZ0123', '456789'];
  const symbolGroups = ['.,!?;:', '-_+=/*', '()[]{}', '@#$%&', '\\|<>', '"\'`~^'];
  const s = { mode: 'mouse', target: 'iPad Pro', connected: true, gyro: false,
    locked: false, remote: false, menu: '', fine: 1, drag: false, epoch: 1,
    x: 50, y: 50, sub: 'touch', group: -1, focus: 0, lower: false, symbols: false,
    draft: '', sending: false, sent: '', voice: 'idle', candidate: '', word: 'Alpha',
    audioKind: 'voice', audioOn: false, muted: true, coexist: false, volume: 50 };
  let sendTimer, touch = null, frozenUntil = 0;
  const escape = value => String(value).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const btn = (action, text, extra = '') => `<button data-action="${action}" ${extra}>${text}</button>`;
  const log = text => { $('eventLog').textContent = text; };
  const groupList = () => s.symbols ? symbolGroups : groups;
  const items = () => s.group < 0 ? groupList() : [...groupList()[s.group]];
  function boundary({mute = false} = {}) {
    s.epoch++; s.gyro = false; s.drag = false; s.sending = false;
    s.voice = 'idle'; s.candidate = ''; touch = null;
    frozenUntil = performance.now() + 150;
    clearTimeout(sendTimer);
    if (mute) s.muted = true;
  }
  function takeLocal() {
    if (!s.remote) return false;
    boundary({mute:true}); s.remote = false;
    log('Local control restored. This action was consumed; remote work will not resume automatically.'); render(); return true;
  }
  function guard() { return !s.locked && !takeLocal(); }
  function setMode(mode) {
    if (!guard()) return;
    if (s.mode !== mode) {
      boundary(); s.mode = mode; s.group = -1; s.focus = 0;
      if (mode === 'keyboard') s.sub = 'touch';
      if (!s.coexist && mode !== 'audio') s.audioOn = false;
      log(`Entered ${labels[mode]} mode; keys released and stale input cancelled.`);
    }
    s.menu = ''; render();
  }
  function addCharacter(c) {
    if (s.sending) return;
    if (s.draft.length >= 64) { log('Preview is limited to 64 characters. Send or clear it first.'); return; }
    s.draft += s.lower ? c.toLowerCase() : c;
    s.group = -1; s.focus = 0; s.candidate = ''; s.voice = 'idle';
    log('Character added to preview, not sent.'); render();
  }
  function confirm() {
    if (s.mode !== 'keyboard' || s.sending) return;
    if (s.sub === 'voice') {
      if (s.candidate) return addCharacter(s.candidate);
      if (s.voice === 'listening') {
        s.candidate = s.word[0]; s.voice = 'candidate';
        log(`Demo candidate ${s.word} → ${s.candidate}. Confirm again to add it to preview.`);
      } else { s.voice = 'listening'; log('Listening simulation started. Click again for the selected sample candidate; no microphone is active.'); }
      render(); return;
    }
    if (s.sub === 'shortcuts') { log('Choose a shortcut explicitly on the touch screen.'); return; }
    if (s.group < 0) { s.group = s.focus; s.focus = 0; render(); }
    else addCharacter(items()[s.focus]);
  }
  function startSend() {
    if (!s.connected || s.sending || !s.draft) return;
    s.sending = true; s.sent = ''; const epoch = s.epoch;
    function tick() {
      if (epoch !== s.epoch || !s.sending || !s.connected || s.locked) return;
      s.sent += s.draft[0]; s.draft = s.draft.slice(1);
      log(`Simulated submission: "${s.sent}", without Enter. Real target content still needs verification.`);
      if (!s.draft) s.sending = false;
      render();
      if (s.sending) sendTimer = setTimeout(tick, 160);
    }
    render(); sendTimer = setTimeout(tick, 160);
  }
  function renderMouse() {
    return `<div class="row gyro-row"><div><h2>Point naturally</h2><p class="hint">Wrist motion · precise touch</p></div>${btn('gyro', s.gyro?'Motion · on':'Motion · off',`aria-pressed="${s.gyro}" ${!s.connected?'disabled':''}`)}</div>
      <div class="row fine"><span>Touch gain</span><div>${[1,.25,.125].map(v=>btn(`fine:${v}`,v===1?'1×':v===.25?'¼×':'⅛×',`aria-pressed="${s.fine===v}"`)).join('')}</div></div>
      <div class="touch-layout"><div id="touchpad" class="touchpad" aria-label="Touchpad: drag to adjust the demo pointer"><span class="pad-label">Touchpad</span><small>Touch pauses motion</small><i class="pointer-dot" style="left:${s.x}%;top:${s.y}%"></i></div><div class="scroll-pad">${btn('scrollUp','↑','aria-label="Scroll up"')}<span>Scroll</span>${btn('scrollDown','↓','aria-label="Scroll down"')}</div></div>
      <div class="toolbar">${btn('left','Left click')}${btn('drag',s.drag?'Release':'Drag',`aria-pressed="${s.drag}"`)}${btn('right','Right click')}</div>`;
  }
  function renderKeyboard() {
    const subs = {motion:'Motion',touch:'Touch',voice:'Voice letters',shortcuts:'Shortcuts'};
    let middle;
    if (s.sub === 'voice') middle = `<div class="voice-box"><span class="candidate">${escape(s.candidate || (s.voice==='listening'?'…':'A–Z'))}</span><div class="voice-description"><label class="hint" for="sampleWord">Sample word · no recording</label><select id="sampleWord">${['Alpha','Bravo','Charlie','Delta','Echo','Foxtrot','Zulu'].map(w=>`<option ${s.word===w?'selected':''}>${w}</option>`).join('')}</select>${btn('confirm',s.candidate?'Confirm':s.voice==='listening'?'Show candidate':'Start demo')}</div></div>`;
    else if (s.sub === 'shortcuts') middle = `<div class="character-grid">${['copy','paste','undo','tab','escape','enter'].map((v,i)=>btn(`shortcut:${v}`,['Copy','Paste','Undo','Tab','Esc','Enter'][i])).join('')}</div>`;
    else middle = `<div class="row char-toolbar">${btn('back',s.group<0?'Choose group':'← Groups')}${btn('case',s.lower?'abc':'ABC')}${btn('symbols',s.symbols?'Letters / digits':'Symbols')}</div><div class="character-grid">${items().map((c,i)=>btn(`character:${i}`,escape(s.lower?c.toLowerCase():c),`${s.sub==='motion'&&i===s.focus?'class="selected"':''} ${s.sending?'disabled':''}`)).join('')}</div>`;
    return `<div class="subnav">${Object.entries(subs).map(([k,v])=>btn(`sub:${k}`,v,`aria-pressed="${s.sub===k}"`)).join('')}</div><div class="preview"><output aria-label="Text preview">${escape(s.draft)||'Ready for input…'}</output><small>${s.sending?'Sending':`${s.draft.length} / 64`}</small>${btn('clear','Clear',s.sending?'disabled':'')}</div>${middle}<div class="keyboard-footer">${btn('delete','Delete',s.sending?'disabled':'')}${btn('space','Space',s.sending?'disabled':'')}${btn(s.sending?'stop':'send',s.sending?'Stop sending':'Send',`class="primary" ${!s.connected||(!s.draft&&!s.sending)?'disabled':''}`)}</div>`;
  }
  function renderAudio() {
    return `<div class="row"><h2>Audio</h2><span class="warning">Unverified</span></div><div class="toolbar">${btn('voiceMode','Voice / meeting',`aria-pressed="${s.audioKind==='voice'}"`)}${btn('speakerMode','Speaker',`aria-pressed="${s.audioKind==='speaker'}"`)}</div><div class="audio-hero"><h3>${!s.audioOn?'Audio is off':s.audioKind==='speaker'?'Speaker demo':s.muted?'Microphone muted':'Microphone demo'}</h3><p class="hint">${s.audioKind==='voice'?'HFP · duplex voice':'A2DP · music only'}</p>${btn('audioPower',s.audioOn?'Stop audio':'Start audio demo','class="primary"')}${s.audioOn&&s.audioKind==='voice'?btn('mute',s.muted?'Unmute':'Mute'):''}</div><label class="volume">Volume <input id="volume" type="range" min="0" max="100" value="${s.volume}"><span>${s.volume}%</span></label><p class="hint">${s.coexist?'Concurrency demo: audio stays on behind HID.':'Independent mode: leaving Audio stops it.'}</p>`;
  }
  function renderOverlay() {
    const o = $('overlay'); o.hidden = !s.locked && !s.menu;
    [$('view'), $('modeNav'), document.querySelector('.device-top')].forEach(el=>{ el.inert = !o.hidden; });
    if (s.locked) o.innerHTML = `<span class="lock-mark" aria-hidden="true">Ⅱ</span><h2>Input locked</h2><p>Keys released, recognition stopped and microphone muted. Unsent characters remain in preview.</p><p>Hold the AI button again to unlock.</p>`;
    else if (s.menu==='modes') o.innerHTML = `<h2>Choose a mode</h2><div class="choices">${Object.entries(labels).map(([k,v])=>btn(`mode:${k}`,v,`aria-pressed="${s.mode===k}"`)).join('')}</div>${btn('close','Back')}`;
    else if (s.menu==='target') o.innerHTML = `<h2>Choose a target</h2><p>One simulated HID target. Switching cancels current input.</p><div class="choices">${['iPhone','iPad Pro','Windows','Mac'].map(t=>btn(`target:${t}`,t,`aria-pressed="${s.target===t}"`)).join('')}</div>${btn('close','Back')}`;
    else if (s.menu==='settings') o.innerHTML = `<h2>Device settings</h2><p>Motion: click to toggle<br>Layout: US ANSI<br>Hold AI: lock all input<br>BOOT: unassigned pending hardware checks</p><p>${s.target==='iPhone'?'Enable AssistiveTouch on iPhone.':'Use an English input layout. Finish input before changing targets.'}</p><p>Spoken letters, Bluetooth audio and concurrency still need firmware implementation and device testing.</p>${btn('close','Back')}`;
  }
  function render() {
    $('view').innerHTML = s.mode==='mouse'?renderMouse():s.mode==='keyboard'?renderKeyboard():renderAudio();
    $('targetButton').textContent = `${s.target} · ${s.connected?'Connected':'Disconnected'}`;
    $('deviceStatus').textContent = `${s.remote?'Remote control':'Local control'} · ${s.mode==='keyboard'?'Preview first · no Enter':s.gyro?'Motion on':'Motion off'}${s.audioOn?` · ${s.audioKind==='speaker'?'Speaker':s.muted?'Mic muted':'Mic on'}`:''}`;
    document.querySelectorAll('[data-mode]').forEach(b=>b.setAttribute('aria-pressed',String(s.mode===b.dataset.mode)));
    $('keyHint').textContent = s.mode==='mouse'?'Click to toggle motion, then rotate your wrist.':s.mode==='keyboard'?'Click to confirm a local group, character or speech candidate.':'Click to mute/unmute. Speaker playback control is pending.';
    $('aiLong').textContent = s.locked?'Hold · unlock':'Hold · lock';
    $('connectionButton').textContent = s.connected?'Disconnect':'Reconnect';
    $('remoteButton').textContent = s.remote?'End remote control':'Remote takeover';
    renderOverlay();
  }
  function handle(action) {
    if (!guard()) return;
    const [name,value] = action.split(':');
    if (name==='mode') return setMode(value);
    if (name==='close') { s.menu=''; render(); return; }
    if (name==='target') { boundary({mute:true}); s.audioOn=false; s.target=value; s.menu=''; log(`Simulated switch to ${value}; unsent draft retained.`); render(); return; }
    if (s.menu) return;
    if (name==='gyro' && s.connected) { s.gyro=!s.gyro; log(s.gyro?'Motion enabled.':'Motion disabled.'); }
    else if (name==='fine') s.fine=Number(value);
    else if (['left','right','drag','scrollUp','scrollDown'].includes(name)) {
      if (!s.connected) { log('Target disconnected. Nothing sent.'); return; }
      if (name==='drag') s.drag=!s.drag;
      log(`Simulated mouse action: ${{left:'left click',right:'right click',drag:s.drag?'hold left button':'release left button',scrollUp:'Scroll up',scrollDown:'Scroll down'}[name]}. `);
    } else if (name==='sub') { boundary(); s.sub=value; s.group=-1; s.focus=0; }
    else if (name==='character') { s.focus=Number(value); confirm(); return; }
    else if (name==='confirm') { confirm(); return; }
    else if (name==='back') { s.group=-1; s.focus=0; }
    else if (name==='case') s.lower=!s.lower;
    else if (name==='symbols') { s.symbols=!s.symbols; s.group=-1; s.focus=0; }
    else if (name==='space') { addCharacter(' '); return; }
    else if (name==='delete'&&!s.sending) s.draft=s.draft.slice(0,-1);
    else if (name==='clear'&&!s.sending) s.draft='';
    else if (name==='send') { startSend(); return; }
    else if (name==='stop') { boundary(); log(`Sending stopped. Submitted "${s.sent}"; remaining characters stay in preview.`); }
    else if (name==='shortcut'&&s.connected) {
      const mod=['Mac','iPhone','iPad Pro'].includes(s.target)?'Command':'Ctrl';
      const key={copy:`${mod}+C`,paste:`${mod}+V`,undo:`${mod}+Z`,tab:'Tab',escape:'Esc',enter:'Enter'}[value];
      log(`Simulated shortcut ${key}; preview text is not sent.`);
    } else if (name==='voiceMode'||name==='speakerMode') { s.audioKind=name==='voiceMode'?'voice':'speaker'; s.audioOn=false; s.muted=true; }
    else if (name==='audioPower') { if (!s.connected) { log('Target is not connected.'); return; } s.audioOn=!s.audioOn; s.muted=true; log('Audio simulation changed; there is no real audio connection.'); }
    else if (name==='mute'&&s.audioOn) s.muted=!s.muted;
    render();
  }
  document.querySelector('.screen').addEventListener('click',e=>{
    const b=e.target.closest('button'); if (!b || b.disabled) return;
    if (b.dataset.mode) setMode(b.dataset.mode);
    else if (b.dataset.action) handle(b.dataset.action);
    else if (b.id==='targetButton'||b.id==='settingsButton') {
      if (!guard()) return; boundary(); s.menu=b.id==='targetButton'?'target':'settings'; render();
    }
  });
  $('aiShort').onclick=()=>{ if (!guard()||s.menu) return; if(s.mode==='mouse')handle('gyro'); else if(s.mode==='keyboard')confirm(); else if(s.audioKind==='voice')handle('mute'); else log('Speaker play/pause requires future AVRCP integration.'); };
  $('aiDouble').onclick=()=>{if(!guard())return;boundary();s.menu=s.menu?'':'modes';render();};
  $('aiLong').onclick=()=>{boundary({mute:true});s.locked=!s.locked;s.remote=false;s.menu='';log(s.locked?'All input locked.':'Unlocked with motion off and microphone muted. Old tasks stay stopped.');render();};
  $('connectionButton').onclick=()=>{boundary({mute:true});s.connected=!s.connected;s.audioOn=false;s.remote=false;log(s.connected?'Simulated reconnect; old input is not replayed.':'Simulated disconnect; input cancelled and microphone muted.');render();};
  $('remoteButton').onclick=()=>{if(s.locked||!s.connected)return;boundary({mute:true});s.remote=!s.remote;log(s.remote?'Remote simulation active. The next local touch or AI press only takes ownership.':'Remote control cancelled.');render();};
  $('coexistence').onchange=e=>{s.coexist=e.target.checked;if(!s.coexist&&s.mode!=='audio'){s.audioOn=false;s.muted=true;}log('Design capability changed; hardware concurrency is still unverified.');render();};
  document.querySelectorAll('[data-direction]').forEach(b=>b.onclick=()=>{
    if (!guard()||s.menu||touch||performance.now()<frozenUntil) return;
    const direction=b.dataset.direction;
    if(s.mode==='keyboard'&&s.sub==='motion'&&!s.sending){const d={up:-3,down:3,left:-1,right:1}[direction];s.focus=Math.max(0,Math.min(items().length-1,s.focus+d));render();}
    else if(s.mode==='mouse'&&s.gyro&&s.connected){move({left:-8,right:8}[direction]||0,{up:-8,down:8}[direction]||0);log('Simulated host pointer movement only.');}
  });
  function move(dx,dy){s.x=Math.max(4,Math.min(96,s.x+dx));s.y=Math.max(6,Math.min(94,s.y+dy));const p=document.querySelector('.pointer-dot');if(p){p.style.left=`${s.x}%`;p.style.top=`${s.y}%`;}}
  $('view').addEventListener('pointerdown',e=>{
    if(s.locked||s.menu)return;
    if(takeLocal())return;
    frozenUntil=performance.now()+150;
    const pad=e.target.closest('#touchpad');
    if(pad&&s.connected){touch={id:e.pointerId,x:e.clientX,y:e.clientY};pad.setPointerCapture(e.pointerId);}
  });
  $('view').addEventListener('pointermove',e=>{if(!touch||touch.id!==e.pointerId||s.locked||!s.connected)return;const pad=e.target.closest('#touchpad');if(!pad)return;const r=pad.getBoundingClientRect();move((e.clientX-touch.x)/r.width*100*s.fine,(e.clientY-touch.y)/r.height*100*s.fine);touch.x=e.clientX;touch.y=e.clientY;});
  const releaseTouch=()=>{touch=null;frozenUntil=performance.now()+150;};
  $('view').addEventListener('pointerup',releaseTouch);$('view').addEventListener('pointercancel',releaseTouch);
  $('view').addEventListener('change',e=>{if(e.target.id==='sampleWord'){s.word=e.target.value;s.candidate='';s.voice='idle';render();}if(e.target.id==='volume'){s.volume=Number(e.target.value);render();}});
  render();
})();
