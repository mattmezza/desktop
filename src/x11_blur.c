#include "x11.h"
#include <math.h>

static Picture
picture(X11 *x, Pixmap pixmap)
{
    XRenderPictureAttributes attributes = {.repeat = RepeatPad};
    return XRenderCreatePicture(x->d, pixmap, x->format, CPRepeat, &attributes);
}

void
x11_blur_clear(X11 *x, WallpaperBlur *b)
{
    if (b->source.picture) XRenderFreePicture(x->d, b->source.picture);
    for (size_t i = 0; i < sizeof b->downsample / sizeof *b->downsample; ++i)
        if (b->downsample[i].picture) XRenderFreePicture(x->d, b->downsample[i].picture);
    if (b->pass.picture) XRenderFreePicture(x->d, b->pass.picture);
    if (b->result.picture) XRenderFreePicture(x->d, b->result.picture);
    *b = (WallpaperBlur){0};
}

static void
resize(X11 *x, BlurImage *image, int w, int h)
{
    if (image->w == w && image->h == h) return;
    if (image->picture) XRenderFreePicture(x->d, image->picture);
    Pixmap pixmap = XCreatePixmap(x->d, x->root, (unsigned)w, (unsigned)h, (unsigned)x->depth);
    *image = (BlurImage){picture(x, pixmap), w, h};
    XFreePixmap(x->d, pixmap); // The picture retains the drawable.
}

static void
scale(X11 *x, BlurImage image, int w, int h)
{
    XTransform transform = {{{XDoubleToFixed(image.w / (double)w), 0, 0},
                             {0, XDoubleToFixed(image.h / (double)h), 0},
                             {0, 0, XDoubleToFixed(1)}}};
    XRenderSetPictureTransform(x->d, image.picture, &transform);
    XRenderSetPictureFilter(x->d, image.picture, FilterBilinear, NULL, 0);
}

static void
gaussian(X11 *x, BlurImage from, BlurImage to, double sigma, bool horizontal)
{
    enum { MAX_TAPS = 12 };
    int extent = (int)fmin(MAX_TAPS, ceil(3 * sigma)), width = 2 * extent + 1;
    double weights[2 * MAX_TAPS + 1], total = 0;
    for (int i = -extent; i <= extent; ++i) {
        double distance = i / sigma;
        total += weights[i + extent] = exp(-.5 * distance * distance);
    }
    XFixed params[2 + 2 * MAX_TAPS + 1] = {XDoubleToFixed(horizontal ? width : 1),
                                            XDoubleToFixed(horizontal ? 1 : width)};
    XFixed sum = 0;
    for (int i = 0; i < width; ++i) sum += params[i + 2] = XDoubleToFixed(weights[i] / total);
    params[extent + 2] += XDoubleToFixed(1) - sum;
    scale(x, from, to.w, to.h);
    XRenderSetPictureFilter(x->d, from.picture, FilterConvolution, params, width + 2);
    XRenderComposite(x->d, PictOpSrc, from.picture, None, to.picture, 0, 0, 0, 0,
                     0, 0, (unsigned)to.w, (unsigned)to.h);
}

static BlurImage
downsample(X11 *x, WallpaperBlur *b, int w, int h, size_t level)
{
    // Twofold bilinear steps average neighboring pixels before reducing them again.
    for (size_t i = b->sampled; i <= level; ++i) {
        BlurImage from = i ? b->downsample[i - 1] : b->source;
        BlurImage *to = &b->downsample[i];
        int divisor = 2 << i;
        int tw = (w + divisor - 1) / divisor, th = (h + divisor - 1) / divisor;
        resize(x, to, tw < from.w ? tw : from.w, th < from.h ? th : from.h);
        scale(x, from, to->w, to->h);
        XRenderComposite(x->d, PictOpSrc, from.picture, None, to->picture, 0, 0, 0, 0,
                         0, 0, (unsigned)to->w, (unsigned)to->h);
        b->sampled = i + 1;
    }
    return b->downsample[level];
}

bool
x11_blur_paint(X11 *x, Viewport *v, const Output *o, int sx, int sy, int w, int h)
{
    Pixmap pixmap = v->frame ? v->frame : v->still;
    if (!x->convolution || !pixmap) return false;
    WallpaperBlur *b = &v->blur;
    if (pixmap != b->drawable) {
        x11_blur_clear(x, b);
        b->drawable = pixmap;
        b->source = (BlurImage){picture(x, pixmap), v->frame ? v->upload.image->width : w,
                                                  v->frame ? v->upload.image->height : h};
    }
    // Blend the lightest blur into the native crop so the last step resolves to exact pixels.
    int radius = v->blur_radius < BLUR_MIN_RADIUS ? BLUR_MIN_RADIUS : v->blur_radius;
    if (radius != b->filtered_radius) {
        double sigma = radius / (double)BLUR_PRECISION;
        size_t level = 1;
        while (level + 1 < sizeof b->downsample / sizeof *b->downsample && sigma >= (double)(5 << level)) ++level;
        BlurImage from = downsample(x, b, w, h, level);
        resize(x, &b->pass, from.w, from.h);
        resize(x, &b->result, from.w, from.h);
        // Downsampling and bilinear reconstruction already contribute some blur.
        double sx_sigma = sigma * from.w / w, sy_sigma = sigma * from.h / h;
        gaussian(x, from, b->pass, sqrt(fmax(.25, sx_sigma * sx_sigma - .25)), true);
        gaussian(x, b->pass, b->result, sqrt(fmax(.25, sy_sigma * sy_sigma - .25)), false);
        scale(x, b->result, w, h);
        b->filtered_radius = radius;
    }
    Picture mask = None;
    if (v->blur_radius < BLUR_MIN_RADIUS) {
        XRenderColor alpha = {.alpha = (unsigned short)(65535 * v->blur_radius / BLUR_MIN_RADIUS)};
        mask = XRenderCreateSolidFill(x->d, &alpha);
    }
    XRenderComposite(x->d, mask ? PictOpOver : PictOpSrc, b->result.picture, mask, x->canvas_pic,
                     sx, sy, 0, 0, o->x, o->y, (unsigned)o->w, (unsigned)o->h);
    if (mask) XRenderFreePicture(x->d, mask);
    return true;
}
