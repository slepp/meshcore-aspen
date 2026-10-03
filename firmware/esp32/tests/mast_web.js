// SPDX-License-Identifier: Apache-2.0
// Execute the actual shipped script with a DOM and native HTTP contract fixture.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const crypto = require('node:crypto');
const page = fs.readFileSync(process.argv[2] || 'MastAdminPage.h', 'utf8');
const script = page.match(/<script>([\s\S]*?)<\/script>/)[1];
const digest = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const sourcePrefix = "function hello(name) reply('Hello '..name) end\n--";
const source4096 = Buffer.from(sourcePrefix + 'x'.repeat(4096 - sourcePrefix.length));
assert.equal(source4096.length, 4096);
const bot = '12'.repeat(32), principal = '34'.repeat(32);
function snapshot(kind='BKD',scope=0,key=principal) {
  const bytes=Buffer.alloc(2422);
  bytes.write(kind); bytes[3]=1; Buffer.from(bot,'hex').copy(bytes,4);
  bytes[36]=scope; Buffer.from(key,'hex').copy(bytes,37);
  Buffer.from(digest(bytes.subarray(0,-32)),'hex').copy(bytes,2390);
  return bytes;
}
class Element {
  constructor(id='') { this.id=id; this.value=''; this.children=[]; this.dataset={}; this.files=[]; this.disabled=false; this.checked=false; }
  appendChild(child) { this.children.push(child); }
  replaceChildren(...children) { this.children=children; }
  click() { if(this.onclick) return this.onclick(); }
  set innerHTML(value) { assert.fail('Device/source text must never be interpreted as HTML: '+value); }
}
function fixture() {
  const elements=new Map(), reads=[], writes=[], requests=[], confirms=[], downloads=[], intervals=[];
  const element=id=>{
    if(!elements.has(id)) elements.set(id,new Element(id));
    return elements.get(id);
  };
  for(const match of page.matchAll(/<(?:button|input|select|textarea|pre|span|tbody|fieldset|strong|p)[^>]*\bid="([^"]+)"[^>]*>/g)) element(match[1]);
  for(const match of page.matchAll(/<button ([^>]*\bdata-(?:read|write)="[^"]+"[^>]*)>/g)) {
    const e=new Element();
    for(const attr of match[1].matchAll(/data-(\w+)="([^"]*)"/g)) e.dataset[attr[1]]=attr[2];
    (e.dataset.read?reads:writes).push(e);
  }
  Object.entries({role:'repeater',bw:'250000',sf:'7',cr:'5',power:'2',seconds:'60',freq:'912525000',
    'data-kind':'kv','data-scope':'caller',principal,'telemetry-on':'off',interval:'60',grant:'shared','grant-on':'off'}).forEach(([k,v])=>element(k).value=v);
  const state={source:source4096,sourceGeneration:1,saved:7,applied:7,upload:null,
    data:snapshot(),dataStatus:'EMPTY ',session:false,confirm:true,api:true,hook:null,activeRequests:0,maxRequests:0,now:1000};
  state.wasmEnabled=false;
  state.wasm={source:Buffer.from([0,97,115,109,1,0,0,0]),sourceGeneration:1,upload:null};
  const native=text=>{
    if(state.override) { const answer=state.override(text); if(answer!==undefined) return answer; }
    if(text==='source api runtimes') return state.wasmEnabled?'Runtimes lua-5.5.1/named-commands-v1 wamr-2.4.1/meshcore-v1':'Runtimes lua-5.5.1/named-commands-v1';
    const wasm=text.startsWith('source wasm ');
    if(wasm && !state.wasmEnabled) return 'Error: Wasm runtime unavailable in this build';
    const selected=wasm?state.wasm:state;
    if(wasm) text=text.replace('source wasm ','source ');
    if(text==='status') return `roles applied=${state.applied} saved=${state.saved} generation=18446744073709551614 bot-saved=1 PHY=912525000,250000,7,5,2 effective-gen=9 temp=0`;
    if(text==='bot status') return 'bot applied=1 saved=1 ready=1 state=ready';
    if(text==='bot key') return 'KEY '+bot;
    if(text==='job') return 'Idle';
    if(text==='source api') return wasm?'API meshcore-v1 runtime=wamr-2.4.1 commands=8 source-bytes=4096':state.api?'API named-commands-v1 lua=5.5.1 commands=8 arguments=4 source-bytes=4096 runtime=shared-v1 jobs=4 kv=2':'API other-v2 lua=6.0';
    if(text==='source api package') return 'Package api=named-commands-v1 caps=cmdmeta,modules,kv,reminders,timers';
    if(text==='source api modules') return 'Modules declare=module require=declared-only count=8 envelope=4096';
    if(text==='source api data') return 'Data kv=1 timers=1 reminders=1 scope=single bytes=2422 version=1 owner=required restore=stage,commit scheduler=no-rearm credentials=excluded';
    if(text==='source metadata') return 'PLAIN schema=unknown';
    if(text==='source status') return `gen=${selected.sourceGeneration} active=0 prev=3 size=${selected.source.length} upload=${selected.upload?.id||'none'} next=${selected.upload?.next||0}/${selected.upload?Math.ceil(selected.upload.size/48):0}; ${selected.sourceOutcome||'source durably saved and active'}`;
    if(text==='source hash') return `SHA256 ${digest(selected.source)} gen=${selected.sourceGeneration}`;
    if(text.startsWith('source read ')) {
      const n=+text.split(' ')[2];
      assert(n<86,'Never read index 86 to find EOF at the 4 KiB boundary');
      return 'DATA '+selected.source.subarray(n*48,(n+1)*48).toString('hex');
    }
    if(text.startsWith('source begin ')) {
      const [, ,id,size,hash]=text.split(' ');
      if(selected.upload && selected.upload.hash!==hash) return 'Error: another upload exists; cancel explicitly';
      selected.upload ||= {id,size:+size,hash,next:0,bytes:Buffer.alloc(+size)};
      return `ACK ${id} next=${selected.upload.next}`;
    }
    if(text.startsWith('source chunk ')) {
      const [, ,id,index,encoded]=text.split(' '), n=+index;
      assert.equal(n,selected.upload.next); assert.equal(id,selected.upload.id);
      Buffer.from(encoded,'hex').copy(selected.upload.bytes,n*48); selected.upload.next++;
      return `ACK ${id} next=${n+1}`;
    }
    if(text.startsWith('source commit ')) {
      assert.equal(text.slice(14),selected.upload.id);
      assert.equal(digest(selected.upload.bytes),selected.upload.hash);
      selected.source=selected.upload.bytes; selected.upload=null; selected.sourceGeneration++;
      return 'Accepted verification; source status reports durable activation outcome';
    }
    if(text==='source cancel') { selected.upload=null; return 'Upload cancelled; active source retained'; }
    if(text==='source rollback' || text==='source remove') {
      state.source=Buffer.from('function recovery() reply("ready") end'); state.sourceGeneration++;
      return 'Accepted verification';
    }
    if(text==='source retry') return 'Accepted retry';
    if(text==='source helptext') return 'Error: is literal help, not a command error';
    if(text.startsWith('roles ')) { state.saved=+text.slice(6); return 'Saved roles; apply reboots into the saved profile'; }
    if(text==='role name repeater') return 'Name: <img src=x onerror=alert(1)>';
    if(text==='telemetry status') return 'Telemetry on=0 interval=60 pending=0 suspended=0 error=none http=0';
    if(text==='telemetry endpoint status') return 'Endpoint configured=0';
    if(text.startsWith('radio ') || text.startsWith('tempradio ') || text==='apply') return 'Accepted radio change after old-PHY reply; queued old-PHY jobs will fail STALE';
    if(text==='data status') return state.dataStatus;
    if(text.startsWith('data export ')) {
      const [, ,kind,scope,key]=text.split(' ');
      state.data=snapshot({kv:'BKD',timers:'BTD',reminders:'BRD'}[kind],['caller','conversation','bot','channel'].indexOf(scope),key);
      const hash=digest(state.data); state.dataStatus=`EXPORTED ${hash} ${hash.slice(0,16)}`;
      return 'PENDING ';
    }
    if(text.startsWith('data read ')) return 'DATA '+state.data.subarray(+text.split(' ')[3]*48,(+text.split(' ')[3]+1)*48).toString('hex');
    if(text.startsWith('data begin ')) {
      const [, ,id,hash]=text.split(' '); state.restore={id,hash,bytes:Buffer.alloc(2422),received:0};
      state.dataStatus='UPLOADING '+id; return `UPLOADING ${id} bytes=2422`;
    }
    if(text.startsWith('data chunk ')) {
      const [, ,id,index,encoded]=text.split(' '), bytes=Buffer.from(encoded,'hex');
      assert.equal(id,state.restore.id); assert.equal(+index*48,state.restore.received);
      bytes.copy(state.restore.bytes,+index*48); state.restore.received+=bytes.length;
      return 'RECEIVED '+state.restore.received;
    }
    if(text.startsWith('data stage ')) {
      assert.equal(state.restore.received,2422); assert.equal(digest(state.restore.bytes),state.restore.hash);
      state.dataStatus='STAGED '+state.restore.id; return 'PENDING '+state.restore.id;
    }
    if(text.startsWith('data restore ')) {
      state.dataStatus='COMMITTED '+state.restore.id; return 'PENDING '+state.restore.id;
    }
    if(text==='data clear') { state.dataStatus='EMPTY '; return 'CLEARED'; }
    return 'Readback: '+text;
  };
  const DateMock=class extends Date { static now() {return state.now;} };
  const context={
    document:{getElementById:element,createElement:()=>new Element(),querySelectorAll:q=>q==='[data-read]'?reads:q==='[data-write]'?writes:[...elements.values(),...reads,...writes]},
    window:{confirm:message=>{confirms.push(message); return state.confirm;},addEventListener(){}},
    TextEncoder,TextDecoder,Uint8Array,Uint32Array,DataView,Blob,AbortController,Date:DateMock,
    URL:{createObjectURL:b=>{downloads.push(b);return 'blob:local';},revokeObjectURL(){}},
    setTimeout:(fn,ms)=>{if(ms===500 || ms===1000) queueMicrotask(fn);return 1;},clearTimeout(){},
    setInterval:fn=>intervals.push(fn),console,
    fetch:async(path,options)=>{
      assert(path.startsWith('/admin/'),'Only same-origin native administration endpoints are used');
      assert.equal(options.redirect,'error'); assert.equal(options.credentials,'omit');
      requests.push({path,body:options.body,headers:options.headers});
      state.activeRequests++; state.maxRequests=Math.max(state.maxRequests,state.activeRequests);
      try {
        await Promise.resolve();
        if(state.hook) await state.hook(path,options.body);
        if(state.httpError) return {ok:false,status:state.httpError,text:async()=>state.httpText||'Injected backend error'};
        if(state.networkError) throw Error('Disconnected');
        if(path==='/admin/login') {state.session=true;return {ok:true,status:200,text:async()=>'ab'.repeat(16)};}
        assert.equal(options.headers['X-Mast-Session'],'ab'.repeat(16));
        if(path==='/admin/logout') {state.session=false;return {ok:true,status:200,text:async()=>'Logged out'};}
        assert(/^[\x20-\x7e]{1,162}$/.test(options.body),'Native command framing must fit');
        const result=native(options.body);
        return {ok:true,status:200,text:async()=>result};
      } finally {state.activeRequests--;}
    },
  };
  vm.createContext(context); vm.runInContext(script,context);
  const evaluate=text=>vm.runInContext(text,context);
  const run=text=>evaluate(`action('fixture',async()=>${text})`);
  const login=async()=>{
    element('password').value='mast-pass';
    element('login-form').onsubmit({preventDefault(){}});
    while(evaluate('busy')) await new Promise(resolve=>setImmediate(resolve));
    assert.equal(element('password').value,''); assert.equal(element('refresh').disabled,false);
  };
  return {element,state,requests,confirms,downloads,intervals,evaluate,run,login};
}
async function tests() {
  const ids=[...page.matchAll(/\bid="([^"]+)"/g)].map(match=>match[1]);
  assert.equal(new Set(ids).size,ids.length,'Page controls and status panels need unique IDs');
  const sections=[...page.matchAll(/<section id="([^"]+)"/g)].map(match=>match[1]);
  assert.deepEqual(sections,['roles','radio','program','services','storage','firmware','advanced']);
  for(const match of page.matchAll(/href="#([^"]+)"/g)) assert(sections.includes(match[1]),'Navigation must reach a section');
  const f=fixture();
  assert.equal(f.element('refresh').disabled,true);
  assert.equal(f.element('source').disabled,false,'Local drafts stay editable without a session');
  for(const size of [0,1,3,55,56,63,64,65,2422,4096]) {
    const bytes=crypto.randomBytes(size);
    assert.equal(f.evaluate(`sha256(Uint8Array.from(${JSON.stringify([...bytes])}))`),digest(bytes));
  }
  await f.login();
  f.element('grant').value='reminders';f.element('grant-on').value='on';
  await f.element('grant-save').click();
  assert.match(f.confirms.at(-1),/private reminders/);
  assert(!f.confirms.at(-1).includes('HTTPS'),'Permission confirmation must describe only the selected permission');
  assert(f.requests.some(r=>r.body==='bot reminders on'),'Human labels must preserve the native permission command');
  assert.match(f.element('role-generation').textContent,/18446744073709551614/,'u64 generation must not round');
  assert.equal(f.element('role-rows').children.length,7);
  await f.element('role-read').click();
  assert.match(f.element('role-detail').textContent,/<img src=x onerror=alert\(1\)>/,'Role text is escaped by DOM textContent');
  // A concurrent owner changed other mask bits since this table was rendered.
  f.state.saved=13;
  await f.element('role-rows').children[0].children[3].children[0].click();
  assert.equal(f.state.saved,12,'Disable only the chosen bit, preserve fresh saved settings');
  f.state.confirm=false;
  const count=f.requests.length;
  await f.element('radio-save').click();
  assert.equal(f.requests.length,count,'Rejected confirmation must not send a write');
  f.state.confirm=true;
  await f.element('radio-temp').click();
  assert.match(f.element('result').textContent,/^Pending:/);
  assert.equal(f.element('result').className,'warning','Queue acceptance must not be presented as completed');
  assert(f.confirms.at(-1).includes('STAL'));
  await f.run(`cmd('telemetry endpoint ca begin abc')`);
  assert.match(f.element('result').textContent,/unavailable over HTTP/);
  await f.run(`cmd('key bot ${'a'.repeat(128)}')`);
  assert(!f.requests.some(r=>r.body?.startsWith('key bot ')));
  await f.run(`cmd('password 736563726574')`);
  assert.match(f.element('result').textContent,/password changes require encrypted Management RF/);
  assert(!f.requests.some(r=>r.body==='password 736563726574'));
  assert.equal(f.evaluate(`readonly.test('password')`),true);
  assert.equal(f.evaluate(`readonly.test('password help')`),true);
  assert.equal(f.evaluate(`readonly.test('password 736563726574')`),false);
  const secretCommands=[
    'role password repeater 736563726574', 'role  password  room  736563726574',
    'telemetry endpoint ca', 'telemetry endpoint ca clear', 'telemetry endpoint token 736563726574',
    'bot https ca home 4142', 'bot https token home 736563726574',
    '  bot https   token home 736563726574  ', 'BOT HTTPS TOKEN home 736563726574',
    'wifi password 70617373776f7264', 'wifi password -', 'wifi 4d657368 70617373776f7264', 'wifi 4d657368 -',
    '  wifi  4d657368  70617373776f7264  ',
    'role channel bot 0 2370726976617465 00112233445566778899aabbccddeeff',
    'role  channel  companion  1  2374657374  00112233445566778899aabbccddeeff',
    'key bot malformed-private-input', 'key bot    '+ 'a'.repeat(128),
  ];
  for(const command of secretCommands) {
    const sent=f.requests.length, prompted=f.confirms.length;
    await f.run(`nativeWrite(${JSON.stringify(command)})`);
    assert.equal(f.requests.length,sent,'Secret setter must be rejected before HTTP: '+command.split(' ').slice(0,3).join(' '));
    assert.equal(f.confirms.length,prompted,'Never render secret bodies in a confirmation');
    assert.match(f.element('result').textContent,/HTTP|unavailable/);
    assert(!f.element('result').textContent.includes('736563726574'));
    assert(!f.element('result').textContent.includes('00112233445566778899aabbccddeeff'));
  }
  for(const command of ['password','password help','wifi status','wifi help','help wifi','ver','board',
    'get name','get owner.info','get radio','get freq','get tx','get wifi.enabled','get wifi.ssid','get wifi.ip','get wifi.status',
    'role channel bot 0','key help','key bot','key bot pending','bot https','bot https status','telemetry endpoint status']) {
    f.evaluate(`validateCommand(${JSON.stringify(command)})`);
    assert.equal(f.evaluate(`readonly.test(${JSON.stringify(command)})`),true,'Safe native query stays read-only: '+command);
  }
  assert.equal(f.evaluate(`readonly.test('get ownerXinfo')`),false);
  for(const command of ['wifi ssid 4d657368','wifi forget','get wifi.pwd','set wifi.ssid example','set wifi.pwd secret','set wifi.enabled 0']) {
    assert.throws(()=>f.evaluate(`validateCommand(${JSON.stringify(command)})`),/encrypted Management RF/);
  }
  for(const command of ['wifi apply','role channel bot 0 off','key bot cancel',
    'bot https endpoint probe 192.0.2.1 api.example.invalid 443 /probe get','bot https commit','bot https discard'])
    f.evaluate(`validateCommand(${JSON.stringify(command)})`);
  await f.element('inventory').click();
  assert.equal(f.element('source-stage').disabled,false);
  f.state.api=false; await f.element('inventory').click();
  assert.equal(f.element('source-stage').disabled,true,'Unknown runtimes must not inherit prior capability');
  f.state.api=true; await f.element('inventory').click();
  f.element('source').value='local unsaved edits';
  await f.element('source-read').click();
  assert.equal(f.element('source').value,source4096.toString());
  assert.equal(f.requests.filter(r=>r.body?.startsWith('source read ')).length,86);
  assert(!f.requests.some(r=>r.body==='source read 86'));
  f.element('source').value="function hello(name) reply(name) end";
  const intended=f.element('source').value;
  f.state.hook=async(path,text)=>{if(text?.startsWith('source begin ')) f.element('source').value='newer editor text';};
  await f.element('source-stage').click();
  assert.equal(f.element('source').value,'newer editor text');
  assert(!f.requests.some(r=>r.body?.startsWith('source commit ')),'Upload must not implicitly install');
  f.state.hook=null;
  await f.element('source-install').click();
  assert.equal(f.state.source.toString(),intended,'Install the uploaded snapshot, never current changed editor');
  assert.match(f.element('result').textContent,/source durably saved and active/);
  assert.equal(f.element('source').value,'newer editor text','Install must preserve newer draft');
  f.element('source').value='résumé '.repeat(4096);
  const oversized=f.requests.length; await f.element('source-stage').click();
  assert.equal(f.requests.length,oversized,'Enforce UTF-8 bytes, not character count');
  assert.match(f.element('result').textContent,/4096 UTF-8 bytes/);
  await f.element('help-read').click();
  assert.equal(f.element('helptext').value,'Error: is literal help, not a command error');
  f.element('source').value='KEEP DRAFT';
  f.state.override=text=>text.startsWith('source read ')?'DATA 00':undefined;
  await f.element('source-read').click();
  assert.equal(f.element('source').value,'KEEP DRAFT','Bad readback must not overwrite edits');
  f.state.override=null;
  f.state.httpError=504; f.state.httpText='Command outcome unknown; inspect status before retry';
  const beforeTimeout=f.requests.length;
  await f.element('radio-save').click();
  assert.equal(f.requests.length,beforeTimeout+1,'A lost write receipt must not be retried');
  assert.match(f.element('result').textContent,/unknown/);
  f.state.httpError=null;
  const locked=f.requests.length; await f.element('radio-save').click();
  assert.equal(f.requests.length,locked,'Unknown outcome blocks another mutation');
  await f.element('inspect').click();
  assert.equal(f.evaluate('uncertain'),true,'Effective PHY cannot confirm the durable radio save');
  f.evaluate('unresolved.splice(0); uncertain=false; updateControls()'); // Start the independent scoped-storage fixture phase.
  await f.element('storage-read').click();
  assert.equal(f.element('data-export').disabled,false);
  await f.element('data-export').click();
  assert.equal(f.downloads.length,1);
  assert.equal(Buffer.from(await f.downloads[0].arrayBuffer()).toString('hex'),snapshot().toString('hex'));
  assert(!f.element('result').textContent.includes('DATA '),'Do not dump private payload chunks into results');
  const timer=snapshot('BTD');
  f.element('backup').files=[{size:timer.length,arrayBuffer:async()=>timer.buffer.slice(timer.byteOffset,timer.byteOffset+timer.byteLength)}];
  await f.run('inspectBackup()');
  const stageCount=f.requests.length; await f.element('data-stage').click();
  assert.equal(f.requests.length,stageCount,'Scheduler staging requires no-rearm consent');
  f.element('no-rearm').checked=true;
  await f.element('data-stage').click();
  assert.match(f.element('result').textContent,/STAGED/);
  assert(!f.requests.some(r=>r.body?.startsWith('data restore ')),'Staging must not restore');
  await f.element('data-restore').click();
  assert(f.requests.some(r=>/^data restore [0-9a-f]{16} no-rearm$/.test(r.body)));
  assert.match(f.element('result').textContent,/COMMITTED/);
  f.state.override=text=>text.startsWith('data restore ')?(f.state.dataStatus='UNKNOWN '+f.state.restore.id,'PENDING '+f.state.restore.id):undefined;
  await f.element('data-stage').click(); await f.element('data-restore').click();
  assert.match(f.element('result').textContent,/partial|export\/read back/);
  const restoreAttempts=f.requests.filter(r=>r.body?.startsWith('data restore ')).length;
  await f.element('data-restore').click();
  assert.equal(f.requests.filter(r=>r.body?.startsWith('data restore ')).length,restoreAttempts);
  f.state.override=null;
  await f.element('data-export').click();
  assert.equal(f.downloads.length,2,'Readback/export must remain possible after unknown restore');
  await f.element('inspect').click();
  f.state.httpError=403; await f.element('refresh').click();
  assert.equal(f.element('refresh').disabled,true);
  assert.equal(f.element('source').disabled,false);
  assert.equal(f.element('source').value,'KEEP DRAFT','Expired authentication preserves draft');
  assert.equal(f.element('backup').value,'');
  f.state.httpError=null;
  await f.login();
  f.state.now+=600001; f.intervals[0]();
  const expired=f.requests.length; await f.run(`cmd('status')`);
  assert.equal(f.requests.length,expired,'Local expiry must stop sending the old token');
  assert.equal(f.element('refresh').disabled,true);
  assert.equal(f.state.maxRequests,1,'UI workflows must serialize the shared command mailbox');
  const resumed=fixture(); await resumed.login(); await resumed.element('inventory').click();
  resumed.element('source').value="function hello(name) reply(name) end\n--"+'r'.repeat(100);
  const original=resumed.element('source').value;
  resumed.state.hook=async(path,text)=>{if(text?.startsWith('source chunk ') && text.split(' ')[3]==='1') throw Error('Lost chunk reply');};
  await resumed.element('source-stage').click();
  assert.match(resumed.element('result').textContent,/outcome unknown/);
  assert.equal(resumed.element('source').value,original);
  assert.equal(resumed.state.upload.next,1);
  resumed.state.hook=null; await resumed.element('inspect').click();
  const start=resumed.requests.length; await resumed.element('source-stage').click();
  const resumedChunks=resumed.requests.slice(start).filter(r=>r.body?.startsWith('source chunk '));
  assert.equal(resumedChunks[0].body.split(' ')[3],'1','Honor the durable resume index rather than replay prior chunks');
  assert(!resumed.requests.some(r=>r.body?.startsWith('source commit ')));
  await resumed.element('source-install').click();
  assert.equal(resumed.state.source.toString(),original);
  // Terminal prose for the old same-hash generation is not new-install confirmation.
  const oldHash=digest(resumed.state.source), oldManifest=`SHA256 ${oldHash} gen=${resumed.state.sourceGeneration}`;
  await resumed.run(`sourceOutcome('${oldHash}','${oldManifest}')`);
  assert.match(resumed.element('result').textContent,/remains pending or was superseded/);
  const invalid=fixture(); await invalid.login(); await invalid.element('storage-read').click();
  const broken=snapshot(); broken[70]=255;
  invalid.element('backup').files=[{size:broken.length,arrayBuffer:async()=>broken.buffer.slice(broken.byteOffset,broken.byteOffset+broken.byteLength)}];
  await invalid.run('inspectBackup()');
  assert.match(invalid.element('result').textContent,/content digest/);
  const corruptRequests=invalid.requests.length; await invalid.element('data-stage').click();
  assert.equal(invalid.requests.length,corruptRequests);
  const changed=fixture(); await changed.login(); await changed.element('inventory').click();
  changed.element('source').value='function hello() reply("original") end';
  await changed.element('source-stage').click();
  // ID/count alone cannot bind another owner's replacement upload to our hash.
  changed.state.upload.hash='ef'.repeat(32);
  const commitCount=changed.requests.filter(r=>r.body?.startsWith('source commit ')).length;
  await changed.element('source-install').click();
  assert.match(changed.element('result').textContent,/another upload exists/);
  assert.equal(changed.requests.filter(r=>r.body?.startsWith('source commit ')).length,commitCount);
  assert.equal(changed.element('source').value,'function hello() reply("original") end');
  const network=fixture(); await network.login();
  let networkReply='Error: HTTPS commit outcome unknown; live endpoints blocked until reboot/readback';
  network.state.override=text=>text.startsWith('bot https ')?networkReply:undefined;
  await network.run(`cmd('bot https commit')`);
  assert.match(network.element('result').textContent,/Error: HTTPS commit outcome unknown/);
  assert.equal(network.element('result').className,'warning','Error prefix must not downgrade an unknown commit to generic rejection');
  assert.equal(network.evaluate('uncertain'),true);
  networkReply='HTTPS endpoints=1 rpc=0 staged=0 uncertain=1 epoch=3';
  await network.run(`cmd('bot https status')`);
  assert.notEqual(network.element('result').className,'success','An uncertain endpoint readback is not confirmed success');
  assert.equal(network.evaluate('uncertain'),true,'Reading uncertain endpoint status must not clear the mutation fence');
  networkReply='Error: HTTPS configuration validation or commit failed';
  await network.run(`cmd('bot https commit')`);
  assert.equal(network.element('result').className,'error','Definite validation rejection remains an actionable backend error');
  networkReply='HTTPS change staged; commit to verify and activate';
  await network.run(`cmd('bot https endpoint probe 192.0.2.1 api.example.invalid 443 /probe get')`);
  assert.notEqual(network.element('result').className,'success','Native staging is not commit success');
  assert.match(network.element('result').textContent,/staged; commit to verify and activate/);
  networkReply='HTTPS configuration verified and committed; previous network requests revoked';
  await network.run(`cmd('bot https commit')`);
  assert.equal(network.element('result').textContent,networkReply,'Preserve the backend verified-commit result without synthesizing another outcome');
  assert(!page.includes('innerHTML'));
  const binary=fixture(); binary.state.wasmEnabled=true;
  await binary.login(); await binary.element('inventory').click();
  assert.equal(binary.element('wasm-option').disabled,false);
  binary.element('source').value='UNSAVED LUA DRAFT';
  const luaHash=digest(binary.state.source);
  binary.element('program-runtime').value='wasm';
  await binary.element('program-runtime').onchange();
  assert.equal(binary.element('lua-editor').hidden,true,'Wasm mode must preserve but hide the Lua editor');
  assert.equal(binary.element('wasm-editor').hidden,false);
  const wasm=Buffer.from([0,97,115,109,1,0,0,0,0xff,0x80,0,1]);
  binary.element('wasm-file').files=[{name:'fixture.wasm',size:wasm.length,arrayBuffer:async()=>wasm.buffer.slice(wasm.byteOffset,wasm.byteOffset+wasm.byteLength)}];
  await binary.element('wasm-file').onchange();
  await binary.element('source-stage').click();
  assert.equal(binary.state.wasm.upload.bytes.toString('hex'),wasm.toString('hex'),'NUL/high-bit Wasm bytes must survive transfer exactly');
  await binary.element('source-install').click();
  assert.equal(binary.state.wasm.source.toString('hex'),wasm.toString('hex'));
  assert.equal(digest(binary.state.source),luaHash,'Wasm installation must not change Lua selection');
  await binary.element('source-read').click();
  assert.equal(Buffer.from(await binary.downloads[0].arrayBuffer()).toString('hex'),wasm.toString('hex'));
  assert.equal(binary.element('source').value,'UNSAVED LUA DRAFT','Wasm download must not decode or overwrite the Lua editor');
  binary.element('program-runtime').value='lua';
  await binary.element('program-runtime').onchange();
  assert.equal(binary.element('lua-editor').hidden,false);
  assert.equal(binary.element('wasm-editor').hidden,true);
  assert.equal(binary.element('source').value,'UNSAVED LUA DRAFT','Switching editors must retain unsaved Lua text');
  binary.element('program-runtime').value='wasm';
  await binary.element('program-runtime').onchange();
  assert(binary.requests.some(r=>r.body?.startsWith('source wasm chunk ')));
  binary.state.wasmEnabled=false; await binary.element('inventory').click();
  assert.equal(binary.element('source-stage').disabled,true,'Disabled runtime must not permit binary installation');
  const disabledRequests=binary.requests.length;
  await binary.element('source-stage').click();
  assert.equal(binary.requests.length,disabledRequests);
  console.log('PASS Wasm UI: compiled capability selection, exact binary transfer/download, separate Lua draft and disabled rejection');
  assert(!page.includes('localStorage'));
  assert(!/<(?:script|link)[^>]*(?:src|href)=["']https?:/.test(page),'No remote assets');
  console.log('PASS admin UI: native auth/expiry, roles, confirmation, escaping, 4 KiB source read/resume/snapshot/install, capability changes, private scoped data stage/no-rearm/unknown and no write replay');
}
tests().catch(error=>{console.error(error);process.exitCode=1;});
