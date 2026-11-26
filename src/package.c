#include "desktop.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <webp/encode.h>

#define DESCRIPTOR "wallpaper.ini"
#define PREVIEW_SIZE 336
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

static bool
join(char out[PATH_MAX], const char *directory, const char *relative)
{
    int n = snprintf(out, PATH_MAX, "%s/%s", directory, relative);
    return n >= 0 && n < PATH_MAX;
}

bool
package_exists(const char *directory)
{
    char path[PATH_MAX];
    struct stat st;
    return join(path, directory, DESCRIPTOR) && !lstat(path, &st);
}

static bool
asset_path(char out[PATH_MAX], const char *directory, const char *relative)
{
    char path[PATH_MAX];
    struct stat st;
    size_t base = strlen(directory);
    return *relative && *relative != '/' && join(path, directory, relative) && realpath(path, out) &&
           !strncmp(out, directory, base) && out[base] == '/' && !stat(out, &st) && S_ISREG(st.st_mode);
}

typedef struct {
    char version[8], type[32], speed[32];
    char image[PATH_MAX], video[PATH_MAX], thumbnail[PATH_MAX];
} Descriptor;

static bool
read_descriptor(FILE *in, Descriptor *d, MediaPackage *p)
{
    struct Field { const char *section, *key; char *value; size_t size; };
#define FIELD(section, name, object, field) {section, name, object->field, sizeof object->field}
    const struct Field fields[] = {
        FIELD("wallpaper", "version", d, version), FIELD("wallpaper", "type", d, type),
        FIELD("wallpaper", "thumbnail", d, thumbnail), FIELD("wallpaper", "title", p, title),
        FIELD("wallpaper", "description", p, description), FIELD("wallpaper", "tags", p, tags),
        FIELD("wallpaper", "author", p, author), FIELD("wallpaper", "source", p, source),
        FIELD("wallpaper", "license", p, license), FIELD("image", "file", d, image),
        FIELD("video", "file", d, video), FIELD("video", "speed", d, speed)
    };
#undef FIELD
    char line[PATH_MAX + 64], section[64] = "";
    while (fgets(line, sizeof line, in)) {
        if (!strchr(line, '\n') && !feof(in)) return false;
        char *s = ini_trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *end = strrchr(s, ']');
            if (!end || *ini_trim(end + 1)) return false;
            *end = '\0';
            s = ini_trim(s + 1);
            if (strlen(s) >= sizeof section) return false;
            strcpy(section, s);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) return false;
        *eq = '\0';
        const char *key = ini_trim(s), *value = ini_trim(eq + 1);
        for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i) {
            const struct Field *field = &fields[i];
            if (strcmp(section, field->section) || strcmp(key, field->key)) continue;
            if (strlen(value) >= field->size) return false;
            strcpy(field->value, value);
            break;
        }
    }
    return !ferror(in);
}

bool
package_read(const char *path, Media *m, char *error)
{
    char directory[PATH_MAX], descriptor[PATH_MAX];
    if (!realpath(path, directory) || !join(descriptor, directory, DESCRIPTOR)) goto invalid;
    int fd = open(descriptor, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) goto invalid;
    struct stat st;
    FILE *in = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size <= 64 * 1024 ? fdopen(fd, "r") : NULL;
    if (!in) { close(fd); goto invalid; }
    Descriptor d = {0};
    MediaPackage *p = calloc(1, sizeof *p);
    bool parsed = p && read_descriptor(in, &d, p);
    fclose(in);
    if (!parsed || strcmp(d.version, "1")) { free(p); goto invalid; }
    if (!strcmp(d.type, "image")) m->type = MEDIA_IMAGE;
    else if (!strcmp(d.type, "video")) m->type = MEDIA_VIDEO;
    else {
        snprintf(error, ERROR_SIZE, "Unsupported wallpaper type in %.255s.", descriptor);
        free(p);
        return false;
    }
    const char *relative = m->type == MEDIA_VIDEO ? d.video : d.image;
    if (!asset_path(p->file, directory, relative)) {
        snprintf(error, ERROR_SIZE, "Wallpaper media is missing or outside %.250s.", directory);
        free(p);
        return false;
    }
    if (!asset_path(p->thumbnail, directory, d.thumbnail)) p->thumbnail[0] = '\0';
    p->speed = 1;
    if (m->type == MEDIA_VIDEO && *d.speed) {
        char *end;
        p->speed = strtod(d.speed, &end);
        if (*end || !isfinite(p->speed) || p->speed < .05 || p->speed > 4) {
            snprintf(error, ERROR_SIZE, "Video speed must be between 0.05 and 4 in %.255s.", descriptor);
            free(p);
            return false;
        }
    }
    m->package = p;
    return true;
invalid:
    snprintf(error, ERROR_SIZE, "Missing or invalid version 1 wallpaper.ini in %.270s.", path);
    return false;
}

