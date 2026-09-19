#include "router.h"
#include "pool.h"
#include <string.h>
#include <stdio.h>
#define NONE UINT16_MAX
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
int router_build(const Bundle *b) {
    const BundleHeader *h = (const BundleHeader *)b->base;
    uint32_t count = b->config.route_node_count, i, edges = 0;
    uint8_t parents[MAX_TRIE_NODES] = {0};
    arena.trie_count = 0;
    for (i = 0; i < count; i++) {
        const uint8_t *d = b->base + h->route_table_offset + i * 64u;
        TrieNode *n = &arena.trie[i];
        memcpy(n->segment, d, 32);
        if (!memchr(n->segment, 0, 32) || (i && !n->segment[0]) || strchr(n->segment, '/')) goto bad;
        n->first_child = n->next_sibling = NONE;
        if (b->version == 2) {
            uint32_t first = u32(d+32), next = u32(d+36);
            if ((first != UINT32_MAX && (first <= i || first >= count)) ||
                (next != UINT32_MAX && (next <= i || next >= count))) goto bad;
            n->first_child = first == UINT32_MAX ? NONE : (uint16_t)first;
            n->next_sibling = next == UINT32_MAX ? NONE : (uint16_t)next;
            n->asset_idx = (int32_t)u32(d+40); n->handler_idx = (int32_t)u32(d+44);
        } else {
            if (u16(d+32) > 8) goto bad;
            n->asset_idx = (int32_t)u32(d+50); n->handler_idx = (int32_t)u32(d+54);
        }
        if (n->asset_idx < -1 || n->handler_idx < -1 ||
            (n->asset_idx >= 0 && (uint32_t)n->asset_idx >= b->config.asset_count) ||
            (n->handler_idx >= 0 && (uint32_t)n->handler_idx >= b->config.handler_count) ||
            (n->asset_idx >= 0 && n->handler_idx >= 0)) goto bad;
    }
    if (b->version == 1) for (i = 0; i < count; i++) {
        const uint8_t *d = b->base + h->route_table_offset + i * 64u;
        uint16_t j, prev = NONE;
        for (j = 0; j < u16(d+32); j++) {
            uint16_t child = u16(d+34+j*2);
            if (child <= i || child >= count || parents[child]++) goto bad;
            if (prev == NONE) arena.trie[i].first_child = child;
            else arena.trie[prev].next_sibling = child;
            prev = child;
        }
    }
    memset(parents, 0, sizeof(parents));
    if (arena.trie[0].segment[0] || arena.trie[0].next_sibling != NONE) goto bad;
    for (i = 0; i < count; i++) {
        uint16_t child;
        for (child = arena.trie[i].first_child; child != NONE; child = arena.trie[child].next_sibling) {
            uint16_t other;
            if (child <= i || ++edges >= count || parents[child]++) goto bad;
            for (other = arena.trie[i].first_child; other != child; other = arena.trie[other].next_sibling)
                if (!strcmp(arena.trie[other].segment, arena.trie[child].segment)) goto bad;
        }
    }
    if (edges != count-1) goto bad;
    arena.trie_count = count; return 0;
bad:
    fprintf(stderr, "webzero: invalid route trie\n"); return -1;
}
static int child_of(uint16_t node, const char *s, size_t len) {
    uint16_t child; int wildcard = -1;
    for (child = arena.trie[node].first_child; child != NONE; child = arena.trie[child].next_sibling) {
        const char *seg = arena.trie[child].segment;
        if (strlen(seg) == len && !memcmp(seg, s, len)) return child;
        if (!strcmp(seg, "*")) wildcard = child;
    }
    return wildcard;
}
RouteMatch router_lookup(const char *path) {
    RouteMatch result = {-1,-1,0};
    uint16_t current = 0;
    const char *p = path;
    const TrieNode *n;
    if (!arena.trie_count) return result;
    while (*p) {
        const char *s; size_t len; int child;
        while (*p == '/') p++;
        if (!*p) break;
        s = p; while (*p && *p != '/') p++;
        len = (size_t)(p-s);
        if (!*p && len > 5 && !memcmp(s+len-5, ".html", 5)) len -= 5;
        if (len > 31) return result;
        child = child_of(current, s, len);
        if (child < 0) return result;
        current = (uint16_t)child;
    }
    n = &arena.trie[current];
    if (n->asset_idx < 0 && n->handler_idx < 0) {
        int idx = child_of(current, "index", 5);
        if (idx < 0) return result;
        n = &arena.trie[idx];
    }
    result.asset_idx = n->asset_idx; result.handler_idx = n->handler_idx;
    result.found = n->asset_idx >= 0 || n->handler_idx >= 0;
    return result;
}
void router_dump(void) {
#ifdef WZ_DEBUG
    uint32_t i;
    for (i=0; i<arena.trie_count; i++) fprintf(stderr, "[%u] %s asset=%d handler=%d\n", i, arena.trie[i].segment, arena.trie[i].asset_idx, arena.trie[i].handler_idx);
#endif
}
