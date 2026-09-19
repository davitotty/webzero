# WebZero

A small, dependency-free native HTTP server for prebuilt sites. Compile a directory into one `.web` bundle, then serve it from a memory mapping with a fixed connection arena.

WebZero 2 replaces the original HTTP and networking implementation, validates bundle contents before serving, and includes automated tests and reproducible benchmarks. See [the measured comparison](docs/BENCHMARKS.md) and [migration notes](docs/MIGRATION.md).

## Quick start

Build the checked-out version with Node.js 18 or newer:

```sh
node tools/wz.js build examples/landing-page
node tools/wz.js serve examples/landing-page.web --js --port 8080
```

Build the native Linux server with GCC and Make:

```sh
make
node tools/wz.js serve examples/landing-page.web --native --port 8080
# Or run it directly, without Node:
./webzero examples/landing-page.web 8080
```

The CLI prefers a compatible native binary in the checkout or `~/.webzero`. If neither exists, it explicitly identifies the Node development server it starts. `--native` fails rather than silently switching implementations; `--js` always selects Node. `WEBZERO_BINARY` can select an explicit native executable.

## What changed in v2

- **Correct nonblocking I/O:** response buffers belong to their connections; partial writes resume when the socket is writable. Slow readers do not block other clients. Keep-alive, fragmented headers, request bodies and pipelining retain their framing.
- **Portable networking:** Linux uses level-triggered epoll. Windows uses nonblocking Winsock/select, with pointer-sized socket handles. Both backends share connection and HTTP logic.
- **Validated bundles:** loaders check section bounds, asset sizes, indices, strings, route ownership and cycles. Malformed bundles fail at startup.
- **Reliable routing:** v2 removes the eight-child-per-directory limit. It retains a bounded 1,024-node route arena. Duplicate routes, oversized segments and unsupported names fail the build instead of silently losing content.
- **Offline compression:** compressible files get Brotli only when it saves space. Images, video and precompressed fonts stay raw. Compressed assets also include identity bytes, so the native server needs no decompression library.
- **HTTP behavior:** GET/HEAD, exact method matching, encoding quality exclusions, WebP negotiation, weak ETags/304, single byte ranges, directory indexes and `.html` aliases. Response headers include `Vary` and `nosniff`.
- **Bounded VM:** instruction limits, checked stack operations, bounded jumps and defined integer wrapping. POST bodies reach native handlers. Dynamic response headers reject control characters.
- **Tooling:** deterministic builds, bounded compression concurrency, atomic output replacement, inspect JSON, version-pinned binary downloads and SHA-256 verification against release manifests.

## Commands

```sh
node tools/wz.js build ./site --output site.web --quality 5
node tools/wz.js inspect site.web --json
node tools/wz.js serve site.web --port 8080
node tools/wz.js optimize ./site --widths 320,640,1280 --quality 82
node tools/wz.js version
node tools/wz.js update
```

Brotli quality defaults to 5; choose 11 when minimum transfer size matters more than build time. `--quality` on `optimize` is JPEG quality (1–100). Image optimization requires `wzimg`, built separately from `tools/wzimg.c`, and skips already generated `@<width>w.jpg` inputs. `.webp` companions are discovered during bundling; WebZero does not create WebP images itself.

The compiler omits dotfiles/dotdirectories, `node_modules`, symlinks, `.web` bundles and `.wz` scratch files. Build from a dedicated public-content directory. Other ordinary files in that directory are intentionally included.

## Runtime limits and scope

- 256 simultaneous connections maximum, configurable downward in the bundle.
- Each connection has an 8 KiB input buffer and a 4.5 KiB response buffer. A complete request, including headers and body, must fit in 8,191 bytes.
- A fixed native arena of approximately 3.3 MiB, plus process stack, code and memory-mapped bundle pages. This is **not** a total RSS guarantee.
- No application `malloc`/`free`, worker threads or mutexes in the native request path. OS and C runtime internals are outside that guarantee.
- Request paths: 511 decoded bytes; route segments: 31 UTF-8 bytes; route nodes and assets: 1,024 each. Files larger than the format's 32-bit offsets are unsupported.
- Inactive connections expire after the bundle timeout, default 30 seconds. This is an inactivity timeout, not a complete slow-client or rate-limiting defense.
- HTTP/1.0 and HTTP/1.1 only. No TLS, HTTP/2, chunked request bodies, multipart range responses, authentication or hot reload. Use a reverse proxy for TLS and public-edge protections.
- Static assets accept GET/HEAD. Native bytecode handlers also accept POST; the current builder does not compile handler source. The Node development server does not execute bytecode.
- Single ranges operate on the selected representation. HEAD ignores Range. If-Range falls back to a full response because validators are weak. Mutable URLs revalidate by default; assets are not incorrectly marked immutable for a year.
- Asset data is sent from the mapping using socket writes. This is **not** a `sendfile`/`TransmitFile` zero-copy implementation.

The code targets old Linux/Windows APIs (including Windows XP API declarations). Validation here runs on modern Linux and Windows; compatibility with actual XP or vintage hardware has not been measured. Node is a build/development dependency only and does not run on XP.

## Build and test

```sh
make                         # native Linux, C99, warnings as errors
make static                  # requires musl-gcc
make windows                 # requires i686-w64-mingw32-gcc
make windows CC_WIN=x86_64-w64-mingw32-gcc
make test                    # native units and socket regressions; Python 3
make debug
python3 tests/native.py ./webzero-debug
npm test                     # tooling, format and Node HTTP suites
npm pack --dry-run --ignore-scripts
```

On Windows with MinGW, put its `bin` directory on PATH and use `make windows CC_WIN=gcc`. The native suite also runs as `python tests/native.py ./webzero.exe`.

CI runs native Linux and Windows tests, Linux sanitizers, and Node compatibility tests. Tagged releases build four binary targets and include `SHA256SUMS`. Downloads are pinned to the package version. Until a matching v2 release exists, build the native binary from source or use `--js`; this source overhaul does not publish an npm package or release automatically.

## Layout

```text
core/                 bundle validation, routing, HTTP parser, connection state, VM
platform/             Linux epoll and Windows select backends
tools/lib/            bundle compiler/reader, Node server, installer, image optimizer
tools/wz.js           CLI argument handling and backend selection
tests/                native unit/socket tests and Node test suites
bench/compare.js      repeatable original-versus-current build and Node HTTP benchmark
docs/                 measured results and migration guide
```

See [BUNDLE_SPEC.md](BUNDLE_SPEC.md) for the format and [SECURITY.md](SECURITY.md) for scope and vulnerability reporting. Apache-2.0; see [LICENSE](LICENSE).
