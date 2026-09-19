# Measured before/after comparison

Measured on 2026-09-19. Baseline: `1d0d2621c219f6d29f8860a1110e73b5ca278666`. New version: the v2 overhaul in this change. Raw data is committed in [benchmark-results.json](benchmark-results.json), [native-old.json](native-old.json) and [native-new.json](native-new.json).

## Results

| Metric | Original | Overhaul | Change |
|---|---:|---:|---|
| Build existing landing-page example, median | 3.307 s | 0.096 s | **34.6× faster** |
| Build with Brotli quality 11, median | 3.307 s | 0.092 s | **36.0× faster** |
| Node server, verified identity responses | 4,590 req/s | 5,045 req/s | **9.9% higher** |
| Node p95 latency, median across trials | 5.63 ms | 5.29 ms | **6.0% lower** |
| Incorrect/failed benchmark responses | 0 / 25,000 | 0 / 25,000 | Both correct in this Node workload |
| Native overhaul regression scenarios | 7 / 45 | 45 / 45 | 38 more scenarios pass |
| Example bundle on disk | 3,744,835 bytes | 3,750,407 bytes | +5,572 bytes (+0.15%) |
| Linux executable file, unstripped | 35,128 bytes | 39,760 bytes | +13.2% |
| Linux BSS reservation (`size`) | 2,230,440 bytes | 3,408,056 bytes | +52.8%, primarily response buffers |

The native test comparison includes newly added HTTP features and deliberately malformed input, not only previously promised behavior. It is a regression/feature suite, **not** a percentage score for overall security or standards compliance. The original native server's offset mismatch returns incorrect asset bytes, so no native throughput speedup is claimed from comparing those broken responses with valid new responses.

## Method

- Windows x64, Intel(R) Core(TM) i7-9700 CPU @ 3.00GHz, Node v25.5.0. Same Node executable and same machine for both versions.
- Build corpus: the original repository's unchanged `examples/landing-page/index.html` and its 3.74 MB PNG. Three alternating build trials, including process startup. The new default is Brotli quality 5; a separate quality-11 series controls for the quality change. Skipping expensive recompression of the PNG produces most of this workload's build gain. Results are not a universal compression benchmark.
- HTTP corpus: one deterministic 87,028-byte HTML page. Five alternating trials of 5,000 requests each; 500 warmup requests per trial, 16 keep-alive clients, explicit identity encoding, loopback networking. Every response is compared byte-for-byte. Reported throughput and p95 are medians of the five per-trial measurements. The old CLI's serving path is Node; the new CLI is explicitly run with `--js` for the same-runtime comparison.
- Baseline Node throughput range: 4,004–4,746 req/s. New range: 3,819–5,972 req/s. Short loopback measurements vary; this is a local measurement, not a production capacity guarantee.
- Native testing: Ubuntu under WSL2, Linux 6.6.87.2, GCC. The baseline's unmodified `make` fails because feature macros are missing. For baseline execution only, the same original sources were compiled with `-D_GNU_SOURCE -std=c99 -O3 -Wall -Wextra -Wpedantic -I.`. New `make` uses C99/O2 and warnings as errors. Executable sizes therefore reflect those documented build configurations, not a matched optimization-level study.
- Native fixtures use the original v1 layout for the baseline and the new v2 layout for the overhaul, with identical logical assets/requests. The v1 fixture intentionally retains all 20 sibling nodes so its original eight-child truncation is exercised. No baseline source code was repaired to improve its score.
- The new native suite also passes on modern Windows x64/MinGW and under AddressSanitizer + UndefinedBehaviorSanitizer on Linux. Seventy C unit assertions, bounded parser mutations, five native VM integration scenarios and eight Node test suites pass separately.

## Tradeoffs and limits

V2 stores identity bytes alongside Brotli for text, trading disk space for correct negotiation without a runtime decompression dependency. Response buffers increase fixed BSS by approximately 1.12 MiB and keep the arena below 4 MiB. Total RSS also includes mapped pages, executable code, stack and system libraries. The file fingerprint scans the bundle at startup; startup cost grows with site size.

These tests do not establish Windows XP compatibility, performance on a Pentium III/Raspberry Pi 1, internet-scale capacity, TLS behavior or zero-copy I/O. Those claims are intentionally absent from the package metadata and README.

## Reproduce

```sh
# Put an unchanged baseline next to the working checkout.
git clone https://github.com/davitotty/webzero ../webzero-baseline
git -C ../webzero-baseline checkout 1d0d2621c219f6d29f8860a1110e73b5ca278666
node bench/compare.js ../webzero-baseline

# Native baseline: its standard make fails; compatibility macro only.
(cd ../webzero-baseline && gcc -D_GNU_SOURCE -std=c99 -O3 -Wall -Wextra -Wpedantic -I. main.c core/pool.c core/bundle.c core/router.c core/vm.c platform/linux.c -o webzero)
python3 tests/native.py ../webzero-baseline/webzero --baseline
# A nonzero exit for the baseline is expected; the JSON records each failure.
make test debug
python3 tests/native.py ./webzero-debug
python3 tests/native_vm.py ./webzero-debug
npm test
```

New benchmark runs write to the ignored `bench/results/` directory. Committed measurements remain an immutable record of this local run; refresh them deliberately when comparing future changes.
