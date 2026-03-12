#!/usr/bin/env node
/**
 * wz.js — WebZero CLI
 * Zero npm dependencies. Node.js 14+ CommonJS only.
 *
 * Commands:
 *   wz build <dir>              → <dir>.web bundle
 *   wz serve <file.web> [--port <n>]  → spawns C binary
 *   wz inspect <file.web>       → dump bundle contents
 *   wz update                   → download/refresh binary
 *   wz version                  → print versions
 */
'use strict';

const fs = require('fs');
const path = require('path');
const os = require('os');
const zlib = require('zlib');
const http = require('http');
const https = require('https');
const { spawn } = require('child_process');

/* ── Constants ─────────────────────────────────────────────────────── */

const WZ_VERSION = '1.0.0';
const WEB_MAGIC = 0x57454230;
const WEB_VERSION = 1;
const DISK_TRIE_NODE_SIZE = 64;
const ASSET_ENTRY_SIZE = 56;

const GITHUB_REPO = 'davitotty/webzero';

const PLATFORM_MAP = {
    'linux-x64': 'webzero-linux-x64',
    'linux-arm': 'webzero-linux-arm',
    'win32-x64': 'webzero-windows-x64.exe',
    'win32-ia32': 'webzero-windows-x86.exe',
};

const MIME_MAP = {
    '.html': 'text/html; charset=utf-8', '.htm': 'text/html; charset=utf-8',
    '.css': 'text/css', '.js': 'application/javascript', '.json': 'application/json',
    '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
    '.gif': 'image/gif', '.webp': 'image/webp', '.svg': 'image/svg+xml',
    '.ico': 'image/x-icon', '.woff': 'font/woff', '.woff2': 'font/woff2',
    '.ttf': 'font/ttf', '.txt': 'text/plain; charset=utf-8',
    '.xml': 'application/xml', '.pdf': 'application/pdf',
    '.mp4': 'video/mp4', '.webm': 'video/webm',
};

function mimeOf(f) { return MIME_MAP[path.extname(f).toLowerCase()] || 'application/octet-stream'; }

/* ── Binary resolution ─────────────────────────────────────────────── */

function getBinaryPath() {
    const binName = process.platform === 'win32' ? 'webzero.exe' : 'webzero';
    return path.join(os.homedir(), '.webzero', binName);
}

function getBinaryVersion() {
    try {
        const vf = path.join(os.homedir(), '.webzero', 'version');
        return fs.existsSync(vf) ? fs.readFileSync(vf, 'utf8').trim() : null;
    } catch (_) { return null; }
}

function getPlatformName() {
    return PLATFORM_MAP[`${process.platform}-${process.arch}`] || null;
}

/* ── Trie helpers ──────────────────────────────────────────────────── */

function urlPathFromFile(sourceDir, filePath) {
    let rel = path.relative(sourceDir, filePath).replace(/\\/g, '/');
    if (rel.endsWith('.html')) rel = rel.slice(0, -5);
    if (!rel.startsWith('/')) rel = '/' + rel;
    return rel;
}

class TrieNode {
    constructor(seg) { this.segment = seg || ''; this.children = {}; this.assetIdx = -1; this.handlerIdx = -1; }
}

function trieInsert(root, urlPath, assetIdx, handlerIdx) {
    const segs = urlPath.split('/').filter(Boolean);
    let node = root;
    for (const seg of segs) {
        if (!node.children[seg]) node.children[seg] = new TrieNode(seg);
        node = node.children[seg];
    }
    node.assetIdx = assetIdx; node.handlerIdx = handlerIdx;
}

function flattenTrie(root) {
    const nodes = []; const queue = [root]; const indexMap = new Map();
    while (queue.length > 0) {
        const node = queue.shift(); indexMap.set(node, nodes.length); nodes.push(node);
        for (const c of Object.values(node.children)) queue.push(c);
    }
    return nodes.map(n => ({
        segment: n.segment,
        children: Object.values(n.children).map(c => indexMap.get(c)),
        assetIdx: n.assetIdx, handlerIdx: n.handlerIdx,
    }));
}

