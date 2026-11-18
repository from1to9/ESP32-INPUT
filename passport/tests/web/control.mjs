// Chromium renders production assets; a bounded HTTP fixture substitutes the
// board and HID host. This does not qualify Safari, radio timing or a device.
import assert from 'node:assert/strict';
import http from 'node:http';
import {readFile,mkdir} from 'node:fs/promises';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const require=createRequire(import.meta.url), {chromium}=require(process.env.PASSPORT_PLAYWRIGHT || 'playwright');
const root=fileURLToPath(new URL('../../..',import.meta.url));
const out=process.env.PASSPORT_CONTROL_OUTPUT || path.join(root,'.agent/local/passport-web/browser');
await mkdir(out,{recursive:true});
const assets=new Map(await Promise.all(['html','css','js'].map(async ext=>[ext,await readFile(path.join(root,`passport/main/web/controller.${ext}`))])));
const catalog=JSON.parse(await readFile(path.join(root,'api/control-v1.json'))).operations;
const supported=new Set(['control.acquire','control.renew','control.release','mouse.move_relative','mouse.click','mouse.scroll','mouse.drag.begin','mouse.drag.refresh','mouse.drag.end','keyboard.text.enqueue','keyboard.key_tap','keyboard.shortcut','mode.select','input.release_all','input.cancel','bluetooth.pairing.open','bluetooth.pairing.close','bluetooth.pairing.cancel_handshake','bluetooth.target.request','bluetooth.disconnect','wifi.scan.start','wifi.scan.cancel','display_name.list','display_name.set','display_name.clear']);
const session={session_token:'a'.repeat(64),session_id:'b'.repeat(32),boot_id:'c'.repeat(32),request_prefix:'d'.repeat(16),csrf:'e'.repeat(32),next_seq:1,scopes:['view','input.control','connections.manage','wifi.configure']};
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let afterMode, beginReply, touchTiming;
let state,commands,results,seq,closed,delayState,delayRenew,failSession,failRenew,failCommand,label,locked,pendingManagement;
const host='10:20:30:40:50:60/0', bond='01'.padEnd(32,'0'), ap='12'.repeat(32);
const hx=s=>Buffer.from(s).toString('hex');
function reset(){afterMode=beginReply=touchTiming=null;state={boot_id:session.boot_id,now_ms:'10000',epoch:'2',transport_generation:'3',state_sequence:'5',ui_revision:'2',management_generation:'1',target_id:host,mode:'mouse',locked:false,release_pending:false,input_ready:true,remote_allowed:true,remote_owner:false,lease_active:false,lease_id:'f'.repeat(32),lease_expires_ms:'13000'};commands=[];results=new Map();seq=1;closed=0;delayState=delayRenew=0;failSession=failRenew=failCommand=false;label='Studio laptop';locked=false;pendingManagement=false;}
reset();
const server=http.createServer(async(req,res)=>{
  res.setHeader('Cache-Control','no-store');res.setHeader('Content-Security-Policy',"default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
  res.setHeader('Referrer-Policy','no-referrer');
  const url=new URL(req.url,'http://fixture');const send=(x,status=200)=>{res.writeHead(status,{'Content-Type':'application/json'});res.end(JSON.stringify(x));};
  if(url.pathname.startsWith('/api/')&&req.method!=='GET'&&req.headers['user-agent']?.includes('Chrome')){
    assert.equal(req.headers['sec-fetch-mode'],'cors');assert.equal(req.headers.origin,origin);
  }
  if(url.pathname==='/api/v1/open-session'){assert.equal(req.headers['x-passport-open'],'1');send(failSession?{error:'busy'}:{...session,next_seq:seq},failSession?503:200);return;}
  if(url.pathname.startsWith('/api/')){
    assert.equal(req.headers.authorization,`Bearer ${session.session_token}`);
    if(req.method!=='GET')assert.equal(req.headers['x-csrf-token'],session.csrf);
    if(url.pathname==='/api/v1/state'){
      if(delayState)await pause(delayState);
      const timing=touchTiming, currentState=state;
      if(timing){
        await pause(45);
        currentState.now_ms=String(Math.floor(performance.now()));
        timing.trace.push('state');
      }
      const protocol={...currentState,locked};
      if(timing)await pause(35);
      send({protocol});return;
    }
    if(url.pathname==='/api/v1/capabilities'){send({access_policy:'open_network'});return;}
    if(url.pathname==='/api/v1/operations'){const i=Number(url.searchParams.get('cursor'));send({items:catalog.slice(i,i+8).map(x=>({name:x.name,available:supported.has(x.name)}))});return;}
    if(req.method==='DELETE'){closed++;state.lease_active=false;send({});return;}
    if(url.pathname==='/api/v1/commands'){
      let body='';for await(const chunk of req)body+=chunk;const c=JSON.parse(body);commands.push(c);
      assert.equal(c.seq,seq,JSON.stringify(commands.map(({seq,op})=>[seq,op])));assert.equal(c.session_id,session.session_id);assert.equal(c.boot_id,session.boot_id);assert(supported.has(c.op));
      assert.equal(c.request_id,session.request_prefix+BigInt(seq).toString(16).padStart(16,'0'));seq++;
      if(c.op==='control.renew'&&delayRenew)await new Promise(r=>setTimeout(r,delayRenew));
      if(c.op==='control.renew'&&failRenew){send({state:'failed',error:'not_ready',next_seq:seq},503);return;}
      if(failCommand) {
        res.writeHead(200,{'Content-Type':'application/json','Content-Length':100});
        res.write('{"state":',()=>res.destroy());return;
      }
      let reason='';
      if(c.op==='control.acquire'){assert.equal(c.args.producer,'script');state.lease_active=true;state.remote_owner=true;}
      if(c.op==='control.release'){state.lease_active=false;state.remote_owner=false;}
      if(c.op==='mode.select') {
        assert.equal(c.lease_id,state.lease_id);
        state.mode=c.args.mode; state.epoch=String(BigInt(state.epoch)+1n);
        afterMode?.();
      }
      if (/^(mouse|keyboard)\./.test(c.op)) {
        assert.equal(c.op.split('.')[0],state.mode);
        assert.equal(c.target_id,state.target_id);assert.equal(c.transport_generation,state.transport_generation);
        assert.equal(c.epoch,state.epoch);assert.equal(c.lease_id,state.lease_id);
        assert(!state.release_pending && state.lease_active);
      }
      if(touchTiming && ['control.acquire','control.renew'].includes(c.op)){
        state.lease_expires_ms=String(Math.floor(performance.now())+3000);
      }
      if(touchTiming && c.op==='mouse.move_relative'){
        const timing=touchTiming;
        const first=!timing.moves.length;
        timing.trace.push('move');
        await pause(first?180:35);
        const slack=Number(c.deadline_ms)-performance.now();
        const expired=slack<=0;
        const receipt={state:expired?'rejected':'sent',error:expired?'expired':'ok',next_seq:seq,command_key:String(seq)};
        timing.moves.push({seq:c.seq,...c.args,slack:Math.floor(slack),state:receipt.state});
        results.set(receipt.command_key,receipt);
        if(expired)send(receipt,400);
        else send({...receipt,state:'accepted'},202);
        return;
      }
      if(c.op==='mouse.move_relative'||c.op==='mouse.scroll')assert(BigInt(c.deadline_ms)-BigInt(state.now_ms)<=250n);
      if(c.op==='display_name.set'){label=c.args.label;reason='label_write_confirmed';}
      if(c.op==='display_name.clear'){label='';reason='label_write_confirmed';}
      const r={state:pendingManagement&&c.op==='bluetooth.pairing.open'?'accepted':'applied',error:'ok',reason,command_key:String(seq),next_seq:seq,uncertain_index:-1};results.set(String(seq),r);
      if(c.op==='mouse.drag.begin'&&beginReply)await beginReply;
      send(r);return;
    }
    if(url.pathname.startsWith('/api/v1/requests/')){send(results.get(url.pathname.split('/').at(-1))||{state:'failed',error:'result_unknown'});return;}
    if(url.pathname==='/api/v1/bluetooth/peers'){send({known:true,revision:'7',generation:'3',connected:true,ready:true,pairing:false,handshake:false,window_seconds:0,message:'Connected and ready',peers:[{id:bond,original:host,label,label_known:true,labelable:true,current:true},{id:'02'.padEnd(32,'0'),original:'10:20:30:40:50:61/0',label:'Studio laptop',label_known:true,labelable:true,current:false}]});return;}
    if(url.pathname==='/api/v1/wifi/profiles'){send({known:true,revision:'4',sta_ip:'192.0.2.1',ap_ip:'',ap_active:false,message:'LAN connected',profiles:[{id:'00000001',ssid_hex:hx('Meridian Studio'),label:'Home office',label_known:true,current:true}]});return;}
    if(url.pathname==='/api/v1/wifi/scans/current'){send({known:true,generation:9,next_cursor:0,scanning:false,records:[{id:ap,index:0,ssid_hex:hx('<img src=x onerror=alert(1)>'),bssid:'001122334455',rssi:-53,security:1,label:'Workshop',label_known:true,labelable:true,saved_matches:0}]});return;}
    if(url.pathname==='/api/v1/display-names'){send({known:true,revision:'8589934595',count:1,next_cursor:0,labels:label?[{kind:'hid_bond',id:bond,label}]:[]});return;}
    send({error:'not_found'},404);return;
  }
  const ext=url.pathname==='/'?'html':url.pathname==='/controller.css'?'css':url.pathname==='/controller.js'?'js':null;
  if(!ext){res.writeHead(404);res.end();return;}res.setHeader('Content-Type',{html:'text/html',css:'text/css',js:'text/javascript'}[ext]);res.end(assets.get(ext));
});
await new Promise(r=>server.listen(0,'127.0.0.1',r));
const browser=await chromium.launch({headless:true,executablePath:process.env.PASSPORT_CHROMIUM});
const origin=`http://127.0.0.1:${server.address().port}`;const errors=[];
async function open(context){const p=await context.newPage();p.on('pageerror',e=>errors.push(e.message));await p.goto(origin);return p;}
async function connect(p){await p.locator('#connect').click();await p.locator('#acquire').waitFor({state:'visible'});await p.waitForFunction(()=>!document.getElementById('acquire').disabled);}
async function acquire(p){await p.locator('#acquire').click();await p.waitForFunction(()=>!document.getElementById('mouse-controls').disabled||!document.getElementById('keyboard-controls').disabled);}
async function bounded(p) {
  const issues=await p.evaluate(()=>{
    const found=[];
    if(document.documentElement.scrollWidth>innerWidth) found.push('page overflow');
    for(const n of document.querySelectorAll('button,select,input,summary,a[href]')) {
      const r=n.getBoundingClientRect();
      if(!r.width||!r.height||n.closest('details:not([open])') && n.tagName!=='SUMMARY')continue;
      const dense=n.closest('.key-row') && innerWidth<=760;
      if(r.height<44 || r.width<(dense?24:44) || r.right>innerWidth+1 || r.left<0)found.push(n.id||n.textContent);
      if(n.parentElement.classList.contains('key-row') && n.nextElementSibling &&
          r.right>n.nextElementSibling.getBoundingClientRect().left) found.push('overlapping keys');
    }
    return found;
  });
  assert.deepEqual(issues,[]);
}
// Use trusted touch events and a running device clock. The first command
// misses admission. Later movements must reach terminal sent on the same lease.
async function delayedTouch(){
  reset();
  const timing=touchTiming={trace:[],moves:[]};
  const context=await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true});
  try{
    const page=await open(context);await connect(page);await acquire(page);
    const pad=page.locator('#pad');await pad.scrollIntoViewIfNeeded();
    const box=await pad.boundingBox(),x=Math.round(box.x+box.width/2),y=Math.round(box.y+box.height/2);
    const cdp=await context.newCDPSession(page);
    const touch=(type,dx=0,dy=0)=>cdp.send('Input.dispatchTouchEvent',{type,
      touchPoints:type==='touchEnd'?[]:[{x:x+dx,y:y+dy,id:1}]});
    await touch('touchStart');
    const firstPost=page.waitForRequest(req=>req.url().endsWith('/commands')&&req.postDataJSON().op==='mouse.move_relative');
    await touch('touchMove',24);await firstPost;
    // This delta arrives during the slow POST and is stale by readback.
    await touch('touchMove',24,12);
    await page.waitForFunction(()=>document.getElementById('result').textContent.includes('expired'));
    assert.equal(await page.locator('#connection').textContent(),'This browser is controlling','expired sample lost control');
    for(let i=1;i<=48;i++){
      await touch('touchMove',24-i,12);
      await pause(18);
    }
    await touch('touchEnd');await pause(330);
    const sent=timing.moves.filter(m=>m.state==='sent');
    assert.equal(timing.moves[0].state,'rejected');
    assert.equal(timing.moves[0].dx,24);
    assert(sent.length>=2,JSON.stringify(timing.moves));
    assert(timing.moves.slice(1).every(m=>m.dx<0 && m.dy===0),JSON.stringify(timing.moves));
    assert.equal(commands.filter(c=>c.op==='control.acquire').length,1);
    assert.equal(await page.locator('#connection').textContent(),'This browser is controlling');
    assert(timing.trace.slice(timing.trace.indexOf('move'),timing.trace.indexOf('move',timing.trace.indexOf('move')+1)).includes('state'));
    assert.match(await page.locator('#result').textContent(),/Sent to the HID transport/);
    console.log('PASS delayed touch',JSON.stringify(timing.moves));
    await page.locator('#stop').click();
    await page.waitForFunction(()=>document.getElementById('connection').textContent!=='This browser is controlling');
  }finally{await context.close();touchTiming=null;}
}
try{
  await delayedTouch();
  if(!process.argv.includes('--behavior-only')) for(const [name,width,height,mobile] of [['desktop',1440,1060,false],['iphone',390,844,true],['ipad',1024,1366,true]])for(const theme of ['dark','light']){
    reset();const ctx=await browser.newContext({viewport:{width,height},colorScheme:theme,isMobile:mobile,hasTouch:mobile,deviceScaleFactor:1});const p=await open(ctx);
    assert.equal(await p.locator('html').getAttribute('data-theme'),theme);await connect(p);await acquire(p);await bounded(p);
    assert(await p.locator('#keyboard-controls').isVisible());
    assert(await p.locator('#mouse-controls').isVisible());
    for (const key of await p.locator('#keycaps button').all()) await key.click({trial:true});
    await p.locator('.topbar').scrollIntoViewIfNeeded();
    await p.screenshot({path:path.join(out,`${name}-${theme}-input.png`),fullPage:true});
    await p.locator('#tab-connections').click();await p.getByText('Your connections',{exact:true}).waitFor();await p.locator('#ble-peers .device').first().waitFor();await bounded(p);
    assert.equal(await p.locator('#wifi-scan img').count(),0);
    await p.screenshot({path:path.join(out,`${name}-${theme}-connections.png`),fullPage:true});await ctx.close();
  }
  for(const failure of ['target','lease','generation','release','response']){
    reset();const context=await browser.newContext();const page=await open(context);await connect(page);await acquire(page);
    afterMode=()=>{
      if(failure==='target')state.target_id='replacement';
      if(failure==='lease')state.lease_id='9'.repeat(32);
      if(failure==='generation')state.transport_generation='4';
      if(failure==='release')state.release_pending=true;
    };
    if(failure==='response')failCommand=true;
    await page.getByRole('button',{name:'Enter',exact:true}).click();
    await page.waitForFunction(()=>document.getElementById('acquire').hidden===false);
    assert.equal(commands.filter(c=>c.op==='mode.select').length,1);
    assert(!commands.some(c=>c.op.startsWith('keyboard.')));
    if(failure==='response')assert.match(await page.locator('#result').textContent(),/unknown/i);
    await context.close();
  }
  // A physical hold is tied to its initiating key, including during mode preparation.
  reset();state.mode='keyboard';
  const holdContext=await browser.newContext({viewport:{width:390,height:844},hasTouch:true,isMobile:true});
  const heldPage=await open(holdContext);await connect(heldPage);await acquire(heldPage);
  afterMode=()=>{state.release_pending=true;};
  const hold=heldPage.locator('#drag');
  await hold.focus();await heldPage.keyboard.down('Space');
  await heldPage.waitForFunction(()=>document.getElementById('keyboard-controls').disabled);
  await heldPage.keyboard.up('Space');state.release_pending=false;
  await heldPage.waitForFunction(()=>!document.getElementById('send').disabled);
  await heldPage.waitForTimeout(480);
  assert(!commands.some(c=>c.op.startsWith('mouse.drag.')));assert.equal(closed,0);
  // Tab transfers focus while the initiating key remains down.
  await hold.focus();await heldPage.keyboard.down('Space');
  await heldPage.waitForFunction(()=>document.getElementById('drag').getAttribute('aria-pressed')==='true');
  await heldPage.keyboard.press('Enter');
  assert.equal(await hold.getAttribute('aria-pressed'),'true');
  await heldPage.keyboard.press('Tab');await heldPage.keyboard.up('Space');
  await heldPage.waitForFunction(()=>document.getElementById('drag').getAttribute('aria-pressed')==='false');
  await heldPage.waitForTimeout(650);
  assert.deepEqual(commands.filter(c=>c.op.startsWith('mouse.drag.')).map(c=>c.op),['mouse.drag.begin','mouse.drag.end']);
  // Check the stop control and the scrolled editing control are both reachable.
  for(const selector of ['#text','[data-shortcut=paste]']){
    const field=heldPage.locator(selector);
    await field.evaluate(el=>el.scrollIntoView({block:'center'}));
    const reachable=await heldPage.evaluate(sel=>['#stop',sel].every(id=>{
      const n=document.querySelector(id),r=n.getBoundingClientRect();
      return r.top>=0 && r.bottom<=innerHeight && n.contains(document.elementFromPoint(r.x+r.width/2,r.y+r.height/2));
    }),selector);
    assert(reachable,selector);
  }
  await heldPage.screenshot({path:path.join(out,'iphone-stop-scrolled.png')});
  await hold.scrollIntoViewIfNeeded();const bounds=await hold.boundingBox();
  await heldPage.mouse.move(bounds.x+bounds.width/2,bounds.y+bounds.height/2);await heldPage.mouse.down();
  await heldPage.waitForFunction(()=>document.getElementById('drag').getAttribute('aria-pressed')==='true');
  delayRenew=450;
  const renewedHold=heldPage.waitForResponse(r=>r.url().endsWith('/commands')&&r.request().postDataJSON()?.op==='control.renew');
  await heldPage.waitForRequest(r=>r.url().endsWith('/commands')&&r.postDataJSON()?.op==='control.renew');
  const during=commands.length;
  await heldPage.getByRole('button',{name:'Enter',exact:true}).press('Enter');
  assert.match(await heldPage.locator('#result').textContent(),/Wait for the current action/);
  assert.equal(await hold.getAttribute('aria-pressed'),'true');assert.equal(closed,0);
  await renewedHold;delayRenew=0;
  assert(!commands.slice(during).some(c=>['mode.select','keyboard.key_tap','mouse.drag.end'].includes(c.op)));
  await heldPage.mouse.up();await holdContext.close();
  // A release after the begin POST but before its reply must close the session.
  reset();const pendingContext=await browser.newContext();const pendingPage=await open(pendingContext);
  await connect(pendingPage);await acquire(pendingPage);
  let reply;beginReply=new Promise(resolve=>{reply=resolve;});
  const beginPost=pendingPage.waitForRequest(r=>r.url().endsWith('/commands')&&r.postDataJSON()?.op==='mouse.drag.begin');
  await pendingPage.locator('#drag').focus();await pendingPage.keyboard.down('Enter');await beginPost;
  const sessionClose=pendingPage.waitForResponse(r=>r.request().method()==='DELETE');
  await pendingPage.keyboard.up('Enter');await sessionClose;reply();
  await pendingPage.waitForTimeout(450);
  assert.equal(closed,1);
  assert.deepEqual(commands.filter(c=>c.op.startsWith('mouse.drag.')).map(c=>c.op),['mouse.drag.begin']);
  await pendingContext.close();
  // A slow renewal must retain the command lane without flashing controls.
  reset();const renewalContext=await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true});
  const renewalPage=await open(renewalContext);await connect(renewalPage);await acquire(renewalPage);
  await renewalPage.evaluate(()=>{
    window.disabledDuringRenewal=[];
    const controls=[...document.querySelectorAll('[data-op],#send,#scan,#pair-open,#refresh-lists')];
    new MutationObserver(()=>{for(const node of controls)if(node.disabled)window.disabledDuringRenewal.push(node.id||node.dataset.op);})
      .observe(document.querySelector('main'),{subtree:true,attributes:true,attributeFilter:['disabled']});
  });
  delayRenew=350;
  const renewed=renewalPage.waitForResponse(r=>r.url().endsWith('/commands')&&r.request().postDataJSON()?.op==='control.renew');
  await renewalPage.waitForRequest(r=>r.url().endsWith('/commands')&&r.postDataJSON()?.op==='control.renew');
  const beforeOverlap=commands.filter(c=>c.op!=='control.renew').length;
  await renewalPage.locator('[data-click=left]').click();
  assert.match(await renewalPage.locator('#result').textContent(),/Wait for the current action/);
  await renewed;
  assert.equal(commands.filter(c=>c.op!=='control.renew').length,beforeOverlap);
  assert(commands.some(c=>c.op==='control.renew'));assert.equal(await renewalPage.locator('#mouse-controls').isDisabled(),false);
  assert.deepEqual(await renewalPage.evaluate(()=>window.disabledDuringRenewal),[]);
  failRenew=true;
  await renewalPage.waitForFunction(()=>document.getElementById('mouse-controls').disabled);
  assert.match(await renewalPage.locator('#result').textContent(),/not_ready/);
  await renewalContext.close();
  // Failed Connect feedback stays beside its action on a narrow screen.
  reset();failSession=true;
  const failedContext=await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true});
  const failedPage=await open(failedContext);await failedPage.locator('#connect').click();
  await failedPage.waitForFunction(()=>document.getElementById('target').textContent.includes('Session unavailable'));
  const feedback=await failedPage.locator('#target').boundingBox();assert(feedback.y>=0&&feedback.y+feedback.height<=844);
  await bounded(failedPage);await failedPage.screenshot({path:path.join(out,'iphone-connect-failed.png'),fullPage:true});
  await failedContext.close();
  reset();const ctx=await browser.newContext({viewport:{width:390,height:844},hasTouch:true,isMobile:true,colorScheme:'light'});const p=await open(ctx);
  await p.locator('#theme').selectOption('dark');await p.reload();assert.equal(await p.locator('html').getAttribute('data-theme'),'dark');await connect(p);await acquire(p);
  await p.locator('[data-click=middle]').click();await p.waitForTimeout(180);assert(commands.some(c=>c.op==='mouse.click'&&c.args.button==='middle'));
  await p.locator('[data-wheel="-3"]').click();await p.waitForTimeout(180);assert(commands.some(c=>c.op==='mouse.scroll'&&c.args.wheel===-3));
  for(const [deltaX,deltaY,wheel,horizontal] of [[60,0,0,3],[-60,0,0,-3],[0,60,-3,0],[0,-60,3,0],[0,0,0,0]]){
    const beforeWheel=commands.length;
    await p.locator('#pad').dispatchEvent('wheel',{deltaX,deltaY});await p.waitForTimeout(180);
    const scrolls=commands.slice(beforeWheel).filter(c=>c.op==='mouse.scroll');
    assert.deepEqual(scrolls.map(c=>c.args),wheel||horizontal?[{wheel,horizontal}]:[]);
  }
  const beforeSwitch=commands.length;
  // A real mode change advances the epoch and may still owe neutral reports.
  afterMode=()=>{state.release_pending=true;setTimeout(()=>{state.release_pending=false;},70);};
  await p.locator('#keycaps').getByRole('button',{name:'Shift',exact:true}).click();
  await p.locator('#keycaps').getByRole('button',{name:'Type Q',exact:true}).click();
  await p.waitForFunction(()=>!document.getElementById('send').disabled && document.getElementById('result').textContent.includes('applied'));
  const switched=commands.slice(beforeSwitch).filter(c=>c.op!=='control.renew');
  assert.deepEqual(switched.map(c=>c.op),['mode.select','keyboard.key_tap']);
  assert.deepEqual(switched[1].args,{usage:20,modifiers:2});
  assert.equal(await p.getByRole('button',{name:'Shift',exact:true}).getAttribute('aria-pressed'),'false');
  afterMode=null;
  // Exercise the rendered keycaps and their HID mappings on the phone viewport.
  await p.getByText('Symbols & navigation',{exact:true}).click();
  await bounded(p);
  const printable="abcdefghijklmnopqrstuvwxyz1234567890-=[]\\;'`,./";
  const usages=[...Array.from({length:26},(_,i)=>4+i),...Array.from({length:10},(_,i)=>30+i),45,46,47,48,49,51,52,53,54,55,56];
  for(const [index,char] of [...printable].entries()){
    const receipt=p.waitForResponse(r=>r.url().endsWith('/commands')&&r.request().postDataJSON()?.op==='keyboard.key_tap');
    await p.getByRole('button',{name:'Type '+char,exact:true}).click();
    const request=(await receipt).request().postDataJSON();
    assert.deepEqual(request.args,{usage:usages[index],modifiers:0});
  }
  await p.getByText('Symbols & navigation',{exact:true}).click();
  await p.locator('#shortcut-modifier').selectOption('8');await p.locator('[data-shortcut=copy]').click();await p.waitForTimeout(150);
  assert(commands.some(c=>c.op==='keyboard.key_tap'&&c.args.usage===6&&c.args.modifiers===8));
  await p.locator('#shortcut-modifier').selectOption('1');await p.locator('[data-shortcut=paste]').click();await p.waitForTimeout(150);
  assert(commands.some(c=>c.op==='keyboard.key_tap'&&c.args.usage===25&&c.args.modifiers===1));
  const sent=commands.length;await p.locator('#text').fill('not\nANSI');await p.locator('#send').click();await p.waitForTimeout(150);assert.equal(commands.length,sent);
  await p.waitForFunction(()=>!document.getElementById('keyboard-controls').disabled);await p.locator('#text').fill('Hello host');await p.locator('#send').click();await p.waitForTimeout(180);assert(commands.some(c=>c.op==='keyboard.text.enqueue'&&c.args.text==='Hello host'));
  const beforeMouse=commands.length;
  await p.locator('[data-click=right]').click();
  await p.waitForFunction(()=>!document.getElementById('send').disabled && document.getElementById('result').textContent.includes('applied'));
  assert.deepEqual(commands.slice(beforeMouse).filter(c=>c.op!=='control.renew').map(c=>c.op),['mode.select','mouse.click']);
  await p.locator('#drag').scrollIntoViewIfNeeded();const dragBox=await p.locator('#drag').boundingBox();await p.mouse.move(dragBox.x+20,dragBox.y+20);await p.mouse.down();
  await p.waitForTimeout(220);assert(commands.some(c=>c.op==='mouse.drag.begin'));
  const held=commands.length;
  await p.getByRole('button',{name:'Enter',exact:true}).press('Enter');
  await p.waitForFunction(()=>!document.getElementById('send').disabled && document.getElementById('result').textContent.includes('applied'));
  const handedOff=commands.slice(held).filter(c=>!['control.renew','mouse.drag.refresh'].includes(c.op));
  assert.deepEqual(handedOff.map(c=>c.op),['mouse.drag.end','mode.select','keyboard.key_tap']);
  assert.equal(handedOff[2].args.usage,40);
  await p.mouse.up();
  await p.mouse.move(dragBox.x+20,dragBox.y+20);await p.mouse.down();
  await p.waitForFunction(()=>document.getElementById('drag').getAttribute('aria-pressed')==='true');
  await p.evaluate(()=>window.dispatchEvent(new Event('blur')));await p.waitForTimeout(220);assert(closed>0);assert.equal(await p.locator('#mouse-controls').evaluate(n=>n.disabled),true);await p.mouse.up();
  const after=commands.length;await p.waitForTimeout(1000);assert.equal(commands.length,after);
  await connect(p);await acquire(p);
  await p.locator('#drag').scrollIntoViewIfNeeded();const box=await p.locator('#drag').boundingBox();await p.mouse.move(box.x+20,box.y+20);await p.mouse.down();await p.waitForTimeout(220);await p.mouse.up();await p.waitForTimeout(220);
  assert(commands.some(c=>c.op==='mouse.drag.end'));assert.equal(await p.locator('#mouse-controls').evaluate(n=>n.disabled),false);
  await p.evaluate(()=>window.dispatchEvent(new Event('offline')));await p.waitForTimeout(150);assert.equal(await p.locator('#mouse-controls').evaluate(n=>n.disabled),true);
  await connect(p);await acquire(p);await p.evaluate(()=>{Object.defineProperty(document,'hidden',{configurable:true,value:true});document.dispatchEvent(new Event('visibilitychange'));});await p.waitForTimeout(150);
  assert.equal(await p.locator('#mouse-controls').evaluate(n=>n.disabled),true);
  await p.evaluate(()=>{delete document.hidden;});
  await connect(p);await p.locator('#tab-connections').click();await p.locator('#ble-peers .device').first().waitFor();
  await p.locator('#ble-peers .device').first().getByText('Edit label',{exact:true}).click();await p.locator('#label-value').fill('Desk');await p.locator('#editor-save').click();await p.waitForTimeout(250);
  assert(commands.some(c=>c.op==='display_name.set'&&c.args.id===bond&&c.args.owner_revision==='7'&&c.resource_revision==='8589934595'));
  const stored=await p.evaluate(()=>({...localStorage}));assert.deepEqual(Object.keys(stored),['passport.browser.theme.v1']);
  await p.setViewportSize({width:844,height:390});await bounded(p);await p.screenshot({path:path.join(out,'iphone-landscape-dark.png'),fullPage:true});
  pendingManagement=true;
  await p.route('**/api/v1/requests/*',route=>route.abort('failed'));
  await p.locator('#pair-open').click();
  await p.waitForFunction(()=>document.getElementById('management-result').textContent.includes('result unknown'));
  assert.equal(await p.locator('#pair-open').isDisabled(),true);assert.equal(await p.locator('#scan').isDisabled(),true);
  const afterUnknown=commands.length;await p.locator('#refresh-lists').click();
  await p.waitForFunction(()=>!document.getElementById('refresh-lists').disabled);
  assert.equal(await p.locator('#pair-open').isDisabled(),true);assert.equal(commands.length,afterUnknown);
  await ctx.close();assert.deepEqual(errors,[]);
  const {Controller,describeResult}=await import('data:text/javascript;base64,'+assets.get('js').toString('base64'));
  // An expired POST can coincide with a real takeover, or be followed by
  // an unavailable state read. An uncorrelated rejection is an unknown reply.
  for(const change of ['lease_active','lease_id','target_id','epoch','transport_generation','locked','release_pending','remote_allowed','input_ready','read-error','bad-key','bad-sequence','lost']){
    reset();
    let moves=0;
    const changeClient=new Controller(async (url,options)=>{
      if(url==='/api/v1/commands' && JSON.parse(options.body).op==='mouse.move_relative'){
        moves++;
        if(change==='lost')throw new TypeError('Response lost');
        if(change==='locked')locked=true;
        else if(change==='release_pending')state.release_pending=true;
        else if(['lease_active','remote_allowed','input_ready'].includes(change))state[change]=false;
        else if(['target_id','lease_id','epoch','transport_generation'].includes(change))state[change]='9';
        const receipt={state:'rejected',error:'expired',command_key:change==='bad-key'?null:'4',next_seq:change==='bad-sequence'?seq:seq+1};
        return new Response(JSON.stringify(receipt),{status:400});
      }
      if(moves && url==='/api/v1/state' && change==='read-error')return new Response('{}',{status:401});
      return fetch(origin+url,options);
    });
    await changeClient.login();await changeClient.acquire();
    const moving=changeClient.run('mouse.move_relative',{dx:2,dy:0});
    if(['read-error','bad-key','bad-sequence','lost'].includes(change))await assert.rejects(moving);
    else await moving;
    assert.equal(changeClient.controlled,false,change);
    assert.equal(changeClient.uncertain,['bad-key','bad-sequence','lost'].includes(change),change);
    await assert.rejects(changeClient.run('mouse.move_relative',{dx:-2,dy:0}));
    assert.equal(moves,1,change);
    await changeClient.disconnect();
  }
  reset();const c=new Controller((url,options)=>fetch(origin+url,options));await c.login();await c.acquire();
  let contact=true;delayState=180;const count=commands.length;
  const move=c.run('mouse.move_relative',{dx:12,dy:0},undefined,{expiresAt:performance.now()+150,current:()=>contact});
  contact=false;await move;assert.equal(commands.length,count);delayState=0;
  state.target_id='new-host';await c.refresh();assert.equal(c.controlled,false);await c.disconnect();
  reset();await c.login();await c.acquire();failCommand=true;const before=commands.length;
  await assert.rejects(c.run('mouse.click',{button:'left'}));assert(c.uncertain&&!c.controlled);assert.equal(commands.length,before+1);
  await assert.rejects(c.acquire());assert.equal(commands.length,before+1);await c.disconnect();
  reset();await c.login();pendingManagement=true;await assert.rejects(c.manage('bluetooth.pairing.open',{generation:'3'},'7',{timeout:0}));assert(c.uncertain&&!c.controlled);await c.disconnect();
  // Once submitted, an observation failure must not reopen the command lane.
  const managementClient=fetcher=>{
    const client=new Controller(fetcher);
    client.session={...session};client.operations.set('bluetooth.pairing.open',true);
    client.refresh=async()=>({locked:false,management_generation:'1',now_ms:'10000'});
    return client;
  };
  const manage=client=>client.manage('bluetooth.pairing.open',{generation:'3'},'7');
  const accepted=()=>new Response(JSON.stringify({state:'accepted',error:'ok',next_seq:2,command_key:'1'}));
  for(const failure of ['network','abort','missing-result']){
    let posts=0;
    const client=managementClient(async url=>{
      if(url==='/api/v1/commands'){posts++;return accepted();}
      if(failure==='network')throw new TypeError('Failed to fetch');
      if(failure==='abort')throw new DOMException('request deadline','AbortError');
      return new Response(JSON.stringify({error:'not_found'}),{status:404});
    });
    client.controlled=true;
    await assert.rejects(manage(client),/result unknown/);
    assert(client.uncertain&&!client.controlled&&!client.busy&&!client.managing,failure);
    assert.match(client.message,/unknown/);await assert.rejects(manage(client));assert.equal(posts,1,failure);
  }
  // A known terminal failure remains distinct from an unobserved outcome.
  const rejected=managementClient(async()=>new Response(JSON.stringify({state:'rejected',error:'stale',next_seq:1})));
  assert.equal((await manage(rejected)).state,'rejected');assert.equal(rejected.uncertain,false);
  // Late old-session responses, successful or failed, cannot alter a new lane.
  for(const outcome of ['network','abort','missing-result','applied']){
    let releaseResult,polled;
    const polling=new Promise(resolve=>{polled=resolve;});
    const client=managementClient(async url=>{
      if(url==='/api/v1/commands')return accepted();
      polled();return new Promise((resolve,reject)=>{releaseResult=()=>{
        if(outcome==='network')reject(new TypeError('Failed to fetch'));
        else if(outcome==='abort')reject(new DOMException('request deadline','AbortError'));
        else resolve(new Response(JSON.stringify(outcome==='applied'?{state:'applied',error:'ok'}:{error:'not_found'})));
      };});
    });
    const updates=[];
    const pending=assert.rejects(client.manage('bluetooth.pairing.open',{generation:'3'},'7',{update:r=>updates.push(r.state)}),/session closed/);
    await polling;
    const fresh={...session,session_id:'9'.repeat(32)};
    client.session=fresh;client.controlled=true;client.message='New session';
    let changes=0;client.onchange=()=>changes++;
    releaseResult();await pending;
    assert.equal(client.session,fresh);assert.equal(client.uncertain,false);assert.equal(client.controlled,true);
    assert(client.busy&&client.managing);assert.equal(client.message,'New session');assert.equal(changes,0);
    assert.deepEqual(updates,['accepted']);
  }
  // Closing during the polling delay must not query with a replacement token.
  let polls=0;
  const replaced=managementClient(async url=>{if(url==='/api/v1/commands')return accepted();polls++;throw new Error('Unexpected poll');});
  await assert.rejects(replaced.manage('bluetooth.pairing.open',{generation:'3'},'7',{update:()=>{replaced.session={...session};}}),/session closed/);
  assert.equal(polls,0);assert.equal(replaced.uncertain,false);
  assert.match(describeResult({state:'cancelled',error:'result_unknown',started_prefix:2,completed_pairs:1,uncertain_index:1},2),/1 \/ 2 key pairs complete.*Character 2 has an uncertain/);
  console.log(`PASS: production controller in Chromium (${process.argv.includes('--behavior-only')?'behavior checks':'desktop/phone/tablet dual themes'}), bounded layout and touch targets, Origin under no-referrer, steady renewal controls and serialized input, failed-renewal stop, visible Connect failure, literal identities, Ctrl/Command, independent wheel axes, typed commands, blur/held-input stop, no replay, management polling uncertainty and session isolation, memory-only credentials and persistent theme`);
}finally{await browser.close();await new Promise(r=>server.close(r));}
