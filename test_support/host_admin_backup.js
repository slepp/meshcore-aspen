// SPDX-License-Identifier: Apache-2.0
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const vm = require('node:vm');
const page = fs.readFileSync('internal/app/admin_page.html', 'utf8');
const script = page.match(/<script>([\s\S]*?)<\/script>/)[1];
const digest = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
class Element {
  constructor() { this.value=''; this.disabled=false; this.files=[]; this.children=[]; }
  replaceChildren() {this.children=[];}
  appendChild(child) {this.children.push(child);}
  click() {return this.onclick?.();}
  set innerHTML(value) {assert.fail('Untrusted text must not be interpreted as HTML: '+value);}
}
async function tests() {
  const elements=new Map();
  const element=id=>{
    if(!elements.has(id))elements.set(id,new Element());
    return elements.get(id);
  };
  const recipient='56'.repeat(32), requests=[], downloads=[];
  const archive=Buffer.alloc(384);
  Buffer.from('MCB\x01\x01\0\0\0').copy(archive);
  Buffer.from(recipient,'hex').copy(archive,40);
  let tampered=false;
  const context={
    document:{getElementById:element,createElement:()=>new Element()},
    window:{confirm:()=>true,addEventListener(){}},
    console,TextEncoder,Uint8Array,Blob,AbortController,Date,
    crypto:crypto.webcrypto,
    URL:{createObjectURL:blob=>{downloads.push(blob);return 'blob:private';},revokeObjectURL(){}},
    setTimeout,clearTimeout,
    fetch:async(path,options)=>{
      requests.push({path,body:options.body});
      assert.equal(options.redirect,'error');
      assert.equal(options.credentials,'omit');
      assert.equal(options.headers['X-Host-Intent'],'admin-v1');
      if(path==='/admin/login')return {ok:true,status:200,text:async()=>'ab'.repeat(32)};
      assert.equal(options.headers['X-Host-Admin'],'ab'.repeat(32));
      if(path==='/admin/status')return {ok:true,status:200,text:async()=>JSON.stringify({roles:{},capabilities:{native_owner:false,node_backup:true,role_owner_commands:[]}})};
      if(path==='/admin/command'){
        const text=options.body;
        if(text==='backup start '+recipient||text==='backup load')return {ok:true,status:200,text:async()=>'PREPARING'};
        assert.equal(text,'backup status');
        return {ok:true,status:200,text:async()=>`READY ${digest(archive).slice(0,16)} bytes=${archive.length} sha=${digest(archive)}`};
      }
      assert.equal(path,'/admin/backup?id='+digest(archive).slice(0,16));
      const value=Buffer.from(archive);
      if(tampered)value[value.length-1]^=1;
      return {ok:true,status:200,arrayBuffer:async()=>value};
    },
  };
  vm.createContext(context);vm.runInContext(script,context);
  element('token').value='private-admin';
  await element('login').click();
  assert.equal(element('backup-create').disabled,false,'Backup is independent of the native bot');
  element('backup-key').value=recipient;
  await element('backup-create').click();
  assert.match(element('result').textContent,/Downloaded encrypted node backup/);
  assert.equal(downloads.length,1);
  assert.deepEqual(Buffer.from(await downloads[0].arrayBuffer()),archive);
  await element('backup-download').click();
  assert.equal(downloads.length,2);
  assert.equal(requests.filter(request=>request.body?.startsWith('backup start ')).length,1);
  tampered=true;
  await element('backup-download').click();
  assert.equal(downloads.length,2);
  assert.match(element('result').textContent,/checksum failed/);
  element('backup-key').value='not a public key';
  const before=requests.length;
  await element('backup-create').click();
  assert.equal(requests.length,before);
  assert.match(element('result').textContent,/operator public key/);
  console.log('PASS Birch backup UI: authenticated whole-host export without native bot, saved download, digest and recipient checks');
}
tests().catch(error=>{console.error(error);process.exitCode=1;});
