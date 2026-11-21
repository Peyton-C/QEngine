/* tee: the OP-TEE attestation arm64 Engine 5.1.0 and later runs before its GUI.
 *
 * Whether the real application does this is read off it: it links libteec or
 * it does not. Without teeshim the calls reach the real library, which has no
 * secure world to talk to under QEMU, and Engine quits to TestApp.
 *
 * The command and its parameters are not reproduced exactly; teeshim does not
 * depend on them either. This asks for a verdict both ways a client can, as a
 * temporary memory reference and as a value, and expects each to read 1. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include "fake.h"

/* The subset of optee_client's tee_client_api.h that teeshim.c declares. */
typedef struct {
    uint32_t timeLow;
    uint16_t timeMid, timeHiAndVersion;
    uint8_t clockSeqAndNode[8];
} TEEC_UUID;
typedef union {
    struct { void *buffer; size_t size; } tmpref;
    struct { void *parent; size_t size, offset; } memref;
    struct { uint32_t a, b; } value;
} TEEC_Parameter;
typedef struct {
    uint32_t started, paramTypes;
    TEEC_Parameter params[4];
    void *session;
} TEEC_Operation;
#define TEEC_VALUE_OUTPUT 0x2
#define TEEC_MEMREF_TEMP_OUTPUT 0x6

void probe_tee(void) {
    if (cfg.real && !app_needs("libteec")) {
        t_skip("tee", "the application does not link libteec");
        return;
    }
    if (!entry("TEEC_InvokeCommand")) dlopen("libteec.so.1", RTLD_NOW | RTLD_GLOBAL);
    uint32_t (*init)(const char *, void *) = entry("TEEC_InitializeContext");
    uint32_t (*open_session)(void *, void *, const TEEC_UUID *, uint32_t, const void *,
                             TEEC_Operation *, uint32_t *) = entry("TEEC_OpenSession");
    uint32_t (*invoke)(void *, uint32_t, TEEC_Operation *, uint32_t *) =
        entry("TEEC_InvokeCommand");
    void (*close_session)(void *) = entry("TEEC_CloseSession");
    void (*finalize)(void *) = entry("TEEC_FinalizeContext");
    if (!init || !open_session || !invoke) {
        if (cfg.real) t_fail("tee", "no TEE client library, and no teeshim");
        else t_skip("tee", "no TEE client here");
        return;
    }
    t_note("tee answered by %s",
           shim_exporting("TEEC_InvokeCommand") ? shim_exporting("TEEC_InvokeCommand")
                                                : "the real libteec");

    /* Opaque to the caller, and far larger than the real structures. */
    static long long context[64], session[64];
    TEEC_UUID ta;
    memset(&ta, 0, sizeof(ta));
    uint32_t origin = 0, rc = init(NULL, context);
    if (rc) {
        t_fail("tee.context", "TEEC_InitializeContext returned 0x%x", rc);
        return;
    }
    t_ok("tee.context", "initialized");
    rc = open_session(context, session, &ta, 0, NULL, NULL, &origin);
    if (rc) {
        t_fail("tee.session", "TEEC_OpenSession returned 0x%x", rc);
        if (finalize) finalize(context);
        return;
    }
    t_ok("tee.session", "opened");

    uint32_t verdict = 0;
    TEEC_Operation op;
    memset(&op, 0, sizeof(op));
    op.paramTypes = TEEC_MEMREF_TEMP_OUTPUT | (TEEC_VALUE_OUTPUT << 4);
    op.params[0].tmpref.buffer = &verdict;
    op.params[0].tmpref.size = sizeof(verdict);
    rc = invoke(session, 0, &op, &origin);
    if (rc) t_fail("tee.attest", "TEEC_InvokeCommand returned 0x%x", rc);
    else if (verdict != 1 || op.params[1].value.a != 1)
        t_fail("tee.attest", "verdict %u by reference, %u by value, expected 1", verdict,
               op.params[1].value.a);
    else t_ok("tee.attest", "attested");

    if (close_session) close_session(session);
    if (finalize) finalize(context);
}
