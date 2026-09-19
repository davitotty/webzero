'use strict';
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { promisify } = require('util');
const { MAGIC, MAX_NODES, parseBundle } = require('./bundle');
const compress = promisify(zlib.brotliCompress);
const MIME = {'.html':'text/html; charset=utf-8','.htm':'text/html; charset=utf-8','.css':'text/css',
    '.js':'application/javascript','.mjs':'application/javascript','.json':'application/json',
    '.txt':'text/plain; charset=utf-8','.svg':'image/svg+xml','.xml':'application/xml','.wasm':'application/wasm',
    '.png':'image/png','.jpg':'image/jpeg','.jpeg':'image/jpeg','.webp':'image/webp','.avif':'image/avif',
    '.gif':'image/gif','.ico':'image/x-icon','.woff':'font/woff','.woff2':'font/woff2','.ttf':'font/ttf',
    '.pdf':'application/pdf','.mp4':'video/mp4','.webm':'video/webm','.map':'application/json'};
const TEXT = new Set(['.html','.htm','.css','.js','.mjs','.json','.txt','.svg','.xml','.wasm','.map']);
function walk(dir, out = []) {
    for (const e of fs.readdirSync(dir, { withFileTypes: true }).sort((a,b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0)) {
        if (e.name.startsWith('.') || e.name === 'node_modules' || /\.(web|wz)$/.test(e.name)) continue;
        const p = path.join(dir,e.name);
        if (e.isDirectory()) walk(p,out); else if (e.isFile()) out.push(p);
    }
    return out;
}
async function build(source, options = {}) {
    source = path.resolve(source);
    if (!fs.statSync(source).isDirectory()) throw new Error('Source must be a directory');
    const quality = options.quality === undefined ? 5 : Number(options.quality);
    if (!Number.isInteger(quality) || quality < 0 || quality > 11) throw new Error('Brotli quality must be an integer from 0 to 11');
    const files = walk(source), fileIndices = new Map(files.map((f,i) => [f,i]));
    if (files.length > MAX_NODES) throw new Error('Too many assets (maximum '+MAX_NODES+')');
    const root = { segment:'', children:new Map(), asset:-1 }, routes = new Set();
    for (let i=0; i<files.length; i++) {
        const url = '/'+path.relative(source,files[i]).split(path.sep).join('/').replace(/\.html$/, '');
        if (routes.has(url)) throw new Error('Route collision: '+url);
        if (Buffer.byteLength(url) > 511) throw new Error('Route exceeds 511 bytes: '+url);
        routes.add(url);
        let node = root;
        for (const seg of url.split('/').filter(Boolean)) {
            if (Buffer.byteLength(seg) > 31 || /[\x00-\x1f\x7f\\?#%]/.test(seg) || seg === '*') throw new Error('Unsupported route segment: '+seg+' (maximum 31 UTF-8 bytes)');
            if (!node.children.has(seg)) node.children.set(seg, {segment:seg,children:new Map(),asset:-1});
            node = node.children.get(seg);
        }
        node.asset = i;
    }
    const nodes = [root];
    for (let i=0; i<nodes.length; i++) for (const child of nodes[i].children.values()) { child.index = nodes.length; nodes.push(child); }
    if (nodes.length > MAX_NODES) throw new Error('Too many route nodes (maximum '+MAX_NODES+')');
    const entries = new Array(files.length); let cursor = 0;
    await Promise.all(Array.from({length:Math.min(4, files.length)}, async () => {
        for (;;) {
            const i = cursor++; if (i >= files.length) return;
            const file = files[i], raw = await fs.promises.readFile(file), ext = path.extname(file).toLowerCase();
            let data = raw, encoding = 0;
            if (TEXT.has(ext) && raw.length >= 128) {
                const br = await compress(raw, { params: { [zlib.constants.BROTLI_PARAM_QUALITY]: quality } });
                if (br.length+32 < raw.length) { data = br; encoding = 1; }
            }
            const companion = file.slice(0, file.length-ext.length)+'.webp';
            const webpIndex = /\.(png|jpe?g|gif|bmp)$/i.test(ext) && fileIndices.has(companion) ? fileIndices.get(companion) : -1;
            entries[i] = { raw, data, encoding, mime:MIME[ext] || 'application/octet-stream', webpIndex };
        }
    }));
    const trie = Buffer.alloc(nodes.length*64), table = Buffer.alloc(entries.length*56), chunks = [];
    for (let i=0; i<nodes.length; i++) {
        const node = nodes[i], children = [...node.children.values()];
        trie.write(node.segment,i*64,31,'utf8'); trie.writeUInt32LE(children.length ? children[0].index : 0xffffffff,i*64+32);
        trie.writeUInt32LE(0xffffffff,i*64+36); trie.writeInt32LE(node.asset,i*64+40); trie.writeInt32LE(-1,i*64+44);
    }
    for (const node of nodes) {
        const children = [...node.children.values()];
        for (let i=0; i<children.length-1; i++) trie.writeUInt32LE(children[i+1].index,children[i].index*64+36);
    }
    let offset = 0;
    for (let i=0; i<entries.length; i++) {
        const e = entries[i], off = i*56;
        table.writeUInt32LE(offset,off); table.writeUInt32LE(e.data.length,off+4); table.writeUInt32LE(e.raw.length,off+8);
        table.write(e.mime,off+12,31,'utf8'); table[off+44] = e.encoding; table.writeInt32LE(e.webpIndex,off+48);
        chunks.push(e.data); offset += e.data.length;
        if (e.encoding) { table.writeUInt32LE(offset,off+52); chunks.push(e.raw); offset += e.raw.length; }
    }
    const config = Buffer.alloc(96), header = Buffer.alloc(28);
    config.write('localhost'); config.writeUInt16LE(8080,64); config.writeUInt16LE(256,66);
    config.writeUInt32LE(30000,68); config.writeUInt32LE(entries.length,72); config.writeUInt32LE(nodes.length,80);
    const assetsOff = 28+trie.length, configOff = assetsOff+table.length+offset;
    if (configOff+96 > 0xffffffff) throw new Error('Bundle exceeds 4 GiB format limit');
    [MAGIC,2,28,assetsOff,configOff,configOff,configOff+96].forEach((v,i) => header.writeUInt32LE(v,i*4));
    const bundle = Buffer.concat([header,trie,table,...chunks,config]);
    parseBundle(bundle); // The writer must satisfy the same contract as both readers.
    const output = path.resolve(options.output || source+'.web'), temp = output+'.'+process.pid+'.tmp';
    try { fs.writeFileSync(temp,bundle,{flag:'wx'}); fs.renameSync(temp,output); }
    finally { if (fs.existsSync(temp)) fs.unlinkSync(temp); }
    return { output, bytes:bundle.length, assets:entries.length, routes:nodes.length,
        originalBytes:entries.reduce((n,e)=>n+e.raw.length,0), transferBytes:entries.reduce((n,e)=>n+e.data.length,0), quality };
}
module.exports = { build, walk };
