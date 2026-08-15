/* Answers the OP-TEE attestation Engine 5.1.0 performs before it starts its GUI.
 *
 * Preloaded ahead of libteec.so.1, so these definitions win the symbol lookup and
 * the real library is never consulted (it stays loaded via Engine's DT_NEEDED,
 * which is harmless). Nothing here dereferences the context or session objects,
 * so no assumption is made about their layout — only TEEC_Operation's, which the
 * call site pins down.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* ABI-compatible subset of optee_client's <tee_client_api.h>. Re-declared rather
 * than included so the build container needs no OP-TEE headers, matching how
 * alsashim declares the libasound types it touches. */

typedef uint32_t TEEC_Result;

#define TEEC_SUCCESS 0x00000000

/* Parameter type nibbles, packed four to a word in TEEC_Operation.paramTypes. */
#define TEEC_NONE                   0x0
#define TEEC_VALUE_INPUT            0x1
#define TEEC_VALUE_OUTPUT           0x2
#define TEEC_VALUE_INOUT            0x3
#define TEEC_MEMREF_TEMP_INPUT      0x5
#define TEEC_MEMREF_TEMP_OUTPUT     0x6
#define TEEC_MEMREF_TEMP_INOUT      0x7
#define TEEC_MEMREF_WHOLE           0xC
#define TEEC_MEMREF_PARTIAL_INPUT   0xD
#define TEEC_MEMREF_PARTIAL_OUTPUT  0xE
#define TEEC_MEMREF_PARTIAL_INOUT   0xF

#define TEEC_ORIGIN_TRUSTED_APP 0x00000004

typedef struct {
    uint32_t timeLow;
    uint16_t timeMid;
    uint16_t timeHiAndVersion;
    uint8_t clockSeqAndNode[8];
} TEEC_UUID;

typedef struct {
    void *buffer;
    size_t size;
} TEEC_TempMemoryReference;

typedef struct {
    void *parent;
    size_t size;
    size_t offset;
} TEEC_RegisteredMemoryReference;

typedef struct {
    uint32_t a;
    uint32_t b;
} TEEC_Value;

typedef union {
    TEEC_TempMemoryReference tmpref;
    TEEC_RegisteredMemoryReference memref;
    TEEC_Value value;
} TEEC_Parameter;

typedef struct {
    uint32_t started;
    uint32_t paramTypes;
    TEEC_Parameter params[4];
    void *session; /* implementation-defined tail; brings sizeof to 0x70 */
} TEEC_Operation;

/* The context and session are opaque here — see the header comment. */
typedef void TEEC_Context;
typedef void TEEC_Session;

static int debug_enabled(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("TEESHIM_DEBUG") != NULL;
    return cached;
}

TEEC_Result TEEC_InitializeContext(const char *name, TEEC_Context *context) {
    (void)name;
    (void)context;
    if (debug_enabled())
        fprintf(stderr, "[teeshim] TEEC_InitializeContext -> SUCCESS\n");
    return TEEC_SUCCESS;
}

void TEEC_FinalizeContext(TEEC_Context *context) {
    (void)context;
    if (debug_enabled())
        fprintf(stderr, "[teeshim] TEEC_FinalizeContext\n");
}

TEEC_Result TEEC_OpenSession(TEEC_Context *context, TEEC_Session *session,
                             const TEEC_UUID *destination,
                             uint32_t connectionMethod, const void *connectionData,
                             TEEC_Operation *operation, uint32_t *returnOrigin) {
    (void)context;
    (void)session;
    (void)connectionMethod;
    (void)connectionData;
    (void)operation;
    if (returnOrigin) *returnOrigin = TEEC_ORIGIN_TRUSTED_APP;
    if (debug_enabled() && destination)
        fprintf(stderr,
                "[teeshim] TEEC_OpenSession ta=%08x-%04x-%04x-"
                "%02x%02x%02x%02x%02x%02x%02x%02x -> SUCCESS\n",
                destination->timeLow, destination->timeMid,
                destination->timeHiAndVersion,
                destination->clockSeqAndNode[0], destination->clockSeqAndNode[1],
                destination->clockSeqAndNode[2], destination->clockSeqAndNode[3],
                destination->clockSeqAndNode[4], destination->clockSeqAndNode[5],
                destination->clockSeqAndNode[6], destination->clockSeqAndNode[7]);
    return TEEC_SUCCESS;
}

void TEEC_CloseSession(TEEC_Session *session) {
    (void)session;
    if (debug_enabled())
        fprintf(stderr, "[teeshim] TEEC_CloseSession\n");
}

/* Writes the "attested" answer into whichever parameters the caller marked as
 * outputs.
 *
 * Driven off paramTypes rather than hardcoded to params[0], so a later build
 * that moves the verdict to a different slot, or asks for it as a VALUE instead
 * of a temp memref, still gets a non-zero answer without another round of
 * disassembly. Output buffers are filled with 1 in every byte-width the caller
 * could plausibly read, largest first, so a u8/u16/u32/u64 read all see 1.
 * Nothing else in Engine uses libteec, so there is no other consumer to confuse.
 */
static void fill_output(TEEC_Parameter *param, uint32_t type) {
    switch (type) {
    case TEEC_VALUE_OUTPUT:
    case TEEC_VALUE_INOUT:
        param->value.a = 1;
        param->value.b = 0;
        break;
    case TEEC_MEMREF_TEMP_OUTPUT:
    case TEEC_MEMREF_TEMP_INOUT:
        if (param->tmpref.buffer && param->tmpref.size >= 1) {
            unsigned char *p = param->tmpref.buffer;
            size_t n = param->tmpref.size;
            /* Little-endian: a leading 0x01 with the rest zero reads as 1 at
             * every width, which a memset(p, 1, n) would not do. */
            p[0] = 1;
            for (size_t i = 1; i < n; i++) p[i] = 0;
        }
        break;
    default:
        break;
    }
}

TEEC_Result TEEC_InvokeCommand(TEEC_Session *session, uint32_t commandID,
                               TEEC_Operation *operation, uint32_t *returnOrigin) {
    static int announced = 0;

    (void)session;
    if (returnOrigin) *returnOrigin = TEEC_ORIGIN_TRUSTED_APP;

    if (operation) {
        for (int i = 0; i < 4; i++)
            fill_output(&operation->params[i],
                        (operation->paramTypes >> (4 * i)) & 0xF);
        operation->started = 1;
    }

    /* One line, once — enough to confirm in journalctl that the attestation was
     * answered rather than skipped, without logging per call. */
    if (!announced) {
        announced = 1;
        printf("answered TEE attestation (command %u)\n", commandID);
        fflush(stdout);
    }
    if (debug_enabled())
        fprintf(stderr, "[teeshim] TEEC_InvokeCommand cmd=%u paramTypes=0x%x"
                        " -> SUCCESS\n",
                commandID, operation ? operation->paramTypes : 0);
    return TEEC_SUCCESS;
}