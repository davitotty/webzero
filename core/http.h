#ifndef WZ_HTTP_H
#define WZ_HTTP_H
#include <stdint.h>
#include <stddef.h>
typedef struct {
    char method[16], path[512], query[256];
    char if_none_match[256], range[128];
    const uint8_t *body;
    uint32_t body_len, consumed;
    int keep_alive, accepts_br, accepts_identity, accepts_webp, head, if_range;
} HTTPRequest;
/* 1: complete; 0: incomplete; negative HTTP status: reject and close. */
int http_parse(const uint8_t *buf, uint32_t len, HTTPRequest *req);
int http_quality(const char *value, size_t len, const char *token, int fallback);
#endif
