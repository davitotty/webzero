'use strict';
const http = require('http');
const zlib = require('zlib');
const { readBundle } = require('./bundle');
function quality(value, token, fallback) {
    let exact, wildcard;
    for (const part of String(value || '').split(',')) {
        const [name, ...params] = part.trim().toLowerCase().split(';');
        let q = 1000;
        for (const p of params) if (p.trim().startsWith('q=')) {
            const v = p.trim().slice(2);
            q = /^(?:0(?:\.\d{0,3})?|1(?:\.0{0,3})?)$/.test(v) ? Number(v)*1000 : 0;
        }
        if (name === token) exact = q; else if (name === '*') wildcard = q;
    }
    if (exact !== undefined) return exact;
    if (token === 'identity') return wildcard === 0 ? 0 : fallback;
    return wildcard === undefined ? fallback : wildcard;
}
function lookup(bundle, url) {
    let route = bundle.routes.get(url);
    if (route) return route;
    // Preserve segment wildcards in legacy bundles; exact segment wins.
    let node = bundle.trie[0];
    for (const seg of url.split('/').filter(Boolean)) {
        let next = node.children.find(i => bundle.trie[i].segment === seg);
        if (next === undefined) next = node.children.find(i => bundle.trie[i].segment === '*');
        if (next === undefined) return null;
        node = bundle.trie[next];
    }
    if (node.asset < 0 && node.handler < 0) {
        const index = node.children.find(i => bundle.trie[i].segment === 'index');
        if (index === undefined) return null;
        node = bundle.trie[index];
    }
    return node.asset >= 0 || node.handler >= 0 ? {asset:node.asset,handler:node.handler} : null;
}
function createServer(file) {
    const bundle = readBundle(file);
    // Legacy bundles lack identity payloads. Decode once, never per request.
    let decodedBytes = 0;
    for (const a of bundle.assets) if (!a.raw) {
        decodedBytes += a.originalLength;
        if (decodedBytes > 128*1024*1024) throw new Error('Legacy decoded assets exceed 128 MiB; rebuild as v2');
        a.raw = zlib.brotliDecompressSync(a.data,{maxOutputLength:Math.max(1,a.originalLength)});
        if (a.raw.length !== a.originalLength) throw new Error('Invalid legacy decompressed length');
    }
    // Precompute immutable representation metadata outside the request path.
    for (let i=0; i<bundle.assets.length; i++) {
        const a=bundle.assets[i]; a.responses=[];
        for (const encoded of [0,1]) {
            const data=encoded ? a.data : a.raw;
            const headers={'Content-Type':a.mime,'Content-Length':data.length,
                'Cache-Control':'public, max-age=0, must-revalidate',Vary:'Accept, Accept-Encoding',
                ETag:'W/"'+bundle.fingerprint+'-'+i+'-'+encoded+'"',
                'Accept-Ranges':'bytes','X-Content-Type-Options':'nosniff'};
            if(encoded) headers['Content-Encoding']='br';
            a.responses.push({data,headers});
        }
    }
    for(const [route,value] of [...bundle.routes]) if(route.endsWith('/index')) {
        const alias=route.slice(0,-6) || '/';
        if(!bundle.routes.has(alias)) bundle.routes.set(alias,value);
    }
    const server = http.createServer({maxHeaderSize:8192}, (req,res) => {
        const end = (status, headers = {}) => { res.writeHead(status, {'Content-Length':0,...headers}); res.end(); };
        req.resume(); // Drain bodies before reusing the connection.
        if (req.method !== 'GET' && req.method !== 'HEAD') return end(405, {Allow:'GET, HEAD'});
        let url;
        try { const raw=req.url.split('?')[0]; url=raw.includes('%') ? decodeURIComponent(raw) : raw; } catch (_) { return end(400); }
        if (!url.startsWith('/') || /[\x00-\x1f\x7f\\?#]/.test(url) || url.split('/').some(s => s === '.' || s === '..')) return end(400);
        if (Buffer.byteLength(url) > 511) return end(414);
        url = url.replace(/\/+/g,'/').replace(/\/$/,'').replace(/\.html$/,'') || '/';
        const route = lookup(bundle,url);
        if (!route) return end(404);
        if (route.handler >= 0) return end(501); // Bytecode execution is native-only.
        let index = route.asset, asset = bundle.assets[index];
        if (quality(req.headers.accept,'image/webp',0) && asset.webpIndex >= 0) { index = asset.webpIndex; asset = bundle.assets[index]; }
        const acceptEncoding=req.headers['accept-encoding'];
        const encoded = asset.encoding && acceptEncoding !== 'identity' && quality(acceptEncoding,'br',0) > 0;
        if (!encoded && acceptEncoding && acceptEncoding !== 'identity' && !quality(acceptEncoding,'identity',1000)) return end(406);
        const representation=asset.responses[Number(!!encoded)];
        const data=representation.data, etag=representation.headers.ETag;
        let headers=representation.headers;
        if (req.headers['if-none-match'] && String(req.headers['if-none-match']).split(',').some(s => s.trim() === '*' || s.trim().replace(/^W\//,'') === etag.slice(2))) {
            res.writeHead(304,headers); res.end(); return;
        }
        let status = 200, body = data;
        const range = req.method === 'GET' && !req.headers['if-range'] && /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || '');
        if (range && (range[1] || range[2])) {
            headers={...headers};
            const first = range[1] ? Number(range[1]) : Math.max(0,data.length-Number(range[2]));
            const last = range[1] && range[2] ? Math.min(data.length-1,Number(range[2])) : data.length-1;
            if (!Number.isSafeInteger(first) || !Number.isSafeInteger(last) || first > last || first >= data.length) {
                return end(416,{...headers,'Content-Length':0,'Content-Range':'bytes */'+data.length});
            }
            status = 206; body = data.subarray(first,last+1);
            headers['Content-Range'] = 'bytes '+first+'-'+last+'/'+data.length; headers['Content-Length'] = body.length;
        }
        res.writeHead(status,headers); res.end(req.method === 'HEAD' ? undefined : body);
    });
    server.maxConnections = bundle.config.maxConnections || 256;
    server.headersTimeout = Math.min(bundle.config.timeout || 30000,30000);
    server.requestTimeout = 30000; server.keepAliveTimeout = bundle.config.timeout || 30000;
    server.setTimeout(bundle.config.timeout || 30000, socket => socket.destroy());
    server.on('clientError',(err,socket) => {
        if (socket.writable) socket.end('HTTP/1.1 '+(err.code === 'HPE_HEADER_OVERFLOW' ? '431 Request Header Fields Too Large' : '400 Bad Request')+'\r\nConnection: close\r\nContent-Length: 0\r\n\r\n');
    });
    return {server,bundle};
}
module.exports = {createServer,quality,lookup};
