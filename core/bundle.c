/*
 * bundle.c — .web bundle loader
 * Maps the entire site into virtual memory at startup.
 * No file I/O during request serving.
 */
#include "bundle.h"
#include "pool.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Portable file mapping                                               */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

static HANDLE g_file_handle   = INVALID_HANDLE_VALUE;
static HANDLE g_mapping_handle = NULL;

static const uint8_t *map_file(const char *path, size_t *out_size) {
    g_file_handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                NULL);
    if (g_file_handle == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(g_file_handle, &sz) || sz.QuadPart <= 0 || (uint64_t)sz.QuadPart > UINT32_MAX) {
        CloseHandle(g_file_handle);
        return NULL;
    }
    *out_size = (size_t)sz.QuadPart;

    g_mapping_handle = CreateFileMappingA(g_file_handle, NULL,
                                          PAGE_READONLY, 0, 0, NULL);
    if (!g_mapping_handle) {
        CloseHandle(g_file_handle);
        return NULL;
    }

    const uint8_t *base = (const uint8_t *)MapViewOfFile(g_mapping_handle,
                                                          FILE_MAP_READ, 0, 0, 0);
    if (!base) {
        CloseHandle(g_mapping_handle);
        CloseHandle(g_file_handle);
        return NULL;
    }
    return base;
}

static void unmap_file(const uint8_t *base, size_t size) {
    (void)size;
    if (base)              UnmapViewOfFile((LPCVOID)base);
    if (g_mapping_handle)  CloseHandle(g_mapping_handle);
    if (g_file_handle != INVALID_HANDLE_VALUE) CloseHandle(g_file_handle);
}

#else /* POSIX */

static int g_fd = -1;

static const uint8_t *map_file(const char *path, size_t *out_size) {
    g_fd = open(path, O_RDONLY);
    if (g_fd < 0) return NULL;

    struct stat st;
    if (fstat(g_fd, &st) < 0 || st.st_size <= 0 || (uint64_t)st.st_size > UINT32_MAX) { close(g_fd); return NULL; }
    *out_size = (size_t)st.st_size;

    const uint8_t *base = (const uint8_t *)mmap(NULL, *out_size,
                                                  PROT_READ,
                                                  MAP_PRIVATE,
                                                  g_fd, 0);
    if (base == MAP_FAILED) { close(g_fd); return NULL; }
    return base;
}

static void unmap_file(const uint8_t *base, size_t size) {
    if (base && base != MAP_FAILED) munmap((void *)base, size);
    if (g_fd >= 0) close(g_fd);
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int bundle_load(const char *path, Bundle *out) {
    memset(out, 0, sizeof(*out));

    out->base = map_file(path, &out->file_size);
    if (!out->base) {
        fprintf(stderr, "webzero: cannot map bundle '%s'\n", path);
        return -1;
    }

    if (bundle_validate(out) != 0) {
        unmap_file(out->base, out->file_size);
        out->base = NULL;
        return -1;
    }

    const BundleHeader *hdr = (const BundleHeader *)out->base;

    /* Parse config section */
    if (hdr->config_offset + sizeof(BundleConfig) > out->file_size) {
        fprintf(stderr, "webzero: bundle config section out of bounds\n");
        unmap_file(out->base, out->file_size);
        out->base = NULL;
        return -1;
    }
    memcpy(&out->config,
           out->base + hdr->config_offset,
           sizeof(BundleConfig));

    /* Set asset/handler pointers */
    out->assets   = (const AssetEntry *)(out->base + hdr->assets_offset);
    out->handlers = (const HandlerEntry *)(out->base + hdr->handlers_offset);

    out->version = hdr->version;
    out->data_offset = hdr->assets_offset + out->config.asset_count * 56u;
    /* Hash once at startup; conditional requests do no payload hashing. */
    out->fingerprint = 2166136261u;
    { size_t i; for (i = 0; i < out->file_size; i++) out->fingerprint = (out->fingerprint ^ out->base[i]) * 16777619u; }
    return 0;
}

void bundle_unload(Bundle *b) {
    if (b && b->base) {
        unmap_file(b->base, b->file_size);
        b->base = NULL;
    }
}

static int span(size_t size, uint32_t off, uint32_t count, size_t width) {
    return off <= size && count <= (size - off) / width;
}
static int valid_string(const char *s, size_t cap) {
    size_t i;
    for (i = 0; i < cap; i++) {
        if (!s[i]) return i > 0;
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] == 127) return 0;
    }
    return 0;
}
int bundle_validate(const Bundle *b) {
    BundleHeader h;
    BundleConfig config;
    uint32_t i, data;
    if (b->file_size < 28 || b->file_size > UINT32_MAX) goto bad;
    memcpy(&h, b->base, sizeof(h));
    if (h.magic != WEB_MAGIC || (h.version != 1 && h.version != WEB_VERSION) || h.total_size != b->file_size) goto bad;
    if (h.route_table_offset < 28 || h.route_table_offset > h.assets_offset || h.assets_offset > h.handlers_offset ||
        h.handlers_offset > h.config_offset || !span(b->file_size, h.config_offset, 1, 96) || h.config_offset != b->file_size - 96) goto bad;
    memcpy(&config, b->base + h.config_offset, sizeof(config));
    if (!memchr(config.hostname, 0, sizeof(config.hostname)) || !config.port || config.max_connections > MAX_CONNS ||
        !config.route_node_count || config.route_node_count > MAX_TRIE_NODES || config.asset_count > MAX_TRIE_NODES ||
        config.handler_count > MAX_TRIE_NODES) goto bad;
    if (!span(h.assets_offset, h.route_table_offset, config.route_node_count, 64) ||
        !span(h.handlers_offset, h.assets_offset, config.asset_count, 56) ||
        !span(h.config_offset, h.handlers_offset, config.handler_count, 8)) goto bad;
    data = h.assets_offset + config.asset_count * 56u;
    for (i = 0; i < config.asset_count; i++) {
        AssetEntry a;
        memcpy(&a, b->base + h.assets_offset + i * 56u, sizeof(a));
        if (!valid_string(a.mime, sizeof(a.mime)) || a.encoding > 1 || a.webp_idx < -1 ||
            (a.webp_idx >= 0 && (uint32_t)a.webp_idx >= config.asset_count) ||
            !span(h.handlers_offset-data, a.offset, a.compressed_len, 1)) goto bad;
        if (!a.encoding && a.compressed_len != a.original_len) goto bad;
        if (h.version == 2 && a.encoding && !span(h.handlers_offset-data, a.raw_offset, a.original_len, 1)) goto bad;
    }
    for (i = 0; i < config.handler_count; i++) {
        HandlerEntry handler;
        memcpy(&handler, b->base + h.handlers_offset + i * 8u, sizeof(handler));
        if (handler.offset < h.handlers_offset + config.handler_count * 8u || !span(h.config_offset, handler.offset, handler.len, 1)) goto bad;
    }
    return 0;
bad:
    fprintf(stderr, "webzero: invalid bundle (format, bounds or metadata)\n");
    return -1;
}
