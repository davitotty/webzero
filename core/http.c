#include "http.h"
#include "pool.h"
#include <string.h>

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int equal(const char *p, size_t n, const char *s) {
    size_t i;
    if (strlen(s) != n) return 0;
    for (i = 0; i < n; i++) if (lower((unsigned char)p[i]) != lower((unsigned char)s[i])) return 0;
    return 1;
}
static int token_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}
static const char *line_end(const char *p, const char *end) {
    while (end - p >= 2) { if (p[0] == '\r' && p[1] == '\n') return p; p++; }
    return NULL;
}
static int copy(char *dst, size_t cap, const char *p, size_t n) {
    if (n >= cap) return 0;
    memcpy(dst, p, n); dst[n] = 0; return 1;
}

/* Exact comma-delimited tokens; explicit q=0 overrides wildcards. */
int http_quality(const char *p, size_t len, const char *token, int fallback) {
    const char *end = p + len;
    int wildcard = -1, exact = -1;
    while (p < end) {
        const char *start, *stop;
        int q = 1000;
        while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) p++;
        start = p;
        while (p < end && *p != ',' && *p != ';' && *p != ' ' && *p != '\t') p++;
        stop = p;
        while (p < end && *p != ',') {
            if (*p++ == ';') {
                while (p < end && (*p == ' ' || *p == '\t')) p++;
                if (p + 1 < end && lower(*p) == 'q' && p[1] == '=') {
                    int digits = 0, scale = 100;
                    p += 2; q = 0;
                    if (p < end && (*p == '0' || *p == '1')) {
                        int one = *p++ == '1'; q = one ? 1000 : 0;
                        if (p < end && *p == '.') {
                            p++;
                            while (p < end && *p >= '0' && *p <= '9') {
                                if (++digits > 3 || (one && *p != '0')) q = -1;
                                if (q >= 0 && !one) q += (*p - '0') * scale;
                                scale /= 10; p++;
                            }
                        }
                        if (p < end && *p != ',' && *p != ';' && *p != ' ' && *p != '\t') q = -1;
                    }
                    if (q < 0) q = 0;
                }
            }
        }
        if (equal(start, (size_t)(stop - start), token)) exact = q;
        else if (stop - start == 1 && *start == '*') wildcard = q;
    }
    if (exact >= 0) return exact;
    if (strcmp(token, "identity") == 0) return wildcard == 0 ? 0 : fallback;
    return wildcard >= 0 ? wildcard : fallback;
}
static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (unsigned char)lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static int decode_path(char *dst, const char *p, size_t n) {
    size_t i, out = 0;
    if (!n || *p != '/') return -400;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '%') {
            int a, b;
            if (i + 2 >= n || (a = hex((unsigned char)p[i+1])) < 0 || (b = hex((unsigned char)p[i+2])) < 0) return -400;
            c = (unsigned char)(a * 16 + b); i += 2;
        }
        if (c < 32 || c == 127 || c == '\\' || c == '?' || c == '#') return -400;
        if (out >= 511) return -414;
        dst[out++] = (char)c;
    }
    dst[out] = 0; p = dst;
    while (*p) {
        const char *s;
        while (*p == '/') p++;
        s = p; while (*p && *p != '/') p++;
        if ((p - s == 1 && *s == '.') || (p - s == 2 && s[0] == '.' && s[1] == '.')) return -400;
    }
    return 1;
}

int http_parse(const uint8_t *buf, uint32_t len, HTTPRequest *r) {
    const char *p = (const char *)buf, *end = p + len, *e, *s, *target, *q;
    uint32_t content_length = 0;
    int host = 0, cl = 0, te = 0, http11, rv, connection_close = 0;
    memset(r, 0, sizeof(*r)); r->accepts_identity = 1;
    e = line_end(p, end);
    if (!e) return len >= CONN_BUF_SIZE - 1 ? -414 : 0;
    s = p;
    while (p < e && token_char((unsigned char)*p)) p++;
    if (p == s || p == e || *p != ' ' || !copy(r->method, sizeof(r->method), s, (size_t)(p-s))) return -400;
    r->head = strcmp(r->method, "HEAD") == 0;
    target = ++p;
    while (p < e && *p != ' ') p++;
    if (p == e) return -400;
    q = memchr(target, '?', (size_t)(p-target));
    rv = decode_path(r->path, target, (size_t)((q ? q : p)-target));
    if (rv < 0) return rv;
    if (q && !copy(r->query, sizeof(r->query), q+1, (size_t)(p-q-1))) return -414;
    p++;
    http11 = e-p == 8 && !memcmp(p, "HTTP/1.1", 8);
    if (!http11 && !(e-p == 8 && !memcmp(p, "HTTP/1.0", 8))) return -505;
    r->keep_alive = http11; p = e + 2;
    for (;;) {
        const char *colon, *v, *ve;
        size_t name_len, n;
        e = line_end(p, end);
        if (!e) return len >= CONN_BUF_SIZE - 1 ? -431 : 0;
        if (e == p) { p += 2; break; }
        colon = memchr(p, ':', (size_t)(e-p));
        if (!colon || colon == p) return -400;
        for (s = p; s < colon; s++) if (!token_char((unsigned char)*s)) return -400;
        for (s = colon+1; s < e; s++) if (((unsigned char)*s < 32 && *s != '\t') || *s == 127) return -400;
        name_len = (size_t)(colon-p); v = colon+1; ve = e;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        n = (size_t)(ve-v);
        if (equal(p, name_len, "host")) { if (++host > 1 || !n) return -400; }
        else if (equal(p, name_len, "content-length")) {
            if (++cl > 1 || !n) return -400;
            for (s = v; s < ve; s++) {
                if (*s < '0' || *s > '9') return -400;
                if (content_length > (CONN_BUF_SIZE - 1u - (unsigned)(*s-'0')) / 10u) return -413;
                content_length = content_length * 10u + (unsigned)(*s-'0');
            }
        } else if (equal(p, name_len, "transfer-encoding")) te = 1;
        else if (equal(p, name_len, "expect")) return -417;
        else if (equal(p, name_len, "connection")) {
            if (http_quality(v, n, "close", 0)) connection_close = 1;
            if (http_quality(v, n, "keep-alive", 0)) r->keep_alive = 1;
        } else if (equal(p, name_len, "accept-encoding")) {
            r->accepts_br = http_quality(v, n, "br", 0);
            r->accepts_identity = http_quality(v, n, "identity", 1000);
        } else if (equal(p, name_len, "accept")) r->accepts_webp = http_quality(v, n, "image/webp", 0);
        else if (equal(p, name_len, "if-none-match")) {
            if (!copy(r->if_none_match, sizeof(r->if_none_match), v, n)) return -431;
        } else if (equal(p, name_len, "range")) {
            if (!copy(r->range, sizeof(r->range), v, n)) return -431;
        } else if (equal(p, name_len, "if-range")) r->if_range = 1;
        p = e+2;
    }
    if (http11 && !host) return -400;
    if (te) return cl ? -400 : -501;
    if (connection_close) r->keep_alive = 0;
    if ((size_t)(p-(const char *)buf) + content_length >= CONN_BUF_SIZE) return -413;
    if ((size_t)(end-p) < content_length) return 0;
    r->body = (const uint8_t *)p; r->body_len = content_length;
    r->consumed = (uint32_t)(p-(const char *)buf) + content_length;
    return 1;
}
