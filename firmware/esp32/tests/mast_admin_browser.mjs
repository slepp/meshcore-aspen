// SPDX-License-Identifier: Apache-2.0
// Run the shipped /admin page in system Chrome against a local native-contract fixture.
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import http from 'node:http';
import crypto from 'node:crypto';
import {spawn} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';

const directory=path.dirname(fileURLToPath(import.meta.url));
const root=path.resolve(directory,'../../..');
const scratch=path.join(root,'.tmp',`mast-admin-browser-${process.pid}`);
const page=(await fs.readFile(path.join(directory,'../MastAdminPage.h'),'utf8')).split('R"html(')[1].split(')html";')[0];
const hash=b=>crypto.createHash('sha256').update(b).digest('hex');
const prefix="function hello(name) reply('Hello '..name) end\n--";
let source=Buffer.from(prefix+'x'.repeat(4096-prefix.length)), upload=null, generation=1, timeout=false, session=false;
const overrides=new Map();
const requests=[],errors=[];
const token='ab'.repeat(16);
let updateState={state:'idle',received:0,size:0,error:'',target:'xiao-esp32s3',boot_health:'healthy',recovery:'',reboot_ready:false,rollback_supported:true,rollback_ready:false,sha256:'',running_sha256:'0'.repeat(64),running_size:0,running_hash_error:''};
let updateReplyLost=false;
const native=text=>{
  if(overrides.has(text)) return overrides.get(text);
  if(text==='status') return 'roles applied=7 saved=3 generation=18446744073709551614 bot-saved=1 PHY=912525000,250000,7,5,2 effective-gen=1 temp=0';
  if(text==='bot status') return 'bot applied=1 saved=1 ready=1 state=ready';
  if(text==='role name repeater') return 'Name: <img src=x onerror=alert(1)>';
  if(text==='source api') return 'API named-commands-v1 lua=5.5.1 commands=8 arguments=4 source-bytes=4096 runtime=shared-v1';
  if(text==='source api package') return 'Package api=named-commands-v1 caps=cmdmeta,modules,kv,timers,reminders';
  if(text==='source api modules') return 'Modules declare=module require=declared-only count=8 envelope=4096';
  if(text==='source metadata') return 'PLAIN schema=unknown';
  if(text==='source hash') return `SHA256 ${hash(source)} gen=${generation}`;
  if(text==='source status') return `gen=${generation} active=0 prev=3 size=${source.length} upload=${upload?.id||'none'} next=${upload?.next||0}/${upload?Math.ceil(upload.size/48):0}; source durably saved and active`;
  if(text.startsWith('source read ')) return 'DATA '+source.subarray(+text.split(' ')[2]*48,(+text.split(' ')[2]+1)*48).toString('hex');
  if(text.startsWith('source begin ')) {
    const [, ,id,size,digest]=text.split(' ');
    if(upload && (upload.id!==id || upload.size!==+size || upload.hash!==digest)) return 'Error: another upload exists; cancel explicitly';
    upload ||= {id,size:+size,hash:digest,next:0,bytes:Buffer.alloc(+size)};
    return `ACK ${id} next=${upload.next}`;
  }
  if(text.startsWith('source chunk ')) {
    const [, ,id,index,bytes]=text.split(' ');
    assert.equal(id,upload.id); assert.equal(+index,upload.next);
    Buffer.from(bytes,'hex').copy(upload.bytes,+index*48); upload.next++; return `ACK ${id} next=${upload.next}`;
  }
  if(text.startsWith('source commit ')) {
    assert.equal(hash(upload.bytes),upload.hash); source=upload.bytes; generation++; upload=null;
    return 'Accepted verification; source status reports durable activation outcome';
  }
  if(text.startsWith('radio ')) return 'Accepted radio change after old-PHY reply; queued old-PHY jobs will fail STALE';
  if(text==='data status') return 'EMPTY ';
  if(text==='job') return 'Idle';
  return 'Readback: '+text;
};
const server=http.createServer(async(req,res)=>{
  try {
    res.setHeader('Cache-Control','no-store');
    res.setHeader('X-Content-Type-Options','nosniff');
    res.setHeader('Content-Security-Policy',"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
    if(req.url==='/admin' && req.method==='GET') {res.setHeader('Content-Type','text/html');res.end(page);return;}
    const chunks=[]; for await(const chunk of req) chunks.push(chunk);
    const bytes=Buffer.concat(chunks),text=bytes.toString();
    requests.push({url:req.url,text});
    if(req.url==='/admin/login') {session=true;res.end(token);return;}
    if(!session || req.headers['x-mast-session']!==token) {res.writeHead(403);res.end('Authenticated session required');return;}
    if(req.url==='/admin/logout') {session=false;res.end('Logged out');return;}
    if(req.url==='/admin/update' && req.method==='GET') {res.end(JSON.stringify(updateState));return;}
    if(req.url==='/admin/update' && req.method==='POST') {
      assert.equal(req.headers['content-type'],'application/octet-stream');
      assert.equal(req.headers['x-mast-target'],'xiao-esp32s3');
      assert.equal(req.headers['x-mast-sha256'],hash(bytes));
      assert.equal(req.headers['x-mast-signature'],'00'.repeat(64));
      updateState={...updateState,state:'verified',received:bytes.length,size:bytes.length,sha256:hash(bytes),reboot_ready:true};
      if(updateReplyLost){res.writeHead(504);res.end('Upload reply lost; outcome unknown');return;}
      res.end(JSON.stringify(updateState));return;
    }
    if(req.url==='/admin/update/reboot') {
      assert.equal(bytes.length,0);res.end(JSON.stringify(updateState));
      updateState={...updateState,state:'idle',size:0,received:0,reboot_ready:false,boot_health:'pending',running_sha256:updateState.sha256,running_size:updateState.size};
      return;
    }
    assert.equal(req.url,'/admin/command'); assert(/^[\x20-\x7e]{1,162}$/.test(text));
    if(timeout) {res.writeHead(504);res.end('Command outcome unknown; inspect status before retry');return;}
    res.end(native(text));
  } catch(e) {errors.push(e);res.writeHead(500);res.end('Fixture error');}
});
let chrome, socket, chromeClosed;
try {
  await fs.mkdir(scratch,{recursive:true});
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const address=`http://127.0.0.1:${server.address().port}/admin`;
  chrome=spawn(process.env.CHROME || '/usr/bin/google-chrome',[
    '--headless=new','--no-sandbox','--disable-gpu','--disable-dev-shm-usage','--no-first-run','--no-default-browser-check',
    '--remote-debugging-port=0',`--user-data-dir=${path.join(scratch,'profile')}`,'about:blank',
  ],{cwd:root,env:{...process.env,TMPDIR:path.relative(root,scratch)},stdio:['ignore','ignore','pipe']});
  chromeClosed=new Promise(resolve=>chrome.once('close',resolve));
  let stderr=''; chrome.stderr.on('data',data=>{stderr=(stderr+data.toString()).slice(-4000);});
  const startup=Date.now(); let port;
  while(!port && Date.now()-startup<15000) {
    try {port=+(await fs.readFile(path.join(scratch,'profile/DevToolsActivePort'),'utf8')).split('\n')[0];}
    catch {await delay(100);}
    if(chrome.exitCode!==null) throw Error('Chrome exited: '+stderr);
  }
  assert(port,'Chrome did not expose its local debugging port: '+stderr);
  const target=await (await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(address)}`,{method:'PUT'})).json();
  socket=new WebSocket(target.webSocketDebuggerUrl);
  const calls=new Map(); let sequence=0;
  socket.addEventListener('message',event=>{
    const message=JSON.parse(event.data), call=calls.get(message.id);
    if(call) {calls.delete(message.id);clearTimeout(call.timer);message.error?call.reject(Error(message.error.message)):call.resolve(message.result);}
  });
  await new Promise((resolve,reject)=>{socket.addEventListener('open',resolve,{once:true});socket.addEventListener('error',reject,{once:true});});
  const cdp=(method,params={})=>new Promise((resolve,reject)=>{
    const id=++sequence, timer=setTimeout(()=>{calls.delete(id);reject(Error('Chrome timed out: '+method+' '+(params.expression||'')));},10000);
    calls.set(id,{resolve,reject,timer});socket.send(JSON.stringify({id,method,params}));
  });
  const evaluate=async expression=>{
    const reply=await cdp('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});
    if(reply.exceptionDetails) throw Error(reply.exceptionDetails.text+': '+reply.exceptionDetails.exception?.description);
    return reply.result.value;
  };
  await cdp('Page.enable');
  await cdp('Runtime.enable');
  await cdp('Page.setDownloadBehavior',{behavior:'allow',downloadPath:scratch});
  for(let n=0;n<100;n++) {if(await evaluate(`typeof action==='function'`)) break;await delay(25);}
  assert(await evaluate(`typeof action==='function'`),'Page script did not initialize');
  await evaluate(`window.__confirms=[]; window.confirm=m=>{window.__confirms.push(m);return true;}; window.__alerts=[]; window.alert=m=>window.__alerts.push(m); window.__errors=[]; window.addEventListener('error',e=>window.__errors.push(e.message));`);
  const idle=async()=>{
    for(let n=0;n<600;n++) {if(!await evaluate('busy')) return;await delay(25);}
    throw Error('Browser UI remained busy');
  };
  const click=async id=>{await evaluate(`document.getElementById(${JSON.stringify(id)}).click()`);await idle();};
  const text=id=>evaluate(`document.getElementById(${JSON.stringify(id)}).textContent`);
  assert(await evaluate(`document.getElementById('refresh').disabled`));
  assert.equal(await evaluate(`document.getElementById('source').disabled`),false);
  await evaluate(`document.getElementById('password').value='browser-pass'`);
  await click('login');
  assert.equal(await evaluate(`document.getElementById('password').value`),'');
  assert.equal(await evaluate(`document.getElementById('refresh').disabled`),false);
  assert.match(await text('role-generation'),/18446744073709551614/);
  await click('role-read');
  assert.match(await text('role-detail'),/<img src=x onerror=alert\(1\)>/);
  assert.equal(await evaluate(`document.querySelectorAll('img').length`),0);
  await click('inventory'); await click('source-read');
  assert.equal(await evaluate(`document.getElementById('source').value`),source.toString());
  assert.equal(requests.filter(r=>r.text.startsWith('source read ')).length,86);
  assert(!requests.some(r=>r.text==='source read 86'));
  await evaluate(`document.getElementById('source').value="function hello(name) reply(name) end";document.getElementById('source').dispatchEvent(new Event('input'));`);
  await click('source-stage');
  assert.equal(requests.filter(r=>r.text.startsWith('source commit ')).length,0);
  await evaluate(`document.getElementById('source').value='KEEP NEWER EDITS'`);
  await click('source-install');
  assert.equal(source.toString(),"function hello(name) reply(name) end");
  assert.equal(await evaluate(`document.getElementById('source').value`),'KEEP NEWER EDITS');
  assert.match(await text('result'),/durably saved and active/);
  await click('radio-copy');
  timeout=true;
  const before=requests.length; await click('radio-save');
  assert.equal(requests.length,before+1); assert.match(await text('result'),/unknown/);
  timeout=false;
  const blocked=requests.length; await click('radio-save'); assert.equal(requests.length,blocked);
  overrides.set('job','Error: radio status unavailable');
  await click('inspect');
  assert(await evaluate('uncertain'),'Command-level error cannot unlock mutations');
  assert.match(await text('result'),/Error: radio status unavailable/);
  overrides.set('job','Readback: job');
  await click('inspect');
  assert(await evaluate('uncertain'),'Unrecognized job readback cannot confirm outcome');
  overrides.delete('job');
  overrides.set('status','roles applied=7');
  await click('inspect');
  assert(await evaluate('uncertain'),'Incomplete status cannot unlock mutations');
  overrides.delete('status');
  await click('inspect');
  assert(await evaluate('uncertain'),'Effective PHY cannot confirm a durable radio save without saved-PHY readback');
  await evaluate(`unresolved.splice(0);uncertain=false;updateControls()`); // Isolate independent source/data fixture cases.
  await evaluate(`uploaded={id:'1234567890abcdef',hash:'${hash(source)}',size:${source.length},runtime:'wasm'};
    inFlight={command:'source wasm commit 1234567890abcdef',runtime:'wasm',mutation:true,snapshot:{...uploaded}};
    unknown('Wasm commit reply lost'); inFlight=null; document.getElementById('program-runtime').value='lua'`);
  overrides.set('source wasm status','Error: Wasm unavailable');
  const wasmReads=requests.length;
  await click('inspect');
  assert(requests.slice(wasmReads).some(r=>r.text==='source wasm status'),'Inspect must target uncertain Wasm, not selected Lua');
  assert(await evaluate('uncertain'));
  overrides.set('source wasm status','source durably saved and active');
  overrides.set('source wasm hash',`SHA256 ${hash(source)} gen=2`);
  await click('inspect');
  assert(await evaluate('uncertain'),'Terminal prose without a complete source status cannot resolve a lost commit');
  overrides.set('source wasm status',`gen=2 active=0 prev=3 size=${source.length} upload=none next=0/0; source durably saved and active`);
  overrides.set('source wasm hash',`SHA256 ${hash(source)} gen=2`);
  await evaluate(`inFlight={command:'data restore fedcba0987654321',runtime:'lua',mutation:true,
    restore:{id:'fedcba0987654321',kind:'kv',scope:'caller',principal:'${'1'.repeat(64)}'}};
    unknown('Caller restore lost'); inFlight=null;`);
  overrides.set('data status','COMMITTED 0000000000000000');
  await click('inspect');
  assert(await evaluate('uncertain'),'One confirmed runtime cannot unlock a different unresolved scope');
  assert.equal(await evaluate('unresolved.length'),1);
  assert.match(await text('result'),/caller/);
  overrides.set('data status','COMMITTED fedcba0987654321');
  await click('inspect');
  assert.equal(await evaluate('uncertain'),false);
  assert.equal(await evaluate(`document.getElementById('source').value`),'KEEP NEWER EDITS');
  await evaluate(`document.getElementById('command').value='telemetry endpoint token begin abc'`);
  const secret=requests.length; await evaluate(`document.getElementById('command-form').requestSubmit()`); await idle();
  assert.equal(requests.length,secret);
  assert.match(await text('result'),/unavailable over HTTP/);
  for(const command of ['bot https   token home 736563726574','wifi password 70617373776f7264',
    'wifi 4d657368 70617373776f7264','role channel bot 0 2374657374 00112233445566778899aabbccddeeff',
    'key bot malformed-private-input']) {
    const sent=requests.length, confirms=await evaluate('window.__confirms.length');
    await evaluate(`document.getElementById('command').value=${JSON.stringify(command)};document.getElementById('command-form').requestSubmit()`);
    await idle();
    assert.equal(requests.length,sent,'Secret command must not reach HTTP');
    assert.equal(await evaluate('window.__confirms.length'),confirms,'Secret input must not become confirmation text');
    assert.match(await text('result'),/HTTP|unavailable/);
  }
  for(const command of ['bot https','bot https  status','password','password help','wifi status','role channel bot 0','key bot pending',
    'stats','stats sensors','stats  memory','stats observer','stats companion','get stats','help stats']) {
    const sent=requests.length, confirms=await evaluate('window.__confirms.length');
    await evaluate(`document.getElementById('command').value=${JSON.stringify(command)};document.getElementById('command-form').requestSubmit()`);
    await idle();
    assert.equal(requests.length,sent+1,'Safe status/help command should reach native authenticated HTTP');
    assert.equal(await evaluate('window.__confirms.length'),confirms,'Safe reads must not need write confirmation');
  }
  timeout=true;
  await evaluate(`$('command').value='stats memory';$('command-form').requestSubmit()`);await idle();
  assert.equal(await evaluate('uncertain'),false,'An uncertain diagnostic read must not create a mutation fence');
  assert.equal(await evaluate('unresolved.length'),0);
  timeout=false;
  await click('firmware-status');
  assert.match(await text('firmware-detail'),/Update: No update in progress/);
  assert.match(await text('firmware-detail'),/Boot health: Healthy/);
  assert(!await evaluate(`$('firmware-detail').textContent.includes('"reboot_ready"')`),'Status should explain the update, not dump API fields');
  const image=Buffer.alloc(128);image[0]=0xe9;image[12]=9;
  Buffer.from('meshcore-esp-target-v1:xiao-esp32s3:').copy(image,32);
  const meta={target:'wrong-target',size:image.length,sha256:hash(image),signature:'00'.repeat(64)};
  const selectFirmware=async manifest=>evaluate(`{
    const binary=new DataTransfer(),signed=new DataTransfer();
    binary.items.add(new File([Uint8Array.from(atob('${image.toString('base64')}'),c=>c.charCodeAt(0))],'application.bin'));
    signed.items.add(new File([${JSON.stringify(JSON.stringify(manifest))}],'manifest.json',{type:'application/json'}));
    $('firmware-image').files=binary.files; $('firmware-manifest').files=signed.files;
  }`);
  await selectFirmware(meta);
  const beforeUpdate=requests.length;await click('firmware-upload');
  assert.equal(requests.length,beforeUpdate,'Mismatched signed target must never be uploaded');
  meta.target='xiao-esp32s3';
  await selectFirmware({...meta,private_key:'fixture-private-material'});
  await click('firmware-upload');
  assert.equal(requests.length,beforeUpdate,'Private key material is not an accepted manifest and must never be uploaded');
  await selectFirmware(meta);
  updateReplyLost=true;await click('firmware-upload');updateReplyLost=false;
  assert(await evaluate('uncertain'));
  assert.equal(requests.filter(r=>r.url==='/admin/update'&&r.text).length,1,'Lost upload must not be replayed');
  await click('inspect');
  assert.equal(await evaluate('uncertain'),false,'Matching verified image hash resolves upload reply loss');
  assert.match(await text('firmware-detail'),/Verified; ready to restart/);
  assert.equal(await evaluate(`$('firmware-reboot').disabled`),false);
  await click('firmware-reboot');
  assert(await evaluate('uncertain'),'Reboot admission is not running/healthy confirmation');
  await click('inspect');
  assert(await evaluate('uncertain'),'Running image with pending health must not confirm healthy boot');
  assert.match(await text('firmware-detail'),/Waiting for startup checks/);
  updateState.boot_health='healthy';updateState.running_sha256='0'.repeat(64);await click('inspect');
  assert(await evaluate('uncertain'),'Healthy old-image rollback must not confirm the intended image');
  updateState.running_sha256=meta.sha256;updateState.state='verified';updateState.reboot_ready=true;updateState.size=image.length;updateState.received=image.length;
  await click('inspect');
  assert(await evaluate('uncertain'),'A same-hash image already running before the scheduled reboot is not reboot completion');
  updateState.state='idle';updateState.reboot_ready=false;updateState.size=0;updateState.received=0;updateState.running_hash_error='Read failed';await click('inspect');
  assert(await evaluate('uncertain'),'Failed running hash readback must not confirm an image');
  updateState.running_hash_error='';delete updateState.running_size;await click('inspect');
  assert(await evaluate('uncertain'),'Missing running image byte count must not confirm reboot');
  assert.match(await text('firmware-detail'),/size unavailable/);
  assert(!(await text('firmware-detail')).includes('undefined'),'Missing fields must not become invented readings');
  updateState.running_size=image.length-1;await click('inspect');
  assert(await evaluate('uncertain'),'Mismatched running image byte count must not confirm reboot');
  updateState.running_size=image.length;await click('inspect');
  assert.equal(await evaluate('uncertain'),false,'Exact running image and health confirms reboot outcome');
  assert.equal(requests.filter(r=>r.url==='/admin/update/reboot').length,1);
  assert.equal(await evaluate(`$('source').value`),'KEEP NEWER EDITS');
  await click('logout');
  assert(await evaluate(`document.getElementById('refresh').disabled`));
  assert.equal(await evaluate(`document.getElementById('source-draft').disabled`),false,'Draft can be downloaded after logout');
  assert.equal(await evaluate(`document.getElementById('source').value`),'KEEP NEWER EDITS');
  assert.deepEqual(await evaluate('window.__alerts'),[]);
  assert.deepEqual(await evaluate('window.__errors'),[]);
  assert.deepEqual(errors,[]);
  console.log('PASS system Chrome /admin: DOM/forms/CSP, session/secret restrictions, Lua/Wasm/scoped readback fences, retained drafts, signed OTA upload and exact running-image/boot-health confirmation; no hardware accessed');
} finally {
  socket?.close();
  if(chrome) {
    if(chrome.exitCode===null) chrome.kill('SIGTERM');
    await Promise.race([chromeClosed,delay(5000)]);
    if(chrome.exitCode===null) chrome.kill('SIGKILL');
    await chromeClosed;
  }
  server.closeAllConnections();
  await new Promise(resolve=>server.close(resolve));
  await fs.rm(scratch,{recursive:true,force:true});
}
