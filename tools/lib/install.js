'use strict';
const fs = require('fs'), path = require('path'), os = require('os'), https = require('https'), crypto = require('crypto');
const version = require('../../package.json').version;
const platforms = {'linux-x64':'webzero-linux-x64','linux-arm':'webzero-linux-arm','win32-x64':'webzero-windows-x64.exe','win32-ia32':'webzero-windows-x86.exe'};
function download(url, maximum = 32*1024*1024, redirects = 0) {
    return new Promise((resolve,reject) => {
        const parsed = new URL(url);
        if (parsed.protocol !== 'https:' || redirects > 5) return reject(new Error('Unsafe URL or too many redirects'));
        const req = https.get(parsed,{headers:{'User-Agent':'webzero/'+version}},res => {
            if ([301,302,303,307,308].includes(res.statusCode)) {
                res.resume();
                if (!res.headers.location) return reject(new Error('Redirect without location'));
                try { resolve(download(new URL(res.headers.location,parsed).href,maximum,redirects+1)); } catch(err) { reject(err); }
                return;
            }
            if (res.statusCode !== 200) { res.resume(); reject(new Error('Download HTTP '+res.statusCode)); return; }
            const chunks = []; let size = 0;
            res.on('data',chunk => { size+=chunk.length; if(size>maximum) res.destroy(new Error('Download exceeds size limit')); else chunks.push(chunk); });
            res.on('error',reject); res.on('aborted',()=>reject(new Error('Incomplete download')));
            res.on('end',()=>resolve(Buffer.concat(chunks)));
        });
        req.setTimeout(30000,()=>req.destroy(new Error('Download timed out'))); req.on('error',reject);
    });
}
async function install() {
    const name = platforms[process.platform+'-'+process.arch];
    if (!name) throw new Error('No prebuilt binary for this platform; build from source or use wz serve --js');
    const base = 'https://github.com/davitotty/webzero/releases/download/v'+version+'/';
    const checksums = (await download(base+'SHA256SUMS',65536)).toString('utf8');
    const line = checksums.split(/\r?\n/).find(line => line.trim().split(/\s+/)[1] === name);
    if (!line || !/^[a-fA-F0-9]{64}\s/.test(line)) throw new Error('Release checksum missing for '+name);
    const bytes = await download(base+name);
    if (crypto.createHash('sha256').update(bytes).digest('hex') !== line.slice(0,64).toLowerCase()) throw new Error('Binary checksum mismatch');
    const dir = path.join(os.homedir(),'.webzero'); fs.mkdirSync(dir,{recursive:true});
    const dest = path.join(dir,process.platform === 'win32'?'webzero.exe':'webzero'), temp = dest+'.'+process.pid+'.tmp';
    try { fs.writeFileSync(temp,bytes,{flag:'wx',mode:0o755}); fs.renameSync(temp,dest); }
    finally { if(fs.existsSync(temp)) fs.unlinkSync(temp); }
    fs.writeFileSync(path.join(dir,'version'),version+'\n');
    console.log('Installed WebZero '+version+' at '+dest);
}
module.exports = {install,download};
