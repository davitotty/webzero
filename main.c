/* WebZero: fixed-memory HTTP server. All response storage is per connection. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "core/bundle.h"
#include "core/router.h"
#include "core/vm.h"
#include "core/connection.h"
static Bundle bundle;
static int etag_matches(const char *p, const char *tag) {
    while (*p) {
        const char *s, *e;
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        s = p; while (*p && *p != ',') p++; e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if (e-s == 1 && *s == '*') return 1;
        if (e-s > 2 && s[0] == 'W' && s[1] == '/') s += 2;
        if ((size_t)(e-s) == strlen(tag+2) && !memcmp(s, tag+2, (size_t)(e-s))) return 1;
    }
    return 0;
}
static int decimal(const char **p, uint64_t *n) {
    const char *start = *p; *n = 0;
    while (**p >= '0' && **p <= '9') {
        unsigned digit = (unsigned)(*(*p)++ - '0');
        if (*n > (UINT64_MAX-digit)/10u) return 0;
        *n = *n * 10u + digit;
    }
    return *p != start;
}
/* 0: ignore unsupported/invalid syntax, 1: range, -1: unsatisfiable. */
static int byte_range(const char *p, uint32_t total, uint32_t *start, uint32_t *length) {
    uint64_t a, b;
    if (strncmp(p, "bytes=", 6) || strchr(p, ',')) return 0;
    p += 6;
    if (*p == '-') {
        p++; if (!decimal(&p, &b) || *p) return 0;
        if (!b || !total) return -1;
        if (b > total) b = total;
        *start = total-(uint32_t)b; *length = (uint32_t)b; return 1;
    }
    if (!decimal(&p, &a) || *p++ != '-') return 0;
    b = total ? total-1u : 0;
    if (*p && (!decimal(&p, &b) || *p)) return 0;
    if (a > b || a >= total) return -1;
    if (b >= total) b = total-1u;
    *start = (uint32_t)a; *length = (uint32_t)(b-a+1); return 1;
}
static void serve_asset(ConnState *c, const HTTPRequest *r, int32_t index) {
    const AssetEntry *a = &bundle.assets[index];
    const uint8_t *data;
    uint32_t size, start = 0, length;
    char tag[64], extra[160] = "", *out = (char *)arena.response_bufs[c-arena.conns];
    int encoded, status = 200, n;
    const char *reason = "OK";
    if (r->accepts_webp && a->webp_idx >= 0) { index = a->webp_idx; a = &bundle.assets[index]; }
    encoded = a->encoding && r->accepts_br > 0;
    if (!encoded && (!r->accepts_identity || (a->encoding && bundle.version == 1))) {
        connection_error(c, 406, r->head); return;
    }
    size = encoded ? a->compressed_len : a->original_len;
    data = bundle.base + bundle.data_offset + (a->encoding && !encoded ? a->raw_offset : a->offset);
    length = size;
    snprintf(tag, sizeof(tag), "W/\"%08x-%x-%d\"", bundle.fingerprint, (unsigned)index, encoded);
    if (etag_matches(r->if_none_match, tag)) { status = 304; reason = "Not Modified"; }
    else if (!r->head && !r->if_range && r->range[0]) {
        int range = byte_range(r->range, size, &start, &length);
        if (range < 0) {
            status = 416; reason = "Range Not Satisfiable"; length = 0;
            snprintf(extra, sizeof(extra), "Content-Range: bytes */%u\r\n", size);
        } else if (range > 0) {
            status = 206; reason = "Partial Content";
            snprintf(extra, sizeof(extra), "Content-Range: bytes %u-%u/%u\r\n", start, start+length-1, size);
        }
    }
    n = snprintf(out, RESPONSE_BUF_SIZE,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "%sCache-Control: public, max-age=0, must-revalidate\r\n"
        "Vary: Accept, Accept-Encoding\r\nETag: %s\r\nAccept-Ranges: bytes\r\n"
        "X-Content-Type-Options: nosniff\r\n%sConnection: %s\r\n\r\n",
        status, reason, a->mime, length, encoded ? "Content-Encoding: br\r\n" : "",
        tag, extra, c->close_after ? "close" : "keep-alive");
    if (n < 0 || n >= RESPONSE_BUF_SIZE) { connection_error(c, 500, r->head); return; }
    c->out_len = (uint32_t)n; c->out_sent = 0; c->body = data + start;
    c->body_len = r->head || status == 304 ? 0 : length; c->body_sent = 0; c->pending = 1;
}
static int safe_header(const char *s) {
    while (*s) { if ((unsigned char)*s < 32 || (unsigned char)*s == 127) return 0; s++; }
    return 1;
}
static void serve_handler(ConnState *c, const HTTPRequest *r, int32_t index) {
    HandlerEntry h;
    VMRequest req;
    VMResponse res;
    char request_body[CONN_BUF_SIZE], *out = (char *)arena.response_bufs[c-arena.conns];
    int n;
    memcpy(&h, &bundle.handlers[index], sizeof(h));
    memcpy(request_body, r->body, r->body_len); request_body[r->body_len] = 0;
    req.method = r->method; req.path = r->path; req.query = r->query;
    req.body = request_body; req.body_len = r->body_len; req.fd = c->fd;
    if (vm_run(bundle.base+h.offset, h.len, &req, &res) != VM_OK ||
        !safe_header(res.content_type) || !safe_header(res.redirect_to) || res.status < 200 || res.status > 599) {
        connection_error(c, 500, r->head); return;
    }
    if (res.redirect_to[0]) {
        n = snprintf(out, RESPONSE_BUF_SIZE, "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: %s\r\n\r\n",
            res.redirect_to, c->close_after ? "close" : "keep-alive"); res.body_len = 0;
    } else {
        if (res.status == 204 || res.status == 304) res.body_len = 0;
        n = snprintf(out, RESPONSE_BUF_SIZE, "HTTP/1.1 %u Response\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: %s\r\n\r\n",
            res.status, res.content_type, res.body_len, c->close_after ? "close" : "keep-alive");
    }
    if (n < 0 || (size_t)n + res.body_len > RESPONSE_BUF_SIZE) { connection_error(c, 500, r->head); return; }
    if (!r->head) memcpy(out+n, res.body, res.body_len);
    c->out_len = (uint32_t)n + (r->head ? 0 : res.body_len); c->out_sent = 0;
    c->body = NULL; c->body_len = c->body_sent = 0; c->pending = 1;
}
static void handle_request(ConnState *c, const HTTPRequest *r) {
    RouteMatch match;
    int get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
    if (!get && !r->head && !post) { connection_error(c, 405, r->head); return; }
    match = router_lookup(r->path);
    if (!match.found) { connection_error(c, 404, r->head); return; }
    if (match.asset_idx >= 0) {
        if (post) { connection_error(c, 405, 0); return; }
        serve_asset(c, r, match.asset_idx);
    } else serve_handler(c, r, match.handler_idx);
}
int main(int argc, char **argv) {
    int port;
    if (argc == 2 && !strcmp(argv[1], "--version")) { puts("webzero 2.0.0 (bundle v1/v2)"); return 0; }
    if (argc < 2 || argc > 3 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        fprintf(stderr, "Usage: webzero <site.web> [port]\nBuild: node tools/wz.js build <directory>\n");
        return argc == 2 ? 0 : 1;
    }
    if (bundle_load(argv[1], &bundle)) return 1;
    if (router_build(&bundle)) { bundle_unload(&bundle); return 1; }
    port = bundle.config.port;
    if (argc == 3) {
        char *end; long value;
        errno = 0; value = strtol(argv[2], &end, 10);
        if (errno || !argv[2][0] || *end || value < 1 || value > 65535) {
            fprintf(stderr, "webzero: invalid port\n"); bundle_unload(&bundle); return 1;
        }
        port = (int)value;
    }
    fprintf(stderr, "webzero: %zu bytes, %u assets, %u routes; arena %zu bytes\n", bundle.file_size,
        bundle.config.asset_count, bundle.config.route_node_count, sizeof(arena));
    if (platform_init(port, bundle.config.max_connections, bundle.config.keepalive_timeout_ms)) { bundle_unload(&bundle); return 1; }
    platform_run(handle_request); bundle_unload(&bundle); return 0;
}
