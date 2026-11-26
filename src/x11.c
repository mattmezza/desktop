#include "x11.h"
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/scrnsaver.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>

static int x_error;
static int
handle_error(Display *d, XErrorEvent *e)
{
    (void)d;
    x_error = e->error_code;
    return 0;
}

static Atom
atom(const X11 *x, const char *name)
{
    return XInternAtom(x->d, name, False);
}

bool
x11_open(X11 *x, char *error)
{
    *x = (X11){0};
    x->d = XOpenDisplay(NULL);
    if (!x->d) {
        snprintf(error, ERROR_SIZE, "Could not open the X11 display.");
        return false;
    }
    Display *d = x->d;
    fcntl(ConnectionNumber(d), F_SETFD, FD_CLOEXEC);
    XSetErrorHandler(handle_error);
    x->screen = DefaultScreen(d);
    x->root = RootWindow(d, x->screen);
    x->depth = DefaultDepth(d, x->screen);
    x->visual = DefaultVisual(d, x->screen);
    int event, err, major = 0, minor = 0;
    if (!XRenderQueryExtension(d, &event, &err) || !XFixesQueryExtension(d, &event, &err) ||
        !(x->format = XRenderFindVisualFormat(d, x->visual))) {
        snprintf(error, ERROR_SIZE, "Desktop needs XRender and XFixes.");
        XCloseDisplay(d);
        x->d = NULL;
        return false;
    }
    x->randr = XRRQueryExtension(d, &x->randr_event, &err) &&
               XRRQueryVersion(d, &major, &minor) && (major > 1 || (major == 1 && minor >= 5));
    x->saver = XScreenSaverQueryExtension(d, &event, &err);
    x->dpms = DPMSQueryExtension(d, &event, &err) && DPMSCapable(d);
    x->shm = XShmQueryExtension(d) != False;
    x->rootmap = atom(x, "_XROOTPMAP_ID");
    x->esetroot = atom(x, "ESETROOT_PMAP_ID");
    x->clients = atom(x, "_NET_CLIENT_LIST");
    x->type = atom(x, "_NET_WM_WINDOW_TYPE");
    x->dock = atom(x, "_NET_WM_WINDOW_TYPE_DOCK");
    x->desktop = atom(x, "_NET_WM_WINDOW_TYPE_DESKTOP");
    return true;
}

static void
drop_upload(X11 *x)
{
    if (x->upload) {
        if (x->attached) {
            XShmDetach(x->d, &x->segment);
            x->upload->data = NULL;
            shmdt(x->segment.shmaddr);
        }
        XDestroyImage(x->upload);
    }
    x->attached = false;
    x->upload = NULL;
}

void
x11_close(X11 *x)
{
    if (!x->d) return;
    drop_upload(x);
    if (x11_owns_background(x)) {
        XDeleteProperty(x->d, x->root, x->rootmap);
        XDeleteProperty(x->d, x->root, x->esetroot);
    }
    XCloseDisplay(x->d);
    x->d = NULL;
}

bool
x11_layout(X11 *x, Layout *layout)
{
    *layout = (Layout){0};
    XWindowAttributes a;
    if (!XGetWindowAttributes(x->d, x->root, &a) || a.width < 1 || a.height < 1)
        return false;
    layout->w = a.width;
    layout->h = a.height;
    if (x->randr) {
        int n = 0;
        XRRMonitorInfo *monitors = XRRGetMonitors(x->d, x->root, True, &n);
        for (int i = 0; monitors && i < n && layout->count < MAX_OUTPUTS; ++i) {
            XRRMonitorInfo *m = &monitors[i];
            if (m->width < 1 || m->height < 1) continue;
            Output *o = &layout->items[layout->count++];
            o->x = m->x;
            o->y = m->y;
            o->w = m->width;
            o->h = m->height;
            char *name = XGetAtomName(x->d, m->name);
            snprintf(o->name, sizeof o->name, "%s", name ? name : "Display");
            if (name) XFree(name);
        }
        if (monitors) XRRFreeMonitors(monitors);
    }
    if (!layout->count) {
        layout->count = 1;
        layout->items[0] = (Output){.w = a.width, .h = a.height, .name = "Screen"};
    }
    return true;
}

void
x11_window_class(Display *d, Window w, const char *instance, const char *class_name)
{
    XClassHint hint = {(char *)instance, (char *)class_name};
    XSetClassHint(d, w, &hint);
}

static unsigned long
xcolor(const X11 *x, const char *value)
{
    XColor c;
    return XParseColor(x->d, DefaultColormap(x->d, x->screen), value, &c) &&
           XAllocColor(x->d, DefaultColormap(x->d, x->screen), &c)
               ? c.pixel : BlackPixel(x->d, x->screen);
}

