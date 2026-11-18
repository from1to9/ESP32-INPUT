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
const motionModule=await readFile(path.join(root,"components/control_protocol/web/motion_window.mjs"));
const catalog=JSON.parse(await readFile(path.join(root,'api/control-v1.json'))).operations;
const supported=new Set(['control.acquire','control.renew','control.release','mouse.move_relative','mouse.click','mouse.scroll','mouse.drag.begin','mouse.drag.refresh','mouse.drag.end','keyboard.text.enqueue','keyboard.key_tap','keyboard.shortcut','mode.select','input.release_all','input.cancel','bluetooth.pairing.open','bluetooth.pairing.close','bluetooth.pairing.cancel_handshake','bluetooth.target.request','bluetooth.disconnect','wifi.scan.start','wifi.scan.cancel','display_name.list','display_name.set','display_name.clear']);
const session={session_token:'a'.repeat(64),session_id:'b'.repeat(32),boot_id:'c'.repeat(32),request_prefix:'d'.repeat(16),csrf:'e'.repeat(32),next_seq:1,scopes:['view','input.control','connections.manage','wifi.configure']};
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let afterMode, beginReply, touchTiming, tickingLease;
let state,commands,results,seq,closed,delayState,delayRenew,failSession,failRenew,failCommand,label,locked,pendingManagement;
const host='10:20:30:40:50:60/0', bond='01'.padEnd(32,'0'), ap='12'.repeat(32);
const hx=s=>Buffer.from(s).toString('hex');
function reset(){tickingLease=false;afterMode=beginReply=touchTiming=null;state={boot_id:session.boot_id,now_ms:'10000',epoch:'2',transport_generation:'3',state_sequence:'5',ui_revision:'2',management_generation:'1',target_id:host,mode:'mouse',locked:false,release_pending:false,input_ready:true,remote_allowed:true,remote_owner:false,lease_active:false,lease_id:'f'.repeat(32),lease_expires_ms:'13000'};commands=[];results=new Map();seq=1;closed=0;delayState=delayRenew=0;failSession=failRenew=failCommand=false;label='Studio laptop';locked=false;pendingManagement=false;}
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
      if(timing || tickingLease) currentState.now_ms=String(Math.floor(performance.now()));
      if(timing) timing.trace.push('state');
      const protocol={...currentState,locked};
      if(timing?.stateReturnMs)await pause(timing.stateReturnMs);
      if(timing?.path){
        const replyDelays=[22,60,85];
        await pause(replyDelays[timing.reads++ % replyDelays.length]);
      }
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
      if((touchTiming || tickingLease) && ['control.acquire','control.renew'].includes(c.op)){
        state.lease_expires_ms=String(Math.floor(performance.now())+3000);
      }
      if(touchTiming && c.op==='mouse.move_relative'){
        const timing=touchTiming;
        const first=!timing.moves.length;
        timing.trace.push('move');
        await pause(first?timing.firstAdmissionMs:35);
        const slack=Number(c.deadline_ms)-performance.now();
        const expired=slack<=0;
        const receipt={state:expired?'rejected':'sent',error:expired?'expired':'ok',next_seq:seq,command_key:String(seq)};
        timing.moves.push({seq:c.seq,...c.args,slack:Math.floor(slack),state:receipt.state});
        results.set(receipt.command_key,receipt);
        if(timing.path){
          assert(BigInt(c.deadline_ms)<=BigInt(Math.ceil(performance.now())+150));
          assert(performance.now()<Number(state.lease_expires_ms),'active input lease');
          await pause(35);
        }
        if(timing.moveReply)await timing.moveReply;
        if(expired && timing.expiryState && req.headers['x-passport-expired-state']==='1'){
          timing.trace.push('attached');
          send({...receipt,protocol:{...state,locked,now_ms:String(Math.floor(performance.now()))}},400);
        }else if(expired)send(receipt,400);
        else if(timing.inline)send(receipt);
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
    if(url.pathname.startsWith('/api/v1/requests/')){
      if(touchTiming?.path)await pause(45);
      send(results.get(url.pathname.split('/').at(-1))||{state:'failed',error:'result_unknown'});return;}
    if(url.pathname==='/api/v1/bluetooth/peers'){send({known:true,revision:'7',generation:'3',connected:true,ready:true,pairing:false,handshake:false,window_seconds:0,message:'Connected and ready',peers:[{id:bond,original:host,label,label_known:true,labelable:true,current:true},{id:'02'.padEnd(32,'0'),original:'10:20:30:40:50:61/0',label:'Studio laptop',label_known:true,labelable:true,current:false}]});return;}
    if(url.pathname==='/api/v1/wifi/profiles'){send({known:true,revision:'4',sta_ip:'192.0.2.1',ap_ip:'',ap_active:false,message:'LAN connected',profiles:[{id:'00000001',ssid_hex:hx('Meridian Studio'),label:'Home office',label_known:true,current:true}]});return;}
    if(url.pathname==='/api/v1/wifi/scans/current'){send({known:true,generation:9,next_cursor:0,scanning:false,records:[{id:ap,index:0,ssid_hex:hx('<img src=x onerror=alert(1)>'),bssid:'001122334455',rssi:-53,security:1,label:'Workshop',label_known:true,labelable:true,saved_matches:0}]});return;}
    if(url.pathname==='/api/v1/display-names'){send({known:true,revision:'8589934595',count:1,next_cursor:0,labels:label?[{kind:'hid_bond',id:bond,label}]:[]});return;}
    send({error:'not_found'},404);return;
  }
  if(url.pathname==='/motion_window.mjs'){
    res.writeHead(200,{'Content-Type':'text/javascript'});res.end(motionModule);return;
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
async function padTouch(context,page){
  const pad=page.locator('#pad');await pad.scrollIntoViewIfNeeded();
  const box=await pad.boundingBox(),x=Math.round(box.x+box.width/2),y=Math.round(box.y+box.height/2);
  const cdp=await context.newCDPSession(page);
  return (type,dx=0,dy=0)=>cdp.send('Input.dispatchTouchEvent',{type,
    touchPoints:type==='touchEnd'?[]:[{x:x+dx,y:y+dy,id:1}]});
}
// Exercise the production controller through a routed browser WebSocket. The
// native frame/owner fixture covers the device side of this same exchange.
async function socketTouch(ending){
  reset();
  const context=await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true});
  const frames=[],lookups=[];
  const originalSession={...session};
  const late=ending==='late-sent' || ending==='session-expired';
  let tickets=0,ws,lateKey,finishLookup;
  try{
    await context.route('**/api/v1/capabilities',route=>route.fulfill({json:{access_policy:'open_network',websocket:true}}));
    await context.route('**/api/v1/ws-ticket',route=>{
      const req=route.request();
      assert.equal(req.method(),'POST');assert.equal(req.headers().authorization,'Bearer '+session.session_token);
      assert.equal(req.headers()['x-csrf-token'],session.csrf);
      tickets++;return route.fulfill({json:{ticket:String(tickets).repeat(64)}});
    });
    if(late)await context.route('**/api/v1/requests/*',async route=>{
      if(!lateKey || !route.request().url().endsWith('/'+lateKey))return route.continue();
      // The owner already submitted this move. Its keyed HTTP read arrives
      // after the input socket closed, so both facts must remain visible.
      ws.close({code:1001,reason:'test disconnect during lookup'});
      await new Promise(resolve=>{finishLookup=resolve;});
      await route.fulfill({json:results.get(lateKey)});
    });
    await context.routeWebSocket('**/api/v1/ws',channel=>{
      assert(!ws);ws=channel;
      ws.onMessage(data=>{
        const c=JSON.parse(data);
        assert.equal(c.op,'mouse.move_relative');assert.equal(c.seq,seq++);
        assert.equal(c.session_id,session.session_id);assert.equal(c.lease_id,state.lease_id);
        assert.equal(c.target_id,state.target_id);assert.equal(c.epoch,state.epoch);
        frames.push(c);
        const receipt={state:'sent',error:'ok',command_key:String(1000+c.seq),next_seq:seq};
        if(frames.length===1){
          results.set(receipt.command_key,receipt);
          ws.send(JSON.stringify({...receipt,state:'accepted'}));
        }else if(frames.length===2){
          ws.send(JSON.stringify({...receipt,state:'rejected',error:'expired',protocol:{...state,locked}}));
        }else if(frames.length===3){
          ws.send(JSON.stringify(receipt));
        }else if(late){
          lateKey=receipt.command_key;
          results.set(lateKey,receipt);ws.send(JSON.stringify({...receipt,state:'accepted'}));
        }else if(ending==='loss'){
          ws.close({code:1001,reason:'test disconnect'});
        }else if(ending==='wrong-sequence'){
          ws.send(JSON.stringify({...receipt,next_seq:c.seq}));
        }
        // In the Stop case the fourth frame remains unanswered.
      });
    });
    const page=await open(context);
    // Record lookup requests before connecting so the first keyed lookup is visible.
    page.on('request',req=>{if(req.url().includes('/api/v1/requests/'))lookups.push(req.url().split('/').at(-1));});
    await connect(page);await acquire(page);assert.equal(tickets,1);
    const touch=await padTouch(context,page);
    await touch('touchStart');
    for(const [i,x] of [8,16,24,32].entries()){
      await touch('touchMove',x);
      const until=performance.now()+3000;
      while(frames.length<=i && performance.now()<until)await pause(5);
      assert.equal(frames.length,i+1);
      if(i<3)await pause(70);
    }
    assert(lookups.includes(String(1000+frames[0].seq)));
    assert(!lookups.includes(String(1000+frames[2].seq)));
    if(ending==='stop'){
      const deleted=page.waitForResponse(r=>r.request().method()==='DELETE');
      await page.locator('#stop').click();assert.equal((await deleted).status(),200);
    }
    await page.waitForFunction(()=>document.getElementById('mouse-controls').disabled);
    if(late){
      finishLookup();
      await page.waitForFunction(()=>document.getElementById('result').textContent.includes('Sent to the HID transport'));
      assert.match(await page.locator('#connection').textContent(),/input channel disconnected/i);
      assert.match(await page.locator('#readiness').textContent(),/Disconnect, then Connect/);
      assert(await page.locator('#acquire').isDisabled());
    }
    await touch('touchMove',44);await touch('touchEnd');await pause(100);
    assert.equal(frames.length,4);assert.equal(tickets,1);
    assert(!commands.some(c=>c.op==='mouse.move_relative'));
    if(late){
      if(ending==='session-expired'){
        await page.route('**/api/v1/state',route=>route.fulfill({status:401,json:{error:'unauthenticated'}}),{times:1});
      }else await page.locator('#disconnect').click();
      await page.waitForFunction(()=>!document.getElementById('connect').hidden);
      assert(!/Disconnect, then Connect/.test(await page.locator('#readiness').textContent()));
      if(ending==='session-expired'){
        session.boot_id=state.boot_id='1'.repeat(32);session.session_id='2'.repeat(32);
        session.session_token='3'.repeat(64);seq=1;state.lease_active=false;
      }
      ws=null;await connect(page);await acquire(page);
      assert.match(await page.locator('#connection').textContent(),/is controlling/);
      assert.equal(frames.length,4);assert.equal(tickets,2);
      const ended=page.waitForResponse(r=>r.request().method()==='DELETE');
      await page.locator('#disconnect').click();assert.equal((await ended).status(),200);
    }
    console.log('PASS WebSocket touch',ending,'single submission, keyed pending, expired pin, direct sent');
  }finally{await context.close();Object.assign(session,originalSession);}
}
async function socketRenew(outcome){
  reset();
  state.lease_expires_ms=outcome==='short-grant'?'13000':'40000';
  const context=await browser.newContext();
  const frames=[],lookups=[],timeline=[];
  let ws,negotiated=false,resolveRenew;
  const written=new Promise(resolve=>{resolveRenew=resolve;});
  try{
    await context.route('**/api/v1/capabilities',r=>r.fulfill({json:{access_policy:'open_network',websocket:true,
      protocol_state_view:true,websocket_motion_window:true,websocket_motion_window_max:2,websocket_motion_ordered:true}}));
    await context.route('**/api/v1/ws-ticket',r=>r.fulfill({json:{ticket:'1'.repeat(64)}}));
    await context.routeWebSocket('**/api/v1/ws',channel=>{
      ws=channel;
      channel.onMessage(data=>{
        const frame=JSON.parse(data);
        if(!negotiated){
          assert.deepEqual(frame,{v:1,type:'motion_window',motion_window:2});
          negotiated=true;
          channel.send(JSON.stringify({motion_terminal:true,renew_terminal:true,motion_window:2}));
          return;
        }
        assert.equal(frame.op,'control.renew');assert.equal(frame.seq,seq++);
        assert.equal(frame.lease_id,state.lease_id);assert.equal(frame.target_id,state.target_id);
        assert.equal(frame.args.state_sequence,state.state_sequence);
        frames.push(frame);timeline.push('renew');resolveRenew();
        if(outcome==='stale' && frames.length===1){
          ws.send(JSON.stringify({command_seq:frame.seq,next_seq:seq,command_key:'711',state:'rejected',error:'stale'}));
        }
      });
    });
    const page=await open(context);
    page.on('request',r=>{
      if(r.url().includes('/api/v1/requests/'))lookups.push(r.url());
      if(r.url().endsWith('/state?view=protocol'))timeline.push('state');
      if(r.url().endsWith('/commands'))timeline.push(r.postDataJSON().op);
    });
    await connect(page);await acquire(page);
    assert(!Object.hasOwn(commands[0].args,'lifetime_ms'));
    if(outcome==='short-grant'){
      // The short grant is authoritative even when acquire omitted a duration.
      await Promise.race([written,pause(2000).then(()=>{throw Error('Missing idle renewal on a short grant');})]);
      assert.deepEqual(timeline.slice(-2),['state','renew']);
      const frame=frames[0];
      ws.send(JSON.stringify({command_seq:frame.seq,next_seq:seq,command_key:'712',state:'applied',error:'ok',
        lease_active:true,lease_id:state.lease_id,lease_expires_ms:'14000'}));
      assert.equal(frames.length,1);assert(!commands.some(c=>c.op==='control.renew'));
      console.log('PASS observed 3 s grant uses the free-lane S31 renewal timer');
      return;
    }
    const click=page.locator('[data-click=left]');
    state.now_ms='33500'; // 6500 ms remain on the real grant reported by the fixture.
    const first=page.waitForResponse(r=>r.url().endsWith('/commands') && r.request().postDataJSON().op==='mouse.click');
    await click.click();await first;
    await page.waitForFunction(()=>!document.querySelector('[data-click=left]').disabled);
    assert.equal(frames.length,0);
    state.now_ms='35100'; // 4900 ms remain. Wait for the initiating click to read it.
    await click.click();await Promise.race([written,pause(4000).then(()=>{throw Error('Missing WS renewal');})]);
    if(outcome==='stale'){
      const until=performance.now()+2000;
      while(frames.length<2 && performance.now()<until)await pause(5);
      assert.equal(frames.length,2);assert.equal(frames[1].seq,frames[0].seq+1);
    }
    assert.deepEqual(timeline.slice(-2),['state','renew']);
    const receipt={command_seq:frames.at(-1).seq,next_seq:seq,command_key:'712',error:'ok'};
    const clicks=()=>commands.filter(c=>c.op==='mouse.click').length;
    if(outcome!=='direct'){
      ws.send(JSON.stringify({...receipt,state:'accepted',lease_active:true,lease_id:state.lease_id,lease_expires_ms:'40000'}));
      await pause(50);assert.equal(clicks(),1);
    }
    if(outcome==='loss'){
      ws.close({code:1001,reason:'lost accepted renewal'});
      await page.waitForFunction(()=>document.getElementById('mouse-controls').disabled);
      assert.equal(clicks(),1);
      results.set(receipt.command_key,{...receipt,state:'applied'});
      const checked=page.waitForResponse(r=>r.url().endsWith('/requests/712'));
      await page.locator('#result-check').click();await checked;
      await page.waitForFunction(()=>document.getElementById('result').textContent.includes('Device applied'));
      assert(await page.locator('#mouse-controls').evaluate(el=>el.disabled));
      assert(await click.isDisabled());
    }else if(['wrong-key','wrong-sequence','old-expiry'].includes(outcome)){
      ws.send(JSON.stringify({...receipt,state:'applied',motion_terminal:true,lease_active:true,lease_id:state.lease_id,
        lease_expires_ms:outcome==='old-expiry'?'40000':'65100',
        ...(outcome==='wrong-key'?{command_key:'713'}:outcome==='wrong-sequence'?{command_seq:receipt.command_seq+1}:{})}));
      await page.waitForFunction(()=>document.getElementById('mouse-controls').disabled);
      assert.equal(clicks(),1);
    }else{
      state.lease_expires_ms='65100';
      const next=page.waitForResponse(r=>r.url().endsWith('/commands') && r.request().postDataJSON().op==='mouse.click');
      ws.send(JSON.stringify({...receipt,state:'applied',lease_active:true,lease_id:state.lease_id,
        lease_expires_ms:state.lease_expires_ms,...(outcome==='direct'?{}:{motion_terminal:true})}));
      await next;assert.equal(clicks(),2);
      assert.equal(timeline[timeline.lastIndexOf('renew')+1],'mouse.click');
    }
    assert.equal(frames.length,outcome==='stale'?2:1);
    assert(!commands.some(c=>c.op==='control.renew'));assert.equal(lookups.length,outcome==='loss'?1:0);
    console.log('PASS negotiated WS renewal',outcome,'30 s grant, exclusive keyed completion, no HTTP renewal or result poll');
  }finally{await context.close();}
}
async function windowTouch(ending){
  reset();state.lease_expires_ms=String(Number(state.now_ms)+30000);
  const context=await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true});
  const moves=[],lookups=[];
  let ws,negotiated=false;
  try{
    await context.route('**/api/v1/capabilities',r=>r.fulfill({json:{access_policy:'open_network',websocket:true,
      websocket_motion_window:true,websocket_motion_window_max:2,websocket_motion_ordered:true}}));
    await context.route('**/api/v1/ws-ticket',r=>r.fulfill({json:{ticket:'1'.repeat(64)}}));
    await context.routeWebSocket('**/api/v1/ws',channel=>{
      ws=channel;
      channel.onMessage(data=>{
        const frame=JSON.parse(data);
        if(!negotiated){
          assert.deepEqual(frame,{v:1,type:'motion_window',motion_window:2});
          negotiated=true;
          channel.send(JSON.stringify({motion_terminal:true,renew_terminal:true,motion_window:2}));
          return;
        }
        assert.equal(frame.op,'mouse.move_relative');assert.equal(frame.seq,seq++);
        assert.equal(frame.lease_id,state.lease_id);assert.equal(frame.target_id,state.target_id);
        moves.push(frame);
      });
    });
    const page=await open(context);
    page.on('request',req=>{if(req.url().includes('/api/v1/requests/'))lookups.push(req.url().split('/').at(-1));});
    await connect(page);await acquire(page);
    const touch=await padTouch(context,page);
    if(ending==='stationary-lock'){
      await touch('touchStart');
      locked=true;state.lease_active=false;
      await page.waitForFunction(()=>document.getElementById('connection').textContent==='Passport is locked',null,{timeout:2200});
      assert(await page.locator('[data-click=left]').isDisabled());
      await touch('touchMove',12);await touch('touchEnd');
      assert.equal(moves.length,0);assert.deepEqual(commands.map(c=>c.op),['control.acquire']);
      console.log('PASS stationary touch reflects local lock without movement or renewal');
      return;
    }
    const waitMoves=async count=>{
      const until=performance.now()+700;
      while(moves.length<count && performance.now()<until)await pause(5);
      assert.equal(moves.length,count);
    };
    const receipt=(i,extra={})=>({state:'sent',error:'ok',command_seq:moves[i].seq,
      next_seq:moves[i].seq+1,command_key:String(1000+moves[i].seq),...extra});
    const reply=(i,extra)=>ws.send(JSON.stringify(receipt(i,extra)));
    await touch('touchStart');await touch('touchMove',10);await waitMoves(1);
    await touch('touchMove',2);await pause(38);assert.equal(moves.length,1);
    reply(0,{state:'accepted',now_ms:state.now_ms});
    await waitMoves(2);
    assert.deepEqual(moves.map(m=>m.args.dx),[10,-8]);
    assert(BigInt(moves[1].deadline_ms)>=BigInt(moves[0].deadline_ms));
    await touch('touchMove',15);await pause(42);assert.equal(moves.length,2);
    if(ending==='reverse'){
      reply(1,{state:'rejected',error:'expired',protocol:{...state,locked:false}});
      await waitMoves(3);
      assert.equal(moves[2].args.dx,13);
      reply(2,{state:'accepted'});reply(2,{motion_terminal:true});
      reply(0,{next_seq:seq,motion_terminal:true});
      await touch('touchEnd');
      await page.waitForFunction(()=>!document.querySelector('[data-click]').disabled);
      const released=page.waitForResponse(r=>r.request().postDataJSON?.()?.op==='control.release');
      await page.locator('#stop').click();await released;
      assert.equal(commands.at(-1).op,'control.release');
      assert.equal(commands.at(-1).seq,moves.at(-1).seq+1);
    }else if(ending.startsWith('drain')){
      let reads=0;
      page.on('request',req=>{if(new URL(req.url()).pathname==='/api/v1/state')reads++;});
      if(ending==='drain-stale')await pause(920);
      reply(1,{state:'rejected',error:'expired',...(ending==='drain-stale'?{protocol:{...state,locked:false}}:{})});
      await pause(45);
      assert.equal(reads,0);assert.equal(moves.length,2);
      if(ending==='drain-stop' || ending==='drain-loss'){
        if(ending==='drain-stop'){
          const ended=page.waitForResponse(r=>r.request().method()==='DELETE');
          await page.locator('#stop').click();assert.equal((await ended).status(),200);
        }else ws.close({code:1001,reason:'close during drain'});
        await page.waitForFunction(()=>document.getElementById('mouse-controls').disabled);
        await touch('touchMove',29);await touch('touchEnd');await pause(40);
        assert.equal(reads,0);assert.equal(moves.length,2);
      }else{
        await touch('touchEnd');
        reply(0,{next_seq:seq,motion_terminal:true});
        await page.waitForFunction(()=>!document.querySelector('[data-click]').disabled);
        assert.equal(reads,1);
        await touch('touchStart');await touch('touchMove',9);await waitMoves(3);
        assert.equal(moves[2].args.dx,9);reply(2,{state:'accepted'});reply(2,{motion_terminal:true});await touch('touchEnd');
      }
    }else if(ending==='late-after-busy'){
      const refused=receipt(1,{state:'rejected',error:'busy'});
      results.set(refused.command_key,refused);
      reply(1,refused);
      await page.waitForFunction(()=>document.getElementById('mouse-controls').disabled);
      reply(0,{next_seq:seq,motion_terminal:true});
      await page.waitForFunction(()=>document.getElementById('result').textContent.includes('Sent to the HID transport'));
      const message=await page.locator('#result').textContent();
      assert(message.includes(`#${moves[1].seq} (key ${refused.command_key}): rejected: busy`));
      assert(message.includes(`#${moves[0].seq} (key ${1000+moves[0].seq}): Sent`));
      const read=page.waitForResponse(r=>r.url().endsWith('/requests/'+refused.command_key));
      await page.locator('#result-check').click();await read;
      assert.equal(lookups.at(-1),refused.command_key);
      await touch('touchMove',25);await touch('touchEnd');assert.equal(moves.length,2);
    }else{
      if(ending==='stop'){
        const closed=page.waitForResponse(r=>r.request().method()==='DELETE');
        await page.locator('#stop').click();assert.equal((await closed).status(),200);
      }else{
        ws.close({code:1001,reason:'test two outstanding moves'});
      }
      await page.waitForFunction(()=>document.getElementById('mouse-controls').disabled);
      assert.match(await page.locator('#result').textContent(),new RegExp(`#${moves[0].seq} \\(key ${1000+moves[0].seq}\\)`));
      assert.match(await page.locator('#result').textContent(),/key unknown/);
      await touch('touchMove',28);await touch('touchEnd');await pause(80);
      assert.equal(moves.length,2);
      if(ending==='loss'){
        const stored=receipt(0,{next_seq:seq});
        results.set(stored.command_key,stored);
        await page.locator('#result-check').click();
        await page.waitForFunction(()=>document.getElementById('result').textContent.includes('Sent to the HID transport'));
        assert(lookups.includes(stored.command_key));
        assert.match(await page.locator('#result').textContent(),/key unknown/);
        assert(await page.locator('#mouse-controls').evaluate(fieldset=>fieldset.disabled));
        assert(await page.locator('[data-click]').first().isDisabled());
      }
    }
    assert(!commands.some(c=>c.op==='mouse.move_relative'));
    console.log('PASS admission-gated window',ending,'second write after first acceptance; third bounded; no replay');
  }finally{await context.close();}
}
async function touchAcrossPadEdge(){
  reset();touchTiming={trace:[],moves:[],firstAdmissionMs:5,stateReturnMs:0,inline:true};
  const context=await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true});
  try{
    const page=await open(context);await connect(page);await acquire(page);
    const pad=page.locator('#pad');await pad.scrollIntoViewIfNeeded();
    const box=await pad.boundingBox(),x=Math.round(box.x+box.width/2),y=Math.floor(box.y+box.height)-8;
    await pad.evaluate(el=>{
      el.addEventListener('pointerdown',e=>{window.padPointer=e.pointerId;});
      window.outsideMoves=[];
      document.addEventListener('pointermove',e=>{
        if(!el.contains(e.target))window.outsideMoves.push(e.pointerId);
      },true);
    });
    const cdp=await context.newCDPSession(page);
    const touch=(type,dx=0,dy=0)=>cdp.send('Input.dispatchTouchEvent',{type,
      touchPoints:['touchCancel','touchEnd'].includes(type)?[]:[{x:x+dx,y:y+dy,id:1}]});
    const moveResponse=()=>page.waitForResponse(r=>r.url().endsWith('/commands') && r.request().postDataJSON().op==='mouse.move_relative');
    for(const end of ['touchEnd','touchCancel']){
      await touch('touchStart');
      await pad.evaluate(el=>{
        if(!el.hasPointerCapture(window.padPointer))throw Error('Pad did not capture the contact');
        el.releasePointerCapture(window.padPointer);
      });
      let posted=moveResponse();
      await touch('touchMove',0,22);await posted;
      posted=moveResponse();
      await touch('touchMove',6,22);
      assert.deepEqual((await posted).request().postDataJSON().args,{dx:6,dy:0});
      assert(await page.evaluate(()=>window.outsideMoves.includes(window.padPointer)));
      await touch(end);
      await pause(180);
    }
    const count=commands.filter(c=>c.op==='mouse.move_relative').length;
    await touch('touchStart',0,22);await touch('touchMove',4,22);
    await touch('touchMove',4,-4);await touch('touchEnd');
    await pause(180);
    assert.equal(commands.filter(c=>c.op==='mouse.move_relative').length,count);
    console.log('PASS trusted touch continues outside pad after capture loss, lift and cancel discard, outside start ignored');
  }finally{await context.close();}
}
async function fractionalTouch(){
  reset();touchTiming={trace:[],moves:[],firstAdmissionMs:1,stateReturnMs:0,inline:true};
  const context=await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true});
  try{
    const page=await open(context);await connect(page);await acquire(page);
    const touch=await padTouch(context,page);
    // Delaying an already-sent vertical move must not change horizontal rounding.
    for(const holdReply of [false,true]){
      const first=touchTiming.moves.length;
      let release;
      touchTiming.moveReply=holdReply ? new Promise(resolve=>{release=resolve;}) : null;
      try{
        await touch('touchStart');await touch('touchMove',0.5,1);await pause(85);
        assert.equal(touchTiming.moves[first]?.state,'sent');
        await touch('touchMove',0,1);await pause(85);await touch('touchMove',-0.5,1);
      }finally{release?.();}
      await pause(100);await touch('touchEnd');
      assert.deepEqual(touchTiming.moves.slice(first).map(({dx,dy,state})=>({dx,dy,state})),
        [{dx:0,dy:1,state:'sent'}]);
    }
    for(const sign of [1,-1]){
      const first=commands.length;
      await touch('touchStart');
      for(let i=1;i<=48;i++){
        await touch('touchMove',sign*i/2);
        await pause(25);
      }
      await pause(90);await touch('touchEnd');
      const moves=commands.slice(first).filter(c=>c.op==='mouse.move_relative');
      assert.equal(moves.reduce((n,c)=>n+c.args.dx,0),sign*24);
      assert(moves.every(c=>c.args.dy===0 && Math.sign(c.args.dx)===sign));
    }
    // Once a reversal cancels a fraction, expiry cannot bring either half back.
    const reversal=commands.length;
    await touch('touchStart');await touch('touchMove',0.5);await pause(80);
    await touch('touchMove',0);await pause(80);await touch('touchMove',-0.5);
    await pause(35);
    assert(!commands.slice(reversal).some(c=>c.op==='mouse.move_relative'));
    await touch('touchMove',-1);await pause(70);await touch('touchEnd');
    assert.deepEqual(commands.slice(reversal).filter(c=>c.op==='mouse.move_relative').map(c=>c.args),[{dx:-1,dy:0}]);
    // Two half steps cannot combine across lift or after the first expires.
    const first=commands.length;
    await touch('touchStart');await touch('touchMove',0.5);await touch('touchEnd');
    await touch('touchStart');await touch('touchMove',0.5);
    await pause(190);await touch('touchMove',1);await pause(75);await touch('touchEnd');
    assert(!commands.slice(first).some(c=>c.op==='mouse.move_relative'));
    // Horizontal integer output must leave the original fractional Y intact.
    await touch('touchStart');await touch('touchMove',1,0.5);await pause(55);
    await touch('touchMove',2,1);await pause(90);await touch('touchEnd');
    const mixed=commands.slice(first).filter(c=>c.op==='mouse.move_relative');
    assert.equal(mixed.reduce((n,c)=>n+c.args.dx,0),2);
    assert.equal(mixed.reduce((n,c)=>n+c.args.dy,0),1);
    console.log('PASS fractional touch: 48 half pixels each way deliver +24/-24, mixed axes, immediate/delayed reversal, lift and original expiry');
  }finally{await context.close();}
}
// Motion during neutral release cannot become part of a later fresh delta.
async function touchAcrossRelease(){
  reset();state.mode='keyboard';afterMode=()=>{state.release_pending=true;};
  touchTiming={trace:[],moves:[],firstAdmissionMs:35,stateReturnMs:0};
  const context=await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true});
  try{
    const page=await open(context);await connect(page);await acquire(page);
    const touch=await padTouch(context,page);
    await touch('touchStart');await touch('touchMove',16);
    await page.waitForFunction(()=>document.getElementById('mouse-controls').disabled);
    await touch('touchMove',16,35);
    state.release_pending=false;
    await page.waitForFunction(()=>!document.getElementById('mouse-controls').disabled);
    await pause(230);
    const before=commands.length;
    const posted=page.waitForResponse(r=>r.url().endsWith('/commands') && r.request().postDataJSON().op==='mouse.move_relative');
    await touch('touchMove',17,35);await posted;await touch('touchEnd');
    await pause(200);
    const later=commands.slice(before).filter(c=>c.op==='mouse.move_relative').map(c=>c.args);
    assert.deepEqual(later,[{dx:1,dy:0}]);
    assert(commands.filter(c=>c.op==='mouse.move_relative').every(c=>c.args.dy===0));
    console.log('PASS touch across neutral release',JSON.stringify(later));
  }finally{await context.close();}
}
// Use trusted touch events and a running device clock. The first command
// misses admission. Later movements must reach terminal sent on the same lease.
async function delayedTouch(expiryState){
  reset();
  delayState=45;
  const timing=touchTiming={trace:[],moves:[],firstAdmissionMs:180,stateReturnMs:35,expiryState};
  const context=await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true});
  try{
    const page=await open(context);await connect(page);await acquire(page);
    const touch=await padTouch(context,page);
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
    const during=timing.trace.slice(timing.trace.indexOf('move'),timing.trace.indexOf('move',timing.trace.indexOf('move')+1));
    assert(during.includes(expiryState?'attached':'state'),JSON.stringify(during));
    if(expiryState)assert(!during.includes('state'),JSON.stringify(during));
    assert.match(await page.locator('#result').textContent(),/Sent to the HID transport/);
    console.log('PASS delayed touch',JSON.stringify(timing.moves));
    await page.locator('#stop').click();
    await page.waitForFunction(()=>document.getElementById('connection').textContent!=='This browser is controlling');
  }finally{await context.close();touchTiming=null;}
}
// The two boards use the same pointer path. These delays model HTTP legs;
// they do not measure BLE or guarantee a physical receiving application.
async function trajectory(name,steps,interval,offset){
  reset();delayState=35;
  const timing=touchTiming={path:true,inline:process.argv.includes('--inline-receipt'),reads:0,trace:[],moves:[],firstAdmissionMs:35};
  const context=await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true});
  try{
    const page=await open(context);
    const sent=[],rejected=[];
    page.on('response',async response=>{
      const route=new URL(response.url()).pathname;
      if(route!=='/api/v1/commands' && !route.startsWith('/api/v1/requests/'))return;
      const receipt=await response.json().catch(()=>null);
      if(!receipt?.command_key)return;
      const c=commands.find(cmd=>String(cmd.seq+1)===receipt.command_key);
      if(c?.op!=='mouse.move_relative')return;
      const row={at:performance.now(),seq:c.seq,dx:c.args.dx};
      if(receipt.state==='sent' && receipt.error==='ok')sent.push(row);
      if(receipt.state==='rejected' && receipt.error==='expired')rejected.push(row);
    });
    await connect(page);await acquire(page);
    await page.waitForResponse(r=>r.url().endsWith('/commands') && r.request().postDataJSON().op==='control.renew');
    await page.waitForResponse(r=>r.url().endsWith('/state'));
    const touch=await padTouch(context,page);
    const start=performance.now(), before=commands.length;
    await touch('touchStart');
    let pathLength=0,last=0;
    for(let i=1;i<=steps;i++){
      const x=offset(i);
      await pause(Math.max(0,start+interval*i-performance.now()));
      await touch('touchMove',x);
      pathLength+=Math.abs(x-last);last=x;
    }
    await touch('touchEnd');
    const duration=Math.round(performance.now()-start);
    await pause(800);
    const after=commands.slice(before),moves=after.filter(c=>c.op==='mouse.move_relative');
    const gaps=sent.slice(1).map((r,i)=>Math.round(r.at-sent[i].at)).sort((a,b)=>a-b);
    const quantile=n=>gaps[Math.ceil(n*gaps.length)-1]??null;
    const sum=(items,abs)=>items.reduce((n,r)=>n+(abs?Math.abs(r.dx):r.dx),0);
    const summary={name,duration,input_path:pathLength,input_net:last,posted:moves.length,sent:sent.length,
      sent_path:sum(sent,true),sent_net:sum(sent,false),expired:rejected.length,p50_ms:quantile(.5),p95_ms:quantile(.95),
      renewals:after.filter(c=>c.op==='control.renew').length,one_lease:moves.every(c=>c.lease_id===state.lease_id)
        && commands.filter(c=>c.op==='control.acquire').length===1 && state.lease_active};
    assert.equal(await page.locator('#connection').textContent(),'This browser is controlling');
    assert(summary.one_lease && moves.length===sent.length+rejected.length);
    assert.equal(new Set([...sent,...rejected].map(c=>c.seq)).size,moves.length);
    assert(moves.every(c=>c.args.dy===0));
    await page.locator('#stop').click();
    await page.waitForFunction(()=>document.getElementById('connection').textContent!=='This browser is controlling');
    console.log(JSON.stringify(summary));
  }finally{await context.close();touchTiming=null;}
}
if(process.argv.includes('--trajectory')){
  try{
    await trajectory('360-step reversal',360,23,i=>3*(32-Math.abs(32-i%64)));
    await trajectory('right 40',20,45,i=>2*i);
    await trajectory('left 40',20,45,i=>-2*i);
  }finally{await browser.close();await new Promise(r=>server.close(r));}
}else try{
  for(const outcome of ['direct','pushed','loss','wrong-key','wrong-sequence','old-expiry','stale','short-grant'])await socketRenew(outcome);
  for(const ending of ['stationary-lock','reverse','loss','stop','drain','drain-stale','drain-stop','drain-loss','late-after-busy'])await windowTouch(ending);
  for(const ending of ['loss','stop','wrong-sequence','late-sent','session-expired'])await socketTouch(ending);
  await fractionalTouch();
  await touchAcrossPadEdge();
  await touchAcrossRelease();
  await delayedTouch(false);
  await delayedTouch(true);
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
  reset();tickingLease=true;state.mode='keyboard';
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
  reset();tickingLease=true;const renewalContext=await browser.newContext({viewport:{width:390,height:844},isMobile:true,hasTouch:true});
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
  const beforeInvalidText=commands.length;
  await p.locator('#text').fill('not\nANSI');
  await p.locator('#send').click();
  await p.waitForFunction(()=>document.getElementById('result').textContent.includes('Remove newlines'));
  // The lease timer can renew independently while the page rejects local text.
  assert(commands.slice(beforeInvalidText).every(command=>command.op==='control.renew'));
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
  // A snapshot is generated between POST start and response parsing. Exercise
  // delayed replies with independent browser and device clocks, without sleeping.
  const realNow=Object.getOwnPropertyDescriptor(performance,'now');
  let browserTime=0;
  Object.defineProperty(performance,'now',{configurable:true,value:()=>browserTime});
  try{
    const schedule=new Controller();
    schedule.state={boot_id:session.boot_id,now_ms:'1000',lease_expires_ms:'4000'};
    schedule.stateAt=0;schedule.pin={longLease:false};schedule.channel={renewTerminal:true};
    assert(!schedule.renewDue(2100,true) && schedule.renewDue(2101,true)); // 900 ms motion reserve
    assert(!schedule.renewDue(1200) && schedule.renewDue(1201)); // 1800 ms for discrete input
    schedule.pin.longLease=true;schedule.state.lease_expires_ms='31000';
    assert(!schedule.renewDue(25000,true) && schedule.renewDue(25001,true));
    for(const scenario of ['late reply','window boundary','stronger anchor','remaining reuse']){
      reset();browserTime=0;
      let posts=0,reads=0;
      const expiry={'late reply':3100,'window boundary':10000,'stronger anchor':3108,'remaining reuse':10000};
      const replyMs={'late reply':2125,'window boundary':900,'stronger anchor':400,'remaining reuse':800};
      const s={...state,lease_active:true,remote_owner:true,now_ms:'1000',lease_expires_ms:String(expiry[scenario])};
      const c=new Controller(async(url,options)=>{
        if(url==='/api/v1/state'){
          reads++;
          let protocol={...s};
          if(scenario==='stronger anchor' && reads===2){
            protocol.now_ms='1005';browserTime=1705;
          }else if(posts)protocol={...s,now_ms:String(1000+browserTime),lease_active:false};
          return new Response(JSON.stringify({protocol}));
        }
        assert.equal(url,'/api/v1/commands');
        const input=JSON.parse(options.body);
        assert.equal(input.op,'mouse.move_relative');posts++;
        const atDevice=1165;
        browserTime+=replyMs[scenario];
        return new Response(JSON.stringify({state:'rejected',error:'expired',next_seq:input.seq+1,
          command_key:'2',protocol:{...s,now_ms:String(atDevice)}}),{status:400});
      });
      c.session={...session};c.operations.set('mouse.move_relative',true);
      c.pin={longLease:false,lease_id:s.lease_id,target_id:s.target_id,epoch:s.epoch,transport_generation:s.transport_generation};c.controlled=true;
      await c.refresh();browserTime=5;
      if(scenario==='stronger anchor'){
        // The older clock sample establishes that renewal is already due.
        // An earlier in-flight reply with only the weaker clock must still
        // require a fresh read when it arrives inside the 900 ms window.
        await c.refresh();browserTime+=5;
        assert(c.renewDue());
        const started=browserTime;browserTime+=replyMs[scenario];
        assert.equal(c.observeState({...s,now_ms:'2800'},s.epoch,started,{inline:true}),null);
        assert.equal(posts,0);
        continue;
      }
      const sample={current:()=>true,takeSample:()=>({args:{dx:1,dy:0},expiresAt:browserTime+150})};
      const before=reads;
      await c.run('mouse.move_relative',{},undefined,sample);
      if(scenario==='remaining reuse'){
        assert(c.controlled && reads===before);
        assert.equal(c.stateAt,805);assert.equal(c.clockSamples.at(-1).time,1165n);
        browserTime=906;
      }else assert(!c.controlled && reads===before+1,scenario);
      await assert.rejects(c.run('mouse.move_relative',{},undefined,sample));
      assert(!c.controlled && posts===1,scenario);
    }
  }finally{
    if(realNow)Object.defineProperty(performance,'now',realNow);
    else delete performance.now;
  }
  console.log('PASS expired snapshot freshness: late POST, stronger clock anchor and remaining cache window');
  // A direct terminal reply and a polled terminal reply must be tied to the
  // submission; an invalid successful-looking reply stops the single lane.
  for(const bad of ['key','sequence','status','polled key']){
    reset();
    let submissions=0;
    const c=new Controller(async(url,options)=>{
      const receipt={state:'sent',error:'ok',command_key:'2',next_seq:seq+1};
      if(url==='/api/v1/commands' && JSON.parse(options.body).op==='mouse.move_relative'){
        submissions++;
        if(bad==='polled key')return new Response(JSON.stringify({...receipt,state:'accepted'}),{status:202});
        if(bad==='key')receipt.command_key=null;
        if(bad==='sequence')receipt.next_seq=seq;
        return new Response(JSON.stringify(receipt),{status:bad==='status'?503:200});
      }
      if(url.startsWith('/api/v1/requests/'))return new Response(JSON.stringify({...receipt,command_key:'3'}));
      return fetch(origin+url,options);
    });
    await c.login();await c.acquire();
    const sample={current:()=>true,takeSample:()=>({args:{dx:1,dy:0},expiresAt:performance.now()+150})};
    await assert.rejects(c.run('mouse.move_relative',{},undefined,sample));
    assert(c.uncertain && !c.controlled);
    await assert.rejects(c.acquire());assert.equal(submissions,1);
    await c.disconnect();
  }
  // An expired POST can coincide with a real takeover, or be followed by
  // an unavailable state read. An uncorrelated rejection is an unknown reply.
  for(const inline of [false,true])for(const change of ['lease_active','lease_id','target_id','epoch','transport_generation','locked','release_pending','remote_allowed','input_ready','read-error','bad-key','bad-sequence','lost','malformed','late-stop']){
    if(!inline && ['malformed','late-stop'].includes(change))continue;
    reset();
    let moves=0,readsAfterMove=0;
    const changeClient=new Controller(async (url,options)=>{
      if(url==='/api/v1/commands' && JSON.parse(options.body).op==='mouse.move_relative'){
        moves++;
        assert.equal(options.headers['X-Passport-Expired-State'],inline?'1':undefined);
        if(change==='lost')throw new TypeError('Response lost');
        if(change==='locked')locked=true;
        else if(change==='release_pending')state.release_pending=true;
        else if(['lease_active','remote_allowed','input_ready'].includes(change))state[change]=false;
        else if(['target_id','lease_id','epoch','transport_generation'].includes(change))state[change]='9';
        const receipt={state:'rejected',error:'expired',command_key:change==='bad-key'?null:'4',next_seq:change==='bad-sequence'?seq:seq+1};
        if(inline && change!=='read-error')receipt.protocol=change==='malformed'?null:{...state,locked};
        if(inline && change==='locked')await pause(40);
        if(change==='late-stop')await changeClient.disconnect();
        return new Response(JSON.stringify(receipt),{status:400});
      }
      if(moves && url==='/api/v1/state'){
        readsAfterMove++;
        if(change==='read-error')return new Response('{}',{status:401});
      }
      return fetch(origin+url,options);
    });
    await changeClient.login();await changeClient.acquire();
    const sample={current:()=>true,takeSample:()=>({args:{dx:2,dy:0},expiresAt:performance.now()+150})};
    const moving=changeClient.run('mouse.move_relative',{dx:2,dy:0},undefined,inline?sample:undefined);
    if(['read-error','bad-key','bad-sequence','lost','late-stop','malformed'].includes(change))await assert.rejects(moving);
    else await moving;
    assert.equal(changeClient.controlled,false,change);
    assert.equal(changeClient.uncertain,['bad-key','bad-sequence','lost','late-stop'].includes(change),change);
    if(inline)assert.equal(readsAfterMove,change==='read-error'?1:0,change);
    if(inline && change==='locked')assert(changeClient.clockSamples.at(-1).rtt>=39);
    if(change==='late-stop')assert.equal(changeClient.state,null);
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
