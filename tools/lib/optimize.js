'use strict';
const fs = require('fs'), path = require('path');
const {spawnSync} = require('child_process');
const RASTER_EXTS = new Set(['.jpg','.jpeg','.png','.gif','.bmp']);
function cmdOptimize(sourceDir, widths, quality) {
    if (!fs.existsSync(sourceDir)) {
        console.error('wz: source directory not found: ' + sourceDir); process.exit(1);
    }

    /* Locate wzimg binary: project root → $PATH */
    function findWzimg() {
        const local = path.join(process.cwd(), process.platform === 'win32' ? 'wzimg.exe' : 'wzimg');
        if (fs.existsSync(local)) return local;
        /* Try PATH by running a no-op probe */
        const probe = spawnSync(process.platform === 'win32' ? 'where' : 'which',
                                ['wzimg'], { encoding: 'utf8' });
        if (probe.status === 0 && probe.stdout.trim()) return 'wzimg';
        return null;
    }

    const wzimg = findWzimg();
    if (!wzimg) {
        console.error('wz: wzimg not found. Build it first:');
        console.error('      cc -O2 -std=c99 -o wzimg tools/wzimg.c');
        process.exit(1);
    }

    function walk(dir, out) {
        for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
            const full = path.join(dir, e.name);
            if (e.isDirectory()) walk(full, out); else if (e.isFile()) out.push(full);
        }
    }

    const allFiles = [];
    walk(sourceDir, allFiles);
    const images = allFiles.filter(f => RASTER_EXTS.has(path.extname(f).toLowerCase()) && !/@\d+w\.jpg$/i.test(f));

    if (images.length === 0) {
        console.log('wz optimize: no raster images found in ' + sourceDir);
        return;
    }

    console.log('wz optimize: processing ' + images.length + ' image(s) at widths [' + widths.join(', ') + '] q=' + quality);

    let generated = 0;
    let skipped   = 0;

    for (const img of images) {
        const ext  = path.extname(img);
        const base = img.slice(0, img.length - ext.length);

        for (const w of widths) {
            const outPath = base + '@' + w + 'w.jpg';

            /* Skip if already up-to-date (output newer than source) */
            if (fs.existsSync(outPath)) {
                const srcMtime = fs.statSync(img).mtimeMs;
                const outMtime = fs.statSync(outPath).mtimeMs;
                if (outMtime >= srcMtime) {
                    skipped++;
                    continue;
                }
            }

            const result = spawnSync(wzimg, [img, outPath, String(w), String(quality)], {
                encoding: 'utf8',
                stdio:    ['ignore', 'pipe', 'pipe'],
            });

            if (result.status !== 0) {
                console.error('wz: wzimg failed for ' + path.basename(img) + ' @' + w + 'w:');
                if (result.stderr) process.stderr.write(result.stderr);
                throw new Error('Image optimization failed');
            }

            const rel = path.relative(sourceDir, outPath);
            console.log('  → ' + rel + (result.stderr ? '  ' + result.stderr.trim() : ''));
            generated++;
        }
    }

    console.log('\nwz optimize: ' + generated + ' generated, ' + skipped + ' up-to-date');
    console.log('Next: run `wz build ' + sourceDir + '` to bundle the variants.');
}


module.exports = cmdOptimize;
