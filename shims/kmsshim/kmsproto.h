/* kmsproto.h — what kmsshim sends kmspresent, one message per SOCK_SEQPACKET
 * packet. Both ends are built from this file, so there is no versioning. */
#ifndef KMSPROTO_H
#define KMSPROTO_H
#include <stdint.h>

enum {
    KMS_MODE = 1,  /* w, h: the size the app was told the screen is */
    KMS_BUF,       /* id, size, and the buffer's memfd as SCM_RIGHTS */
    KMS_FREEBUF,   /* id */
    KMS_FB,        /* id, buf, offset, w, h, pitch, format (DRM fourcc) */
    KMS_RMFB,      /* id */
    KMS_SHOW,      /* id: show this framebuffer; x1..y2 bound what changed */
};

struct kmsmsg {
    uint32_t type, id, buf, offset, w, h, pitch, format;
    int32_t x1, y1, x2, y2;
    uint64_t size;
};
#endif