function serializeTrie(nodes) {
    const buf = Buffer.alloc(nodes.length * DISK_TRIE_NODE_SIZE, 0);
    for (let i = 0; i < nodes.length; i++) {
        const off = i * DISK_TRIE_NODE_SIZE; const node = nodes[i];
        Buffer.from(node.segment, 'utf8').copy(buf, off, 0, 31);
        const cc = Math.min(node.children.length, 8);
        buf.writeUInt16LE(cc, off + 32);
        for (let j = 0; j < cc; j++) buf.writeUInt16LE(node.children[j], off + 34 + j * 2);
        buf.writeInt32LE(node.assetIdx, off + 50);
        buf.writeInt32LE(node.handlerIdx, off + 54);
    }
    return buf;
}

/* ── Compression ───────────────────────────────────────────────────── */

function brotliCompress(data) {
    return new Promise((res, rej) => zlib.brotliCompress(data, {
        params: { [zlib.constants.BROTLI_PARAM_QUALITY]: 11 }
    }, (e, r) => e ? rej(e) : res(r)));
}

/* ── Build ─────────────────────────────────────────────────────────── */

async function cmdBuild(sourceDir) {
    if (!fs.existsSync(sourceDir)) {
        console.error('wz: source directory not found: ' + sourceDir); process.exit(1);
    }
    console.log('wz: building ' + sourceDir + '...');

    function walk(dir, out) {
        for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
            const full = path.join(dir, e.name);
            if (e.isDirectory()) walk(full, out); else if (e.isFile()) out.push(full);
        }
    }
    const files = []; walk(sourceDir, files);

    const assetChunks = []; const assetEntries = []; const handlerEntries = [];
    const trieRoot = new TrieNode(''); let currentOffset = 0;

    for (const file of files) {
        if (path.extname(file).toLowerCase() === '.wz') continue;
        const raw = fs.readFileSync(file);
        const compressed = await brotliCompress(raw);
        const urlPath = urlPathFromFile(sourceDir, file);
        const assetIdx = assetEntries.length;
        assetEntries.push({
            offset: currentOffset, compressed_len: compressed.length,
            original_len: raw.length, mime: mimeOf(file), encoding: 1
        });
        assetChunks.push(compressed); currentOffset += compressed.length;
        trieInsert(trieRoot, urlPath, assetIdx, -1);
        console.log('  + ' + urlPath.padEnd(40) + raw.length + 'B → ' + compressed.length + 'B (br)');
    }

    const trieNodes = flattenTrie(trieRoot); const trieBytes = serializeTrie(trieNodes);

    const configBuf = Buffer.alloc(96, 0);
    Buffer.from('localhost').copy(configBuf, 0, 0, 63);
    configBuf.writeUInt16LE(8080, 64); configBuf.writeUInt16LE(256, 66);
    configBuf.writeUInt32LE(30000, 68); configBuf.writeUInt32LE(assetEntries.length, 72);
    configBuf.writeUInt32LE(handlerEntries.length, 76); configBuf.writeUInt32LE(trieNodes.length, 80);

    const assetTableBuf = Buffer.alloc(assetEntries.length * ASSET_ENTRY_SIZE, 0);
    for (let i = 0; i < assetEntries.length; i++) {
        const e = assetEntries[i]; const off = i * ASSET_ENTRY_SIZE;
        assetTableBuf.writeUInt32LE(e.offset, off); assetTableBuf.writeUInt32LE(e.compressed_len, off + 4);
        assetTableBuf.writeUInt32LE(e.original_len, off + 8);
        Buffer.from(e.mime).copy(assetTableBuf, off + 12, 0, 31);
        assetTableBuf.writeUInt8(e.encoding, off + 44);
    }
    const assetDataBuf = Buffer.concat(assetChunks);

    const HEADER_SIZE = 28; const trieOff = HEADER_SIZE;
    const assetsOff = trieOff + trieBytes.length;
    const handlersOff = assetsOff + assetTableBuf.length + assetDataBuf.length;
    const configOff = handlersOff;
    const totalSize = configOff + configBuf.length;

    const header = Buffer.alloc(HEADER_SIZE, 0);
    header.writeUInt32LE(WEB_MAGIC, 0); header.writeUInt32LE(WEB_VERSION, 4);
    header.writeUInt32LE(trieOff, 8); header.writeUInt32LE(assetsOff, 12);
    header.writeUInt32LE(handlersOff, 16); header.writeUInt32LE(configOff, 20);
    header.writeUInt32LE(totalSize, 24);

    const bundle = Buffer.concat([header, trieBytes, assetTableBuf, assetDataBuf, configBuf]);
    const outPath = sourceDir.replace(/[/\\]$/, '') + '.web';
    fs.writeFileSync(outPath, bundle);

    console.log('\nwz: bundle written to ' + outPath);
    console.log('    total size : ' + bundle.length + ' bytes');
    console.log('    assets     : ' + assetEntries.length);
    console.log('    trie nodes : ' + trieNodes.length);
    console.log('    handlers   : ' + handlerEntries.length);
}

