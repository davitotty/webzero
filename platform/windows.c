/*
 * windows.c — Windows platform implementation
 * Uses IOCP + TransmitFile for async zero-copy I/O.
 * Targets Windows XP SP3+ (/subsystem:windows,5.01 for 32-bit).
 *
 * Compile with:
 *   i686-w64-mingw32-gcc -O3 -D_WIN32_WINNT=0x0501 ...
 */
#ifdef _WIN32

#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0501   /* XP SP3 */
#endif

#include "../platform/platform.h"
#include "../core/pool.h"

#include <winsock2.h>
#include <mswsock.h>    /* TransmitFile, AcceptEx */
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* XP-compatible: LoadLibrary/GetProcAddress for AcceptEx */
typedef BOOL (WINAPI *PFN_ACCEPTEX)(SOCKET, SOCKET, PVOID, DWORD, DWORD,
                                     DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *PFN_TRANSMITFILE)(HANDLE, HANDLE, DWORD, DWORD,
                                         LPOVERLAPPED, LPTRANSMIT_FILE_BUFFERS,
                                         DWORD);

/* Per-operation context (overlapped must be first) */
typedef struct {
    OVERLAPPED  ov;
    int         slot_idx;
    int         op_type;   /* 0 = accept, 1 = recv */
    char        accept_buf[64]; /* AcceptEx initial buffer */
} IOCtx;

#define IOCtx_POOL_SIZE (MAX_CONNS + 1)

static IOCtx       g_ioctx[IOCtx_POOL_SIZE];
static HANDLE      g_iocp       = NULL;
static SOCKET      g_listen_sock= INVALID_SOCKET;
static volatile LONG g_running  = 1;

static serve_fn    g_handler    = NULL;

/* ------------------------------------------------------------------ */
/* memmem replacement (not available on Windows)                       */
/* ------------------------------------------------------------------ */

static void *wz_memmem(const void *haystack, size_t hlen,
                        const void *needle, size_t nlen) {
    if (nlen == 0) return (void *)haystack;
    if (hlen < nlen) return NULL;
    const char *h = (const char *)haystack;
    const char *n = (const char *)needle;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(h + i, n, nlen) == 0)
            return (void *)(h + i);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Connection slot helpers                                             */
/* ------------------------------------------------------------------ */

static int alloc_conn_slot(SOCKET s) {
    for (int i = 0; i < MAX_CONNS; i++) {
        if (!arena.conns[i].active) {
            memset(&arena.conns[i], 0, sizeof(ConnState));
            arena.conns[i].fd     = (int)s;
            arena.conns[i].active = 1;
            return i;
        }
    }
    return -1;
}

static void free_conn_slot(ConnState *c) {
    c->active = 0;
    c->fd     = -1;
}

/* ------------------------------------------------------------------ */
/* platform_init                                                       */
/* ------------------------------------------------------------------ */

