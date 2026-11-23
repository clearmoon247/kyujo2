#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    unsigned char *rgb;
    int w, h;
    unsigned fx, fy;
} HostCanvas;
void host_canvas_fill(HostCanvas *, int, int, int, int, unsigned, unsigned);
void host_text_draw(HostCanvas *, int, int, int, const char *, unsigned, int);
int host_text_width(int, const char *);
void menu_background(HostCanvas *);
void menu_cursor(HostCanvas *, int);
#ifdef __cplusplus
}
#endif
