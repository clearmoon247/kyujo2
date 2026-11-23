#ifndef KYUJO2_RENDER_MEMORY_H
#define KYUJO2_RENDER_MEMORY_H
#include <stdint.h>
extern uint8_t *gRdpMem;
/* Guest words use host endianness. This selects the N64 byte within a word,
 * including when a texture starts in the middle of the word. */
#define GUEST_BYTE(p, index) (*(uint8_t *)((uintptr_t)((p) + (index)) ^ 3u))
#endif
