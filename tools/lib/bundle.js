'use strict';
const fs = require('fs');
const crypto = require('crypto');
const MAGIC = 0x57454230, MAX_NODES = 1024;
function parseBundle(buf) {
    function fail(message) { throw new Error('Invalid bundle: ' + message); }
    function span(end, off, count, width) {
        if (off > end || count > Math.floor((end-off)/width)) fail('section out of bounds');
    }
    function string(off, width, empty = false) {
        const end = buf.indexOf(0, off);
        if (end < off || end >= off+width || (!empty && end === off)) fail('unterminated/empty string');
        const value = buf.toString('utf8', off, end);
        if (/[\x00-\x1f\x7f]/.test(value)) fail('control character');
        return value;
    }
    if (buf.length < 28 || buf.readUInt32LE(0) !== MAGIC) fail('magic/header');
    const version = buf.readUInt32LE(4);
    if (version !== 1 && version !== 2) fail('unsupported version ' + version);
    const trieOff = buf.readUInt32LE(8), assetsOff = buf.readUInt32LE(12);
    const handlersOff = buf.readUInt32LE(16), configOff = buf.readUInt32LE(20);
    if (buf.readUInt32LE(24) !== buf.length || configOff !== buf.length-96 || trieOff < 28 ||
        trieOff > assetsOff || assetsOff > handlersOff || handlersOff > configOff) fail('section order/size');
    span(buf.length, configOff, 1, 96);
    const config = { hostname: string(configOff, 64, true), port: buf.readUInt16LE(configOff+64),
        maxConnections: buf.readUInt16LE(configOff+66), timeout: buf.readUInt32LE(configOff+68) };
    const count = buf.readUInt32LE(configOff+72), handlers = buf.readUInt32LE(configOff+76), nodes = buf.readUInt32LE(configOff+80);
    if (!config.port || config.maxConnections > 256 || !nodes || nodes > MAX_NODES || count > MAX_NODES || handlers > MAX_NODES) fail('limits');
    span(assetsOff, trieOff, nodes, 64); span(handlersOff, assetsOff, count, 56); span(configOff, handlersOff, handlers, 8);
    const dataStart = assetsOff + count*56, assets = [];
    for (let i = 0; i < count; i++) {
        const off = assetsOff+i*56, offset = buf.readUInt32LE(off), length = buf.readUInt32LE(off+4);
        const originalLength = buf.readUInt32LE(off+8), mime = string(off+12, 32);
        const encoding = buf[off+44], webpIndex = buf.readInt32LE(off+48), rawOffset = buf.readUInt32LE(off+52);
        if (encoding > 1 || webpIndex < -1 || webpIndex >= count || (!encoding && length !== originalLength)) fail('asset metadata');
        span(handlersOff-dataStart, offset, length, 1);
        if (encoding && version === 2) span(handlersOff-dataStart, rawOffset, originalLength, 1);
        const data = buf.subarray(dataStart+offset, dataStart+offset+length);
        const raw = !encoding ? data : version === 2 ? buf.subarray(dataStart+rawOffset, dataStart+rawOffset+originalLength) : null;
        assets.push({ mime, encoding, webpIndex, data, raw, originalLength });
    }
    for (let i=0; i<handlers; i++) {
        const off = handlersOff+i*8, start = buf.readUInt32LE(off), len = buf.readUInt32LE(off+4);
        if (start < handlersOff+handlers*8) fail('handler overlap');
        span(configOff, start, len, 1);
    }
    const trie = [], parents = new Uint8Array(nodes);
    for (let i=0; i<nodes; i++) {
        const off = trieOff+i*64, segment = string(off, 32, i === 0);
        const children = [], asset = buf.readInt32LE(off+(version === 1 ? 50 : 40)), handler = buf.readInt32LE(off+(version === 1 ? 54 : 44));
        if (segment.includes('/') || asset < -1 || asset >= count || handler < -1 || handler >= handlers || (asset >= 0 && handler >= 0)) fail('route metadata');
        if (version === 1) {
            const n = buf.readUInt16LE(off+32); if (n > 8) fail('child count');
            for (let j=0; j<n; j++) children.push(buf.readUInt16LE(off+34+j*2));
        } else {
            let child = buf.readUInt32LE(off+32), previous = i;
            const sibling = buf.readUInt32LE(off+36);
            if (sibling !== 0xffffffff && (sibling <= i || sibling >= nodes)) fail('sibling');
            while (child !== 0xffffffff) {
                if (child <= previous || child >= nodes) fail('cyclic/invalid children');
                children.push(child); previous = child; child = buf.readUInt32LE(trieOff+child*64+36);
            }
        }
        for (const child of children) if (child <= i || child >= nodes || parents[child]++) fail('route graph');
        trie.push({ segment, children, asset, handler });
    }
    if (trie[0].segment || (version === 2 && buf.readUInt32LE(trieOff+36) !== 0xffffffff) || parents.subarray(1).some(p => p !== 1)) fail('unreachable routes');
    const routes = new Map(), paths = [''];
    for (let i=0; i<nodes; i++) {
        const n = trie[i], segments = new Set();
        if (n.asset >= 0 || n.handler >= 0) routes.set(paths[i] || '/', { asset: n.asset, handler: n.handler });
        for (const child of n.children) {
            if (segments.has(trie[child].segment)) fail('duplicate route');
            segments.add(trie[child].segment); paths[child] = paths[i]+'/'+trie[child].segment;
        }
    }
    const fingerprint = crypto.createHash('sha256').update(buf).digest('hex').slice(0,32);
    return { version, config, assets, routes, trie, fingerprint, bytes: buf.length, handlerCount: handlers };
}
function readBundle(file) { return parseBundle(fs.readFileSync(file)); }
module.exports = { MAGIC, MAX_NODES, parseBundle, readBundle };
