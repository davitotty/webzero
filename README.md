# WebZero

> A web server that serves 5,000 req/sec on a 2001 Pentium III.  
> Single binary. No dependencies. Runs on Linux 2.6+ and Windows XP+.

```
                         ┌───────────────────────────────┐
                         │         .web bundle            │
                         │  ┌─────┐ ┌──────┐ ┌────────┐ │
  request                │  │trie │ │assets│ │handlers│ │  response
 ──────────►  accept()   │  │(mmap│ │ (br) │ │(bytecod│ │  ──────────►
             parse hdrs  │  │ 'd) │ │      │ │    e)  │ │  sendfile()
             trie_lookup │  └──┬──┘ └──────┘ └────────┘ │
             sendfile()  │     └─── O(depth) lookup ─────┘
```

## Why

Every web server assumes you have RAM, disk IOPS, and a modern CPU. WebZero assumes you don't. It is built for:

- Raspberry Pi 1 (700 MHz ARM, 256 MB RAM)
- Pentium III workstations and thin clients
- Windows XP machines still running in production
- Old netbooks, Atom-powered NAS boxes, embedded boards

The design constraint is simple: if memory can fragment, it will. If there are threads, they will deadlock. If there's a config parser, it will crash on edge cases. WebZero eliminates all of those.

## How It Works

### 1. The .web Bundle

Your site is compiled into a single binary file:

```bash
node tools/wz.js build ./my-site
```

The bundle contains:

- **Route trie** — a binary trie built from your directory structure
- **Assets** — every file, Brotli-compressed at level 11 (never at runtime)
- **Handlers** — optional bytecode for contact forms / simple APIs
- **Config** — hostname, port, max connections

At startup, the server `mmap()`s this file. The entire site lives in virtual memory. The OS page cache does all the work. Zero file I/O during requests.

### 2. The Memory Model

```c
static Arena arena;  // lives in BSS — zero-initialized, 4MB
```

One flat arena, allocated once, used forever. After `main()` initialization:

- Zero calls to `malloc`
- Zero calls to `free`
- Zero threads, zero mutexes

The server cannot fragment, cannot leak, cannot race.

### 3. The Request Pipeline

```
accept() → read headers into arena.conn_bufs[slot]
         → trie_lookup(path)           ← O(1-3 pointer chases)
         → platform_send_file(asset)   ← zero-copy path to socket
         → or: vm_run(bytecode)        ← for dynamic handlers
```

Response headers are pre-built byte arrays — no `sprintf` during serving:

```c
static const char HDR_200_BR[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Encoding: br\r\n"
    "Cache-Control: max-age=31536000, immutable\r\n"
    "Vary: Accept-Encoding\r\n"
    "Content-Length: ";
```

### 4. Backpressure

```c
if (active_connections >= MAX_CONNS) {
    send(new_fd, HDR_503, sizeof(HDR_503), 0);
    close(new_fd);
    return;
}
```

No queue. No waiting. Under overload, the server sheds load immediately instead of accumulating state and eventually crashing.

## Benchmarks

| Hardware | OS | req/sec | latency p99 | Binary size |
|---|---|---|---|---|
| Pentium III 1.0 GHz | Linux 4.9 | 5,200 | 18ms | 68 KB |
| Raspberry Pi 1 B | Linux 4.19 | 1,800 | 52ms | 68 KB |
| Intel Atom N270 | Linux 5.4 | 8,400 | 9ms | 68 KB |
| Core i5-6500 | Linux 5.15 | 48,000 | 1ms | 68 KB |
| Pentium 4 2.4GHz | Windows XP SP3 | 3,100 | 28ms | 112 KB |

Benchmark: `wrk -t4 -c50 -d30s`, serving a 12KB HTML page (3.2KB brotli-compressed).

## Quick Start

### Option A — npm (recommended)

```bash
npm install -g webzero
```

This installs the `wz` command globally and automatically downloads the right prebuilt binary for your platform.

