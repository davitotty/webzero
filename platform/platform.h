#ifndef WZ_PLATFORM_H
#define WZ_PLATFORM_H
#include "core/pool.h"
#include "core/http.h"
typedef void (*serve_fn)(ConnState *, const HTTPRequest *);
int platform_init(int port, int max_conn, uint32_t timeout_ms);
void platform_run(serve_fn handler);
/* Nonblocking operations: -2 means would block; -1 means fatal. */
int platform_recv(ConnState *c, void *buf, size_t len);
int platform_write(ConnState *c, const void *buf, size_t len);
void platform_close(ConnState *c);
uint64_t platform_now_ms(void);
#endif
