// SPDX-License-Identifier: Apache-2.0
// Interaction demonstrator only. No network, Bluetooth or microphone access.
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const labels = { mouse: '鼠标', keyboard: '键盘', audio: '音频' };
  const groups = ['ABCDEF', 'GHIJKL', 'MNOPQR', 'STUVWX', 'YZ0123', '456789'];
  const symbolGroups = ['.,!?;:', '-_+=/*', '()[]{}', '@#$%&', '\\|<>', '"\'`~^'];
  const s = { mode: 'mouse', target: 'iPad Pro', connected: true, gyro: false,
    locked: false, remote: false, menu: '', fine: 1, drag: false, epoch: 1,
    x: 50, y: 50, sub: 'motion', group: -1, focus: 0, lower: false, symbols: false,
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
    log('已本地接管；本次动作被消费，远程任务不会自动恢复。'); render(); return true;
  }
  function guard() { return !s.locked && !takeLocal(); }
  function setMode(mode) {
    if (!guard()) return;
    if (s.mode !== mode) {
      boundary(); s.mode = mode; s.group = -1; s.focus = 0;
      if (!s.coexist && mode !== 'audio') s.audioOn = false;
      log(`进入${labels[mode]}模式；已释放按键并清除旧输入。`);
    }
    s.menu = ''; render();
  }
  function addCharacter(c) {
    if (s.sending) return;
    if (s.draft.length >= 64) { log('预览最多 64 个字符，请先发送或清理。'); return; }
    s.draft += s.lower ? c.toLowerCase() : c;
    s.group = -1; s.focus = 0; s.candidate = ''; s.voice = 'idle';
    log('字符已加入预览，尚未发送。'); render();
  }
  function confirm() {
    if (s.mode !== 'keyboard' || s.sending) return;
    if (s.sub === 'voice') {
      if (s.candidate) return addCharacter(s.candidate);
      if (s.voice === 'listening') {
        s.candidate = s.word[0]; s.voice = 'candidate';
        log(`演示候选 ${s.word} → ${s.candidate}，再确认一次加入预览。`);
      } else { s.voice = 'listening'; log('演示监听已开启。再次单击结束，生成所选样例候选；没有调用麦克风。'); }
      render(); return;
    }
    if (s.sub === 'shortcuts') { log('快捷键请触屏明确选择。'); return; }
    if (s.group < 0) { s.group = s.focus; s.focus = 0; render(); }
    else addCharacter(items()[s.focus]);
  }
  function startSend() {
    if (!s.connected || s.sending || !s.draft) return;
    s.sending = true; s.sent = ''; const epoch = s.epoch;
    function tick() {
      if (epoch !== s.epoch || !s.sending || !s.connected || s.locked) return;
      s.sent += s.draft[0]; s.draft = s.draft.slice(1);
      log(`模拟已提交「${s.sent}」；不含 Enter。真实输入框内容仍需确认。`);
      if (!s.draft) s.sending = false;
      render();
      if (s.sending) sendTimer = setTimeout(tick, 160);
    }
    render(); sendTimer = setTimeout(tick, 160);
  }
  function renderMouse() {
    return `<div class="row gyro-row"><div><h2>随手指向</h2><p class="hint">转动手腕 · 触屏精调</p></div>${btn('gyro', s.gyro?'体感 · 开启':'体感 · 关闭',`aria-pressed="${s.gyro}" ${!s.connected?'disabled':''}`)}</div>
      <div class="row fine"><span>触控灵敏度</span><div>${[1,.25,.125].map(v=>btn(`fine:${v}`,v===1?'1×':v===.25?'¼×':'⅛×',`aria-pressed="${s.fine===v}"`)).join('')}</div></div>
      <div class="touch-layout"><div id="touchpad" class="touchpad" aria-label="触控板，拖动以精调演示指针"><span class="pad-label">触控板</span><small>触摸时暂停体感</small><i class="pointer-dot" style="left:${s.x}%;top:${s.y}%"></i></div><div class="scroll-pad">${btn('scrollUp','↑','aria-label="向上滚动"')}<span>滚动</span>${btn('scrollDown','↓','aria-label="向下滚动"')}</div></div>
      <div class="toolbar">${btn('left','左键')}${btn('drag',s.drag?'释放拖动':'拖动',`aria-pressed="${s.drag}"`)}${btn('right','右键')}</div>`;
  }
  function renderKeyboard() {
    const subs = {motion:'体感',touch:'触屏',voice:'语音字母',shortcuts:'快捷键'};
    let middle;
    if (s.sub === 'voice') middle = `<div class="voice-box"><span class="candidate">${escape(s.candidate || (s.voice==='listening'?'…':'A–Z'))}</span><div class="voice-description"><label class="hint" for="sampleWord">演示口令 · 无真实录音</label><select id="sampleWord">${['Alpha','Bravo','Charlie','Delta','Echo','Foxtrot','Zulu'].map(w=>`<option ${s.word===w?'selected':''}>${w}</option>`).join('')}</select>${btn('confirm',s.candidate?'确认候选':s.voice==='listening'?'结束并看候选':'开始演示监听')}</div></div>`;
    else if (s.sub === 'shortcuts') middle = `<div class="character-grid">${['copy','paste','undo','tab','escape','enter'].map((v,i)=>btn(`shortcut:${v}`,['复制','粘贴','撤销','Tab','Esc','Enter'][i])).join('')}</div>`;
    else middle = `<div class="row char-toolbar">${btn('back',s.group<0?'选择字母组':'← 返回组')}${btn('case',s.lower?'abc':'ABC')}${btn('symbols',s.symbols?'字母 / 数字':'符号')}</div><div class="character-grid">${items().map((c,i)=>btn(`character:${i}`,escape(s.lower?c.toLowerCase():c),`${s.sub==='motion'&&i===s.focus?'class="selected"':''} ${s.sending?'disabled':''}`)).join('')}</div>`;
    return `<div class="subnav">${Object.entries(subs).map(([k,v])=>btn(`sub:${k}`,v,`aria-pressed="${s.sub===k}"`)).join('')}</div><div class="preview"><output aria-label="待发送字符">${escape(s.draft)||'等待输入…'}</output><small>${s.sending?'发送中':`${s.draft.length} / 64`}</small>${btn('clear','清空',s.sending?'disabled':'')}</div>${middle}<div class="keyboard-footer">${btn('delete','删除',s.sending?'disabled':'')}${btn('space','空格',s.sending?'disabled':'')}${btn(s.sending?'stop':'send',s.sending?'停止发送':'发送',`class="primary" ${!s.connected||(!s.draft&&!s.sending)?'disabled':''}`)}</div>`;
  }
  function renderAudio() {
    return `<div class="row"><h2>麦克风 / 音响</h2><span class="warning">链路待实测</span></div><div class="toolbar">${btn('voiceMode','语音 / 会议',`aria-pressed="${s.audioKind==='voice'}"`)}${btn('speakerMode','音响',`aria-pressed="${s.audioKind==='speaker'}"`)}</div><div class="audio-hero"><h3>${!s.audioOn?'音频未开启':s.audioKind==='speaker'?'音响演示中':s.muted?'麦克风已静音':'麦克风演示中'}</h3><p class="hint">${s.audioKind==='voice'?'HFP · 双向语音':'A2DP · 音乐播放，无麦克风上传'}</p>${btn('audioPower',s.audioOn?'关闭音频':'演示开启音频','class="primary"')}${s.audioOn&&s.audioKind==='voice'?btn('mute',s.muted?'取消静音':'静音麦克风'):''}</div><label class="volume">音量 <input id="volume" type="range" min="0" max="100" value="${s.volume}"><span>${s.volume}%</span></label><p class="hint">${s.coexist?'并行设计演示：切回键鼠可保留音频。':'独立模式：切回键鼠后关闭音频。'}</p>`;
  }
  function renderOverlay() {
    const o = $('overlay'); o.hidden = !s.locked && !s.menu;
    [$('view'), $('modeNav'), document.querySelector('.device-top')].forEach(el=>{ el.inert = !o.hidden; });
    if (s.locked) o.innerHTML = `<span class="lock-mark" aria-hidden="true">Ⅱ</span><h2>输入已锁定</h2><p>按键已释放，语音已停止，麦克风已静音。未发送的字符保留在预览中。</p><p>再次长按实体 AI 键解锁。</p>`;
    else if (s.menu==='modes') o.innerHTML = `<h2>选择主模式</h2><div class="choices">${Object.entries(labels).map(([k,v])=>btn(`mode:${k}`,v,`aria-pressed="${s.mode===k}"`)).join('')}</div>${btn('close','返回')}`;
    else if (s.menu==='target') o.innerHTML = `<h2>选择目标</h2><p>这里演示一个 HID 目标，切换会中断当前输入。</p><div class="choices">${['iPhone','iPad Pro','Windows','Mac'].map(t=>btn(`target:${t}`,t,`aria-pressed="${s.target===t}"`)).join('')}</div>${btn('close','返回')}`;
    else if (s.menu==='settings') o.innerHTML = `<h2>设备设置</h2><p>体感：点击切换开关<br>键盘布局：US ANSI<br>长按 AI：全局锁定<br>BOOT：待硬件确认后分配</p><p>${s.target==='iPhone'?'iPhone 需开启 AssistiveTouch。':'请使用英文输入法；切换目标前先结束输入。'}</p><p>语音字母、蓝牙音频和并行能力均待固件实现与实测。</p>${btn('close','返回')}`;
  }
  function render() {
    $('view').innerHTML = s.mode==='mouse'?renderMouse():s.mode==='keyboard'?renderKeyboard():renderAudio();
    $('targetButton').textContent = `${s.target} · ${s.connected?'已连接':'已断线'}`;
    $('deviceStatus').textContent = `${s.remote?'远程控制':'本地控制'} · ${s.mode==='keyboard'?'预览后发送 · 不附带回车':s.gyro?'体感跟随开启':'体感关闭'}${s.audioOn?` · ${s.audioKind==='speaker'?'音响':s.muted?'麦克风静音':'麦克风开启'}`:''}`;
    document.querySelectorAll('[data-mode]').forEach(b=>b.setAttribute('aria-pressed',String(s.mode===b.dataset.mode)));
    $('keyHint').textContent = s.mode==='mouse'?'单击切换体感，开启后跟随手腕。':s.mode==='keyboard'?'单击确认本机选组、字符或语音候选。':'单击切换麦克风静音；音响播放控制待适配。';
    $('aiLong').textContent = s.locked?'长按 · 解锁':'长按 · 锁定';
    $('connectionButton').textContent = s.connected?'模拟断线':'模拟重连';
    $('remoteButton').textContent = s.remote?'取消远程控制':'模拟远程接管';
    renderOverlay();
  }
  function handle(action) {
    if (!guard()) return;
    const [name,value] = action.split(':');
    if (name==='mode') return setMode(value);
    if (name==='close') { s.menu=''; render(); return; }
    if (name==='target') { boundary({mute:true}); s.audioOn=false; s.target=value; s.menu=''; log(`模拟切换到 ${value}，未发送草稿保留。`); render(); return; }
    if (s.menu) return;
    if (name==='gyro' && s.connected) { s.gyro=!s.gyro; log(s.gyro?'体感已开启。':'体感已关闭。'); }
    else if (name==='fine') s.fine=Number(value);
    else if (['left','right','drag','scrollUp','scrollDown'].includes(name)) {
      if (!s.connected) { log('目标已断线，没有发送。'); return; }
      if (name==='drag') s.drag=!s.drag;
      log(`模拟鼠标动作：${{left:'左键点击',right:'右键点击',drag:s.drag?'按住左键拖动':'释放左键',scrollUp:'向上滚动',scrollDown:'向下滚动'}[name]}。`);
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
    else if (name==='stop') { boundary(); log(`发送已停止。已提交「${s.sent}」；预览保留剩余字符。`); }
    else if (name==='shortcut'&&s.connected) {
      const mod=['Mac','iPhone','iPad Pro'].includes(s.target)?'Command':'Ctrl';
      const key={copy:`${mod}+C`,paste:`${mod}+V`,undo:`${mod}+Z`,tab:'Tab',escape:'Esc',enter:'Enter'}[value];
      log(`模拟发送独立快捷键 ${key}；不发送预览内容。`);
    } else if (name==='voiceMode'||name==='speakerMode') { s.audioKind=name==='voiceMode'?'voice':'speaker'; s.audioOn=false; s.muted=true; }
    else if (name==='audioPower') { if (!s.connected) { log('目标未连接。'); return; } s.audioOn=!s.audioOn; s.muted=true; log('仅改变音频设计演示状态；没有真实音频链路。'); }
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
  $('aiShort').onclick=()=>{ if (!guard()||s.menu) return; if(s.mode==='mouse')handle('gyro'); else if(s.mode==='keyboard')confirm(); else if(s.audioKind==='voice')handle('mute'); else log('音响播放 / 暂停需要后续 AVRCP 适配。'); };
  $('aiDouble').onclick=()=>{if(!guard())return;boundary();s.menu=s.menu?'':'modes';render();};
  $('aiLong').onclick=()=>{boundary({mute:true});s.locked=!s.locked;s.remote=false;s.menu='';log(s.locked?'已全局锁定。':'已解锁；体感关闭、麦克风静音，不恢复旧任务。');render();};
  $('connectionButton').onclick=()=>{boundary({mute:true});s.connected=!s.connected;s.audioOn=false;s.remote=false;log(s.connected?'模拟重连完成，不重放旧输入。':'模拟断线，已取消输入并静音。');render();};
  $('remoteButton').onclick=()=>{if(s.locked||!s.connected)return;boundary({mute:true});s.remote=!s.remote;log(s.remote?'模拟远程控制中：下一次本地触摸或按 AI 只完成接管。':'已取消远程控制。');render();};
  $('coexistence').onchange=e=>{s.coexist=e.target.checked;if(!s.coexist&&s.mode!=='audio'){s.audioOn=false;s.muted=true;}log('已更改设计演示能力；未证明硬件支持并行。');render();};
  document.querySelectorAll('[data-direction]').forEach(b=>b.onclick=()=>{
    if (!guard()||s.menu||touch||performance.now()<frozenUntil) return;
    const direction=b.dataset.direction;
    if(s.mode==='keyboard'&&s.sub==='motion'&&!s.sending){const d={up:-3,down:3,left:-1,right:1}[direction];s.focus=Math.max(0,Math.min(items().length-1,s.focus+d));render();}
    else if(s.mode==='mouse'&&s.gyro&&s.connected){move({left:-8,right:8}[direction]||0,{up:-8,down:8}[direction]||0);log('仅模拟目标指针移动。');}
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
