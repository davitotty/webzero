#include "connection.h"
#include <stdio.h>
#include <string.h>

int connection_alloc(intptr_t fd, int limit) {
    int i;
    for (i = 0; i < limit; i++) if (!arena.conns[i].active) {
        ConnState *c = &arena.conns[i];
        memset(c, 0, sizeof(*c)); c->fd = fd; c->active = 1;
        c->last_active = platform_now_ms(); return i;
    }
    return -1;
}
void connection_error(ConnState *c, int status, int head) {
    const char *reason;
    char *out = (char *)arena.response_bufs[c-arena.conns];
    (void)head;
    switch (status) {
        case 400: reason = "Bad Request"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        case 406: reason = "Not Acceptable"; break;
        case 413: reason = "Content Too Large"; break;
        case 414: reason = "URI Too Long"; break;
        case 417: reason = "Expectation Failed"; break;
        case 431: reason = "Request Header Fields Too Large"; break;
        case 501: reason = "Not Implemented"; break;
        case 505: reason = "HTTP Version Not Supported"; break;
        default: status = 500; reason = "Internal Server Error"; break;
    }
    c->out_len = (uint32_t)snprintf(out, RESPONSE_BUF_SIZE,
        "HTTP/1.1 %d %s\r\nContent-Length: 0\r\n%sConnection: %s\r\n\r\n",
        status, reason, status == 405 ? "Allow: GET, HEAD\r\n" : "", c->close_after ? "close" : "keep-alive");
    c->out_sent = 0; c->body = NULL; c->body_len = c->body_sent = 0; c->pending = 1;
}
static void flush(ConnState *c) {
    size_t budget = 256u * 1024u;
    while (c->active && c->pending && budget) {
        const uint8_t *data;
        size_t n;
        int sent, header = c->out_sent < c->out_len;
        if (header) { data = arena.response_bufs[c-arena.conns] + c->out_sent; n = c->out_len - c->out_sent; }
        else { data = c->body ? c->body + c->body_sent : NULL; n = c->body_len - c->body_sent; }
        if (!n) { c->pending = 0; if (c->close_after) platform_close(c); return; }
        if (n > budget) n = budget;
        sent = platform_write(c, data, n);
        if (sent == -2) return;
        if (sent <= 0) { platform_close(c); return; }
        if (header) c->out_sent += (uint32_t)sent; else c->body_sent += (size_t)sent;
        budget -= (size_t)sent; c->last_active = platform_now_ms();
    }
}
static void process(ConnState *c, serve_fn handler) {
    uint8_t *buf = arena.conn_bufs[c-arena.conns];
    while (c->active && !c->pending && c->buf_len) {
        HTTPRequest req;
        int result = http_parse(buf, c->buf_len, &req);
        if (!result) return;
        if (result < 0) { c->close_after = 1; connection_error(c, -result, req.head); }
        else {
            c->close_after = (uint8_t)!req.keep_alive;
            handler(c, &req);
            c->buf_len -= req.consumed;
            memmove(buf, buf + req.consumed, c->buf_len);
        }
        flush(c);
    }
}
void connection_write(ConnState *c, serve_fn handler) { flush(c); process(c, handler); }
void connection_read(ConnState *c, serve_fn handler) {
    uint8_t *buf = arena.conn_bufs[c-arena.conns];
    while (c->active && !c->pending) {
        int n = platform_recv(c, buf + c->buf_len, CONN_BUF_SIZE - 1u - c->buf_len);
        if (n == -2) return;
        if (n <= 0) { platform_close(c); return; }
        c->buf_len += (uint32_t)n; buf[c->buf_len] = 0;
        c->last_active = platform_now_ms(); process(c, handler);
    }
}
void connection_expire(uint64_t now, uint32_t timeout) {
    int i;
    for (i = 0; i < MAX_CONNS; i++) {
        ConnState *c = &arena.conns[i];
        if (c->active && now - c->last_active >= timeout) platform_close(c);
    }
}
