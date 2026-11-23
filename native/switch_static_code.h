#ifndef KYUJO2_SWITCH_STATIC_CODE_H
#define KYUJO2_SWITCH_STATIC_CODE_H
/* Kyujo2 ships its native code in the NRO. The bundled mod library has no
 * executable allocator on this platform. Report allocation failure to SLJIT. */
#define SLJIT_EXECUTABLE_ALLOCATOR 0
#define SLJIT_UTIL_STACK 0
#define SLJIT_MALLOC_EXEC(size, context) ((void*)0)
#define SLJIT_FREE_EXEC(pointer, context) ((void)0)
#endif
