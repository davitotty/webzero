/*
 * vm.h — Tiny bytecode interpreter for dynamic handlers
 * Handles form submissions and simple API endpoints.
 */
#ifndef WZ_VM_H
#define WZ_VM_H

#include <stdint.h>
#include <stddef.h>

/* Opcodes */
typedef enum {
    OP_HALT      = 0x00,
    OP_PUSH_STR  = 0x01,  /* push string constant (index into const pool) */
    OP_PUSH_INT  = 0x02,  /* push 32-bit integer immediate */
    OP_LOAD_REQ  = 0x03,  /* load request field (method, path, body, header) */
    OP_STORE_RES = 0x04,  /* store into response (status, body, header) */
    OP_ADD       = 0x05,
    OP_EQ        = 0x06,
    OP_JMP       = 0x07,  /* unconditional jump */
    OP_JMP_IF    = 0x08,  /* jump if top of stack != 0 */
    OP_SEND      = 0x09,  /* finalize and send response */
    OP_REDIRECT  = 0x0A,  /* 302 redirect to string on stack */
    OP_GETPARAM  = 0x0B,  /* get query param by name */
    OP_RESPOND   = 0x0C,  /* respond with status code + body on stack */
} Opcode;

/* Request context passed to VM */
typedef struct {
    const char *method;   /* "GET", "POST", etc. */
    const char *path;
    const char *query;    /* raw query string after '?' */
    const char *body;
    uint32_t    body_len;
    intptr_t    fd;       /* socket to write response to */
} VMRequest;

/* Response context built by VM */
typedef struct {
    uint16_t status;
    char     content_type[64];
    char     body[4096];
    uint32_t body_len;
    char     redirect_to[256];
} VMResponse;

/* VM execution result */
typedef enum {
    VM_OK        = 0,
    VM_ERR_HALT  = 1,
    VM_ERR_OOB   = 2,  /* out of bounds */
    VM_ERR_STACK = 3,
    VM_ERR_LIMIT = 4,
} VMResult;

VMResult vm_run(const uint8_t *bytecode, uint32_t len,
                VMRequest *req, VMResponse *res);

#endif /* WZ_VM_H */
