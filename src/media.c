#include "desktop.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <jpeglib.h>
#include <math.h>
#include <png.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <webp/decode.h>
#include <dirent.h>

extern char **environ;

static bool
dimensions(int w, int h)
{
    return w > 0 && h > 0 && (size_t)w * (size_t)h <= MAX_PIXELS;
}

static bool
image_alloc(Image *out, int w, int h)
{
    if (!dimensions(w, h)) return false;
    unsigned char *data = malloc((size_t)w * h * 4);
    if (!data) return false;
    *out = (Image){w, h, data};
    return true;
}

void
image_free(Image *image)
{
    free(image->rgba);
    *image = (Image){0};
}

void
media_free(Media *m)
{
    free(m->path);
    free(m->package);
    image_free(&m->thumb);
    *m = (Media){0};
}

bool
media_copy(Media *result, const Media *source)
{
    Media m = *source;
    m.thumb = (Image){0};
    m.path = strdup(source->path);
    m.package = source->package ? malloc(sizeof *m.package) : NULL;
    if (!m.path || (source->package && !m.package)) { media_free(&m); return false; }
    if (m.package) *m.package = *source->package;
    *result = m;
    return true;
}

const char *
media_file(const Media *m)
{
    return m->package ? m->package->file : m->path;
}

const char *
media_title(const Media *m)
{
    return m->package && *m->package->title ? m->package->title : filename(m->path);
}

const char *
media_kind(const Media *m)
{
    return m->type == MEDIA_VIDEO ? "video" : "image";
}

double
media_speed(const Media *m)
{
    return m->package ? m->package->speed : 1;
}

bool
media_matches(const Media *m, MediaType type, const char *query)
{
    if (type != MEDIA_ANY && m->type != type) return false;
    const MediaPackage *p = m->package;
    const char *fields[] = {media_title(m), filename(media_file(m)), filename(m->path), media_kind(m),
                            p ? p->tags : "", p ? p->description : "", p ? p->author : "", p ? p->source : "",
                            p ? p->license : ""};
    while (*query) {
        while (isspace((unsigned char)*query)) ++query;
        size_t n = strcspn(query, " \t\r\n");
        if (!n) break;
        char word[256];
        if (n >= sizeof word) return false;
        memcpy(word, query, n);
        word[n] = '\0';
        bool found = false;
        for (size_t i = 0; i < sizeof fields / sizeof *fields && !found; ++i)
            found = strcasestr(fields[i], word) != NULL;
        if (!found) return false;
        query += n;
    }
    return true;
}

static bool
is_video(const char *path)
{
    static const char *const extensions[] = {
        ".mp4", ".m4v", ".mov", ".mkv", ".webm", ".avi", ".mpg",
        ".mpeg", ".ts", ".m2ts", ".ogv", ".wmv", ".gif", ".apng"
    };
    const char *ext = strrchr(filename(path), '.');
    if (!ext) return false;
    for (size_t i = 0; i < sizeof extensions / sizeof *extensions; ++i)
        if (!strcasecmp(ext, extensions[i])) return true;
    return false;
}

static FILE *
media_open(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    FILE *f = fdopen(fd, "rb");
    if (!f) close(fd);
    return f;
}

static int
spawn_output(const char *program, const char *fallback, char *const argv[], pid_t *pid)
{
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) return -1;
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (!error) {
        error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (!error) error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        if (!error) error = posix_spawnp(pid, program, &actions, NULL, argv, environ);
        if (fallback && error == ENOENT) error = posix_spawnp(pid, fallback, &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(pipefd[1]);
    if (error) { close(pipefd[0]); errno = error; return -1; }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    return pipefd[0];
}

void
child_stop(pid_t pid)
{
    if (pid <= 0 || waitpid(pid, NULL, WNOHANG) != 0) return;
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

static size_t
read_until(int fd, void *buf, size_t count, int timeout)
{
    size_t used = 0;
    uint64_t expiry = now_ms() + (unsigned)timeout;
    while (used < count) {
        uint64_t now = now_ms();
        if (now >= expiry) break;
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, (int)(expiry - now)) < 1) {
            if (errno == EINTR) continue;
            break;
        }
        ssize_t got = read(fd, (char *)buf + used, count - used);
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (got <= 0) break;
        used += (size_t)got;
    }
    return used;
}