/* ── Inspect ───────────────────────────────────────────────────────── */

function cmdInspect(bundlePath) {
    if (!fs.existsSync(bundlePath)) {
        console.error('wz: bundle not found: ' + bundlePath); process.exit(1);
    }
    const buf = fs.readFileSync(bundlePath);
    const magic = buf.readUInt32LE(0); const version = buf.readUInt32LE(4);
    const trieOff = buf.readUInt32LE(8); const assetsOff = buf.readUInt32LE(12);
    const handlersOff = buf.readUInt32LE(16); const configOff = buf.readUInt32LE(20);
    const totalSize = buf.readUInt32LE(24);

    console.log('=== WebZero Bundle: ' + bundlePath + ' ===');
    console.log('magic          : 0x' + magic.toString(16).toUpperCase());
    console.log('version        : ' + version);
    console.log('total size     : ' + totalSize + ' bytes');
    console.log('route_table_at : ' + trieOff);
    console.log('assets_at      : ' + assetsOff);
    console.log('handlers_at    : ' + handlersOff);
    console.log('config_at      : ' + configOff);

    const hostname = buf.slice(configOff, configOff + 64).toString('utf8').replace(/\0.*$/, '');
    const port = buf.readUInt16LE(configOff + 64);
    const maxConn = buf.readUInt16LE(configOff + 66);
    const keepaliveMs = buf.readUInt32LE(configOff + 68);
    const assetCount = buf.readUInt32LE(configOff + 72);
    const handlerCount = buf.readUInt32LE(configOff + 76);
    const trieNodeCount = buf.readUInt32LE(configOff + 80);

    console.log('\nConfig:');
    console.log('  hostname     : ' + hostname);
    console.log('  port         : ' + port);
    console.log('  max_conn     : ' + maxConn);
    console.log('  keepalive_ms : ' + keepaliveMs);
    console.log('  assets       : ' + assetCount);
    console.log('  handlers     : ' + handlerCount);
    console.log('  trie nodes   : ' + trieNodeCount);

    console.log('\nAssets:');
    for (let i = 0; i < assetCount; i++) {
        const base = assetsOff + i * ASSET_ENTRY_SIZE;
        const clen = buf.readUInt32LE(base + 4); const olen = buf.readUInt32LE(base + 8);
        const mime = buf.slice(base + 12, base + 44).toString('utf8').replace(/\0.*$/, '');
        const enc = buf.readUInt8(base + 44);
        const ratio = olen > 0 ? ((1 - clen / olen) * 100).toFixed(1) : '0.0';
        console.log('  [' + i + '] ' + mime.padEnd(38) + ' ' + olen + 'B → ' + clen + 'B (−' + ratio + '%) enc=' + enc);
    }

    console.log('\nTrie Nodes:');
    for (let i = 0; i < trieNodeCount; i++) {
        const base = trieOff + i * DISK_TRIE_NODE_SIZE;
        const seg = buf.slice(base, base + 32).toString('utf8').replace(/\0.*$/, '') || '(root)';
        const cc = buf.readUInt16LE(base + 32);
        const ai = buf.readInt32LE(base + 50); const hi = buf.readInt32LE(base + 54);
        const ch = []; for (let j = 0; j < cc; j++) ch.push(buf.readUInt16LE(base + 34 + j * 2));
        console.log('  [' + i + '] "' + seg + '" children=[' + ch.join(',') + '] asset=' + ai + ' handler=' + hi);
    }
}

