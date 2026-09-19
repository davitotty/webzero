#!/usr/bin/env node
'use strict';
const fs = require('fs'), path = require('path'), os = require('os');
const {spawn,spawnSync} = require('child_process');
const {build} = require('./lib/build');
const {readBundle} = require('./lib/bundle');
const {createServer} = require('./lib/serve');
const version = require('../package.json').version;
function parse(args) {
    const positional = [], options = {};
    const flags = new Set(['js','native','json']);
    const values = new Set(['port','output','quality','widths']);
    for (let i=0; i<args.length; i++) {
        const a = args[i];
        if (!a.startsWith('--')) { positional.push(a); continue; }
        const name = a.slice(2);
        if (flags.has(name)) options[name] = true;
        else if (values.has(name)) {
            if (!args[i+1] || args[i+1].startsWith('--')) throw new Error('Missing value for '+a);
            options[name] = args[++i];
        } else throw new Error('Unknown option '+a);
    }
    return {positional,options};
}
function integer(value, min, max, name) {
    if (!/^\d+$/.test(String(value)) || !Number.isSafeInteger(Number(value)) || Number(value)<min || Number(value)>max) throw new Error('Invalid '+name+' ('+min+'..'+max+')');
    return Number(value);
}
async function main(args) {
    const command = args.shift();
    if (!command || ['--help','-h','help'].includes(command)) {
        console.log(`WebZero ${version}
  wz build <directory> [--output site.web] [--quality 0..11] [--json]
  wz serve <site.web> [--port 8080] [--native | --js]
  wz inspect <site.web> [--json]
  wz optimize <directory> [--widths 320,640,1280] [--quality 82]
  wz update
  wz version

Builds deterministic v2 bundles with Brotli and identity representations.
Serve prefers a compatible native binary; --js selects the Node development server.`); return;
    }
    if (['version','--version','-v'].includes(command)) { console.log('WebZero '+version+'; Node '+process.version+'; '+process.platform+'-'+process.arch); return; }
    const {positional:p,options:o} = parse(args);
    if (command === 'update') { await require('./lib/install').install(); return; }
    if (!p[0]) throw new Error(command+' requires a path');
    if (command === 'build') {
        const result = await build(p[0],{output:o.output,quality:o.quality === undefined ? 5 : integer(o.quality,0,11,'Brotli quality')});
        console.log(o.json ? JSON.stringify(result) : `Built ${result.output}\n${result.assets} assets, ${result.routes} route nodes, ${result.bytes} bytes on disk\n${result.transferBytes} bytes over Brotli; ${result.originalBytes} bytes identity`);
    } else if (command === 'inspect') {
        const b = readBundle(p[0]);
        const report = {version:b.version,bytes:b.bytes,config:b.config,handlers:b.handlerCount,
            assets:b.assets.map((a,i)=>({index:i,mime:a.mime,encoding:a.encoding?'br':'identity',bytes:a.data.length,originalBytes:a.originalLength,webpIndex:a.webpIndex})),
            routes:[...b.routes].map(([route,value])=>({route,...value}))};
        console.log(JSON.stringify(report,null,o.json?0:2));
    } else if (command === 'serve') {
        if (o.js && o.native) throw new Error('Choose either --js or --native');
        const b = readBundle(p[0]), port = integer(o.port || p[1] || b.config.port,1,65535,'port');
        const bin = process.platform === 'win32' ? 'webzero.exe' : 'webzero';
        const candidates = [process.env.WEBZERO_BINARY,path.resolve(__dirname,'..',bin),path.join(os.homedir(),'.webzero',bin)].filter(Boolean);
        const native = !o.js && candidates.find(file => {
            if (!fs.existsSync(file)) return false;
            const probe = spawnSync(file,['--version'],{encoding:'utf8',timeout:3000,windowsHide:true});
            return probe.status === 0 && /^webzero 2\./.test(probe.stdout);
        });
        if (o.native && !native) throw new Error('No compatible v2 native binary found; build from source or use --js');
        if (native) {
            const child = spawn(native,[path.resolve(p[0]),String(port)],{stdio:'inherit',windowsHide:true});
            child.on('error',err => { console.error(err.message); process.exitCode=1; });
            child.on('exit',(code,signal) => { process.exitCode=code === null ? (signal === 'SIGINT' || signal === 'SIGTERM' ? 0 : 1) : code; });
            for (const signal of ['SIGINT','SIGTERM']) process.on(signal,()=>child.kill(signal));
        } else {
            const {server} = createServer(p[0]);
            server.on('error',err => { console.error('wz: '+err.message); process.exitCode=1; });
            server.listen(port,()=>console.log(`WebZero ${version}: http://localhost:${port} (Node development server, ${b.assets.length} assets)`));
            for (const signal of ['SIGINT','SIGTERM']) process.on(signal,()=>{ server.close(); if (server.closeAllConnections) server.closeAllConnections(); });
        }
    } else if (command === 'optimize') {
        const widths = [...new Set(String(o.widths || '320,640,1280').split(',').map(w=>integer(w,1,16383,'image width')))];
        require('./lib/optimize')(p[0],widths,integer(o.quality || 82,1,100,'JPEG quality'));
    } else throw new Error('Unknown command '+command);
}
if (require.main === module) main(process.argv.slice(2)).catch(err=>{ console.error('wz: '+err.message); process.exitCode=1; });
module.exports = {main,parse,integer};
