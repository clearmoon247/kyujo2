#include <windows.h>
#include <stdlib.h>
int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command,int show) {
    FreeConsole();
    if (AttachConsole(strtoul(command,NULL,10))) return 1;
    return GetLastError()==ERROR_INVALID_HANDLE ? 0 : 2;
}