/* ── Serve ─────────────────────────────────────────────────────────── */

function printStartupBanner(bundleFile, port, routeCount) {
    const bname = path.basename(bundleFile);
    const ver = 'v' + WZ_VERSION;
    process.stdout.write('\n');
    process.stdout.write('┌─────────────────────────────────┐\n');
    process.stdout.write('│  WebZero ' + ver.padEnd(23) + '│\n');
    process.stdout.write('│  bundle : ' + bname.padEnd(22) + '│\n');
    process.stdout.write('│  port   : ' + String(port).padEnd(22) + '│\n');
    process.stdout.write('│  routes : ' + String(routeCount).padEnd(22) + '│\n');
    process.stdout.write('│  memory : 4.0 MB reserved        │\n');
    process.stdout.write('│  ready  ✓                        │\n');
    process.stdout.write('└─────────────────────────────────┘\n');
    process.stdout.write('\n');
}

function getRouteCount(bundleFile) {
    try {
        const buf = fs.readFileSync(bundleFile); const configOff = buf.readUInt32LE(20);
        return buf.readUInt32LE(configOff + 80);
    } catch (_) { return 0; }
}

function cmdServe(bundleFile, port) {
    try {
        if (!fs.existsSync(bundleFile)) {
            console.error('wz: bundle not found: ' + bundleFile); process.exit(1);
        }

        const buf = fs.readFileSync(bundleFile);
        const configOff = buf.readUInt32LE(20);
        const assetCount = buf.readUInt32LE(configOff + 72);
        const trieNodeCount = buf.readUInt32LE(configOff + 80);
        const assetsOff = buf.readUInt32LE(12);
        const trieOff = buf.readUInt32LE(8);

        // Load assets
        const assets = [];
        for (let i = 0; i < assetCount; i++) {
            const base = assetsOff + i * ASSET_ENTRY_SIZE;
            const offset = buf.readUInt32LE(base);
            const clen = buf.readUInt32LE(base + 4);
            const olen = buf.readUInt32LE(base + 8);
            const mime = buf.slice(base + 12, base + 44).toString('utf8').replace(/\0.*$/, '');
            const enc = buf.readUInt8(base + 44);
            // data starts after asset table
            const dataStart = assetsOff + assetCount * ASSET_ENTRY_SIZE;
            const data = buf.slice(dataStart + offset, dataStart + offset + clen);
            assets.push({ mime, enc, data, olen });
        }

        // Load trie
        const trieNodes = [];
        for (let i = 0; i < trieNodeCount; i++) {
            const base = trieOff + i * DISK_TRIE_NODE_SIZE;
            const seg = buf.slice(base, base + 32).toString('utf8').replace(/\0.*$/, '');
            const cc = buf.readUInt16LE(base + 32);
            const children = [];
            for (let j = 0; j < cc; j++) children.push(buf.readUInt16LE(base + 34 + j * 2));
            const ai = buf.readInt32LE(base + 50);
            trieNodes.push({ seg, children, ai });
        }

        // Trie lookup
        function lookup(urlPath) {
            const segs = urlPath.split('/').filter(Boolean);
            let node = trieNodes[0];
            for (const seg of segs) {
                const child = node.children.map(i => trieNodes[i]).find(n => n.seg === seg);
                if (!child) return -1;
                node = child;
            }
            return node.ai;
        }

        printStartupBanner(bundleFile, port, trieNodeCount);

        const server = http.createServer(function(req, res) {
            let urlPath = req.url.split('?')[0];
            if (urlPath === '/') urlPath = '/index';

            let ai = lookup(urlPath);
            if (ai === -1 && !urlPath.includes('.')) ai = lookup(urlPath + '/index');
            if (ai === -1) { res.writeHead(404); res.end('Not Found'); return; }

            const asset = assets[ai];
            const acceptsBrotli = (req.headers['accept-encoding'] || '').includes('br');

            if (asset.enc === 1 && acceptsBrotli) {
                res.writeHead(200, {
                    'Content-Type': asset.mime,
                    'Content-Encoding': 'br',
                    'Content-Length': asset.data.length,
                    'Cache-Control': 'max-age=3600',
                });
                res.end(asset.data);
            } else if (asset.enc === 1) {
                // decompress for browsers that don't support brotli
                zlib.brotliDecompress(asset.data, function(err, decoded) {
                    if (err) { res.writeHead(500); res.end('decompress error'); return; }
                    res.writeHead(200, {
                        'Content-Type': asset.mime,
                        'Content-Length': decoded.length,
                        'Cache-Control': 'max-age=3600',
                    });
                    res.end(decoded);
                });
            } else {
                res.writeHead(200, { 'Content-Type': asset.mime, 'Content-Length': asset.data.length });
                res.end(asset.data);
            }
        });

        server.listen(port, function() {});
        process.on('SIGINT', function() { server.close(); process.exit(0); });
        process.on('SIGTERM', function() { server.close(); process.exit(0); });

    } catch (err) {
        console.error('wz serve error: ' + err.message); process.exit(1);
    }
}

