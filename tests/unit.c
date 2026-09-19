#include "core/http.h"
#include "core/vm.h"
#include "core/pool.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { assert(x); checks++; } while (0)
static int parse(const char *s, HTTPRequest *r) { return http_parse((const uint8_t *)s,(uint32_t)strlen(s),r); }
int main(void) {
    HTTPRequest r;
    const char *request = "GET /index?x=1 HTTP/1.1\r\nHost: test\r\n\r\n";
    size_t i;
    for (i=0; i<strlen(request); i++) CHECK(http_parse((const uint8_t *)request,(uint32_t)i,&r)==0);
    CHECK(parse(request,&r)==1); CHECK(!strcmp(r.path,"/index")); CHECK(!strcmp(r.query,"x=1")); CHECK(r.keep_alive);
    CHECK(parse("GET / HTTP/1.1\r\n\r\n",&r)==-400);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",&r)==-400);
    CHECK(parse("GET / HTTP/1.0\r\n\r\n",&r)==1 && !r.keep_alive);
    CHECK(parse("GET /%2e%2e/x HTTP/1.1\r\nHost: a\r\n\r\n",&r)==-400);
    CHECK(parse("GET /%00 HTTP/1.1\r\nHost: a\r\n\r\n",&r)==-400);
    CHECK(parse("GET /%zz HTTP/1.1\r\nHost: a\r\n\r\n",&r)==-400);
    CHECK(parse("GET /a%20b HTTP/1.1\r\nHost: a\r\n\r\n",&r)==1 && !strcmp(r.path,"/a b"));
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",&r)==-400);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n\r\nx",&r)==0);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n\r\nxyGET",&r)==1 && r.body_len==2 && r.consumed==strlen("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n\r\nxy"));
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n",&r)==-501);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n",&r)==-400);
    CHECK(parse("GET / HTTP/1.1\r\nHost : a\r\n\r\n",&r)==-400);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive, close\r\n\r\n",&r)==1 && !r.keep_alive);
    CHECK(http_quality("br;q=0, *;q=1",13,"br",0)==0);
    CHECK(http_quality("gzip, br;q=0.5",14,"br",0)==500);
    CHECK(http_quality("zebra",5,"br",0)==0);
    CHECK(http_quality("*;q=0",5,"identity",1000)==0);
    CHECK(http_quality("br;q=2",6,"br",0)==0);
    {
        VMRequest req = {0}; VMResponse res;
        uint8_t loop[] = {OP_JMP,253,255}, under[] = {OP_ADD}, badjump[] = {OP_JMP,10,0};
        uint8_t code[200]; size_t n=0;
        CHECK(vm_run(loop,sizeof(loop),&req,&res)==VM_ERR_LIMIT);
        CHECK(vm_run(under,sizeof(under),&req,&res)==VM_ERR_STACK);
        CHECK(vm_run(badjump,sizeof(badjump),&req,&res)==VM_ERR_OOB);
        for (i=0;i<33;i++) { code[n++]=OP_PUSH_INT; memset(code+n,0,4); n+=4; }
        CHECK(vm_run(code,(uint32_t)n,&req,&res)==VM_ERR_STACK);
        { uint8_t truncated[]={OP_PUSH_STR,255,0}; CHECK(vm_run(truncated,sizeof(truncated),&req,&res)==VM_ERR_OOB); }
    }
    /* Bounded parser mutations exercise every byte position, including NULs. */
    {
        uint8_t mutated[256]; unsigned j;
        for (i=0; i<strlen(request); i++) for (j=0; j<256; j++) {
            memcpy(mutated, request, strlen(request)); mutated[i]=(uint8_t)j;
            (void)http_parse(mutated,(uint32_t)strlen(request),&r);
        }
    }
    pool_scratch_reset(); CHECK(pool_scratch_alloc(SIZE_MAX)==NULL); CHECK(pool_scratch_alloc(SCRATCH_SIZE)!=NULL); CHECK(pool_scratch_alloc(1)==NULL);
    printf("%u unit assertions passed\n",checks); return 0;
}