static bool
probe_video(const Config *cfg, const char *path, Media *m, char *error)
{
    char *const args[] = {(char *)cfg->ffprobe, "-v", "error", "-select_streams", "V:0",
        "-show_entries", "stream=width,height,sample_aspect_ratio:stream_side_data=rotation",
        "-of", "default=noprint_wrappers=1", (char *)path, NULL};
    pid_t pid;
    const char *fallback = !strcmp(cfg->ffprobe, "desktop-ffprobe") ? "ffprobe" : NULL;
    int fd = spawn_output(cfg->ffprobe, fallback, args, &pid);
    if (fd < 0) {
        snprintf(error, ERROR_SIZE, "Video probing needs %.300s%s.", cfg->ffprobe,
                 fallback ? " or ffprobe" : "");
        return false;
    }
    char data[2048];
    data[read_until(fd, data, sizeof data - 1, 5000)] = '\0';
    close(fd);
    child_stop(pid);
    int rotation = 0, num = 0, den = 0;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (sscanf(line, "width=%d", &m->w) == 1) continue;
        if (sscanf(line, "height=%d", &m->h) == 1) continue;
        if (sscanf(line, "sample_aspect_ratio=%d:%d", &num, &den) == 2) continue;
        (void)sscanf(line, "rotation=%d", &rotation);
    }
    m->sar = num > 0 && den > 0 ? (double)num / den : 1;
    if (rotation % 180 == 90 || rotation % 180 == -90) {
        int width = m->w;
        m->w = m->h;
        m->h = width;
        m->sar = 1 / m->sar;
    }
    if (dimensions(m->w, m->h) && m->sar >= 0.05 && m->sar <= 20) return true;
    snprintf(error, ERROR_SIZE, "Unreadable or oversized video.");
    return false;
}

