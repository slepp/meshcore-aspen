// Run the shipped host page in system Chrome against the scoped HTTP contract.
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import http from 'node:http';
import crypto from 'node:crypto';
import {spawn} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../../..');
const scratch=path.join(root,'.tmp',`host-admin-browser-${process.pid}`);
const page=await fs.readFile(path.join(root,'internal/app/admin_page.html'),'utf8');
const requests=[],token='fixture-token-'.repeat(4);
const session='ab'.repeat(32);
let connected=false;
let owner=true,timeout=false,failedHash=false,upload=null,source=Buffer.from('old'),gen=1;
let roleScopes=[],roleTimeout=false;
const roleStates={room:{name:'Birch Room',repeat:'off'},repeater:{name:'Birch Relay',repeat:'off'}},roleRequests=[],roleOverrides=new Map();
const hash=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
const server=http.createServer(async(req,res)=>{
  res.setHeader('Cache-Control','no-store');
  res.setHeader('Content-Security-Policy',"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
  if(req.url==='/admin'){res.end(page);return;}
  if(req.url==='/admin/login'){
    let credential='';for await(const chunk of req)credential+=chunk;
    if(credential!==token||req.headers['x-host-intent']!=='admin-v1'){res.writeHead(401);res.end('Authentication required');return;}
    connected=true;res.end(session);return;
  }
  if(!connected||req.headers['x-host-admin']!==session||req.headers['x-host-intent']!=='admin-v1'){res.writeHead(401);res.end('Authentication required');return;}
  if(req.url==='/admin/logout'){connected=false;res.end('Logged out');return;}
  if(req.url==='/admin/status'){res.end(JSON.stringify({roles:{bot:{state:'running'}},capabilities:{native_owner:owner,role_owner_commands:roleScopes}}));return;}
  let text='';for await(const chunk of req)text+=chunk;
  requests.push(text);
  if(req.url.startsWith('/admin/role/')){
    const role=req.url.split('/').at(-1);
    if(!roleScopes.includes(role)){res.writeHead(404);res.end('Role owner unavailable');return;}
    roleRequests.push({role,text});
    if(roleOverrides.has(role+'/'+text)){
      const reply=roleOverrides.get(role+'/'+text);
      if(reply.startsWith('Error'))res.writeHead(409);
      res.end(reply);return;
    }
    if(text.startsWith('set name '))roleStates[role].name=text.slice(9);
    if(text.startsWith('set repeat '))roleStates[role].repeat=text.slice(11);
    if(roleTimeout){res.writeHead(504);res.end('Role outcome unknown');return;}
    if(text==='get name'){res.end('> '+roleStates[role].name);return;}
    if(text==='get repeat'){res.end('> '+roleStates[role].repeat);return;}
    res.end(text.startsWith('set ')?'ok':'Role readback: '+text);return;
  }
  if(timeout){res.writeHead(504);res.end('Host owner timeout; outcome unknown');return;}
  if(text==='source api'){res.end('API named-commands-v1 lua=5.5.1 commands=8');return;}
  if(text==='source status'){res.end(`gen=${gen} active=0 prev=3 size=${source.length} upload=none next=0/0; source durably saved and active`);return;}
  if(text==='source hash'){
    if(failedHash){res.writeHead(409);res.end('Error: durable hash unavailable');return;}
    res.end(`SHA256 ${hash(source)} gen=${gen}`);return;
  }
  const words=text.split(' ');
  if(text.startsWith('source begin ')){
    if(!upload)upload={id:words[2],size:+words[3],hash:words[4],bytes:Buffer.alloc(+words[3]),next:0};
    res.end(`ACK ${upload.id} next=${upload.next}`);return;
  }
  if(text.startsWith('source chunk ')){
    Buffer.from(words[4],'hex').copy(upload.bytes,+words[3]*48);upload.next++;res.end(`ACK ${upload.id} next=${upload.next}`);return;
  }
  if(text.startsWith('source commit ')){
    assert.equal(hash(upload.bytes),upload.hash);source=upload.bytes;gen++;upload=null;res.end('Accepted verification');return;
  }
  if(text.startsWith('data export ')){res.writeHead(409);res.end('Error: scope unavailable');return;}
  res.end('Readback: '+text);
});
let chrome,socket,closed;
try{
  await fs.mkdir(scratch,{recursive:true});
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  chrome=spawn(process.env.CHROME||'/usr/bin/google-chrome',['--headless=new','--no-sandbox','--disable-gpu','--disable-dev-shm-usage','--no-first-run',
    '--remote-debugging-port=0',`--user-data-dir=${path.join(scratch,'profile')}`,'about:blank'],
  {cwd:root,env:{...process.env,TMPDIR:scratch},stdio:['ignore','ignore','pipe']});
  closed=new Promise(resolve=>chrome.once('close',resolve));
  let stderr='';chrome.stderr.on('data',data=>{stderr=(stderr+data).slice(-4000);});
  let port;
  for(let n=0;n<150&&!port;n++){try{port=+(await fs.readFile(path.join(scratch,'profile/DevToolsActivePort'),'utf8')).split('\n')[0];}catch{await delay(100);}}
  assert(port,'Chrome startup failed: '+stderr);
  const target=await(await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(`http://127.0.0.1:${server.address().port}/admin`)}`,{method:'PUT'})).json();
  socket=new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve,reject)=>{socket.addEventListener('open',resolve,{once:true});socket.addEventListener('error',reject,{once:true});});
  let sequence=0;const calls=new Map();
  socket.addEventListener('message',event=>{
    const message=JSON.parse(event.data),call=calls.get(message.id);
    if(call){calls.delete(message.id);clearTimeout(call.timer);message.error?call.reject(Error(message.error.message)):call.resolve(message.result);}
  });
  const evaluate=expression=>new Promise((resolve,reject)=>{
    const id=++sequence,timer=setTimeout(()=>{calls.delete(id);reject(Error('Chrome evaluate timeout'));},15000);
    calls.set(id,{timer,reject,resolve:reply=>reply.exceptionDetails?reject(Error(reply.exceptionDetails.exception?.description)):resolve(reply.result.value)});
    socket.send(JSON.stringify({id,method:'Runtime.evaluate',params:{expression,returnByValue:true,awaitPromise:true}}));
  });
  for(let n=0;n<100;n++){if(await evaluate(`typeof action==='function'`))break;await delay(50);}
  await evaluate(`window.confirm=()=>true;window.__errors=[];window.addEventListener('error',e=>window.__errors.push(e.message));`);
  const idle=async()=>{for(let n=0;n<600;n++){if(!await evaluate('busy'))return;await delay(25);}throw Error('UI remained busy');};
  const click=async id=>{await evaluate(`document.getElementById('${id}').click()`);await idle();};
  assert(await evaluate(`$('monitor').disabled`));
  await evaluate(`$('token').value='wrong'`);await click('login');
  assert.match(await evaluate(`$('result').textContent`),/Authentication required/);
  owner=false;await evaluate(`$('token').value=${JSON.stringify(token)}`);await click('login');
  assert(await evaluate(`$('upload').disabled`),'Unsupported native owner must capability-disable writes');
  assert(await evaluate(`$('role-name-save').disabled`),'No role provider must capability-disable role controls');
  roleScopes=['room','repeater'];await click('monitor');
  assert.equal(await evaluate(`$('role-name-save').disabled`),false,'Room/repeater controls must not require a native bot');
  for(const secret of ['set prv.key '+ 'a'.repeat(128),'password secret','set guest.password secret','set radio 912525000,250000,7,5','reboot']){
    const sent=requests.length;
    await evaluate(`$('role-command').value=${JSON.stringify(secret)}`);await click('role-run');
    assert.equal(requests.length,sent,'Restricted role operations must not reach HTTP');
  }
  await evaluate(`$('host-role').value='room';$('host-role-name').value='Birch Updated';controls()`);
  roleTimeout=true;await click('role-name-save');roleTimeout=false;
  assert(await evaluate('uncertain'));
  const roleWrites=roleRequests.filter(r=>r.text.startsWith('set ')).length;
  await click('role-name-save');assert.equal(roleRequests.filter(r=>r.text.startsWith('set ')).length,roleWrites);
  await evaluate(`$('host-role').value='repeater';controls()`);await click('role-read');
  assert(await evaluate('uncertain'),'A successful different-role readback cannot unlock the pending room write');
  roleOverrides.set('room/get name','Error: readback unavailable');await click('inspect');
  assert(await evaluate('uncertain'));
  roleOverrides.set('room/get name','incomplete');await click('inspect');assert(await evaluate('uncertain'));
  roleOverrides.delete('room/get name');await click('inspect');
  assert.equal(await evaluate('uncertain'),false);
  assert.equal(roleRequests.at(-1).role,'room','Inspection targets the affected role, not the selector');
  assert.equal(roleRequests.filter(r=>r.text.startsWith('set ')).length,roleWrites,'Inspection never repeats role mutations');
  assert.equal(await evaluate(`$('host-role-name').value`),'Birch Updated','Role draft is retained during unknown outcomes');
  await evaluate(`$('host-repeat').value='on'`);await click('role-repeat-save');
  assert.equal(roleStates.repeater.repeat,'on');
  owner=true;await click('monitor');
  assert.equal(await evaluate(`$('upload').disabled`),false);
  assert.equal(await evaluate(`$('token').value`),'','Credential is cleared after session login');
  assert.equal(await evaluate('token'),session,'Requests use session, not the login password');
  for(const secret of ['bot https token home secret','key bot '+ 'a'.repeat(128),'wifi password secret','role password bot secret']){
    const sent=requests.length;
    await evaluate(`$('command').value=${JSON.stringify(secret)}`);await click('run');
    assert.equal(requests.length,sent,'Restricted secret commands must not reach plaintext HTTP');
    assert.match(await evaluate(`$('result').textContent`),/no request sent/);
  }
  await evaluate(`$('source').value="function hello(name) reply(name) end"`);await click('upload');
  assert.equal(requests.filter(x=>x.startsWith('source commit ')).length,0);
  await evaluate(`$('source').value='KEEP NEWER DRAFT'`);await click('install');
  assert.equal(source.toString(),"function hello(name) reply(name) end");
  assert.equal(await evaluate(`$('source').value`),'KEEP NEWER DRAFT');
  assert.equal(await evaluate('uncertain'),false);
  await evaluate(`$('command').value='data export kv caller ${'1'.repeat(64)}'`);await click('run');
  assert.match(await evaluate(`$('result').textContent`),/Error: scope unavailable/);
  timeout=true;await evaluate(`$('name').value='Birch'`);await click('name-save');timeout=false;
  assert(await evaluate('uncertain'));
  const writes=requests.length;await click('name-save');assert.equal(requests.length,writes);
  await click('inspect');assert(await evaluate('uncertain'),'Unrelated bot/data status cannot unlock a lost name write');
  await evaluate(`pending={command:'source commit ${hash(source).slice(0,16)}',snapshot:{id:'${hash(source).slice(0,16)}',hash:'${hash(source)}',size:${source.length}}}`);
  failedHash=true;await click('inspect');assert(await evaluate('uncertain'));
  failedHash=false;await click('inspect');assert.equal(await evaluate('uncertain'),false);
  await evaluate('expires=Date.now()-1');await click('monitor');
  assert.match(await evaluate(`$('result').textContent`),/session expired/);
  await evaluate(`$('token').value=${JSON.stringify(token)}`);await click('login');
  await click('logout');assert(await evaluate(`$('upload').disabled`));
  assert.equal(await evaluate(`$('source').value`),'KEEP NEWER DRAFT');
  assert.deepEqual(await evaluate('window.__errors'),[]);
  console.log('PASS system Chrome host /admin: token auth, capabilities, native errors, durable upload/install, snapshot/draft separation and uncertain readback fence');
}finally{
  socket?.close();
  if(chrome){if(chrome.exitCode===null)chrome.kill('SIGTERM');await Promise.race([closed,delay(5000)]);if(chrome.exitCode===null)chrome.kill('SIGKILL');await closed;}
  server.closeAllConnections();await new Promise(resolve=>server.close(resolve));
  await fs.rm(scratch,{recursive:true,force:true});
}
