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
drop_upload(Display *d, Upload *upload)
{
    if (upload->image) {
        if (upload->attached) {
            XShmDetach(d, &upload->segment);
            upload->image->data = NULL;
            shmdt(upload->segment.shmaddr);
        }
        XDestroyImage(upload->image);
    }
    *upload = (Upload){0};
}

void
x11_clear_video(X11 *x)
{
    if (!x->d) return;
    for (size_t i = 0; i < MAX_OUTPUTS; ++i) {
        Viewport *v = &x->viewports[i];
        if (v->frame) x11_blur_clear(x, &v->blur);
        if (v->frame_pic) XRenderFreePicture(x->d, v->frame_pic);
        if (v->frame) XFreePixmap(x->d, v->frame);
        drop_upload(x->d, &v->upload);
        v->frame_pic = None;
        v->frame = None;
    }
    x->video_updates = 0;
}

void
x11_close(X11 *x)
{
    if (!x->d) return;
    x11_clear_video(x);
    for (size_t i = 0; i < MAX_OUTPUTS; ++i) x11_blur_clear(x, &x->viewports[i].blur);
    drop_upload(x->d, &x->upload);
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

static bool
intersects(const Output *o, int x, int y, int w, int h)
{
    return x < o->x + o->w && y < o->y + o->h && x + w > o->x && y + h > o->y;
}

bool
x11_scene(X11 *x, const Layout *layout, const Config *cfg)
{
    if (layout->w > 32767 || layout->h > 32767) return false;
    x->layout = *layout;
    x->crops_valid = x->overlaps = false;
    for (size_t i = 0; i < layout->count; ++i) {
        const Output *o = &layout->items[i];
        for (size_t j = 0; j < i; ++j)
            if (intersects(&layout->items[j], o->x, o->y, o->w, o->h)) x->overlaps = true;
    }
    if (x->canvas && x->canvas_w == layout->w && x->canvas_h == layout->h) return true;
    Display *d = x->d;
    if (!x->gc) {
        XGCValues values = {.graphics_exposures = False};
        x->gc = XCreateGC(d, x->root, GCGraphicsExposures, &values);
    }
    if (!x->live) {
        XSetWindowAttributes wa = {.override_redirect = True, .event_mask = ExposureMask,
                                   .background_pixmap = None, .backing_store = Always};
        x->live = XCreateWindow(d, x->root, 0, 0, (unsigned)layout->w, (unsigned)layout->h, 0,
                                x->depth, InputOutput, x->visual,
                                CWOverrideRedirect | CWEventMask | CWBackPixmap | CWBackingStore, &wa);
        x11_window_class(d, x->live, "desktop-background", "DesktopBackground");
        XStoreName(d, x->live, "Desktop wallpaper");
        XChangeProperty(d, x->live, x->type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&x->desktop, 1);
        unsigned long bypass = 2;
        XChangeProperty(d, x->live, atom(x, "_NET_WM_BYPASS_COMPOSITOR"), XA_CARDINAL, 32,
                        PropModeReplace, (unsigned char *)&bypass, 1);
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
setup_upload(X11 *x, Upload *upload, int w, int h)
{
    Display *d = x->d;
    drop_upload(d, upload);
    if (x->shm) {
        XShmSegmentInfo *s = &upload->segment;
        upload->image = XShmCreateImage(d, x->visual, (unsigned)x->depth, ZPixmap, NULL, s,
                                         (unsigned)w, (unsigned)h);
        if (upload->image) {
            s->shmid = shmget(IPC_PRIVATE, (size_t)upload->image->bytes_per_line * h, IPC_CREAT | 0600);
            if (s->shmid >= 0) {
                s->shmaddr = shmat(s->shmid, NULL, 0);
                if (s->shmaddr != (void *)-1) {
                    upload->image->data = s->shmaddr;
                    s->readOnly = True;
                    XSync(d, False);
                    x_error = 0;
                    upload->attached = XShmAttach(d, s) != False;
                    XSync(d, False);
                    upload->attached = upload->attached && !x_error;
                    if (!upload->attached) {
                        upload->image->data = NULL;
                        shmdt(s->shmaddr);
                    }
                }
                shmctl(s->shmid, IPC_RMID, NULL);
            }
        }
        if (upload->attached) return true;
        drop_upload(d, upload);
        x->shm = false;
    }
    upload->image = XCreateImage(d, x->visual, (unsigned)x->depth, ZPixmap, 0, NULL,
                             (unsigned)w, (unsigned)h, 32, 0);
    if (!upload->image) return false;
    upload->image->data = malloc((size_t)upload->image->bytes_per_line * h);
    if (!upload->image->data) { drop_upload(d, upload); return false; }
    return true;
}

static unsigned long
channel(unsigned c, unsigned long mask)
{
    if (!mask) return 0;
    unsigned shift = (unsigned)__builtin_ctzl(mask);
    return ((c * (mask >> shift) + 127) / 255) << shift;
}

static void
upload_pixels(X11 *x, Upload *upload, Pixmap target, const Image *image, const Config *cfg)
{
    int w = image->w, h = image->h;
    XImage *dst = upload->image;
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
    if (upload->attached) {
        XShmPutImage(x->d, target, x->gc, dst, 0, 0, 0, 0, (unsigned)w, (unsigned)h, False);
        XSync(x->d, False);
    } else XPutImage(x->d, target, x->gc, dst, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
}

bool
x11_upload(X11 *x, const Image *image, const Config *cfg)
{
    int w = image->w, h = image->h;
    if (w > 32767 || h > 32767) return false;
    if (!x->upload.image || x->upload.image->width != w || x->upload.image->height != h)
        if (!setup_upload(x, &x->upload, w, h)) return false;
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
    upload_pixels(x, &x->upload, x->source, image, cfg);
    x->crops_valid = false;
    return true;
}

bool
x11_video_frame(X11 *x, size_t i, const Image *image, const Config *cfg)
{
    Display *d = x->d;
    Viewport *v = &x->viewports[i];
    const Output *o = &x->layout.items[i];
    int w = image->w, h = image->h;
    if (!v->frame) {
        if (!setup_upload(x, &v->upload, w, h)) return false;
        v->frame = XCreatePixmap(d, x->root, (unsigned)w, (unsigned)h, (unsigned)x->depth);
        if (w != o->w || h != o->h) {
            XRenderPictureAttributes pa = {.repeat = RepeatPad};
            v->frame_pic = XRenderCreatePicture(d, v->frame, x->format, CPRepeat, &pa);
            XTransform transform = {{{XDoubleToFixed(w / (double)o->w), 0, 0},
                                     {0, XDoubleToFixed(h / (double)o->h), 0},
                                     {0, 0, XDoubleToFixed(1)}}};
            XRenderSetPictureTransform(d, v->frame_pic, &transform);
            XRenderSetPictureFilter(d, v->frame_pic, FilterBilinear, NULL, 0);
        }
    }
    upload_pixels(x, &v->upload, v->frame, image, cfg);
    v->blur.sampled = 0;
    v->blur.cached = 0;
    x->video_updates |= UINT32_C(1) << i;
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

static void
render_crop(X11 *x, Picture destination, const Config *cfg, const Media *media,
            const Output *o, int x_offset, int y_offset, int padding)
{
    double left, top, w, h;
    media_crop(cfg, media, o->w, o->h, &left, &top, &w, &h);
    double sx = x->source_w / (media->w * media->sar), sy = x->source_h / (double)media->h;
    double step_x = w * sx / o->w, step_y = h * sy / o->h;
    XTransform transform = {{ {XDoubleToFixed(step_x), 0, XDoubleToFixed(left * sx - padding * step_x)},
                              {0, XDoubleToFixed(step_y), XDoubleToFixed(top * sy - padding * step_y)},
                              {0, 0, XDoubleToFixed(1)} }};
    XRenderSetPictureTransform(x->d, x->source_pic, &transform);
    XRenderComposite(x->d, PictOpSrc, x->source_pic, None, destination, 0, 0, 0, 0,
                     x_offset, y_offset, (unsigned)(o->w + 2 * padding), (unsigned)(o->h + 2 * padding));
}

static void
prepare_crops(X11 *x, const Config *cfg, const Media *media, int padding)
{
    bool blur = cfg->blur && cfg->blur_radius;
    for (size_t i = 0; i < MAX_OUTPUTS; ++i) {
        Viewport *v = &x->viewports[i];
        x11_blur_clear(x, &v->blur);
        if (v->still) XFreePixmap(x->d, v->still);
        v->still = None;
        v->dx = v->dy = 0;
        if ((!padding && !blur) || i >= x->layout.count) continue;
        const Output *o = &x->layout.items[i];
        v->still = XCreatePixmap(x->d, x->root, (unsigned)(o->w + 2 * padding),
                                (unsigned)(o->h + 2 * padding), (unsigned)x->depth);
        Picture target = XRenderCreatePicture(x->d, v->still, x->format, 0, NULL);
        render_crop(x, target, cfg, media, o, 0, 0, padding);
        XRenderFreePicture(x->d, target);
        if (blur && !media->video)
            x11_blur_prepare(x, v, o->w + 2 * padding, o->h + 2 * padding, cfg->blur_radius);
    }
    x->crops_valid = true;
}

static int
pixel_offset(double target, int previous)
{
    // Let the target cross a wider threshold before changing the displayed pixel.
    return fabs(target - previous) >= .75 ? (int)lround(target) : previous;
}

void
x11_paint(X11 *x, const Config *cfg, const Media *media, const double *dx,
          const double *dy, const double *blur_radius, double fade, bool full)
{
    if (!x->source) return;
    Display *d = x->d;
    const Layout *l = &x->layout;
    int padding = !media->video && cfg->parallax ? cfg->travel : 0;
    if (!x->crops_valid) {
        prepare_crops(x, cfg, media, padding);
        full = true;
    }
    if (full) fill(x, x->canvas, cfg);
    int left = l->w, top = l->h, right = 0, bottom = 0;
    uint32_t updates = x->video_updates;
    x->video_updates = 0;
    for (size_t i = 0; i < l->count; ++i) {
        const Output *o = &l->items[i];
        Viewport *v = &x->viewports[i];
        int radius = (int)lround(blur_radius[i] * BLUR_PRECISION);
        if (v->blur_radius != radius) updates |= UINT32_C(1) << i;
        v->blur_radius = radius;
        if (v->still) {
            int next_x = pixel_offset(dx[i], v->dx), next_y = pixel_offset(dy[i], v->dy);
            if (next_x != v->dx || next_y != v->dy) updates |= UINT32_C(1) << i;
            v->dx = next_x;
            v->dy = next_y;
        }
        if (updates & (UINT32_C(1) << i)) {
            if (o->x < left) left = o->x;
            if (o->y < top) top = o->y;
            if (o->x + o->w > right) right = o->x + o->w;
            if (o->y + o->h > bottom) bottom = o->y + o->h;
        }
    }
    if (!full && (left >= right || top >= bottom)) return;
    for (size_t i = 0; i < l->count; ++i) {
        const Output *o = &l->items[i];
        Viewport *v = &x->viewports[i];
        if (!full && !x->overlaps && !(updates & (UINT32_C(1) << i))) continue;
        int sx = padding + v->dx, sy = padding + v->dy;
        int w = o->w + 2 * padding, h = o->h + 2 * padding;
        if (v->blur_radius >= BLUR_MIN_RADIUS) {
            x11_blur_paint(x, v, o, sx, sy, w, h, cfg->blur_radius);
            continue;
        }
        // Sample each still once at output resolution so fine details stay stable during motion.
        if (v->frame_pic) XRenderComposite(d, PictOpSrc, v->frame_pic, None, x->canvas_pic, 0, 0, 0, 0,
                                           o->x, o->y, (unsigned)o->w, (unsigned)o->h);
        else if (v->frame) XCopyArea(d, v->frame, x->canvas, x->gc, 0, 0,
                                    (unsigned)o->w, (unsigned)o->h, o->x, o->y);
        else if (v->still) XCopyArea(d, v->still, x->canvas, x->gc, sx, sy,
                                    (unsigned)o->w, (unsigned)o->h, o->x, o->y);
        else render_crop(x, x->canvas_pic, cfg, media, o, o->x, o->y, 0);
        if (v->blur_radius > 0) x11_blur_paint(x, v, o, sx, sy, w, h, cfg->blur_radius);
    }
    if (fade > 0 && x->former_pic) {
        XRenderColor color = {.alpha = (unsigned short)(fade * 65535)};
        Picture alpha = XRenderCreateSolidFill(x->d, &color);
        XRenderComposite(x->d, PictOpOver, x->former_pic, alpha, x->canvas_pic, 0, 0, 0, 0, 0, 0,
                         (unsigned)l->w, (unsigned)l->h);
        XRenderFreePicture(x->d, alpha);
    }
    if (!x->live_mapped) {
        XLowerWindow(d, x->live);
        XMapWindow(d, x->live);
        x->live_mapped = true;
    }
    if (full) { left = top = 0; right = l->w; bottom = l->h; }
    XCopyArea(d, x->canvas, x->live, x->gc, left, top, (unsigned)(right - left),
              (unsigned)(bottom - top), left, top);
    XFlush(d);
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

static uint32_t
window_outputs(X11 *x, Window w)
{
    XWindowAttributes wa;
    if (!XGetWindowAttributes(x->d, w, &wa) || wa.map_state != IsViewable ||
        wa.class == InputOnly || wa.override_redirect)
        return 0;
    Window child;
    int px, py;
    if (!XTranslateCoordinates(x->d, w, x->root, 0, 0, &px, &py, &child)) return 0;
    uint32_t outputs = 0;
    for (size_t i = 0; i < x->layout.count; ++i)
        if (intersects(&x->layout.items[i], px, py, wa.width, wa.height)) outputs |= UINT32_C(1) << i;
    if (!outputs) return 0;
    unsigned long *types, n;
    if (window_list(x, w, x->type, XA_ATOM, 16, &types, &n)) {
        for (unsigned long i = 0; i < n; ++i)
            if (types[i] == x->dock || types[i] == x->desktop) outputs = 0;
        XFree(types);
    }
    return outputs;
}

uint32_t
x11_occupied(X11 *x)
{
    uint32_t occupied = 0;
    uint32_t all = (uint32_t)((UINT64_C(1) << x->layout.count) - 1);
    unsigned long *windows, count;
    if (window_list(x, x->root, x->clients, XA_WINDOW, 8192, &windows, &count)) {
        for (unsigned long i = 0; i < count && occupied != all; ++i)
            occupied |= window_outputs(x, windows[i]);
        XFree(windows);
        return occupied;
    }
    Window root, parent, *children = NULL;
    unsigned n;
    if (!XQueryTree(x->d, x->root, &root, &parent, &children, &n)) return false;
    for (unsigned i = 0; i < n && occupied != all; ++i)
        occupied |= window_outputs(x, children[i]);
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