static unsigned
read16(const unsigned char *p, bool little)
{
    return little ? (unsigned)p[0] | ((unsigned)p[1] << 8) :
                    ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static uint32_t
read32(const unsigned char *p, bool little)
{
    return little ? (uint32_t)read16(p, true) | ((uint32_t)read16(p + 2, true) << 16) :
                    ((uint32_t)read16(p, false) << 16) | (uint32_t)read16(p + 2, false);
}

static int
exif_orientation(jpeg_saved_marker_ptr marker)
{
    for (; marker; marker = marker->next) {
        if (marker->data_length < 14 || memcmp(marker->data, "Exif\0\0", 6)) continue;
        const unsigned char *p = marker->data + 6;
        size_t length = marker->data_length - 6;
        bool little = !memcmp(p, "II", 2);
        if ((!little && memcmp(p, "MM", 2)) || read16(p + 2, little) != 42) continue;
        uint32_t thumb = read32(p + 4, little);
        if (thumb > length - 2) continue;
        unsigned entries = read16(p + thumb, little);
        for (size_t offset = (size_t)thumb + 2; entries && offset + 12 <= length; --entries, offset += 12)
            if (read16(p + offset, little) == 0x0112 && read16(p + offset + 2, little) == 3) {
                unsigned n = read16(p + offset + 8, little);
                if (n >= 1 && n <= 8) return (int)n;
            }
    }
    return 1;
}

typedef struct { struct jpeg_error_mgr base; jmp_buf recovery; } JpegError;
static void
jpeg_failure(j_common_ptr jpeg)
{
    JpegError *error = (JpegError *)jpeg->err;
    longjmp(error->recovery, 1);
}

static bool
jpeg_read(FILE *file, int max, Media *m, Image *out)
{
    struct jpeg_decompress_struct *jpeg = calloc(1, sizeof *jpeg);
    if (!jpeg) return false;
    JpegError error;
    jpeg->err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_failure;
    if (setjmp(error.recovery)) {
        jpeg_destroy_decompress(jpeg);
        free(jpeg);
        if (out) image_free(out);
        return false;
    }
    jpeg_create_decompress(jpeg);
    jpeg_stdio_src(jpeg, file);
    jpeg_save_markers(jpeg, JPEG_APP0 + 1, 0xffff);
    jpeg_read_header(jpeg, TRUE);
    int orientation = exif_orientation(jpeg->marker_list);
    bool transpose = orientation >= 5;
    m->w = transpose ? (int)jpeg->image_height : (int)jpeg->image_width;
    m->h = transpose ? (int)jpeg->image_width : (int)jpeg->image_height;
    bool ok = dimensions(m->w, m->h);
    if (ok && out) {
        if (max) {
            int longest = m->w > m->h ? m->w : m->h;
            while (jpeg->scale_denom < 8 && longest / (int)jpeg->scale_denom >= 2 * max)
                jpeg->scale_denom *= 2;
        }
        jpeg->out_color_space = JCS_EXT_RGBA;
        jpeg_start_decompress(jpeg);
        int w = (int)jpeg->output_width, h = (int)jpeg->output_height;
        ok = image_alloc(out, transpose ? h : w, transpose ? w : h);
        if (ok) {
            JSAMPARRAY row = (*jpeg->mem->alloc_sarray)((j_common_ptr)jpeg, JPOOL_IMAGE, (JDIMENSION)w * 4, 1);
            for (int y = 0; y < h; ++y) {
                jpeg_read_scanlines(jpeg, row, 1);
                if (orientation == 1) {
                    memcpy(out->rgba + (size_t)y * w * 4, row[0], (size_t)w * 4);
                    continue;
                }
                for (int x = 0; x < w; ++x) {
                    int px = x, py = y;
                    switch (orientation) {
                    case 2: px = w - 1 - x; break;
                    case 3: px = w - 1 - x; py = h - 1 - y; break;
                    case 4: py = h - 1 - y; break;
                    case 5: px = y; py = x; break;
                    case 6: px = h - 1 - y; py = x; break;
                    case 7: px = h - 1 - y; py = w - 1 - x; break;
                    case 8: px = y; py = w - 1 - x; break;
                    }
                    memcpy(out->rgba + ((size_t)py * out->w + px) * 4, row[0] + x * 4, 4);
                }
            }
            jpeg_finish_decompress(jpeg);
        }
    }
    jpeg_destroy_decompress(jpeg);
    free(jpeg);
    return ok;
}

static bool
png_read(FILE *file, Media *m, Image *out)
{
    png_image image = {.version = PNG_IMAGE_VERSION};
    if (!png_image_begin_read_from_stdio(&image, file)) return false;
    m->w = (int)image.width;
    m->h = (int)image.height;
    bool ok = dimensions(m->w, m->h);
    image.format = PNG_FORMAT_RGBA;
    if (ok && out) {
        ok = image_alloc(out, m->w, m->h) &&
             png_image_finish_read(&image, NULL, out->rgba, 0, NULL) != 0;
        if (!ok) image_free(out);
    }
    png_image_free(&image);
    return ok;
}

static bool
webp_read(FILE *file, Media *m, Image *out)
{
    unsigned char header[64];
    size_t n = fread(header, 1, sizeof header, file);
    if (!WebPGetInfo(header, n, &m->w, &m->h) || !dimensions(m->w, m->h)) return false;
    if (!out) return true;
    struct stat st;
    if (fstat(fileno(file), &st) || st.st_size <= 0 || st.st_size > (off_t)MAX_PIXELS * 4) return false;
    unsigned char *buffer = malloc((size_t)st.st_size);
    if (!buffer) return false;
    rewind(file);
    bool ok = fread(buffer, 1, (size_t)st.st_size, file) == (size_t)st.st_size &&
              image_alloc(out, m->w, m->h) &&
              WebPDecodeRGBAInto(buffer, (size_t)st.st_size, out->rgba,
                                 (size_t)m->w * m->h * 4, m->w * 4);
    free(buffer);
    if (!ok) image_free(out);
    return ok;
}

static bool
read_picture(const char *path, int max, Media *m, Image *out)
{
    FILE *f = media_open(path);
    if (!f) return false;
    unsigned char header[16] = {0};
    size_t n = fread(header, 1, sizeof header, f);
    rewind(f);
    bool ok = false;
    if (n >= 3 && header[0] == 0xff && header[1] == 0xd8)
        ok = jpeg_read(f, max, m, out);
    else if (n >= 8 && !png_sig_cmp(header, 0, 8))
        ok = png_read(f, m, out);
    else if (n >= 12 && !memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WEBP", 4))
        ok = webp_read(f, m, out);
    fclose(f);
    return ok;
}

bool
media_probe(const Config *cfg, const char *path, Media *m, char *error)
{
    *m = (Media){.sar = 1};
    struct stat st;
    if (stat(path, &st) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
        snprintf(error, ERROR_SIZE, "Could not open a wallpaper file or package.");
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!package_read(path, m, error)) return false;
    } else m->type = is_video(path) ? MEDIA_VIDEO : MEDIA_IMAGE;
    const char *file = m->package ? m->package->file : path;
    bool video = m->type == MEDIA_VIDEO;
    bool ok = video ? probe_video(cfg, file, m, error) : read_picture(file, 0, m, NULL);
    if (!ok) {
        if (!video) snprintf(error, ERROR_SIZE, "Unreadable or unsupported image.");
        media_free(m);
        return false;
    }
    m->path = strdup(path);
    m->modified = st.st_mtim;
    if (m->path) return true;
    media_free(m);
    snprintf(error, ERROR_SIZE, "Out of memory.");
    return false;
}

static void
fit_dimensions(double w, int h, int limit, int *width, int *height)
{
    double scale = fmin(1, limit / fmax(w, h));
    *width = (int)fmax(1, round(w * scale));
    *height = (int)fmax(1, round(h * scale));
}

static int
video_output(const Config *cfg, const Media *m, int w, int h, const char *crop, pid_t *pid)
{
    char filter[240];
    if (crop && media_speed(m) != 1)
        snprintf(filter, sizeof filter, "settb=AVTB,setpts=(PTS-STARTPTS)/%.8f,fps=%d,%s,scale=%d:%d:flags=bilinear,setsar=1",
                 media_speed(m), cfg->video_fps, crop, w, h);
    else if (crop)
        snprintf(filter, sizeof filter, "fps=%d,%s,scale=%d:%d:flags=bilinear,setsar=1", cfg->video_fps, crop, w, h);
    else snprintf(filter, sizeof filter, "scale=%d:%d:flags=bilinear,setsar=1", w, h);
    char *args[48];
    size_t a = 0;
#define ARG(x) args[a++] = (char *)(x)
    ARG(cfg->ffmpeg); ARG("-nostdin"); ARG("-v"); ARG("error");
    ARG("-threads"); ARG("2"); ARG("-filter_threads"); ARG("1");
    if (crop) { ARG("-stream_loop"); ARG("-1"); }
    ARG("-i"); ARG(media_file(m)); ARG("-map"); ARG("0:V:0"); ARG("-an"); ARG("-sn"); ARG("-dn");
    ARG("-vf"); ARG(filter);
    if (!crop) { ARG("-frames:v"); ARG("1"); }
    ARG("-pix_fmt"); ARG("rgba"); ARG("-c:v"); ARG("rawvideo"); ARG("-threads"); ARG("1");
    ARG("-f"); ARG("rawvideo"); ARG("pipe:1"); ARG(NULL);
#undef ARG
    return spawn_output(cfg->ffmpeg, !strcmp(cfg->ffmpeg, "desktop-ffmpeg") ? "ffmpeg" : NULL, args, pid);
}

int
video_spawn(const Config *cfg, const Media *m, const Output *o, Image *frame, pid_t *pid)
{
    double left, top, width, height;
    media_crop(cfg, m, o->w, o->h, &left, &top, &width, &height);
    int x = (int)fmin(m->w - 1, fmax(0, round(left / m->sar)));
    int y = (int)fmin(m->h - 1, fmax(0, round(top)));
    int right = (int)fmax(x + 1, fmin(m->w, round((left + width) / m->sar)));
    int bottom = (int)fmax(y + 1, fmin(m->h, round(top + height)));
    char crop[80];
    snprintf(crop, sizeof crop, "crop=%d:%d:%d:%d:exact=1", right - x, bottom - y, x, y);
    int w, h;
    fit_dimensions(o->w, o->h, cfg->video_max, &w, &h);
    if (!image_alloc(frame, w, h)) return -1;
    int fd = video_output(cfg, m, w, h, crop, pid);
    if (fd < 0) image_free(frame);
    return fd;
}

static bool
shrink(Image *image, int limit)
{
    int w, h;
    fit_dimensions(image->w, image->h, limit, &w, &h);
    if (w == image->w && h == image->h) return true;
    Image reduced = {0};
    if (!image_alloc(&reduced, w, h)) return false;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t a = ((size_t)(y * (double)image->h / h) * image->w +
                        (size_t)(x * (double)image->w / w)) * 4;
            memcpy(reduced.rgba + ((size_t)y * w + x) * 4, image->rgba + a, 4);
        }
    }
    image_free(image);
    *image = reduced;
    return true;
}