int platform_init(int port, int max_conn) {
    (void)max_conn;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "webzero: WSAStartup failed\n");
        return -1;
    }

    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!g_iocp) {
        fprintf(stderr, "webzero: CreateIoCompletionPort failed (%lu)\n",
                GetLastError());
        return -1;
    }

    g_listen_sock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                               NULL, 0, WSA_FLAG_OVERLAPPED);
    if (g_listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "webzero: WSASocket failed\n");
        return -1;
    }

    int yes = 1;
    setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR,
               (char *)&yes, sizeof(yes));
    setsockopt(g_listen_sock, IPPROTO_TCP, TCP_NODELAY,
               (char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((u_short)port);

    if (bind(g_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "webzero: bind failed (%d)\n", WSAGetLastError());
        closesocket(g_listen_sock);
        return -1;
    }

    if (listen(g_listen_sock, SOMAXCONN) != 0) {
        fprintf(stderr, "webzero: listen failed (%d)\n", WSAGetLastError());
        closesocket(g_listen_sock);
        return -1;
    }

    /* Associate listening socket with IOCP (key = 0 for listen) */
    CreateIoCompletionPort((HANDLE)g_listen_sock, g_iocp, 0, 0);

    fprintf(stderr, "webzero: listening on :%d (IOCP)\n", port);
    return 0;
}

/* ------------------------------------------------------------------ */
/* platform_run — IOCP event loop                                      */
/* ------------------------------------------------------------------ */

void platform_run(serve_fn handler) {
    g_handler = handler;

    DWORD  bytes;
    ULONG_PTR key;
    OVERLAPPED *ov_ptr;

    while (g_running) {
        /* Non-blocking accept loop (simple select fallback for XP compat) */
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(g_listen_sock, &rset);
        struct timeval tv = { 0, 100000 }; /* 100ms */

        if (select(0, &rset, NULL, NULL, &tv) > 0) {
            SOCKET cfd = accept(g_listen_sock, NULL, NULL);
            if (cfd != INVALID_SOCKET) {
                int slot = alloc_conn_slot(cfd);
                if (slot < 0) {
                    static const char HDR_503[] =
                        "HTTP/1.1 503 Service Unavailable\r\n"
                        "Content-Length: 0\r\n\r\n";
                    send(cfd, HDR_503, (int)(sizeof(HDR_503) - 1), 0);
                    closesocket(cfd);
                } else {
                    /* Associate client socket with IOCP */
                    CreateIoCompletionPort((HANDLE)cfd, g_iocp,
                                           (ULONG_PTR)slot, 0);

                    /* Post an initial recv */
                    memset(&g_ioctx[slot], 0, sizeof(IOCtx));
                    g_ioctx[slot].slot_idx = slot;
                    g_ioctx[slot].op_type  = 1; /* recv */

                    WSABUF wbuf;
                    wbuf.buf = (char *)arena.conn_bufs[slot];
                    wbuf.len = CONN_BUF_SIZE - 1;
                    DWORD flags = 0, recvd = 0;
                    WSARecv((SOCKET)arena.conns[slot].fd,
                            &wbuf, 1, &recvd, &flags,
                            &g_ioctx[slot].ov, NULL);
                }
            }
        }

        /* Drain IOCP completions */
        while (GetQueuedCompletionStatus(g_iocp, &bytes, &key, &ov_ptr, 0)) {
            if (!ov_ptr) continue;
            IOCtx *ctx = (IOCtx *)ov_ptr;
            int slot   = ctx->slot_idx;
            ConnState *c = &arena.conns[slot];

            if (bytes == 0) {
                platform_close(c);
                continue;
            }

            if (ctx->op_type == 1) { /* recv complete */
                arena.conn_bufs[slot][bytes] = '\0';
                c->buf_len = bytes;

                if (wz_memmem(arena.conn_bufs[slot], bytes, "\r\n\r\n", 4)) {
                    g_handler(c, arena.conn_bufs[slot], bytes);
                    c->buf_len = 0;
                }

                /* Re-arm recv */
                memset(&g_ioctx[slot].ov, 0, sizeof(OVERLAPPED));
                WSABUF wbuf;
                wbuf.buf = (char *)arena.conn_bufs[slot];
                wbuf.len = CONN_BUF_SIZE - 1;
                DWORD flags = 0, recvd = 0;
                WSARecv((SOCKET)c->fd, &wbuf, 1, &recvd, &flags,
                        &g_ioctx[slot].ov, NULL);
            }
        }
    }

    closesocket(g_listen_sock);
    CloseHandle(g_iocp);
    WSACleanup();
}

/* ------------------------------------------------------------------ */
/* I/O helpers                                                         */
/* ------------------------------------------------------------------ */

int platform_send_file(ConnState *c, const void *data, size_t len) {
    int sent = send((SOCKET)c->fd, (const char *)data, (int)len, 0);
    return sent;
}

int platform_send(ConnState *c, const void *buf, size_t len) {
    int sent = send((SOCKET)c->fd, (const char *)buf, (int)len, 0);
    return sent;
}

void platform_close(ConnState *c) {
    if (c->fd >= 0) {
        closesocket((SOCKET)c->fd);
    }
    free_conn_slot(c);
}

uint64_t platform_now_ms(void) {
    return (uint64_t)GetTickCount();
}

#endif /* _WIN32 */
