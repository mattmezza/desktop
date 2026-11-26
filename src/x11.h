#ifndef DESKTOP_X11_H
#define DESKTOP_X11_H
#include "desktop.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/XShm.h>

typedef struct {
    XImage *image;
    XShmSegmentInfo segment;
    bool attached;
} Upload;

typedef struct {
    Picture picture;
    int w, h;
} BlurImage;

typedef struct {
    Pixmap drawable;
    BlurImage source, downsample[5], pass, result;
    size_t sampled;
    int filtered_radius;
} WallpaperBlur;

enum { BLUR_PRECISION = 32, BLUR_MIN_RADIUS = 4 * BLUR_PRECISION };

typedef struct {
    Pixmap still, frame;
    Picture frame_pic;
    Upload upload;
    WallpaperBlur blur;
    int dx, dy, blur_radius;
} Viewport;

typedef struct {
    Display *d;
    int screen, depth, randr_event;
    bool randr, saver, dpms, shm, convolution;
    Window root, live;
    Atom rootmap, esetroot, clients, type, dock, desktop;
    Visual *visual;
    XRenderPictFormat *format;
    GC gc;
    Layout layout;
    Pixmap canvas, snapshot, former, source;
    Picture canvas_pic, former_pic, source_pic;
    Upload upload;
    int source_w, source_h, canvas_w, canvas_h;
    Viewport viewports[MAX_OUTPUTS];
    uint32_t video_updates;
    bool live_mapped, crops_valid, overlaps;
} X11;

bool x11_open(X11 *, char *);
void x11_close(X11 *);
bool x11_layout(X11 *, Layout *);
bool x11_scene(X11 *, const Layout *, const Config *);
bool x11_upload(X11 *, const Image *, const Config *);
bool x11_video_frame(X11 *, size_t, const Image *, const Config *);
void x11_clear_video(X11 *);
void x11_capture(X11 *);
void x11_paint(X11 *, const Config *, const Media *, const double *, const double *, const double *, double, bool);
void x11_publish(X11 *);
bool x11_owns_background(X11 *);
void x11_pointer(X11 *, int *, int *);
uint32_t x11_occupied(X11 *);
bool x11_blanked(X11 *);
bool x11_layout_event(const X11 *, const XEvent *);
void x11_window_class(Display *, Window, const char *, const char *);
void x11_blur_clear(X11 *, WallpaperBlur *);
bool x11_blur_paint(X11 *, Viewport *, const Output *, int, int, int, int);
#endif
