# WebZero bundle format

All integers are little-endian. The builder emits **version 2**. Readers also accept structurally valid version-1 bundles. Binary layout is independent of host C structure padding. Runtime targets are little-endian machines.

## File layout

```text
28-byte header
64-byte route nodes
56-byte asset entries
asset payloads (Brotli and/or identity)
8-byte handler entries and bytecode, when present
96-byte configuration
```

Header fields, each `uint32`, in order: magic `0x57454230`, version, route-table offset, asset-table offset, handler-table offset, configuration offset, exact file size. Offsets in the header are absolute. Sections must be ordered and wholly inside the file. The configuration is the final 96 bytes.

## Version-2 route node (64 bytes)

| Offset | Type | Meaning |
|---|---|---|
| 0 | char[32] | UTF-8 path segment, NUL-terminated; root is empty |
| 32 | uint32 | First child node, or `0xffffffff` |
| 36 | uint32 | Next sibling node, or `0xffffffff` |
| 40 | int32 | Asset index, or -1 |
| 44 | int32 | Handler index, or -1 |
| 48 | byte[16] | Reserved, writer emits zeros |

Node 0 is the root. Every other node has exactly one parent. Children and sibling links point forward in the array; sibling chains are strictly increasing. All nodes are reachable. Siblings have unique segment names. A node cannot name both an asset and a handler. The current runtime limit is 1,024 nodes; sibling count has no additional eight-entry limit.

The compiler serializes nodes breadth-first, in deterministic filename order. UTF-8 segments must fit in 31 bytes. The compiler rejects route collisions after removing `.html`, rather than overwriting one asset. Lookup supports extensionless HTML, explicit `.html`, directory index routes and legacy segment wildcards. The filesystem compiler does not accept wildcard names.

## Asset entry (56 bytes, both versions)

| Offset | Type | Meaning |
|---|---|---|
| 0 | uint32 | Selected payload offset, relative to **start of asset data** |
| 4 | uint32 | Selected payload byte length |
| 8 | uint32 | Identity/original byte length |
| 12 | char[32] | NUL-terminated MIME type, no control characters |
| 44 | uint8 | Encoding: 0 = identity, 1 = Brotli |
| 45 | byte[3] | Reserved |
| 48 | int32 | WebP companion asset index, or -1 |
| 52 | uint32 | v2 identity payload offset when encoding=1; reserved in v1 |

`asset_data_start = assets_offset + asset_count * 56`.

For encoding 0, the two lengths must match and offset 0 identifies the identity payload. For encoding 1 in v2, offset 0 identifies Brotli bytes and offset 52 identifies identity bytes; **both offsets are relative to asset_data_start**. All payload ranges must end before the handler section. The file is limited to the 32-bit addressable size in its header.

The original C implementation incorrectly treated asset offsets as absolute, while the original JavaScript compiler wrote them relative to the asset data area. The v2 implementation uses the compiler's relative convention for both versions.

The compiler compresses eligible text/wasm assets only when at least 33 bytes are saved. Quality defaults to 5 and can be selected from 0 to 11. Already compressed formats are stored raw. Identity duplication is deliberate: the native server never has to decode Brotli during a request.

## Handler entries

Each entry contains an absolute `uint32` bytecode offset and a `uint32` bytecode length. Bytecode follows the handler table and ends before configuration. The stock compiler currently emits zero handlers; custom producers may use the native VM instruction set in `core/vm.h` and `core/vm.c`.

Instructions encode inline operands in little-endian order. PUSH_STR has a `uint16` byte count followed by bytes; PUSH_INT has an `int32`; jumps have a signed `int16` displacement relative to the PC after their operand. Execution is bounded to 10,000 instructions, stack depth 32, and strings of at most 255 bytes. Overflowing integer addition wraps modulo 2^32. Invalid bounds, stack operations or instruction limits fail the request with 500.

## Configuration (96 bytes)

| Offset | Type | Meaning |
|---|---|---|
| 0 | char[64] | NUL-terminated hostname metadata |
| 64 | uint16 | Default port |
| 66 | uint16 | Connection limit (1–256; 0 uses 256) |
| 68 | uint32 | Inactivity timeout in ms (0 uses 30,000) |
| 72 | uint32 | Asset count |
| 76 | uint32 | Handler count |
| 80 | uint32 | Route-node count |
| 84 | byte[12] | Reserved |

Hostname is metadata, not a bind-address setting. Servers listen on all IPv4 interfaces by default.

## Version-1 compatibility

V1 route nodes store segment[32], child count (`uint16` at 32), up to eight child indices (`uint16[8]` at 34), asset index (`int32` at 50), handler index (`int32` at 54), and six padding bytes. Readers convert this tree to their internal representation after validating it. Invalid or orphaned legacy trees are rejected.

V1 compressed assets contain no identity fallback. The C server returns 406 when a client cannot accept their representation. The Node development server may decode them at startup, subject to its decoded-memory cap. Rebuilding the source directory as v2 restores full encoding negotiation.