bool
media_decode(const Config *cfg, const Media *m, int max, Image *out, char *error)
{
    bool ok = false;
    *out = (Image){0};
    if (m->type == MEDIA_VIDEO) {
        int w, h;
        fit_dimensions(m->w * m->sar, m->h, max ? max : cfg->video_max, &w, &h);
        if (image_alloc(out, w, h)) {
            pid_t pid;
            int fd = video_output(cfg, m, w, h, NULL, &pid);
            if (fd >= 0) {
                size_t bytes = (size_t)w * h * 4;
                ok = read_until(fd, out->rgba, bytes, 8000) == bytes;
                close(fd);
                child_stop(pid);
            }
        }
    } else {
        Media ignored = {0};
        ok = read_picture(media_file(m), max, &ignored, out) && (!max || shrink(out, max));
    }
    if (ok) return true;
    image_free(out);
    snprintf(error, ERROR_SIZE, "Could not decode %.240s%s.", filename(media_file(m)),
             m->type == MEDIA_VIDEO ? " with FFmpeg" : "");
    return false;
}

bool
media_thumbnail(const Config *cfg, const Media *m, Image *out, char *error)
{
    *out = (Image){0};
    Media ignored = {0};
    if (m->package && *m->package->thumbnail && read_picture(m->package->thumbnail, 336, &ignored, out) &&
        shrink(out, 336)) return true;
    image_free(out);
    return media_decode(cfg, m, 336, out, error);
}