/* ── Update (download binary) ──────────────────────────────────────── */

function downloadFile(url, destPath, cb) {
    const proto = url.startsWith('https') ? https : http;
    proto.get(url, function (res) {
        if (res.statusCode === 301 || res.statusCode === 302) {
            return downloadFile(res.headers.location, destPath, cb);
        }
        if (res.statusCode !== 200) {
            return cb(new Error('HTTP ' + res.statusCode + ' from ' + url));
        }

        const total = parseInt(res.headers['content-length'] || '0', 10);
        let received = 0; const chunks = [];
        const BAR_WIDTH = 20;

        res.on('data', function (chunk) {
            chunks.push(chunk); received += chunk.length;
            if (total > 0) {
                const pct = Math.floor((received / total) * 100);
                const fill = Math.floor((received / total) * BAR_WIDTH);
                const bar = '[' + '='.repeat(fill) + ' '.repeat(BAR_WIDTH - fill) + ']';
                const kb = (received / 1024).toFixed(0) + 'KB';
                process.stdout.write('\r' + bar + ' ' + pct + '% ' + kb + '   ');
            }
        });
        res.on('end', function () {
            process.stdout.write('\n');
            try { fs.writeFileSync(destPath, Buffer.concat(chunks)); cb(null); }
            catch (e) { cb(e); }
        });
        res.on('error', cb);
    }).on('error', cb);
}

