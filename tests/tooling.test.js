'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
const fs = require('fs'), os = require('os'), path = require('path'), http = require('http'), zlib = require('zlib');
const {build} = require('../tools/lib/build');
const {parseBundle,readBundle} = require('../tools/lib/bundle');
const {createServer,quality} = require('../tools/lib/serve');
const {integer,parse} = require('../tools/wz');
async function fixture(fn) {
    const temp = fs.mkdtempSync(path.join(os.tmpdir(),'webzero-test-')), source = path.join(temp,'site');
    fs.mkdirSync(source);
    try { return await fn(source,temp); } finally { fs.rmSync(temp,{recursive:true,force:true}); }
}
function request(server,url='/',headers={},method='GET') {
    return new Promise((resolve,reject)=>{
        const req = http.request({host:'127.0.0.1',port:server.address().port,path:url,headers,method},res=>{
            const chunks=[]; res.on('data',c=>chunks.push(c)); res.on('end',()=>resolve({status:res.statusCode,headers:res.headers,body:Buffer.concat(chunks)}));
            res.on('error',reject);
        }); req.on('error',reject); req.end();
    });
}
test('deterministic bundles, >8 siblings, identity and Brotli, special object keys',()=>fixture(async source=>{
    for (let i=0;i<20;i++) fs.writeFileSync(path.join(source,'page'+i+'.html'),'hello '.repeat(500));
    fs.writeFileSync(path.join(source,'__proto__.html'),'prototype route');
    fs.writeFileSync(path.join(source,'constructor.html'),'constructor route');
    fs.writeFileSync(path.join(source,'.env'),'must not ship');
    fs.mkdirSync(path.join(source,'.git')); fs.writeFileSync(path.join(source,'.git','config'),'secret');
    const first=await build(source), bytes=fs.readFileSync(first.output);
    await build(source); assert.deepEqual(fs.readFileSync(first.output),bytes);
    const b=readBundle(first.output); assert.equal(b.assets.length,22); assert.equal(b.routes.size,22);
    assert(b.routes.has('/__proto__')); assert(b.routes.has('/page19'));
    const a=b.assets[b.routes.get('/page19').asset]; assert.equal(a.encoding,1); assert.deepEqual(zlib.brotliDecompressSync(a.data),a.raw);
}));
test('route collisions fail without replacing existing output',()=>fixture(async source=>{
    fs.writeFileSync(path.join(source,'a.html'),'a'); const first=await build(source), before=fs.readFileSync(first.output);
    fs.writeFileSync(path.join(source,'a'),'b'); await assert.rejects(build(source),/collision/); assert.deepEqual(fs.readFileSync(first.output),before);
}));
test('UTF-8 segment limit is enforced rather than silently truncating',()=>fixture(async source=>{
    fs.writeFileSync(path.join(source,'é'.repeat(16)+'.html'),'a'); await assert.rejects(build(source),/segment/);
}));
test('precompressed media stays raw and WebP companions are indexed',()=>fixture(async source=>{
    fs.writeFileSync(path.join(source,'hero.png'),Buffer.alloc(10000,42)); fs.writeFileSync(path.join(source,'hero.webp'),'webp');
    const b=readBundle((await build(source)).output), a=b.assets[b.routes.get('/hero.png').asset]; assert.equal(a.encoding,0); assert(a.webpIndex>=0);
}));
test('malformed bundle mutations are rejected',()=>fixture(async source=>{
    fs.writeFileSync(path.join(source,'index.html'),'hello '.repeat(100));
    const bytes=fs.readFileSync((await build(source)).output);
    const mutations=[b=>b.writeUInt32LE(0,0),b=>b.writeUInt32LE(3,4),b=>b.writeUInt32LE(0xffffffff,20),b=>b.writeUInt32LE(0,24),
        b=>b.writeUInt32LE(0,28+32),b=>b.writeUInt32LE(0xffffffff,b.readUInt32LE(12)),b=>b.fill(65,b.readUInt32LE(12)+12,b.readUInt32LE(12)+44),
        b=>b.writeUInt32LE(2048,b.readUInt32LE(20)+80),b=>b.writeInt32LE(999,b.readUInt32LE(12)+48),
        b=>b.writeUInt32LE(0xfffffffe,b.readUInt32LE(12)+52),b=>b.writeInt32LE(999,28+64+40)];
    for(const mutate of mutations){const b=Buffer.from(bytes);mutate(b);assert.throws(()=>parseBundle(b),/Invalid bundle/);}
    for(let i=0;i<28;i++) assert.throws(()=>parseBundle(bytes.subarray(0,i)),/Invalid bundle/);
}));
test('HTTP identity, Brotli q=0, HEAD, caching, ranges, aliases, methods and WebP',()=>fixture(async source=>{
    const content='<!doctype html><title>test</title>'+ 'body '.repeat(1000);
    fs.writeFileSync(path.join(source,'index.html'),content); fs.mkdirSync(path.join(source,'docs'));fs.writeFileSync(path.join(source,'docs','index.html'),'docs');
    fs.writeFileSync(path.join(source,'hero.png'),'png');fs.writeFileSync(path.join(source,'hero.webp'),'webp');
    const {server}=createServer((await build(source)).output); await new Promise(r=>server.listen(0,'127.0.0.1',r));
    try {
        const raw=await request(server);assert.equal(raw.body.toString(),content);assert.equal(raw.headers['content-encoding'],undefined);
        const br=await request(server,'/',{'Accept-Encoding':'br'});assert.equal(br.headers['content-encoding'],'br');assert.equal(zlib.brotliDecompressSync(br.body).toString(),content);
        const refused=await request(server,'/',{'Accept-Encoding':'br;q=0, *;q=1'});assert.equal(refused.body.toString(),content);assert.equal(refused.headers['content-encoding'],undefined);
        assert.equal((await request(server,'/',{'Accept-Encoding':'identity;q=0,br;q=0'})).status,406);
        const head=await request(server,'/',{},'HEAD');assert.equal(head.body.length,0);assert.equal(Number(head.headers['content-length']),Buffer.byteLength(content));assert.equal(head.headers['content-type'],raw.headers['content-type']);
        assert.equal((await request(server,'/',{'If-None-Match':raw.headers.etag})).status,304);
        const partial=await request(server,'/',{Range:'bytes=5-9'});assert.equal(partial.status,206);assert.equal(partial.body.toString(),content.slice(5,10));
        assert.equal((await request(server,'/',{Range:'bytes=999999-'})).status,416);
        assert.equal((await request(server,'/docs/')).body.toString(),'docs');assert.equal((await request(server,'/index.html')).body.toString(),content);
        assert.equal((await request(server,'/',{},'POST')).status,405);assert.equal((await request(server,'/missing')).status,404);
        assert.equal((await request(server,'/hero.png',{Accept:'image/webp;q=0'})).body.toString(),'png');
        assert.equal((await request(server,'/hero.png',{Accept:'image/webp'})).body.toString(),'webp');
        assert.equal((await request(server,'/%00')).status,400);assert.equal((await request(server,'/%2e%2e/x')).status,400);
    } finally { await new Promise(r=>server.close(r)); }
}));
test('quality parsing matches exact tokens and explicit exclusions',()=>{
    assert.equal(quality('zebra','br',0),0); assert.equal(quality('br;q=0,*;q=1','br',0),0);
    assert.equal(quality('br;q=0.125','br',0),125);assert.equal(quality('br;q=9','br',0),0);
    assert.equal(quality('*;q=0','identity',1000),0);
});
test('CLI rejects invalid ports and options',()=>{
    for(const v of ['8080x','0','65536','1.5','-1','']) assert.throws(()=>integer(v,1,65535,'port'));
    assert.throws(()=>parse(['--port']));assert.throws(()=>parse(['--unknown']));assert.equal(integer('8080',1,65535,'port'),8080);
});