static bool
scan(const Config *cfg, const char *dir, Library *lib, size_t *capacity, unsigned depth, char *error)
{
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(error, ERROR_SIZE, "Cannot open %.260s (%s).", dir, strerror(errno));
        return false;
    }
    struct dirent *entry;
    bool ok = true;
    while (ok && (entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        char path[PATH_MAX];
        int n = snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
        if (n < 0 || n >= (int)sizeof path) continue;
        struct stat st;
        if (lstat(path, &st)) continue;
        bool linked = S_ISLNK(st.st_mode);
        if (linked && stat(path, &st)) continue;
        bool package = S_ISDIR(st.st_mode) && package_exists(path);
        if (S_ISDIR(st.st_mode) && !package) {
            if (!linked && cfg->recursive && depth < 64) ok = scan(cfg, path, lib, capacity, depth + 1, error);
            continue;
        }
        if (!package && !S_ISREG(st.st_mode)) continue;
        bool video = !package && is_video(path);
        if (!package && ((video && !cfg->videos) || (!video && !cfg->images))) continue;
        if (!package && !video) {
            const char *ext = strrchr(entry->d_name, '.');
            if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg") &&
                         strcasecmp(ext, ".png") && strcasecmp(ext, ".webp"))) continue;
        }
        Media m;
        char resolved[PATH_MAX];
        const char *reference = linked && realpath(path, resolved) ? resolved : path;
        if (!media_probe(cfg, reference, &m, error)) {
            if (package) continue;
            m = (Media){.path = strdup(reference), .type = video ? MEDIA_VIDEO : MEDIA_IMAGE,
                        .failed = true, .modified = st.st_mtim};
        }
        if (!m.path) { ok = false; break; }
        if ((m.type == MEDIA_VIDEO && !cfg->videos) || (m.type == MEDIA_IMAGE && !cfg->images)) {
            media_free(&m);
            continue;
        }
        if (lib->count == *capacity) {
            size_t next = *capacity ? *capacity * 2 : 64;
            Media *items = realloc(lib->items, next * sizeof *items);
            if (!items) { media_free(&m); ok = false; break; }
            lib->items = items;
            *capacity = next;
        }
        lib->items[lib->count++] = m;
    }
    closedir(d);
    return ok;
}

