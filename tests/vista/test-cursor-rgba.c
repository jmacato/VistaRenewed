#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char guchar;
typedef struct { int unused; } DisplayChangeListener;
typedef struct {
    struct { DisplayChangeListener dcl; void *drawing_area; } gfx;
} VirtualConsole;
typedef struct { int width, height, hot_x, hot_y; uint32_t data[6]; } QEMUCursor;
typedef struct { int stride; guchar pixels[48]; } GdkPixbuf;
typedef struct { int unused; } GdkCursor;
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define GDK_COLORSPACE_RGB 0
static bool realized;
static int cursor_updates;
static GdkPixbuf storage;
static GdkCursor cursor_object;
static const guchar expected[2][12] = {
    {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255},
    {0x12, 0x34, 0x56, 0x80, 0xab, 0xcd, 0xef, 0, 255, 255, 255, 255}
};
static bool gtk_widget_get_realized(void *w) { (void)w; return realized; }
static void *gtk_widget_get_display(void *w) { return w; }
static void *gtk_widget_get_window(void *w) { return w; }
static GdkPixbuf *gdk_pixbuf_new(int space, bool alpha, int bits, int w, int h)
{
    assert(space == GDK_COLORSPACE_RGB && alpha && bits == 8 && w == 3 && h == 2);
    memset(&storage, 0xcc, sizeof(storage));
    storage.stride = 24; /* Deliberately padded rows. */
    return &storage;
}
static guchar *gdk_pixbuf_get_pixels(GdkPixbuf *p) { return p->pixels; }
static int gdk_pixbuf_get_rowstride(GdkPixbuf *p) { return p->stride; }
static GdkCursor *gdk_cursor_new_from_pixbuf(void *display, GdkPixbuf *p, int x, int y)
{
    (void)display;
    assert(x == 2 && y == 1);
    for (int row = 0; row < 2; row++) {
        assert(memcmp(p->pixels + row * p->stride, expected[row], 12) == 0);
        for (int i = 12; i < p->stride; i++) assert(p->pixels[row * p->stride + i] == 0xcc);
    }
    return &cursor_object;
}
static void gdk_window_set_cursor(void *w, GdkCursor *c)
{ (void)w; assert(c == &cursor_object); cursor_updates++; }
static void g_object_unref(void *p) { assert(p == &storage || p == &cursor_object); }

/* SOURCE_UNDER_TEST */

int main(void)
{
    VirtualConsole vc = {0};
    QEMUCursor c = {3, 2, 2, 1,
        {0xffff0000, 0xff00ff00, 0xff0000ff, 0x80123456, 0x00abcdef, 0xffffffff}};
    QEMUCursor original = c;
    gd_cursor_define(&vc.gfx.dcl, &c);
    assert(cursor_updates == 0);
    realized = true;
    gd_cursor_define(&vc.gfx.dcl, &c);
    assert(cursor_updates == 1 && memcmp(&c, &original, sizeof(c)) == 0);
    puts("Cursor pixels, alpha, row stride, hotspot and ownership verified");
    return 0;
}