static void
fill(X11 *x, Drawable target, const Config *cfg)
{
    XSetForeground(x->d, x->gc, xcolor(x, cfg->background));
    XFillRectangle(x->d, target, x->gc, 0, 0, (unsigned)x->layout.w, (unsigned)x->layout.h);
}

bool
x11_scene(X11 *x, const Layout *layout, const Config *cfg)
{
    if (layout->w > 32767 || layout->h > 32767) return false;
    x->layout = *layout;
    if (x->canvas && x->canvas_w == layout->w && x->canvas_h == layout->h) return true;
    Display *d = x->d;
    if (!x->gc) {
        XGCValues values = {.graphics_exposures = False};
        x->gc = XCreateGC(d, x->root, GCGraphicsExposures, &values);
    }
    if (!x->live) {
        XSetWindowAttributes wa = {.override_redirect = True, .event_mask = ExposureMask,
                                   .background_pixmap = None};
        x->live = XCreateWindow(d, x->root, 0, 0, (unsigned)layout->w, (unsigned)layout->h, 0,
                                x->depth, InputOutput, x->visual,
                                CWOverrideRedirect | CWEventMask | CWBackPixmap, &wa);
        x11_window_class(d, x->live, "desktop-background", "DesktopBackground");
        XStoreName(d, x->live, "Desktop wallpaper");
        XChangeProperty(d, x->live, x->type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&x->desktop, 1);
        XserverRegion region = XFixesCreateRegion(d, NULL, 0);
        XFixesSetWindowShapeRegion(d, x->live, ShapeInput, 0, 0, region);
        XFixesDestroyRegion(d, region);
        XSelectInput(d, x->root, StructureNotifyMask | PropertyChangeMask | SubstructureNotifyMask);
        if (x->randr)
            XRRSelectInput(d, x->root, RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
    } else XResizeWindow(d, x->live, (unsigned)layout->w, (unsigned)layout->h);
    if (x->canvas_pic) XRenderFreePicture(d, x->canvas_pic);
    if (x->canvas) XFreePixmap(d, x->canvas);
    if (x->former_pic) XRenderFreePicture(d, x->former_pic);
    if (x->former) XFreePixmap(d, x->former);
    x->former = None;
    x->former_pic = None;
    x->canvas_w = layout->w;
    x->canvas_h = layout->h;
    x->canvas = XCreatePixmap(d, x->root, (unsigned)layout->w, (unsigned)layout->h, (unsigned)x->depth);
    x->canvas_pic = XRenderCreatePicture(d, x->canvas, x->format, 0, NULL);
    fill(x, x->canvas, cfg);
    return true;
}

static bool
setup_upload(X11 *x, int w, int h)
{
    drop_upload(x);
    if (x->shm) {
        XShmSegmentInfo *s = &x->segment;
        x->upload = XShmCreateImage(x->d, x->visual, (unsigned)x->depth, ZPixmap, NULL, s,
                                    (unsigned)w, (unsigned)h);
        if (x->upload) {
            s->shmid = shmget(IPC_PRIVATE, (size_t)x->upload->bytes_per_line * h, IPC_CREAT | 0600);
            if (s->shmid >= 0) {
                s->shmaddr = shmat(s->shmid, NULL, 0);
                if (s->shmaddr != (void *)-1) {
                    x->upload->data = s->shmaddr;
                    s->readOnly = True;
                    XSync(x->d, False);
                    x_error = 0;
                    x->attached = XShmAttach(x->d, s) != False;
                    XSync(x->d, False);
                    x->attached = x->attached && !x_error;
                    if (!x->attached) {
                        x->upload->data = NULL;
                        shmdt(s->shmaddr);
                    }
                }
                shmctl(s->shmid, IPC_RMID, NULL);
            }
        }
        if (x->attached) return true;
        drop_upload(x);
        x->shm = false;
    }
    x->upload = XCreateImage(x->d, x->visual, (unsigned)x->depth, ZPixmap, 0, NULL,
                             (unsigned)w, (unsigned)h, 32, 0);
    if (!x->upload) return false;
    x->upload->data = malloc((size_t)x->upload->bytes_per_line * h);
    if (!x->upload->data) { drop_upload(x); return false; }
    return true;
}

static unsigned long
channel(unsigned c, unsigned long mask)
{
    if (!mask) return 0;
    unsigned shift = (unsigned)__builtin_ctzl(mask);
    return ((c * (mask >> shift) + 127) / 255) << shift;
}

bool
x11_upload(X11 *x, const Image *image, const Config *cfg)
{
    int w = image->w, h = image->h;
    if (w > 32767 || h > 32767) return false;
    if (!x->upload || x->upload->width != w || x->upload->height != h)
        if (!setup_upload(x, w, h)) return false;
    XImage *dst = x->upload;
    unsigned bg = (unsigned)strtoul(cfg->background + (*cfg->background == '#'), NULL, 16);
    bool native = dst->bits_per_pixel == 32 && dst->byte_order == LSBFirst &&
                  dst->red_mask == 0xff0000 && dst->green_mask == 0xff00 && dst->blue_mask == 0xff;
    for (int y = 0; y < h; ++y) {
        unsigned char *line = (unsigned char *)dst->data + (size_t)y * dst->bytes_per_line;
        const unsigned char *src = image->rgba + (size_t)y * w * 4;
        for (int i = 0; i < w; ++i, src += 4) {
            unsigned r = src[0], g = src[1], b = src[2], a = src[3];
            if (a < 255) {
                r = (r * a + ((bg >> 16) & 255) * (255 - a)) / 255;
                g = (g * a + ((bg >> 8) & 255) * (255 - a)) / 255;
                b = (b * a + (bg & 255) * (255 - a)) / 255;
            }
            if (native) {
                line[i * 4] = (unsigned char)b;
                line[i * 4 + 1] = (unsigned char)g;
                line[i * 4 + 2] = (unsigned char)r;
                line[i * 4 + 3] = 0;
            } else XPutPixel(dst, i, y, channel(r, dst->red_mask) | channel(g, dst->green_mask) | channel(b, dst->blue_mask));
        }
    }
    Display *d = x->d;
    if (!x->source || x->source_w != w || x->source_h != h) {
        if (x->source_pic) XRenderFreePicture(d, x->source_pic);
        if (x->source) XFreePixmap(d, x->source);
        x->source = XCreatePixmap(d, x->root, (unsigned)w, (unsigned)h, (unsigned)x->depth);
        XRenderPictureAttributes pa = {.repeat = RepeatPad};
        x->source_pic = XRenderCreatePicture(d, x->source, x->format, CPRepeat, &pa);
        XRenderSetPictureFilter(d, x->source_pic, FilterBilinear, NULL, 0);
        x->source_w = w;
        x->source_h = h;
    }
    if (x->attached) {
        XShmPutImage(d, x->source, x->gc, dst, 0, 0, 0, 0, (unsigned)w, (unsigned)h, False);
        XSync(d, False);
    } else XPutImage(d, x->source, x->gc, dst, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    return true;
}

void
x11_capture(X11 *x)
{
    if (!x->former) {
        x->former = XCreatePixmap(x->d, x->root, (unsigned)x->layout.w, (unsigned)x->layout.h, (unsigned)x->depth);
        x->former_pic = XRenderCreatePicture(x->d, x->former, x->format, 0, NULL);
    }
    XCopyArea(x->d, x->canvas, x->former, x->gc, 0, 0, (unsigned)x->layout.w, (unsigned)x->layout.h, 0, 0);
}

void
x11_paint(X11 *x, const Config *cfg, const Media *media, const double *dx,
          const double *dy, double fade, bool live)
{
    if (!x->source) return;
    const Layout *l = &x->layout;
    fill(x, x->canvas, cfg);
    for (size_t i = 0; i < l->count; ++i) {
        const Output *o = &l->items[i];
        double left, top, w, h;
        media_crop(cfg, media, o->w, o->h, &left, &top, &w, &h);
        double sx = x->source_w / (media->w * media->sar), sy = x->source_h / (double)media->h;
        double step_x = w * sx / o->w, step_y = h * sy / o->h;
        XTransform transform = {{ {XDoubleToFixed(step_x), 0, XDoubleToFixed(left * sx + dx[i] * step_x)},
                                  {0, XDoubleToFixed(step_y), XDoubleToFixed(top * sy + dy[i] * step_y)},
                                  {0, 0, XDoubleToFixed(1)} }};
        XRenderSetPictureTransform(x->d, x->source_pic, &transform);
        XRenderComposite(x->d, PictOpSrc, x->source_pic, None, x->canvas_pic, 0, 0, 0, 0,
                         o->x, o->y, (unsigned)o->w, (unsigned)o->h);
    }
    if (fade > 0 && x->former_pic) {
        XRenderColor color = {.alpha = (unsigned short)(fade * 65535)};
        Picture alpha = XRenderCreateSolidFill(x->d, &color);
        XRenderComposite(x->d, PictOpOver, x->former_pic, alpha, x->canvas_pic, 0, 0, 0, 0, 0, 0,
                         (unsigned)l->w, (unsigned)l->h);
        XRenderFreePicture(x->d, alpha);
    }
    if (live) {
        if (!x->live_mapped) {
            XSetWindowBackgroundPixmap(x->d, x->live, x->canvas);
            XLowerWindow(x->d, x->live);
            XMapWindow(x->d, x->live);
            x->live_mapped = true;
        }
        XCopyArea(x->d, x->canvas, x->live, x->gc, 0, 0, (unsigned)l->w, (unsigned)l->h, 0, 0);
    } else if (x->live_mapped) {
        XUnmapWindow(x->d, x->live);
        x->live_mapped = false;
    }
    XFlush(x->d);
}

void
x11_publish(X11 *x)
{
    const Layout *l = &x->layout;
    Display *d = x->d;
    Pixmap snapshot = XCreatePixmap(d, x->root, (unsigned)l->w, (unsigned)l->h, (unsigned)x->depth);
    XCopyArea(d, x->canvas, snapshot, x->gc, 0, 0, (unsigned)l->w, (unsigned)l->h, 0, 0);
    XSetWindowBackgroundPixmap(d, x->root, snapshot);
    XChangeProperty(d, x->root, x->rootmap, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&snapshot, 1);
    XChangeProperty(d, x->root, x->esetroot, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&snapshot, 1);
    XClearWindow(d, x->root);
    if (x->snapshot) XFreePixmap(d, x->snapshot);
    x->snapshot = snapshot;
    XSync(d, False);
}

void
x11_pointer(X11 *x, int *px, int *py)
{
    Window root, child;
    int wx, wy;
    unsigned mask;
    if (!XQueryPointer(x->d, x->root, &root, &child, px, py, &wx, &wy, &mask))
        *px = *py = -1;
}

static bool
window_list(X11 *x, Window win, Atom prop, Atom kind, unsigned long limit,
            unsigned long **values, unsigned long *count)
{
    Atom actual;
    int format;
    unsigned long extra;
    unsigned char *result = NULL;
    bool ok = XGetWindowProperty(x->d, win, prop, 0, (long)limit, False, kind, &actual, &format,
                                count, &extra, &result) == Success && actual == kind && format == 32;
    if (!ok && result) { XFree(result); result = NULL; }
    *values = (unsigned long *)result;
    return ok;
}

bool
x11_owns_background(X11 *x)
{
    unsigned long *values, count;
    if (!x->snapshot || !window_list(x, x->root, x->rootmap, XA_PIXMAP, 1, &values, &count))
        return false;
    bool owns = count == 1 && values[0] == x->snapshot;
    XFree(values);
    return owns;
}

static bool
window_visible(X11 *x, Window w)
{
    XWindowAttributes wa;
    if (!XGetWindowAttributes(x->d, w, &wa) || wa.map_state != IsViewable ||
        wa.class == InputOnly || wa.override_redirect)
        return false;
    Window child;
    int px, py;
    if (!XTranslateCoordinates(x->d, w, x->root, 0, 0, &px, &py, &child)) return false;
    bool touches = false;
    for (size_t i = 0; i < x->layout.count; ++i) {
        const Output *o = &x->layout.items[i];
        if (px < o->x + o->w && py < o->y + o->h && px + wa.width > o->x && py + wa.height > o->y)
            touches = true;
    }
    if (!touches) return false;
    unsigned long *types, n;
    if (!window_list(x, w, x->type, XA_ATOM, 16, &types, &n)) return true;
    bool app = true;
    for (unsigned long i = 0; i < n; ++i)
        if (types[i] == x->dock || types[i] == x->desktop) app = false;
    XFree(types);
    return app;
}

bool
x11_occupied(X11 *x)
{
    unsigned long *windows, count;
    if (window_list(x, x->root, x->clients, XA_WINDOW, 8192, &windows, &count)) {
        bool occupied = false;
        for (unsigned long i = 0; i < count && !occupied; ++i)
            occupied = window_visible(x, windows[i]);
        XFree(windows);
        return occupied;
    }
    Window root, parent, *children = NULL;
    unsigned n;
    if (!XQueryTree(x->d, x->root, &root, &parent, &children, &n)) return false;
    bool occupied = false;
    for (unsigned i = 0; i < n && !occupied; ++i)
        occupied = window_visible(x, children[i]);
    if (children) XFree(children);
    return occupied;
}

bool
x11_blanked(X11 *x)
{
    CARD16 power;
    BOOL enabled;
    if (x->dpms && DPMSInfo(x->d, &power, &enabled) && enabled && power != DPMSModeOn)
        return true;
    XScreenSaverInfo info;
    return x->saver && XScreenSaverQueryInfo(x->d, x->root, &info) && info.state == ScreenSaverOn;
}

bool
x11_layout_event(const X11 *x, const XEvent *ev)
{
    if (ev->type == ConfigureNotify && ev->xconfigure.window == x->root) return true;
    return x->randr && (ev->type == x->randr_event + RRScreenChangeNotify || ev->type == x->randr_event + RRNotify);
}