static int
media_compare(const void *a, const void *b)
{
    const Media *ma = a, *mb = b;
    int name = strcasecmp(media_title(ma), media_title(mb));
    return name ? name : strcmp(ma->path, mb->path);
}

bool
library_read(const Config *cfg, Library *lib, char *error)
{
    *lib = (Library){0};
    char directory[PATH_MAX];
    if (!realpath(cfg->directory, directory)) {
        snprintf(error, ERROR_SIZE, "Cannot open %.260s (%s).", cfg->directory, strerror(errno));
        return false;
    }
    size_t capacity = 0;
    if (!scan(cfg, directory, lib, &capacity, 0, error)) {
        library_free(lib);
        return false;
    }
    if (lib->count > 1) qsort(lib->items, lib->count, sizeof *lib->items, media_compare);
    return true;
}

void
library_free(Library *lib)
{
    for (size_t i = 0; i < lib->count; ++i) media_free(&lib->items[i]);
    free(lib->items);
    *lib = (Library){0};
}

bool
media_eligible(const Config *cfg, const Layout *layout, const Media *m, char *reason)
{
    if (m->failed || !m->w || !m->h) {
        if (reason) snprintf(reason, ERROR_SIZE, "%s is unreadable.", m->type == MEDIA_VIDEO ? "Video" : "Image");
        return false;
    }
    int margin = m->type == MEDIA_IMAGE && cfg->parallax ? cfg->travel * 2 : 0;
    for (size_t i = 0; i < layout->count; ++i) {
        const Output *o = &layout->items[i];
        if (m->w * m->sar < o->w + margin || m->h < o->h + margin) {
            if (reason) snprintf(reason, ERROR_SIZE, "Too small for %s (%d × %d%s).",
                                 o->name, o->w, o->h, margin ? " plus parallax" : "");
            return false;
        }
    }
    return true;
}

void
media_crop(const Config *cfg, const Media *m, int w, int h,
           double *x, double *y, double *width, double *height)
{
    double source_w = m->w * m->sar, source_h = m->h;
    int travel = m->type == MEDIA_IMAGE && cfg->parallax ? cfg->travel : 0;
    double scale = fmax((w + travel * 2) / source_w, (h + travel * 2) / source_h);
    *width = w / scale;
    *height = h / scale;
    *x = (source_w - *width - 2 * travel / scale) * cfg->focal_x / 100 + travel / scale;
    *y = (source_h - *height - 2 * travel / scale) * cfg->focal_y / 100 + travel / scale;
}

bool
hook_run(const char *cmd, const Media *m, bool wait)
{
    if (!*cmd) return true;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (!pid) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        setenv("DESKTOP_WALLPAPER", media_file(m), 1);
        setenv("DESKTOP_MEDIA", media_kind(m), 1);
        if (m->package) setenv("DESKTOP_PACKAGE", m->path, 1);
        else unsetenv("DESKTOP_PACKAGE");
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (!wait) return true;
    int status;
    pid_t result;
    do { result = waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    return result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
