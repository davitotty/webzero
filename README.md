# WebZero

**Build your site once. Serve it with a small, fixed-memory runtime.**

WebZero turns a directory of HTML, CSS, JavaScript, and assets into a single `.web` bundle. Its native C server maps that bundle into memory and serves it without a Node process, runtime compression, or application heap allocations in the request path.

Built for static sites, local documentation, appliance interfaces, and machines with a limited memory budget.

**C99** · **Linux + Windows** · **Zero npm dependencies** · **Apache-2.0**

[Quick start](#quick-start) · [How it works](#how-it-works) · [Benchmarks](#measured-improvements) · [CLI](#cli-reference) · [Documentation](#documentation)

## Why WebZero?

| Design choice | What you get |
| --- | --- |
| One site bundle | Copy one `.web` file alongside the server binary to deploy a site. |
| Compression at build time | Serve prepared Brotli or original bytes without a compression library in the native runtime. |
| Fixed connection storage | Bounded buffers and connection counts, with no application `malloc`/`free` in the native request path. |
| Nonblocking networking | Slow readers keep their own response state while other connections continue making progress. |
| Validation before serving | Invalid section offsets, asset references, and route graphs fail at startup. |
| Deterministic tooling | Identical inputs and build options produce identical bundles in the same toolchain. |

The native runtime uses the system C library on Linux and Windows system libraries. Node is needed for building bundles and running the optional development server.

## Quick start

**Requirements:** Node.js 18 or newer. A C compiler is optional for the first run.

> The v2 overhaul currently lives on `codex/webzero-overhaul`. These instructions use that source branch; they do not depend on a published v2 npm package or prebuilt release.

```sh
git clone --branch codex/webzero-overhaul https://github.com/davitotty/webzero.git
cd webzero

node tools/wz.js build examples/landing-page
node tools/wz.js serve examples/landing-page.web --js --port 8080
```

Open **[localhost:8080](http://localhost:8080)**. No `npm install` is required.

To build your own site, replace `examples/landing-page` with a directory containing your public files:

```sh
node tools/wz.js build ./my-site --output my-site.web
node tools/wz.js serve my-site.web --js --port 8080
```

### Run the native server

On Linux, with GCC and Make installed:

```sh
make
./webzero my-site.web 8080
```

After building the bundle and server, deployment needs the native executable, the `.web` file, and the platform's system libraries. Node is no longer required.

| Build target | Command | Output |
| --- | --- | --- |
| Linux | `make` | `webzero` |
| Static Linux, with musl-gcc | `make static` | `webzero-static` |
| Windows x86, cross-compiled with MinGW | `make windows` | `webzero.exe` |
| Windows x64, cross-compiled with MinGW | `make windows CC_WIN=x86_64-w64-mingw32-gcc` | `webzero.exe` |
| Windows, in a MinGW shell with GCC and Make on PATH | `make windows CC_WIN=gcc` | `webzero.exe` |

On Windows, launch the result with `./webzero.exe my-site.web 8080` in a MinGW shell or `.\webzero.exe my-site.web 8080` in PowerShell.

### Choose a serving mode

```sh
node tools/wz.js serve my-site.web --native  # Require a compatible native binary
node tools/wz.js serve my-site.web --js      # Always use the Node development server
node tools/wz.js serve my-site.web           # Prefer native; fall back to Node
```

Native discovery checks `WEBZERO_BINARY`, the checkout, then `~/.webzero`. The CLI reports the backend it starts. `--native` returns an error if no compatible binary is available.

## How it works

```text
BUILD TIME                               SERVING TIME

Public site directory                    Native server starts
        |                                        |
        v                                        v
Discover files and build routes          Validate and memory-map site.web
        |                                        |
        v                                        v
Prepare Brotli + original bytes          Accept and parse HTTP requests
        |                                        |
        v                                        v
Write one deterministic .web bundle       Find route and select representation
                                                 |
                                                 v
                                         Send bytes; resume partial writes
```

The bundle contains routes, asset metadata, asset bytes, configuration, and optional native handler bytecode. Linux uses epoll; Windows uses nonblocking Winsock/select. Both backends share the HTTP parser and connection state machine.

Compressed text includes an uncompressed fallback. Already compressed media stays raw. This uses more disk space in exchange for avoiding request-time decompression in the native server. Asset bytes are sent from the memory mapping using socket writes.

See the [bundle format specification](BUNDLE_SPEC.md) for the on-disk layout.

## Measured improvements

Compared with original commit `1d0d262` on the same development machine:

| Measurement | Original | WebZero 2 | Change |
| --- | ---: | ---: | ---: |
| Build the existing landing-page example | 3.307 s | 0.096 s | **34.6× faster** |
| Node development server throughput | 4,590 req/s | 5,045 req/s | **+9.9%** |
| Node development server p95 latency | 5.63 ms | 5.29 ms | **6.0% lower** |
| Overhaul regression scenarios passed | 7 / 45 | 45 / 45 | **38 more passing** |

Build times are medians of three trials. HTTP results are medians of five trials with 16 keep-alive clients and 5,000 requests per trial; every response body is checked. Most of the build gain comes from avoiding unnecessary PNG recompression. The regression suite includes new features as well as bug fixes.

**Tradeoffs:** the example bundle grew **0.15%**. Native Linux BSS grew from **2.23 MB to 3.41 MB**, primarily for per-connection response storage. BSS is not total resident memory.

These are local measurements on modern hardware, not native-server throughput claims or performance guarantees for old machines. Read the [full methodology, raw results, and reproduction commands](docs/BENCHMARKS.md).

## HTTP and routing

- **Static content:** GET and HEAD, extensionless HTML routes, explicit `.html` aliases, and directory indexes.
- **Negotiation:** Brotli and identity responses, encoding quality exclusions, and optional WebP companions.
- **Caching:** weak ETags, `If-None-Match` / 304, revalidation by default, and `Vary` for encoding and image negotiation.
- **Downloads:** single byte ranges over the selected representation. HEAD ignores Range; If-Range falls back to a full response.
- **Connections:** keep-alive, fragmented requests, pipelining, resumable writes, inactivity expiry, and overload rejection.
- **Native handlers:** bounded bytecode execution and POST bodies. The stock builder does not compile handler source; the Node development server does not execute bytecode.

For example, `docs/index.html` is available at `/docs`, `/docs/`, `/docs/index`, and `/docs/index.html`.

## CLI reference

All commands below run from the repository checkout.

| Task | Command |
| --- | --- |
| Build a site | `node tools/wz.js build ./site` |
| Choose the output file | `node tools/wz.js build ./site --output site.web` |
| Choose Brotli quality, 0–11 | `node tools/wz.js build ./site --quality 11` |
| Inspect assets and routes | `node tools/wz.js inspect site.web` |
| Get compact JSON | `node tools/wz.js inspect site.web --json` |
| Choose a serving port | `node tools/wz.js serve site.web --port 8080` |
| Print versions | `node tools/wz.js version` |
| Download a matching native release | `node tools/wz.js update` |
| Show help | `node tools/wz.js --help` |

Brotli quality defaults to **5**. Use **11** when smaller transfer sizes matter more than build time. `build --json` returns machine-readable build statistics.

Binary downloads are pinned to the package version and checked against the release's `SHA256SUMS`. If a matching release is unavailable, build from source or use `--js`.

### Prepare images

Build the optional image helper on Linux, then generate responsive JPEG variants:

```sh
make wzimg
node tools/wz.js optimize ./site --widths 320,640,1280 --quality 82
node tools/wz.js build ./site
```

Here, `--quality` means **JPEG quality, 1–100**. Generated files use names such as `hero@320w.jpg`; reference them from your HTML's `srcset`. Re-running optimization skips current outputs and does not resize generated variants again.

To enable WebP negotiation, place a same-basename companion such as `hero.webp` beside `hero.png` before building. WebZero discovers the companion; creating the WebP image is a separate build step.

## Limits and deployment

| Resource | Native limit |
| --- | --- |
| Concurrent connections | Up to 256; configurable downward in the bundle |
| Complete request, including headers and body | 8,191 bytes |
| Decoded request path | 511 bytes |
| Route segment | 31 UTF-8 bytes |
| Route nodes / assets | 1,024 each; no separate eight-child limit |
| Connection inactivity timeout | 30 seconds by default; configurable in the bundle |
| VM execution | 10,000 instructions, 32 stack values, 255-byte strings |
| Bundle offsets | 32-bit; bundles must fit within the format limit |

Build from a **dedicated public-content directory**. The compiler excludes dotfiles, dotdirectories, `node_modules`, symlinks, `.web` bundles, and `.wz` scratch files. Other ordinary files are included. Duplicate routes and unsupported filenames produce errors.

The native arena is approximately **3.3 MiB**, plus stack, code, system libraries, and mapped bundle pages. The native request path uses no application heap allocations, worker threads, or mutexes; that guarantee does not extend to OS or library internals.

WebZero serves HTTP/1.0 and HTTP/1.1 over IPv4 and listens on all IPv4 interfaces. Use a reverse proxy for TLS and public-edge controls. TLS, authentication, HTTP/2, chunked request bodies, multipart ranges, and hot reload are outside the current implementation. Restart the server to load a replacement bundle. Inactivity expiry is not a complete slow-client defense.

The code targets older Linux and Windows APIs, including Windows XP API declarations. Tests run on modern Linux and Windows; actual XP and vintage-hardware compatibility remain unverified. Node 18+ is a build/development requirement, not a runtime requirement for the native executable.

## Development and verification

```sh
npm test                                  # Bundle, CLI, and Node HTTP tests
make test                                 # Native unit and socket tests; needs Python 3
make debug                                # AddressSanitizer + UndefinedBehaviorSanitizer
python3 tests/native.py ./webzero-debug
python3 tests/native_vm.py ./webzero-debug
npm pack --dry-run --ignore-scripts        # Check package contents
```

CI covers native Linux and Windows, Linux sanitizers, and Node 18, 22, and 24. The overhaul was verified with 45 native socket/format scenarios, five native VM scenarios, 70 C unit assertions plus parser mutations, and eight Node test suites. See [GitHub Actions](https://github.com/davitotty/webzero/actions) for current run results.

On Windows, run native integration tests with `python tests/native.py ./webzero.exe` and `python tests/native_vm.py ./webzero.exe`.

<details>
<summary>Repository layout</summary>

```text
core/              Bundle validation, routing, HTTP, connection state, and VM
platform/          Linux epoll and Windows select backends
tools/wz.js        CLI entry point
tools/lib/         Compiler, bundle reader, Node server, installer, image tooling
tools/wzimg.c      Optional build-time image helper
tests/             Native and Node regression suites
bench/             Reproducible comparison tools
docs/              Migration guide, benchmark methodology, and raw results
examples/          Sites you can build and serve immediately
```

</details>

## Documentation

- [Migrating from v1](docs/MIGRATION.md): rebuild requirements, legacy bundles, and behavior changes.
- [Bundle specification](BUNDLE_SPEC.md): binary layout, validation rules, and handler encoding.
- [Benchmark report](docs/BENCHMARKS.md): measurements, limitations, and reproduction steps.
- [Security policy](SECURITY.md): intended scope and private vulnerability reporting.

## License

[Apache License 2.0](LICENSE).