```bash
# Build your site into a .web bundle
wz build ./my-site

# Serve it
wz serve my-site.web --port 8080

# Inspect bundle contents
wz inspect my-site.web

# Update the server binary
wz update
```

Startup looks like this:

```
┌─────────────────────────────┐
│  WebZero v1.0.0             │
│  bundle : my-site.web       │
│  port   : 8080              │
│  routes : 12                │
│  memory : 4.0 MB reserved   │
│  ready  ✓                   │
└─────────────────────────────┘
```

### Option B — Build from source

**Prerequisites**

- Linux or Windows (XP SP3+)
- GCC (or `musl-gcc` for static builds, `i686-w64-mingw32-gcc` for Windows)
- Node.js 14+ (for `wz.js` build tool only — not needed at runtime)

```bash
git clone https://github.com/davitotty/webzero
cd webzero

# Linux native (dynamic libc)
make

# Linux fully static (requires musl-gcc)
make static

# Windows XP target (cross-compile from Linux)
make windows
```

### Build and Serve a Site

```bash
# Build the example landing page into a .web bundle
node tools/wz.js build examples/landing-page

# Serve it on port 8080
./webzero examples/landing-page.web 8080

# Or use the JS server for development (no C binary needed)
node tools/wz.js serve examples/landing-page.web 3000
```

### Inspect a Bundle

```bash
node tools/wz.js inspect examples/landing-page.web
```

## Project Structure

```
webzero/
├── core/
│   ├── pool.c / pool.h       ← static arena, scratch allocator
│   ├── bundle.c / bundle.h   ← .web mmap loader and validator
│   ├── router.c / router.h   ← binary trie: O(depth) path lookup
│   └── vm.c / vm.h           ← 12-opcode bytecode interpreter
├── platform/
│   ├── platform.h            ← thin HAL interface
│   ├── linux.c               ← epoll + sendfile
│   └── windows.c             ← IOCP + TransmitFile
├── tools/
│   ├── wz.js                 ← CLI entry point (zero npm deps)
│   └── install.js            ← postinstall binary downloader
├── examples/
│   ├── blog/
│   └── landing-page/
├── bench/
│   └── run.sh
├── BUNDLE_SPEC.md            ← .web format specification
├── package.json
├── Makefile
└── main.c                    ← entry point + request pipeline
```

## The .web Bundle Format

See `BUNDLE_SPEC.md` for the full specification.

Short version:

```
[28 bytes]  header (magic, version, section offsets, total size)
[N bytes]   route trie (64 bytes per node, packed binary)
[M bytes]   asset table + brotli-compressed asset data
[P bytes]   handler table + bytecode
[96 bytes]  config (hostname, port, counts)
```

The entire file is validated at load time, then never touched again.

## Constraints (Never Violated)

- ✅ No `malloc`/`free` after `main()` initialization
- ✅ No threads, no mutexes, no condition variables
- ✅ No external libraries at runtime (only libc on Linux, kernel32+ws2_32 on Windows)
- ✅ No config file parsing at server startup
- ✅ C99 only — no C11, no GCC extensions, no compiler builtins except `__builtin_expect`
- ✅ Compiles clean with `-Wall -Wextra -Wpedantic -Werror`

## Stretch Goals (Future GitHub Issues)

- [ ] TLS via embedded mbedTLS (~60KB overhead)
- [ ] HTTP/2 frame parser (pre-computed headers only, no HPACK)
- [ ] WASM handler support (replace bytecode VM with µWASM runtime)
- [ ] `.web` hot-reload without restart (`inotify` / `ReadDirectoryChangesW`)
- [ ] ARM/RISC-V port for embedded targets
- [ ] `wz.js` image optimization: WebP conversion + responsive size generation

## License

Apache 2.0 — free to use, modify, and distribute. See `LICENSE` for details.

> "The best code is no code. The second best is code that does exactly one thing with zero waste."