static bool
copy_file(const char *source, const char *destination)
{
    FILE *in = fopen(source, "rb");
    if (!in) return false;
    FILE *out = fopen(destination, "wbx");
    bool ok = out != NULL;
    if (out) {
        unsigned char bytes[64 * 1024];
        size_t n;
        while ((n = fread(bytes, 1, sizeof bytes, in)) != 0)
            if (fwrite(bytes, 1, n, out) != n) { ok = false; break; }
        if (ferror(in) || fflush(out) || fsync(fileno(out))) ok = false;
        if (fclose(out)) ok = false;
    }
    fclose(in);
    return ok;
}

bool
package_create(const Config *cfg, const char *source, const char *destination, char *error)
{
    char resolved[PATH_MAX], final[PATH_MAX], stage[PATH_MAX];
    if (!realpath(source, resolved) || strlen(destination) >= sizeof final || !*destination) goto failed;
    strcpy(final, destination);
    size_t n = strlen(final);
    while (n > 1 && final[n - 1] == '/') final[--n] = '\0';
    const char *name = filename(resolved), *leaf = filename(final);
    if (!*leaf || !*name || !strcmp(leaf, ".") || !strcmp(leaf, "..") || strpbrk(name, "\r\n") ||
        isspace((unsigned char)name[0]) || isspace((unsigned char)name[strlen(name) - 1])) {
        errno = EINVAL;
        goto failed;
    }
    struct stat st;
    if (!lstat(final, &st)) { errno = EEXIST; goto failed; }
    if (errno != ENOENT || !make_parents(final)) goto failed;
    int length = snprintf(stage, sizeof stage, "%.*s.desktop-pack-XXXXXX", (int)(leaf - final), final);
    if (length < 0 || length >= (int)sizeof stage) { errno = ENAMETOOLONG; goto failed; }
    if (stat(resolved, &st) || !S_ISREG(st.st_mode)) {
        snprintf(error, ERROR_SIZE, "Pack expects a regular image or video file.");
        return false;
    }
    Media m;
    if (!media_probe(cfg, resolved, &m, error)) return false;
    Image image;
    bool decoded = media_decode(cfg, &m, PREVIEW_SIZE, &image, error);
    const char *kind = media_kind(&m);
    media_free(&m);
    if (!decoded) return false;
    uint8_t *webp = NULL;
    size_t bytes = WebPEncodeRGBA(image.rgba, image.w, image.h, image.w * 4, 82, &webp);
    image_free(&image);
    if (!bytes) { snprintf(error, ERROR_SIZE, "Could not encode the thumbnail."); return false; }
    if (!mkdtemp(stage)) { WebPFree(webp); goto failed; }
    const char *media_name = !strcmp(name, DESCRIPTOR) ? "image" : name;
    const char *thumbnail = !strcmp(name, "thumbnail.webp") ? "preview.webp" : "thumbnail.webp";
    char file[PATH_MAX] = "", poster[PATH_MAX] = "", descriptor[PATH_MAX] = "", contents[2 * PATH_MAX];
    length = snprintf(contents, sizeof contents,
                      "[wallpaper]\nversion = 1\ntype = %s\nthumbnail = %s\n\n[%s]\nfile = %s\n%s",
                      kind, thumbnail, kind, media_name, !strcmp(kind, "video") ? "speed = 1\n" : "");
    errno = ENAMETOOLONG;
    bool ok = length > 0 && length < (int)sizeof contents && join(file, stage, media_name) &&
              join(poster, stage, thumbnail) && join(descriptor, stage, DESCRIPTOR) &&
              copy_file(resolved, file) && atomic_replace(poster, webp, bytes, 0644) &&
              atomic_replace(descriptor, contents, (size_t)length, 0644) &&
              syscall(SYS_renameat2, AT_FDCWD, stage, AT_FDCWD, final, RENAME_NOREPLACE) == 0;
    WebPFree(webp);
    if (ok) return true;
    int saved = errno;
    if (*file) unlink(file);
    if (*poster) unlink(poster);
    if (*descriptor) unlink(descriptor);
    rmdir(stage);
    errno = saved;
failed:
    snprintf(error, ERROR_SIZE, "Could not create wallpaper package %.245s (%s).", destination, strerror(errno));
    return false;
}
