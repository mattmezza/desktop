#ifndef DESKTOP_H
#define DESKTOP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <sys/types.h>
#include <time.h>

#define ERROR_SIZE 512
#define MAX_OUTPUTS 32
#define MAX_REQUEST (PATH_MAX + 1024)
#define MAX_PIXELS (100u * 1024u * 1024u)
#define STRING_SIZE PATH_MAX

#define CONFIG_OPTIONS(OPTION) \
    OPTION(PATH, directory, "library", "directory", "${XDG_CONFIG_HOME}/wallpapers", 0, 0) \
    OPTION(BOOL, recursive, "library", "recursive", false, 0, 1) \
    OPTION(BOOL, images, "library", "images", true, 0, 1) \
    OPTION(BOOL, videos, "library", "videos", true, 0, 1) \
    OPTION(ENUM, rotation, "rotation", "mode", "login", 0, 0) \
    OPTION(INT, minutes, "rotation", "minutes", 30, 1, 10080) \
    OPTION(BOOL, no_repeat, "rotation", "no_repeat", true, 0, 1) \
    OPTION(INT, focal_x, "layout", "focal_x", 50, 0, 100) \
    OPTION(INT, focal_y, "layout", "focal_y", 50, 0, 100) \
    OPTION(COLOR, background, "layout", "background", "#0D0E0F", 0, 0) \
    OPTION(INT, crossfade_ms, "layout", "crossfade_ms", 180, 0, 1500) \
    OPTION(BOOL, parallax, "parallax", "enabled", true, 0, 1) \
    OPTION(BOOL, parallax_empty, "parallax", "empty_only", true, 0, 1) \
    OPTION(INT, travel, "parallax", "travel_px", 14, 0, 160) \
    OPTION(INT, ease_ms, "parallax", "ease_ms", 180, 0, 2000) \
    OPTION(INT, parallax_fps, "parallax", "fps", 30, 1, 60) \
    OPTION(BOOL, video_empty, "video", "empty_only", true, 0, 1) \
    OPTION(INT, video_ramp_ms, "video", "ramp_ms", 1000, 0, 5000) \
    OPTION(INT, video_fps, "video", "fps", 24, 1, 60) \
    OPTION(INT, video_max, "video", "max_dimension", 3840, 640, 8192) \
    OPTION(PATH, ffmpeg, "video", "ffmpeg", "desktop-ffmpeg", 0, 0) \
    OPTION(PATH, ffprobe, "video", "ffprobe", "desktop-ffprobe", 0, 0) \
    OPTION(INT, slowdown_ms, "motion", "slowdown_ms", 320, 0, 2000) \
    OPTION(BOOL, idle_pause, "motion", "pause_on_blank", true, 0, 1) \
    OPTION(STRING, after_login, "hooks", "after_login", "", 0, 0) \
    OPTION(STRING, after_set, "hooks", "after_set", "", 0, 0) \
    OPTION(PATH, font, "ui", "font", "", 0, 0) \
    OPTION(PATH, mono, "ui", "mono", "", 0, 0) \
    OPTION(INT, font_size, "ui", "font_size", 14, 11, 24) \
    OPTION(INT, width, "ui", "width", 900, 560, 1600) \
    OPTION(INT, height, "ui", "height", 650, 460, 1100) \
    OPTION(INT, corner_radius, "ui", "corner_radius", -1, -1, 40) \
    OPTION(INT, animation_ms, "ui", "animation_ms", 130, 0, 500) \
    OPTION(COLOR, ui_background, "ui", "background", "", 0, 0) \
    OPTION(COLOR, panel, "ui", "panel", "", 0, 0) \
    OPTION(COLOR, foreground, "ui", "foreground", "", 0, 0) \
    OPTION(COLOR, muted, "ui", "muted", "", 0, 0) \
    OPTION(COLOR, border, "ui", "border", "", 0, 0) \
    OPTION(COLOR, accent, "ui", "accent", "", 0, 0) \
    OPTION(COLOR, on_accent, "ui", "on_accent", "", 0, 0)

typedef enum { ROT_MANUAL, ROT_LOGIN, ROT_INTERVAL, ROT_DAILY } RotationMode;
typedef struct {
#define FIELD_BOOL(n) bool n;
#define FIELD_INT(n) int n;
#define FIELD_STRING(n) char n[STRING_SIZE];
#define FIELD_PATH(n) FIELD_STRING(n)
#define FIELD_COLOR(n) char n[8];
#define FIELD_ENUM(n) RotationMode n;
#define FIELD(type, n, sec, key, def, min, max) FIELD_##type(n)
    CONFIG_OPTIONS(FIELD)
#undef FIELD
} Config;

typedef struct {
    char config[PATH_MAX], state[PATH_MAX], socket[PATH_MAX], lock[PATH_MAX];
} Paths;

typedef struct {
    int x, y, w, h;
    char name[128];
} Output;

typedef struct {
    int w, h;
    size_t count;
    Output items[MAX_OUTPUTS];
} Layout;

typedef struct {
    int w, h;
    unsigned char *rgba;
} Image;

typedef struct {
    char *path;
    int w, h;
    double sar;
    bool video, failed, used;
    struct timespec modified;
    Image thumb;
    bool thumb_done;
} Media;

typedef struct {
    Media *items;
    size_t count;
} Library;

typedef struct {
    char path[PATH_MAX];
    time_t changed;
    bool paused, inhibited;
} State;

typedef struct {
    const char *key, *value;
} Setting;

bool paths_init(Paths *, char *);
bool make_parents(const char *);
bool config_read(const Paths *, Config *, char *);
bool config_write(const Paths *, const Setting *, size_t, char *);
bool state_read(const Paths *, State *);
bool state_write(const Paths *, const State *);
bool atomic_replace(const char *, const void *, size_t, mode_t);
void json_string(FILE *, const char *);
uint64_t now_ms(void);
const char *filename(const char *);
extern const char *const rotation_names[];

bool media_probe(const Config *, const char *, Media *, char *);
bool media_decode(const Config *, const Media *, int, Image *, char *);
void image_free(Image *);
bool library_read(const Config *, Library *, char *);
void library_free(Library *);
bool media_eligible(const Config *, const Layout *, const Media *, char *);
void media_crop(const Config *, const Media *, int, int, double *, double *, double *, double *);
int video_spawn(const Config *, const Media *, const Output *, Image *, pid_t *);
void child_stop(pid_t);
bool hook_run(const char *, const char *, const char *, bool);

int daemon_run(const Paths *, int);
bool daemon_start(const Paths *, char *);
int command_run(const Paths *, size_t, const char *const *, FILE *, char *);
int ui_run(const Paths *);
int launch_menu(const Paths *);
#endif
