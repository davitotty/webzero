/* Windows XP-compatible nonblocking select loop; no outstanding overlapped
 * buffers can outlive their connection or be reused by another socket. */
#ifdef _WIN32
#define FD_SETSIZE 257
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "core/connection.h"
static SOCKET listener = INVALID_SOCKET;
static volatile LONG running = 1;
static int limit;
static uint32_t timeout;
static BOOL WINAPI stop(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&running, 0); return TRUE;
    }
    return FALSE;
}
uint64_t platform_now_ms(void) {
    /* Extend XP's 32-bit monotonic tick counter across its 49-day wrap. */
    static DWORD previous;
    static uint64_t high;
    DWORD now = GetTickCount();
    if (now < previous) high += (uint64_t)1 << 32;
    previous = now; return high + now;
}
int platform_init(int port, int max_conn, uint32_t timeout_ms) {
    WSADATA wsa;
    struct sockaddr_in addr;
    u_long nonblocking = 1;
    int exclusive = 1;
    limit = max_conn > 0 && max_conn <= MAX_CONNS ? max_conn : MAX_CONNS;
    timeout = timeout_ms ? timeout_ms : 30000;
    if (WSAStartup(MAKEWORD(2,2), &wsa)) return -1;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) goto fail;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));
    memset(&addr, 0, sizeof(addr)); addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons((u_short)port);
    if (ioctlsocket(listener, FIONBIO, &nonblocking) || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 128)) goto fail;
    SetConsoleCtrlHandler(stop, TRUE);
    fprintf(stderr, "webzero: listening on :%d (select, %d connections)\n", port, limit);
    return 0;
fail:
    fprintf(stderr, "webzero: socket setup failed (%d)\n", WSAGetLastError());
    if (listener != INVALID_SOCKET) closesocket(listener);
    WSACleanup(); return -1;
}
void platform_close(ConnState *c) {
    if (!c->active) return;
    closesocket((SOCKET)c->fd); c->fd = -1; c->active = c->pending = 0;
}
int platform_recv(ConnState *c, void *buf, size_t len) {
    int n = recv((SOCKET)c->fd, (char *)buf, (int)len, 0);
    return n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK ? -2 : n;
}
int platform_write(ConnState *c, const void *buf, size_t len) {
    int n = send((SOCKET)c->fd, (const char *)buf, (int)len, 0);
    return n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK ? -2 : n;
}
void platform_run(serve_fn handler) {
    while (running) {
        fd_set reads, writes, errors;
        struct timeval tv = {0, 100000};
        int i;
        FD_ZERO(&reads); FD_ZERO(&writes); FD_ZERO(&errors); FD_SET(listener, &reads);
        for (i = 0; i < limit; i++) if (arena.conns[i].active) {
            SOCKET s = (SOCKET)arena.conns[i].fd;
            if (arena.conns[i].pending) FD_SET(s, &writes); else FD_SET(s, &reads);
            FD_SET(s, &errors);
        }
        if (select(0, &reads, &writes, &errors, &tv) == SOCKET_ERROR) break;
        /* Process old readiness before accepting, avoiding slot reuse hazards. */
        for (i = 0; i < limit; i++) if (arena.conns[i].active) {
            ConnState *c = &arena.conns[i]; SOCKET s = (SOCKET)c->fd;
            if (FD_ISSET(s, &errors)) { platform_close(c); continue; }
            if (FD_ISSET(s, &writes)) connection_write(c, handler);
            if (c->active && !c->pending && FD_ISSET(s, &reads)) connection_read(c, handler);
        }
        if (FD_ISSET(listener, &reads)) for (i = 0; i < 64; i++) {
            SOCKET s = accept(listener, NULL, NULL);
            u_long nonblocking = 1;
            int yes = 1;
            if (s == INVALID_SOCKET) break;
            if (ioctlsocket(s, FIONBIO, &nonblocking)) { closesocket(s); continue; }
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
            if (connection_alloc((intptr_t)s, limit) < 0) {
                const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                (void)send(s, busy, (int)sizeof(busy)-1, 0); closesocket(s);
            }
        }
        connection_expire(platform_now_ms(), timeout);
    }
    { int i; for (i = 0; i < MAX_CONNS; i++) platform_close(&arena.conns[i]); }
    closesocket(listener); WSACleanup(); SetConsoleCtrlHandler(stop, FALSE);
}
#endif
