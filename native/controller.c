#include "guest_calls.h"
#include <stdio.h>

#ifdef KYUJO2_SHARED_MENU
int kyujo2_pak_present(int);
int kyujo2_pak_read(int,unsigned,unsigned char *);
int kyujo2_pak_write(int,unsigned,const unsigned char *);
void osPfsIsPlug_recomp(uint8_t *rdram, recomp_context *ctx) {
    unsigned mask = 0;
    for (int port = 0; port < 4; ++port)
        if (kyujo2_pak_present(port)) mask |= 1u << port;
    MEM_B(0, ctx->r5) = mask;
    ctx->r2 = 0;
}
void __osContRamRead_recomp(uint8_t *rdram, recomp_context *ctx) {
    unsigned char bytes[32];
    int port = (int)ctx->r5;
    if (!kyujo2_pak_read(port, ((unsigned)ctx->r6 & 0x7ff) << 5, bytes)) { ctx->r2 = 1; return; }
    for (int i = 0; i < 32; ++i) MEM_B(i, ctx->r7) = bytes[i];
    ctx->r2 = 0;
}
void __osContRamWrite_recomp(uint8_t *rdram, recomp_context *ctx) {
    unsigned char bytes[32];
    int port = (int)ctx->r5;
    for (int i = 0; i < 32; ++i) bytes[i] = MEM_BU(i, ctx->r7);
    ctx->r2 = kyujo2_pak_write(port, ((unsigned)ctx->r6 & 0x7ff) << 5, bytes) ? 0 : 1;
}
void __osPfsGetStatus_recomp(uint8_t *rdram, recomp_context *ctx) {
    ctx->r2 = kyujo2_pak_present((int)ctx->r5) ? 0 : 1;
}
#else
/* The desktop configuration exposes a controller with an empty expansion slot. */
void osPfsIsPlug_recomp(uint8_t *rdram, recomp_context *ctx) {
    MEM_B(0, ctx->r5) = 0;
    ctx->r2 = 0;
}
void __osContRamRead_recomp(uint8_t *rdram, recomp_context *ctx) { ctx->r2 = 1; }
void __osContRamWrite_recomp(uint8_t *rdram, recomp_context *ctx) { ctx->r2 = 1; }
void __osPfsGetStatus_recomp(uint8_t *rdram, recomp_context *ctx) { ctx->r2 = 1; }

#endif

void set_watch_lohi(uint8_t *rdram, recomp_context *ctx) {
    /* Main clears both guest hardware watch registers. */
    if (ctx->r4 != 0) {
        fprintf(stderr, "Guest requested an unsupported watchpoint at %08x\n", (unsigned)ctx->r4);
        abort();
    }
}
