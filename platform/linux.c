/* Linux: level-triggered epoll, bounded buffers, resumable writes. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "core/connection.h"
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
static int listener = -1, poller = -1, limit;
static uint32_t timeout;
static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }
static int nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
int platform_init(int port, int max_conn, uint32_t timeout_ms) {
    struct sockaddr_in addr;
    struct epoll_event ev;
    int yes = 1;
    limit = max_conn > 0 && max_conn <= MAX_CONNS ? max_conn : MAX_CONNS;
    timeout = timeout_ms ? timeout_ms : 30000;
    signal(SIGINT, stop); signal(SIGTERM, stop); signal(SIGPIPE, SIG_IGN);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) goto fail;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr)); addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons((uint16_t)port);
    if (nonblock(listener) || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 128)) goto fail;
    /* epoll_create works on older Linux 2.6 kernels, unlike epoll_create1. */
    poller = epoll_create(MAX_CONNS);
    if (poller < 0) goto fail;
    memset(&ev, 0, sizeof(ev)); ev.events = EPOLLIN; ev.data.ptr = NULL;
    if (epoll_ctl(poller, EPOLL_CTL_ADD, listener, &ev)) goto fail;
    fprintf(stderr, "webzero: listening on :%d (epoll, %d connections)\n", port, limit);
    return 0;
fail:
    perror("webzero: platform_init");
    if (listener >= 0) close(listener);
    if (poller >= 0) close(poller);
    return -1;
}
void platform_close(ConnState *c) {
    if (!c->active) return;
    epoll_ctl(poller, EPOLL_CTL_DEL, (int)c->fd, NULL); close((int)c->fd);
    c->active = c->pending = 0; c->fd = -1;
}
int platform_recv(ConnState *c, void *buf, size_t len) {
    ssize_t n;
    do { n = recv((int)c->fd, buf, len, 0); } while (n < 0 && errno == EINTR);
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : (int)n;
}
int platform_write(ConnState *c, const void *buf, size_t len) {
    ssize_t n;
    do { n = send((int)c->fd, buf, len, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : (int)n;
}
uint64_t platform_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
void platform_run(serve_fn handler) {
    struct epoll_event events[64];
    while (running) {
        int i, n = epoll_wait(poller, events, 64, 100);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (i = 0; i < n; i++) {
            ConnState *c = events[i].data.ptr;
            if (!c) {
                int accepted;
                /* Bound accept work so floods cannot starve existing sockets. */
                for (accepted = 0; accepted < 64; accepted++) {
                    int fd = accept(listener, NULL, NULL), slot, yes = 1;
                    struct epoll_event ev;
                    if (fd < 0) break;
                    if (nonblock(fd)) { close(fd); continue; }
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
                    slot = connection_alloc(fd, limit);
                    if (slot < 0) {
                        const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                        (void)send(fd, busy, sizeof(busy)-1, MSG_NOSIGNAL); close(fd); continue;
                    }
                    memset(&ev, 0, sizeof(ev)); ev.events = EPOLLIN; ev.data.ptr = &arena.conns[slot];
                    if (epoll_ctl(poller, EPOLL_CTL_ADD, fd, &ev)) platform_close(&arena.conns[slot]);
                }
            } else if (c->active) {
                struct epoll_event ev;
                if (events[i].events & EPOLLERR) { platform_close(c); continue; }
                if (events[i].events & EPOLLOUT) connection_write(c, handler);
                if (c->active && !c->pending && (events[i].events & (EPOLLIN | EPOLLHUP))) connection_read(c, handler);
                if (!c->active) continue;
                memset(&ev, 0, sizeof(ev)); ev.events = c->pending ? EPOLLOUT : EPOLLIN; ev.data.ptr = c;
                if (epoll_ctl(poller, EPOLL_CTL_MOD, (int)c->fd, &ev)) platform_close(c);
            }
        }
        connection_expire(platform_now_ms(), timeout);
    }
    { int i; for (i = 0; i < MAX_CONNS; i++) platform_close(&arena.conns[i]); }
    close(poller); close(listener);
}
#endif
