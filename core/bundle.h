/*
 * bundle.h — .web bundle format parser and mmap loader
 * The entire site lives in a single memory-mapped binary file.
 */
#ifndef WZ_BUNDLE_H
#define WZ_BUNDLE_H

#include <stdint.h>
#include <stddef.h>

#define WEB_MAGIC   0x57454230u   /* "WEB0" */
#define WEB_VERSION 2u

/* On-disk header (28 bytes, little-endian) */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t route_table_offset;
    uint32_t assets_offset;
    uint32_t handlers_offset;
    uint32_t config_offset;
    uint32_t total_size;
} __attribute__((packed)) BundleHeader;

/*
 * Asset entry — on-disk layout is exactly 56 bytes (shared with tools/lib/bundle.js).
 *
 * Byte map:
 *   [0 ]  offset          uint32
 *   [4 ]  compressed_len  uint32
 *   [8 ]  original_len    uint32
 *   [12]  mime[32]        char[32]
 *   [44]  encoding        uint8   (0=raw, 1=brotli)
 *   [45]  _pad1[3]        uint8[3]
 *   [48]  webp_idx        int32   (-1 = no WebP variant)
 *   [52]  raw_offset      uint32 (v2 only)
 * Total: 56 bytes
 */
typedef struct {
    uint32_t offset;
    uint32_t compressed_len;
    uint32_t original_len;
    char     mime[32];
    uint8_t  encoding;      /* 0=raw, 1=brotli */
    uint8_t  _pad1[3];
    int32_t  webp_idx;      /* index of WebP variant asset, or -1 */
    uint32_t raw_offset; /* v2: identity payload offset relative to asset data */
} __attribute__((packed)) AssetEntry;

/* Handler entry (stored as flat array in HANDLERS section) */
typedef struct {
    uint32_t offset;        /* byte offset to bytecode */
    uint32_t len;           /* bytecode length in bytes */
} HandlerEntry;

/* Config section (fixed-size, parsed once at startup) */
typedef struct {
    char     hostname[64];
    uint16_t port;
    uint16_t max_connections;
    uint32_t keepalive_timeout_ms;
    uint32_t asset_count;
    uint32_t handler_count;
    uint32_t route_node_count;
} BundleConfig;

/* Loaded bundle (post-mmap) */
typedef struct {
    const uint8_t  *base;           /* mmap base pointer */
    size_t          file_size;
    uint32_t version, data_offset, fingerprint;
    BundleConfig    config;
    const AssetEntry   *assets;     /* pointer into mmap */
    const HandlerEntry *handlers;   /* pointer into mmap */
    /* trie is loaded into arena.trie at startup */
} Bundle;

/* Load a .web file. Maps it into memory. Returns 0 on success. */
int bundle_load(const char *path, Bundle *out);

/* Unmap the bundle (called only on server shutdown). */
void bundle_unload(Bundle *b);

/* Validate header, sections, payload ranges and metadata. Router validates the tree. */
int bundle_validate(const Bundle *b);

#endif /* WZ_BUNDLE_H */
