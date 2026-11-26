#include "desktop.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

extern const char defaults_start[], defaults_end[];
const char *const rotation_names[] = {"manual", "login", "interval", "daily"};
typedef enum { BOOL, INT, ENUM, STRING, PATH, COLOR } Type;
typedef struct {
    Type type;
    const char *section, *key, *initial;
    size_t offset, size;
    int min, max;
} Option;

#define STR_I(x) #x
#define STR(x) STR_I(x)
#define DEFAULT_BOOL(x) STR(x)
#define DEFAULT_INT(x) STR(x)
#define DEFAULT_ENUM(x) x
#define DEFAULT_STRING(x) x
#define DEFAULT_PATH(x) x
#define DEFAULT_COLOR(x) x
#define ENTRY(t, n, s, k, d, a, b) \
    {t, s, k, DEFAULT_##t(d), offsetof(Config, n), sizeof(((Config *)0)->n), a, b},
static const Option options[] = { CONFIG_OPTIONS(ENTRY) };
#undef ENTRY
#define NOPTIONS (sizeof options / sizeof options[0])

static bool
path_join(char dst[PATH_MAX], const char *base, const char *name)
{
    int n = snprintf(dst, PATH_MAX, "%s/%s", base, name);
    return n >= 0 && n < PATH_MAX;
}

static bool
base_dir(char out[PATH_MAX], const char *variable, const char *default_path)
{
    const char *dir = getenv(variable), *home = getenv("HOME");
    if (dir && dir[0] == '/') return strlen(dir) < PATH_MAX && strcpy(out, dir);
    return home && home[0] == '/' && path_join(out, home, default_path);
}

bool
paths_init(Paths *p, char *error)
{
    const char *display = getenv("DISPLAY"), *runtime = getenv("XDG_RUNTIME_DIR");
    char base[PATH_MAX], session[64];
    uint64_t hash = UINT64_C(14695981039346656037);
    if (display) for (; *display; ++display) {
        if (display[0] == '.' && !strcmp(display, ".0")) break;
        hash = (hash ^ (unsigned char)*display) * UINT64_C(1099511628211);
    }
    snprintf(session, sizeof session, "desktop/%016llx", (unsigned long long)hash);
    if (!base_dir(base, "XDG_CONFIG_HOME", ".config") ||
        !path_join(p->config, base, "desktop/config.ini") ||
        !base_dir(base, "XDG_STATE_HOME", ".local/state") ||
        !path_join(p->state, base, "desktop/state"))
        goto invalid;
    if (!runtime || runtime[0] != '/') runtime = base;
    char prefix[PATH_MAX];
    if (path_join(prefix, runtime, session) &&
        path_join(p->socket, prefix, "control.sock") &&
        path_join(p->lock, prefix, "lock"))
        return true;
invalid:
    snprintf(error, ERROR_SIZE, "Could not resolve Desktop's XDG paths.");
    return false;
}

bool
make_parents(const char *file)
{
    if (strlen(file) >= PATH_MAX) return false;
    char buf[PATH_MAX];
    strcpy(buf, file);
    for (char *p = buf + 1; (p = strchr(p, '/')); *p++ = '/') {
        *p = '\0';
        if (mkdir(buf, 0700) && errno != EEXIST) return false;
    }
    return true;
}

bool
atomic_replace(const char *path, const void *buf, size_t count, mode_t mode)
{
    char temporary[PATH_MAX];
    int n = snprintf(temporary, sizeof temporary, "%s.XXXXXX", path);
    if (n < 0 || n >= PATH_MAX || !make_parents(path)) return false;
    int fd = mkstemp(temporary);
    if (fd < 0) return false;
    const char *bytes = buf;
    bool ok = fchmod(fd, mode) == 0;
    while (ok && count) {
        ssize_t got = write(fd, bytes, count);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { ok = false; break; }
        count -= (size_t)got;
        bytes += got;
    }
    ok = ok && fsync(fd) == 0;
    if (close(fd)) ok = false;
    if (ok && !rename(temporary, path)) return true;
    unlink(temporary);
    return false;
}

char *
ini_trim(char *s)
{
    while (isspace((unsigned char)*s)) ++s;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

static const Option *
find_option(const char *section, const char *key)
{
    for (size_t i = 0; i < NOPTIONS; ++i)
        if (!strcmp(options[i].section, section) && !strcmp(options[i].key, key))
            return &options[i];
    return NULL;
}

static bool
copy_value(char *dest, size_t size, const char *value)
{
    if (strlen(value) >= size || strchr(value, '\r') || strchr(value, '\n')) return false;
    strcpy(dest, value);
    return true;
}

static bool
assign(Config *cfg, const Option *o, const char *value)
{
    void *dest = (char *)cfg + o->offset;
    switch (o->type) {
    case BOOL:
        if (!strcasecmp(value, "true") || !strcmp(value, "1")) *(bool *)dest = true;
        else if (!strcasecmp(value, "false") || !strcmp(value, "0")) *(bool *)dest = false;
        else return false;
        return true;
    case INT: {
        char *end;
        errno = 0;
        long number = strtol(value, &end, 10);
        if (end == value || *end || errno || number < o->min || number > o->max) return false;
        *(int *)dest = (int)number;
        return true;
    }
    case ENUM:
        for (size_t i = 0; i < sizeof rotation_names / sizeof *rotation_names; ++i)
            if (!strcmp(rotation_names[i], value)) { *(RotationMode *)dest = (RotationMode)i; return true; }
        return false;
    case PATH: {
        static const char token[] = "${XDG_CONFIG_HOME}";
        if (!strncmp(value, token, sizeof token - 1) && (value[sizeof token - 1] == '/' || !value[sizeof token - 1])) {
            char base[PATH_MAX];
            if (!base_dir(base, "XDG_CONFIG_HOME", ".config")) return false;
            int n = snprintf(dest, o->size, "%s%s", base, value + sizeof token - 1);
            return n >= 0 && (size_t)n < o->size;
        }
        if (*value == '~' && (value[1] == '/' || !value[1])) {
            const char *home = getenv("HOME");
            if (!home) return false;
            int n = snprintf(dest, o->size, "%s%s", home, value + 1);
            return n >= 0 && (size_t)n < o->size;
        }
        return copy_value(dest, o->size, value);
    }
    case COLOR:
        if (*value && (value[0] != '#' || strlen(value) != 7 ||
                       strspn(value + 1, "0123456789abcdefABCDEF") != 6)) return false;
        return copy_value(dest, o->size, value);
    case STRING:
        return copy_value(dest, o->size, value);
    }
    return false;
}

static const Option *
parse_line(char *line, char section[64], const char **value)
{
    char *s = ini_trim(line);
    if (!*s || *s == '#' || *s == ';') return NULL;
    if (*s == '[') {
        char *end = strrchr(s, ']');
        section[0] = '\0';
        if (end && !*ini_trim(end + 1)) {
            *end = '\0';
            copy_value(section, 64, ini_trim(s + 1));
        }
        return NULL;
    }
    char *eq = strchr(s, '=');
    if (!eq) return NULL;
    *eq = '\0';
    *value = ini_trim(eq + 1);
    return find_option(section, ini_trim(s));
}

bool
config_read(const Paths *p, Config *cfg, char *error)
{
    memset(cfg, 0, sizeof *cfg);
    for (size_t i = 0; i < NOPTIONS; ++i)
        if (!assign(cfg, &options[i], options[i].initial)) {
            snprintf(error, ERROR_SIZE, "Could not resolve the default %s.", options[i].key);
            return false;
        }
    FILE *f = fopen(p->config, "r");
    if (!f) {
        if (errno == ENOENT) return true;
        snprintf(error, ERROR_SIZE, "Cannot open config.ini (%s).", strerror(errno));
        return false;
    }
    char *line = NULL, section[64] = "";
    size_t capacity = 0, number = 0;
    bool ok = true;
    while (getline(&line, &capacity, f) >= 0) {
        ++number;
        const char *value = NULL;
        const Option *o = parse_line(line, section, &value);
        if (o && !assign(cfg, o, value)) {
            snprintf(error, ERROR_SIZE, "Invalid value for %s.%s on config.ini line %zu.",
                     o->section, o->key, number);
            ok = false;
            break;
        }
    }
    if (ferror(f)) {
        snprintf(error, ERROR_SIZE, "Cannot read config.ini (%s).", strerror(errno));
        ok = false;
    }
    free(line);
    fclose(f);
    return ok;
}

bool
config_write(const Paths *p, const Setting *settings, size_t count, char *error)
{
    Config check = {0};
    const char *values[NOPTIONS] = {0};
    for (size_t i = 0; i < count; ++i) {
        char key[128];
        const Option *o = NULL;
        if (copy_value(key, sizeof key, settings[i].key)) {
            char *dot = strchr(key, '.');
            if (dot) {
                *dot++ = '\0';
                o = find_option(key, dot);
            }
        }
        if (!o || !assign(&check, o, settings[i].value)) {
            snprintf(error, ERROR_SIZE, "Invalid setting or value for %.300s.", settings[i].key);
            return false;
        }
        values[o - options] = settings[i].value;
    }
    char target[PATH_MAX], lock[PATH_MAX];
    if (!realpath(p->config, target)) {
        if (errno != ENOENT) goto failed;
        struct stat st;
        if (!lstat(p->config, &st)) { errno = ENOENT; goto failed; }
        strcpy(target, p->config);
    }
    int size = snprintf(lock, sizeof lock, "%s.lock", p->state);
    if (size < 0 || size >= (int)sizeof lock || !make_parents(lock)) goto failed;
    int fd = open(lock, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) goto failed;
    if (flock(fd, LOCK_EX)) { close(fd); goto failed; }

    FILE *in = fopen(target, "r");
    if (!in && errno == ENOENT)
        in = fmemopen((void *)defaults_start, (size_t)((uintptr_t)defaults_end - (uintptr_t)defaults_start), "r");
    char *result = NULL, *line = NULL, section[64] = "";
    size_t length = 0, capacity = 0;
    bool found[NOPTIONS] = {0};
    FILE *out = in ? open_memstream(&result, &length) : NULL;
    bool ok = out != NULL;
    if (out) {
        while (getline(&line, &capacity, in) >= 0) {
            char *copy = strdup(line);
            if (!copy) { ok = false; break; }
            const char *unused = NULL;
            const Option *o = parse_line(copy, section, &unused);
            if (o && values[o - options]) {
                fprintf(out, "%s = %s\n", o->key, values[o - options]);
                found[o - options] = true;
            } else fputs(line, out);
            free(copy);
        }
        for (size_t i = 0; i < NOPTIONS; ++i)
            if (values[i] && !found[i])
                fprintf(out, "\n[%s]\n%s = %s\n", options[i].section, options[i].key, values[i]);
        ok = ok && !ferror(in) && !ferror(out);
        if (fclose(out)) ok = false;
    }
    if (in) fclose(in);
    free(line);
    struct stat st;
    mode_t mode = stat(target, &st) ? 0600 : (st.st_mode & 0777);
    if (ok) ok = atomic_replace(target, result, length, mode);
    free(result);
    close(fd);
    if (ok) return true;
failed:
    snprintf(error, ERROR_SIZE, "Could not save config.ini (%s).", strerror(errno));
    return false;
}

bool
state_read(const Paths *p, State *s)
{
    memset(s, 0, sizeof *s);
    FILE *f = fopen(p->state, "r");
    if (!f) return false;
    long long changed;
    int paused;
    char line[128];
    bool ok = fgets(line, sizeof line, f) &&
              sscanf(line, "desktop-state-1 %lld %d", &changed, &paused) == 2;
    if (ok) {
        s->changed = (time_t)changed;
        s->paused = paused != 0;
        size_t n = fread(s->path, 1, sizeof s->path - 1, f);
        s->path[n] = '\0';
        ok = !ferror(f) && fgetc(f) == EOF;
    }
    if (!ok) memset(s, 0, sizeof *s);
    fclose(f);
    return ok;
}

bool
state_write(const Paths *p, const State *s)
{
    char bytes[PATH_MAX + 80];
    int n = snprintf(bytes, sizeof bytes, "desktop-state-1 %lld %d\n%s",
                     (long long)s->changed, s->paused, s->path);
    return n >= 0 && (size_t)n < sizeof bytes && atomic_replace(p->state, bytes, (size_t)n, 0600);
}

void
json_string(FILE *out, const char *s)
{
    fputc('"', out);
    for (; *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') fprintf(out, "\\%c", ch);
        else if (ch < 32) fprintf(out, "\\u%04x", ch);
        else fputc(ch, out);
    }
    fputc('"', out);
}

uint64_t
now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

const char *
filename(const char *s)
{
    const char *slash = strrchr(s, '/');
    return slash ? slash + 1 : s;
}