function cmdUpdate() {
    try {
        const platformName = getPlatformName();
        if (!platformName) {
            console.error('wz: unsupported platform: ' + process.platform + '-' + process.arch);
            console.error('    Supported: linux-x64, linux-arm, win32-x64, win32-ia32');
            process.exit(1);
        }

        const wzDir = path.join(os.homedir(), '.webzero');
        const binExt = process.platform === 'win32' ? 'webzero.exe' : 'webzero';
        const binPath = path.join(wzDir, binExt);
        const verPath = path.join(wzDir, 'version');

        if (!fs.existsSync(wzDir)) fs.mkdirSync(wzDir, { recursive: true });

        const url = 'https://github.com/' + GITHUB_REPO + '/releases/latest/download/' + platformName;
        console.log('Downloading webzero binary for ' + process.platform + '-' + process.arch + '...');
        console.log('URL: ' + url);

        downloadFile(url, binPath, function (err) {
            if (err) {
                console.error('\nwz: download failed: ' + err.message);
                console.error('    Attempted URL: ' + url);
                console.error('    You can manually download and place at: ' + binPath);
                process.exit(1);
            }

            if (process.platform !== 'win32') {
                try { fs.chmodSync(binPath, 0o755); } catch (_) { }
            }

            try { fs.writeFileSync(verPath, WZ_VERSION); } catch (_) { }

            console.log('Done. Binary saved to ' + binPath);
            console.log('Version: ' + WZ_VERSION);
        });
    } catch (err) {
        console.error('wz update error: ' + err.message); process.exit(1);
    }
}

/* ── Version ───────────────────────────────────────────────────────── */

function cmdVersion() {
    console.log('wz CLI    : v' + WZ_VERSION);
    const binVer = getBinaryVersion();
    const binPath = getBinaryPath();
    if (binVer) {
        console.log('binary    : v' + binVer + ' (' + binPath + ')');
    } else if (fs.existsSync(binPath)) {
        console.log('binary    : installed (version unknown) — ' + binPath);
    } else {
        console.log('binary    : not installed — run: wz update');
    }
    console.log('node      : ' + process.version);
    console.log('platform  : ' + process.platform + '-' + process.arch);
}

/* ── CLI entry ─────────────────────────────────────────────────────── */

try {
    const args = process.argv.slice(2);
    const cmd = args[0];

    // Manual --port parser
    function getPort(defaultPort) {
        const idx = args.indexOf('--port');
        if (idx !== -1 && args[idx + 1]) return parseInt(args[idx + 1], 10);
        // positional fallback (legacy: wz serve bundle.web 8080)
        for (let i = 2; i < args.length; i++) {
            const n = parseInt(args[i], 10);
            if (!isNaN(n) && n > 0 && n < 65536) return n;
        }
        return defaultPort;
    }

    if (!cmd || cmd === '--help' || cmd === '-h') {
        process.stdout.write([
            '',
            '  WebZero CLI v' + WZ_VERSION,
            '',
            '  wz build <dir>               build a .web bundle from a directory',
            '  wz serve <file.web> [--port <n>]  start the server (spawns C binary)',
            '  wz inspect <file.web>        dump bundle contents',
            '  wz update                    download/refresh C binary for this platform',
            '  wz version                   print CLI and binary versions',
            '',
        ].join('\n') + '\n');
        process.exit(0);
    }

    switch (cmd) {
        case 'build':
            if (!args[1]) { console.error('wz: build requires a source directory'); process.exit(1); }
            cmdBuild(args[1]).catch(function (e) { console.error('wz build error: ' + e.message); process.exit(1); });
            break;

        case 'serve':
            if (!args[1]) { console.error('wz: serve requires a bundle path'); process.exit(1); }
            cmdServe(args[1], getPort(8080));
            break;

        case 'inspect':
            if (!args[1]) { console.error('wz: inspect requires a bundle path'); process.exit(1); }
            cmdInspect(args[1]);
            break;

        case 'update':
            cmdUpdate();
            break;

        case 'version':
        case '-v':
        case '--version':
            cmdVersion();
            break;

        default:
            console.error('wz: unknown command \'' + cmd + '\'. Run: wz --help');
            process.exit(1);
    }
} catch (err) {
    console.error('wz: unexpected error: ' + err.message);
    process.exit(1);
}
