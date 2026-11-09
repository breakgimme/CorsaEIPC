// Offline browser tests against the actual shipped HTML and catalogue.
// Install dependencies with npm ci and the browser with npx playwright install chromium.
const fs=require('node:fs'),assert=require('node:assert/strict');
const {chromium}=require('playwright');
const html=fs.readFileSync(__dirname+'/WebUI.h','utf8').match(/R"HTML\(([\s\S]*)\)HTML"/)[1];
const catalogue=JSON.parse(fs.readFileSync(__dirname+'/OptionCatalogue.h','utf8').match(/R"JSON\(([\s\S]*)\)JSON"/)[1]);
(async()=>{
 const browser=await chromium.launch({headless:true,...(process.env.PLAYWRIGHT_CHANNEL?{channel:process.env.PLAYWRIGHT_CHANNEL}:{})});
 const page=await browser.newPage({viewport:{width:1280,height:1000}});
 const calls=[],errors=[];let saved=null,failLoad=false,failSave=false,deviceBoot='boot1',token='abc123',a4Data='00'.repeat(5);
 const initial='81000000000000A5';
 page.on('pageerror',e=>errors.push(e.message));
 await page.route('http://ipc.test/**',async route=>{
  const req=route.request(),path=new URL(req.url()).pathname;
  const body=new URLSearchParams(req.postData()||'');
  const json=(data,status=200)=>route.fulfill({status,contentType:'application/json',body:JSON.stringify(data)});
  if(path==='/')return route.fulfill({contentType:'text/html',body:html});
  if(path==='/api/options/catalogue')return json(catalogue);
  if(path==='/api/log')return route.fulfill({contentType:'text/plain',headers:{'X-Log-Cursor':'0','X-Log-Boot':deviceBoot},body:''});
  calls.push({path,body:Object.fromEntries(body)});
  if(path==='/api/options/load'){
   if(failLoad)return json({error:'IPC read failed. No retry.'},502);
   const p=catalogue.find(p=>p.did===body.get('did'));
   return json({did:p.did,token,data:p.did==='A4'?a4Data:p.did==='A3'?initial:'A5'.repeat(p.size)});
  }
  if(path==='/api/options/save'){
   saved=Object.fromEntries(body);
   if(failSave)return json({error:'Unlock failed. Nothing written; no retry.'},502);
   token='def456';return json({verified:true,token,data:body.get('data')});
  }
  if(path==='/api/command')return route.fulfill({contentType:'text/plain',body:'OK\n'});
  throw new Error('Unexpected path '+path);
 });
 try{
  await page.goto('http://ipc.test/');await page.waitForFunction(()=>document.querySelector('#status').textContent.startsWith('Ready.'));
  assert.equal(calls.length,0,'Opening the editor must cause no CAN commands');
  assert.equal(await page.locator('#did option').count(),9);
  assert.equal(await page.locator('#send').isDisabled(),true);
  await page.click('#load');await page.waitForFunction(()=>document.querySelector('#status').textContent.startsWith('Read complete'));
  assert.equal(calls.length,1);assert.equal(calls[0].body.did,'A3');
  const target=catalogue.find(p=>p.did==='A3').options.find(o=>o.byte===0&&o.mask===0x40);
  assert.match(target.name,/ECO/);
  const switcher=page.locator('#opt-0-64');assert.equal(await switcher.isChecked(),false);
  await switcher.check();assert.equal(calls.length,1,'Local toggles must not send CAN requests');
  await page.fill('#search','coolant');assert.match(await page.locator('#options').innerText(),/Coolant/i);
  await page.fill('#search','');assert.equal(await switcher.isChecked(),true,'Filtering must retain other edits');
  assert.equal(await page.locator('#send').isDisabled(),true,'Review confirmation is required');
  await page.check('#confirm');await page.click('#send');
  await page.waitForFunction(()=>document.querySelector('#status').textContent.startsWith('WRITE VERIFIED'));
  assert.equal(calls.length,2,'Send must use one save POST; no hidden HTTP unlock/retry');
  assert.equal(saved.data,'C1000000000000A5','Only selected mask may change; other bytes/bits remain identical');
  assert.equal(saved.token,'abc123');assert.equal(saved.confirm,'1');
  assert.equal(await page.locator('#send').isDisabled(),true);
  await switcher.uncheck();await page.click('#reset');assert.equal(await switcher.isChecked(),true);
  // Grouped fields use raw physical values, preserve unrelated masks.
  await page.selectOption('#did','A8');assert.equal(await page.locator('#options input').count(),0);
  assert.equal(calls.length,2,'Switching groups must not read automatically');
  await page.click('#load');await page.waitForFunction(()=>document.querySelector('#status').textContent.startsWith('Read complete'));
  const field=page.locator('#opt-0-3');await field.selectOption('2');
  assert.equal(calls.length,3);assert.match(await page.locator('#changes').innerText(),/Current Display Unit/);
  // Save failure invalidates the token/client editor and does not retry.
  failSave=true;await page.check('#confirm');await page.click('#send');
  await page.waitForFunction(()=>document.querySelector('#status').textContent.includes('Unlock failed'));
  assert.equal(calls.length,4);assert.equal(saved.data.slice(0,2),'A6');
  assert.equal(await page.locator('#send').isDisabled(),true);
  failLoad=true;await page.click('#load');await page.waitForFunction(()=>document.querySelector('#status').textContent.includes('IPC read failed'));
  assert.equal(calls.length,5);assert.equal(await page.locator('#options input').count(),0);
  failLoad=false;failSave=false;await page.selectOption('#did','A3');await page.click('#load');
  await page.waitForFunction(()=>document.querySelector('#status').textContent.startsWith('Read complete'));
  await page.locator('#opt-0-64').check();
  await page.screenshot({path:__dirname+'/build/graphical-editor-desktop.png',fullPage:true});
  await page.setViewportSize({width:390,height:844});await page.screenshot({path:__dirname+'/build/graphical-editor-mobile.png',fullPage:true});
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false,'Mobile editor must not overflow horizontally');
  // Restart invalidates the loaded baseline; periodic polling is read-only HTTP.
  await page.evaluate(()=>{logBoot='boot1';});deviceBoot='boot2';await page.evaluate(()=>pollLogs());
  assert.equal(await page.locator('#options input').count(),0);
  // Synthetic short A4 payload verifies variable-length configuration handling.
  const a4Start=calls.length;
  await page.selectOption('#did','A4');await page.click('#load');
  await page.waitForFunction(()=>document.querySelector('#status').textContent.startsWith('Read complete'));
  assert.match(await page.locator('#read-info').innerText(),/5 bytes \(catalogue: 11\)/);
  const a4Controls=catalogue.find(p=>p.did==='A4').options.filter(o=>o.byte<5);
  assert.equal(await page.locator('#options .option').count(),a4Controls.length,'Only returned bytes may expose controls');
  assert.equal(await page.locator('#opt-7-15').count(),0,'DOD field outside short layout must be hidden');
  await page.locator('#opt-0-128').check();await page.check('#confirm');await page.click('#send');
  await page.waitForFunction(()=>document.querySelector('#status').textContent.startsWith('WRITE VERIFIED'));
  assert.equal(saved.data,'8000000000','A4 save must remain exactly 5 bytes, without added padding');
  assert.equal(calls.length,a4Start+2,'Short A4 load/save must not add hidden reads or retries');
  a4Data='00'.repeat(11);await page.click('#load');
  await page.waitForFunction(()=>document.querySelector('#status').textContent.startsWith('Read complete'));
  assert.equal(await page.locator('#opt-7-15').count(),1,'Full A4 layout still exposes later fields');
  assert.match(await page.locator('#read-info').innerText(),/11 bytes/);
  await page.setViewportSize({width:390,height:844});
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false,'Editor must fit mobile');
  assert.deepEqual(errors,[]);
  console.log('Browser checks passed: DID/A4 editor and mobile layout.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
