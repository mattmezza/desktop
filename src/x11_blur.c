#include "x11.h"
#include <math.h>

static const int radii[BLUR_LEVELS] = {4, 6, 9, 13, 18, 26, 37, 52, 74, 96};

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
    if (b->mix.picture) XRenderFreePicture(x->d, b->mix.picture);
    for (size_t i = 0; i < BLUR_LEVELS; ++i)
        if (b->levels[i].picture) XRenderFreePicture(x->d, b->levels[i].picture);
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
composite(X11 *x, BlurImage from, BlurImage to, Picture mask)
{
    XRenderComposite(x->d, mask ? PictOpOver : PictOpSrc, from.picture, mask, to.picture, 0, 0, 0, 0,
                     0, 0, (unsigned)to.w, (unsigned)to.h);
}

static Picture
opacity(X11 *x, double fraction)
{
    XRenderColor color = {.alpha = (unsigned short)lround(65535 * fraction)};
    return XRenderCreateSolidFill(x->d, &color);
}

static void
gaussian(X11 *x, BlurImage from, BlurImage to, double sigma, bool horizontal)
{
    int extent = (int)fmin(12, ceil(3 * sigma));
    XFixed unit = XDoubleToFixed(1);
    XTransform transform = {{{unit, 0, 0}, {0, unit, 0}, {0, 0, unit}}};
    XRenderSetPictureFilter(x->d, from.picture, FilterBilinear, NULL, 0);
    XRenderSetPictureTransform(x->d, from.picture, &transform);
    composite(x, from, to, None);
    double total = 1, exponent = -.5 / (sigma * sigma);
    for (int i = 1; i <= extent; i += 2) {
        double a = exp(exponent * i * i);
        double b = i < extent ? exp(exponent * (i + 1) * (i + 1)) : 0;
        double weight = a + b;
        // Sample two neighboring taps at their weighted position using one bilinear lookup.
        double distance = i + b / weight;
        for (int direction = -1; direction <= 1; direction += 2) {
            total += weight;
            Picture mask = opacity(x, weight / total);
            transform.matrix[horizontal ? 0 : 1][2] = XDoubleToFixed(direction * distance);
            XRenderSetPictureTransform(x->d, from.picture, &transform);
            composite(x, from, to, mask);
            XRenderFreePicture(x->d, mask);
        }
    }
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
        composite(x, from, *to, None);
        b->sampled = i + 1;
    }
    return b->downsample[level];
}

static WallpaperBlur *
blur_source(X11 *x, Viewport *v, int w, int h)
{
    Pixmap pixmap = v->frame ? v->frame : v->still;
    WallpaperBlur *b = &v->blur;
    if (pixmap != b->drawable) {
        x11_blur_clear(x, b);
        b->drawable = pixmap;
        b->source = (BlurImage){picture(x, pixmap), v->frame ? v->upload.image->width : w,
                                                  v->frame ? v->upload.image->height : h};
    }
    return b;
}

static BlurImage
blur_level(X11 *x, WallpaperBlur *b, int w, int h, size_t index, double sigma)
{
    BlurImage *output = &b->levels[index];
    if (b->cached & (1u << index)) return *output;
    size_t level = 1;
    while (level + 1 < sizeof b->downsample / sizeof *b->downsample && sigma >= (double)(5 << level)) ++level;
    BlurImage from = downsample(x, b, w, h, level);
    // Store all levels at the same resolution so blending across a level boundary is continuous.
    resize(x, output, b->downsample[1].w, b->downsample[1].h);
    resize(x, &b->pass, from.w, from.h);
    BlurImage target = *output;
    if (level > 1) {
        resize(x, &b->result, from.w, from.h);
        target = b->result;
    }
    // Downsampling and bilinear reconstruction already contribute some blur.
    double sx_sigma = sigma * from.w / w, sy_sigma = sigma * from.h / h;
    gaussian(x, from, b->pass, sqrt(fmax(.25, sx_sigma * sx_sigma - .25)), true);
    gaussian(x, b->pass, target, sqrt(fmax(.25, sy_sigma * sy_sigma - .25)), false);
    if (level > 1) {
        scale(x, target, output->w, output->h);
        composite(x, target, *output, None);
    }
    b->cached |= 1u << index;
    return *output;
}

void
x11_blur_prepare(X11 *x, Viewport *v, int w, int h, int maximum)
{
    WallpaperBlur *b = blur_source(x, v, w, h);
    int limit = maximum < radii[0] ? radii[0] : maximum;
    for (size_t i = 0; i < BLUR_LEVELS; ++i) {
        blur_level(x, b, w, h, i, fmin(radii[i], limit));
        if (radii[i] >= limit) break;
    }
}

void
x11_blur_paint(X11 *x, Viewport *v, const Output *o, int sx, int sy, int w, int h, int maximum)
{
    WallpaperBlur *b = blur_source(x, v, w, h);
    double radius = v->blur_radius / (double)BLUR_PRECISION;
    int limit = maximum < radii[0] ? radii[0] : maximum;
    size_t upper = 0;
    while (upper + 1 < BLUR_LEVELS && radius > radii[upper]) ++upper;
    double high = fmin(radii[upper], limit);
    BlurImage output = blur_level(x, b, w, h, upper, high);
    if (upper && radius < high) {
        double low = radii[upper - 1];
        BlurImage lighter = blur_level(x, b, w, h, upper - 1, low);
        resize(x, &b->mix, output.w, output.h);
        scale(x, lighter, output.w, output.h);
        scale(x, output, output.w, output.h);
        Picture mask = opacity(x, (radius * radius - low * low) / (high * high - low * low));
        composite(x, lighter, b->mix, None);
        composite(x, output, b->mix, mask);
        XRenderFreePicture(x->d, mask);
        output = b->mix;
    }
    scale(x, output, w, h);
    // Blend the lightest blur into the native crop so the last step resolves to exact pixels.
    Picture mask = radius < radii[0] ? opacity(x, radius / radii[0]) : None;
    XRenderComposite(x->d, mask ? PictOpOver : PictOpSrc, output.picture, mask, x->canvas_pic,
                     sx, sy, 0, 0, o->x, o->y, (unsigned)o->w, (unsigned)o->h);
    if (mask) XRenderFreePicture(x->d, mask);
}
