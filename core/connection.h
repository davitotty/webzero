#ifndef WZ_CONNECTION_H
#define WZ_CONNECTION_H
#include "platform/platform.h"
int connection_alloc(intptr_t fd, int limit);
void connection_read(ConnState *c, serve_fn handler);
void connection_write(ConnState *c, serve_fn handler);
void connection_error(ConnState *c, int status, int head);
void connection_expire(uint64_t now, uint32_t timeout);
#endif
