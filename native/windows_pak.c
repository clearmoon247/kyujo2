/* Kyujo's accessory uses UTF-8 paths on Windows via the application's manifest. */
#include "platform.h"
void host_set_exit_hook(void (*callback)(void)) {
    /* host_gbpak_load also registers the flush with atexit. Kyujo2 returns from main. */
    (void)callback;
}
