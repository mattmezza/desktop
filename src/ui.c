#include "x11.h"
#include "cui/cui_components.h"
#include "cui/cui_software.h"
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE_MAX 12
#define ICON_SIZE 24
extern const unsigned char geist_start[], geist_end[], geist_medium_start[], geist_medium_end[];
extern const unsigned char geist_mono_start[], geist_mono_end[], icons_start[];

typedef enum { MONITOR, SETTINGS, BACK, SHUFFLE, PREVIOUS, NEXT, PLAY, CHECK, SEARCH, FOLDER, PHOTO, SPARKLES, ICONS } Icon;
typedef enum { BROWSE, PREFERENCES, EFFECTS } EditorScreen;
enum { BLUR_SWITCH, PARALLAX_SWITCH, PARALLAX_EMPTY_SWITCH, VIDEO_EMPTY_SWITCH, SWITCH_COUNT };
typedef enum { NONE, SCAN, THUMB, PREVIEW } Job;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    int signal[2];
    bool stop, busy, completed, ok;
    Job job;
    Config cfg;
    Media media;
    size_t index;
    Library lib;
    Image image;
    char error[ERROR_SIZE];
} Worker;

typedef struct {
    const Paths *paths;
    Config cfg, edit_cfg;
    X11 x;
    Layout layout;
    Window window;
    GC gc;
    Atom wm_delete, clipboard, utf8, paste, targets, instance;
    XIM im;
    XIC ic;
    int w, h;
    XImage *pixels;
    Cursor cursor[CUI_CURSOR_HELP + 1];
    cui_cursor shown_cursor;
    cui_context *ui;
    cui_software *software;
    cui_font *font, *mono, *medium;
    cui_theme theme;
    cui_edit *search, *folder, *minutes;
    cui_input_state search_input, folder_input, minutes_input;
    cui_switch_state switches[SWITCH_COUNT];
    cui_platform platform;
    char search_text[256], folder_text[PATH_MAX], minutes_text[16];
    char active[PATH_MAX], message[ERROR_SIZE], clipboard_text[PATH_MAX];
    unsigned char icon_pixels[ICONS][ICON_SIZE * ICON_SIZE * 4];
    Worker worker;
    Library library;
    size_t selected, previewed, page;
    Image preview;
    EditorScreen screen;
    MediaType filter;
    uint64_t entered, last_frame, update_at;
    bool dirty, working, scan, finish, theme_dirty, suitable_only;
    unsigned visible[PAGE_MAX];
    size_t visible_count, pages;
} Editor;

static void *
media_worker(void *arg)
{
    Worker *w = arg;
    pthread_mutex_lock(&w->mutex);
    while (!w->stop) {
        while (!w->stop && w->job == NONE) pthread_cond_wait(&w->cond, &w->mutex);
        if (w->stop) break;
        Job job = w->job;
        pthread_mutex_unlock(&w->mutex);
        w->error[0] = '\0';
        if (job == SCAN) w->ok = library_read(&w->cfg, &w->lib, w->error);
        else if (job == THUMB) w->ok = media_thumbnail(&w->cfg, &w->media, &w->image, w->error);
        else w->ok = media_decode(&w->cfg, &w->media, 1000, &w->image, w->error);
        pthread_mutex_lock(&w->mutex);
        w->completed = true;
        (void)write(w->signal[1], "w", 1);
        while (!w->stop && w->completed) pthread_cond_wait(&w->cond, &w->mutex);
    }
    pthread_mutex_unlock(&w->mutex);
    return NULL;
}

static bool
worker_start(Worker *w)
{
    if (pipe2(w->signal, O_CLOEXEC | O_NONBLOCK)) return false;
    pthread_mutex_init(&w->mutex, NULL);
    pthread_cond_init(&w->cond, NULL);
    if (!pthread_create(&w->thread, NULL, media_worker, w)) return true;
    pthread_mutex_destroy(&w->mutex);
    pthread_cond_destroy(&w->cond);
    close(w->signal[0]);
    close(w->signal[1]);
    return false;
}

static void
worker_end(Worker *w)
{
    pthread_mutex_lock(&w->mutex);
    w->stop = true;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
    pthread_join(w->thread, NULL);
    pthread_mutex_destroy(&w->mutex);
    pthread_cond_destroy(&w->cond);
    close(w->signal[0]);
    close(w->signal[1]);
    media_free(&w->media);
    library_free(&w->lib);
    image_free(&w->image);
}

static void
worker_request(Editor *e, Job job, size_t index)
{
    Worker *w = &e->worker;
    pthread_mutex_lock(&w->mutex);
    if (!w->busy) {
        if (job != SCAN && !media_copy(&w->media, &e->library.items[index])) {
            pthread_mutex_unlock(&w->mutex);
            return;
        }
        w->index = index;
        w->cfg = e->cfg;
        w->job = job;
        w->busy = true;
        pthread_cond_signal(&w->cond);
    }
    pthread_mutex_unlock(&w->mutex);
}

static void
choose_current(Editor *e)
{
    e->selected = SIZE_MAX;
    for (size_t i = 0; i < e->library.count; ++i)
        if (!strcmp(e->active, e->library.items[i].path)) { e->selected = i; break; }
    if (e->selected == SIZE_MAX) {
        for (size_t i = 0; i < e->library.count; ++i)
            if (media_eligible(&e->cfg, &e->layout, &e->library.items[i], NULL)) { e->selected = i; break; }
    }
    if (e->selected == SIZE_MAX && e->library.count) e->selected = 0;
    e->previewed = SIZE_MAX;
    image_free(&e->preview);
}

static void
worker_collect(Editor *e)
{
    Worker *w = &e->worker;
    char bytes[16];
    (void)read(w->signal[0], bytes, sizeof bytes);
    pthread_mutex_lock(&w->mutex);
    if (w->completed) {
        if (w->job == SCAN) {
            library_free(&e->library);
            e->library = w->lib;
            w->lib = (Library){0};
            if (!w->ok) snprintf(e->message, sizeof e->message, "%s", w->error);
            choose_current(e);
        } else if (w->index < e->library.count) {
            Media *m = &e->library.items[w->index];
            if (w->job == THUMB) {
                m->thumb = w->image;
                w->image = (Image){0};
                m->thumb_done = true;
            } else if (e->selected == w->index) {
                image_free(&e->preview);
                e->preview = w->image;
                w->image = (Image){0};
                e->previewed = w->index;
            }
        }
        image_free(&w->image);
        media_free(&w->media);
        w->job = NONE;
        w->completed = w->busy = false;
        pthread_cond_signal(&w->cond);
        e->dirty = true;
    }
    pthread_mutex_unlock(&w->mutex);
}

static void
worker_schedule(Editor *e)
{
    if (e->worker.busy) return;
    if (e->scan) { e->scan = false; worker_request(e, SCAN, 0); return; }
    if (e->screen != BROWSE) return;
    if (e->selected < e->library.count && e->previewed != e->selected &&
        !e->library.items[e->selected].failed) {
        worker_request(e, PREVIEW, e->selected);
        return;
    }
    for (size_t i = 0; i < e->visible_count; ++i)
        if (!e->library.items[e->visible[i]].thumb_done && !e->library.items[e->visible[i]].failed) {
            worker_request(e, THUMB, e->visible[i]);
            return;
        }
}

static cui_color
rgb(unsigned n)
{
    return (cui_color){(unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n, 255};
}

static const char *
resource(XrmDatabase db, const char *key)
{
    XrmValue value;
    char *class_name = NULL;
    return db && XrmGetResource(db, key, key, &class_name, &value) ? value.addr : NULL;
}

static cui_color
color(const Editor *e, const char *name, cui_color fallback)
{
    XColor c;
    return name && *name && XParseColor(e->x.d, DefaultColormap(e->x.d, e->x.screen), name, &c)
        ? (cui_color){c.red >> 8, c.green >> 8, c.blue >> 8, 255} : fallback;
}

static cui_color
theme_color(Editor *e, XrmDatabase db, const char *desktop, const char *mwm,
            const char *ini, cui_color fallback)
{
    return color(e, ini, color(e, resource(db, desktop), mwm ?
                 color(e, resource(db, mwm), fallback) : fallback));
}

static void
read_theme(Editor *e, bool fonts)
{
    unsigned char *data = NULL;
    Atom type;
    int format;
    unsigned long n, more;
    XrmInitialize();
    XrmDatabase db = NULL;
    if (XGetWindowProperty(e->x.d, e->x.root, XA_RESOURCE_MANAGER, 0, 262144, False,
                           XA_STRING, &type, &format, &n, &more, &data) == Success &&
        type == XA_STRING && format == 8 && !more && data)
        db = XrmGetStringDatabase((char *)data);
    if (data) XFree(data);
    Config *c = &e->cfg;
    cui_theme t = cui_theme_dark();
    t.background = theme_color(e, db, "desktop.background", "mwm.normbg", c->ui_background, rgb(0x0d0e0f));
    t.text = theme_color(e, db, "desktop.foreground", "mwm.normfg", c->foreground, rgb(0xe8e7e8));
    t.muted = theme_color(e, db, "desktop.muted", "mwm.normborder", c->muted,
                         cui_color_mix_oklab(t.background, t.text, .7f));
    t.border = theme_color(e, db, "desktop.border", NULL, c->border,
                          cui_color_mix_oklab(t.background, t.muted, .28f));
    t.primary = theme_color(e, db, "desktop.accent", "mwm.selbg", c->accent, rgb(0x968587));
    t.on_primary = theme_color(e, db, "desktop.onAccent", "mwm.selfg", c->on_accent, rgb(0x0d0e0f));
    t.surface = theme_color(e, db, "desktop.panel", NULL, c->panel,
                            cui_color_mix_oklab(t.background, t.text, .06f));
    int radius = 6;
    const char *radius_name = resource(db, "desktop.cornerRadius");
    if (!radius_name) radius_name = resource(db, "mwm.segradius");
    if (radius_name) {
        char *end;
        long n_radius = strtol(radius_name, &end, 10);
        if (end != radius_name && !*end && n_radius >= 0 && n_radius <= 40) radius = (int)n_radius;
    }
    t.radius = c->corner_radius >= 0 ? (float)c->corner_radius : (float)radius;
    t.font_size = (float)c->font_size;
    t.focus = t.primary;
    t.input = t.text;
    t.input_background = t.surface;
    t.selection = cui_color_mix_oklab(t.surface, t.primary, .6f);
    t.caret = t.text;
    t.switch_thumb = t.text;
    t.switch_background = cui_color_mix_oklab(t.surface, t.muted, .28f);
    t.accent = t.primary;
    t.accent_foreground = t.on_primary;
    t.dark = (t.background.r * 3 + t.background.g * 6 + t.background.b) < 1280;
    e->theme = t;
    if (fonts) {
        const char *font = *c->font ? c->font : resource(db, "desktop.font");
        const char *mono = *c->mono ? c->mono : resource(db, "desktop.mono");
        if (font) e->font = cui_font_open(font);
        if (mono) e->mono = cui_font_open(mono);
        if (!e->font) {
            e->font = cui_font_open_memory(geist_start, (size_t)((uintptr_t)geist_end - (uintptr_t)geist_start));
            e->medium = cui_font_open_memory(geist_medium_start,
                         (size_t)((uintptr_t)geist_medium_end - (uintptr_t)geist_medium_start));
        }
        if (!e->mono) e->mono = cui_font_open_memory(geist_mono_start,
                         (size_t)((uintptr_t)geist_mono_end - (uintptr_t)geist_mono_start));
        if (e->font && e->mono) cui_font_set_style(e->font, (cui_text_style){.monospace = true}, e->mono);
        if (e->font && e->medium)
            cui_font_set_style(e->font, (cui_text_style){.weight = CUI_TEXT_WEIGHT_MEDIUM}, e->medium);
    }
    if (db) XrmDestroyDatabase(db);
}

static void
box(Editor *e, cui_rect r, cui_color background, float radius, cui_color border)
{
    cui_begin_absolute_box(e->ui, 0, (cui_box){
        .background = background, .radius = radius, .border = border, .border_width = border.a ? 1 : 0
    }, r);
    cui_end_box(e->ui);
}

static cui_str
elide(Editor *e, const char *text, float width, float size, cui_text_style style, char output[512])
{
    size_t count = strnlen(text, 480);
    cui_str view = {text, count};
    if (cui_measure_text_ex(e->ui, view, size, style).width <= width) return view;
    while (count) {
        --count;
        while (count && ((unsigned char)text[count] & 0xc0) == 0x80) --count;
        memcpy(output, text, count);
        memcpy(output + count, "…", 4);
        view = (cui_str){output, count + 3};
        if (cui_measure_text_ex(e->ui, view, size, style).width <= width) return view;
    }
    return cui_string("");
}

static void
text_at(Editor *e, cui_rect r, const char *s, float size, cui_color color,
        bool mono, bool medium, bool center)
{
    cui_text_style style = {.monospace = mono, .weight = medium ? CUI_TEXT_WEIGHT_MEDIUM : CUI_TEXT_WEIGHT_NORMAL};
    char buffer[512];
    cui_str value = elide(e, s, fmaxf(0, r.w), size, style, buffer);
    cui_begin_absolute_box(e->ui, 0, (cui_box){
        .align_x = center ? CUI_ALIGN_CENTER : CUI_ALIGN_START, .align_y = CUI_ALIGN_CENTER
    }, r);
    cui_text_line(e->ui, value, size, color, size + 5, style);
    cui_end_box(e->ui);
}

static void
wrapped_text_at(Editor *e, cui_rect r, const char *text, float size, float line_height, cui_color color)
{
    cui_text_style style = {0};
    cui_begin_absolute_box(e->ui, 0, (cui_box){.clip = true}, r);
    while (*text) {
        const char *end = text, *space = NULL;
        while (*end) {
            const char *next = end + 1;
            while (((unsigned char)*next & 0xc0) == 0x80) ++next;
            if (*end == ' ') space = end;
            if (end > text && cui_measure_text(e->ui, (cui_str){text, (size_t)(next - text)}, size).width > r.w) {
                if (space) end = space;
                break;
            }
            end = next;
        }
        cui_text_line(e->ui, (cui_str){text, (size_t)(end - text)}, size, color, line_height, style);
        text = end;
        while (*text == ' ') ++text;
    }
    cui_end_box(e->ui);
}

static void
picture(Editor *e, cui_rect r, const Image *image, cui_rect source, float opacity)
{
    if (!image->rgba || r.w < 1 || r.h < 1) return;
    cui_begin_absolute_box(e->ui, 0, (cui_box){.clip = true}, r);
    cui_paint_bitmap(e->ui, 0, (cui_bitmap_paint){
        .bitmap = {image->rgba, (unsigned)image->w, (unsigned)image->h, (size_t)image->w * 4, 0},
        .source = source, .opacity = opacity
    }, (cui_box){.width = r.w, .height = r.h});
    cui_end_box(e->ui);
}

static void
icon(Editor *e, Icon i, cui_rect r, cui_color color_value)
{
    if (i == ICONS) return;
    cui_begin_absolute_box(e->ui, 0, (cui_box){0}, r);
    cui_paint_bitmap(e->ui, 0, (cui_bitmap_paint){
        .bitmap = {e->icon_pixels[i], ICON_SIZE, ICON_SIZE, ICON_SIZE * 4, 0},
        .opacity = (float)color_value.a / 255, .tint = color_value, .custom_tint = true
    }, (cui_box){.width = r.w, .height = r.h});
    cui_end_box(e->ui);
}

typedef enum { GHOST, OUTLINED, PRIMARY, ACTIVE } ButtonStyle;

static bool
button(Editor *e, const char *key, const char *label, cui_rect r, Icon glyph,
       ButtonStyle kind, bool disabled)
{
    cui_theme t = e->theme;
    cui_id id = cui_id_from(e->ui, key);
    cui_interaction prior = cui_interaction_state(e->ui, id);
    bool filled = kind == PRIMARY || kind == ACTIVE;
    cui_color bg = kind == PRIMARY ? t.primary : kind == ACTIVE ?
                   cui_color_mix_oklab(t.surface, t.primary, .24f) : (cui_color){0};
    cui_color line = kind == OUTLINED || kind == ACTIVE ? t.border : (cui_color){0};
    cui_color fg = kind == PRIMARY ? t.on_primary : t.text;
    if (!disabled && (prior.hovered || prior.pressed))
        bg = cui_color_mix_oklab(filled ? bg : t.surface, t.text, prior.pressed ? .20f : .10f);
    if (disabled) { bg = filled ? t.surface : (cui_color){0}; fg = t.muted; }
    if (prior.focus_visible) line = t.primary;
    cui_begin_absolute_box(e->ui, id, (cui_box){
        .background = bg, .radius = t.radius, .border = line, .border_width = line.a ? 1 : 0
    }, r);
    cui_interaction action = cui_interact(e->ui, id, CUI_ROLE_BUTTON, cui_string(label),
                                          disabled, kind == ACTIVE, 0);
    if (!disabled) cui_set_cursor(e->ui, id, CUI_CURSOR_POINTER);
    if (*label && glyph != ICONS && r.w >= 64) {
        float width = cui_measure_text(e->ui, cui_string(label), t.font_size).width;
        float left = fmaxf(10, (r.w - width - 24) / 2);
        icon(e, glyph, (cui_rect){left, (r.h - 17) / 2, 17, 17}, fg);
        text_at(e, (cui_rect){left + 24, 0, r.w - left - 28, r.h}, label, t.font_size, fg, false, true, false);
    } else if (glyph != ICONS)
        icon(e, glyph, (cui_rect){(r.w - 18) / 2, (r.h - 18) / 2, 18, 18}, fg);
    else text_at(e, (cui_rect){4, 0, r.w - 8, r.h}, label, t.font_size, fg, false, true, true);
    cui_end_box(e->ui);
    return action.activated;
}

static cui_input_result
input(Editor *e, const char *name, cui_rect r, cui_edit *edit, cui_input_state *state,
      const char *placeholder)
{
    cui_id id = cui_id_from(e->ui, name);
    cui_begin_absolute_box(e->ui, 0, (cui_box){0}, r);
    cui_input_result result = cui_input_ex(e->ui, id, &e->theme, cui_string(name), edit, state,
        (cui_input_options){.width = r.w, .height = r.h, .placeholder = cui_string(placeholder),
                            .clipboard = &e->platform});
    cui_set_cursor(e->ui, id, CUI_CURSOR_TEXT);
    cui_end_box(e->ui);
    return result;
}

static void
navigate(Editor *e, EditorScreen screen)
{
    if (screen != BROWSE) e->edit_cfg = e->cfg;
    if (screen == PREFERENCES) {
        cui_edit_set_content(e->folder, (cui_edit_content){.text = cui_string(e->cfg.directory)});
        char value[16];
        snprintf(value, sizeof value, "%d", e->cfg.minutes);
        cui_edit_set_content(e->minutes, (cui_edit_content){.text = cui_string(value)});
    }
    cui_focus(e->ui, 0);
    e->screen = screen;
    e->entered = now_ms();
    e->message[0] = '\0';
    e->dirty = true;
}

static bool
action(Editor *e, const char *command, const char *argument)
{
    const char *args[] = {command, argument};
    char error[ERROR_SIZE] = "";
    if (command_run(e->paths, argument ? 2 : 1, args, NULL, error)) {
        snprintf(e->message, sizeof e->message, "%s", error);
        e->dirty = true;
        return false;
    }
    e->message[0] = '\0';
    e->dirty = true;
    return true;
}

static void
refresh_active(Editor *e, bool select_active)
{
    State live;
    char *path = NULL;
    char error[ERROR_SIZE] = "";
    size_t size = 0;
    FILE *stream = open_memstream(&path, &size);
    int result = 1;
    if (stream) {
        const char *args[] = {"status", "--path"};
        result = command_run(e->paths, 2, args, stream, error);
        fclose(stream);
    }
    if (!result && size && size <= PATH_MAX) {
        if (path[size - 1] == '\n') path[size - 1] = '\0';
        strcpy(e->active, path);
    } else if (state_read(e->paths, &live)) strcpy(e->active, live.path);
    free(path);
    if (select_active) {
        choose_current(e);
        if (e->selected != SIZE_MAX) e->page = e->selected / PAGE_MAX;
    }
}

static void
apply_selected(Editor *e)
{
    if (e->selected >= e->library.count) return;
    Media *m = &e->library.items[e->selected];
    if (action(e, "set", m->path)) {
        snprintf(e->active, sizeof e->active, "%s", m->path);
        snprintf(e->message, sizeof e->message, "Wallpaper applied.");
    }
}

static void
shuffle_now(Editor *e)
{
    if (action(e, "next", NULL)) {
        cui_edit_set_content(e->search, (cui_edit_content){.text = cui_string("")});
        e->filter = MEDIA_ANY;
        refresh_active(e, true);
        snprintf(e->message, sizeof e->message, "Wallpaper shuffled.");
    }
}

static void
preview(Editor *e, const Media *m, cui_rect area)
{
    cui_theme t = e->theme;
    box(e, area, t.surface, t.radius + 2, t.border);
    if (!m || !e->layout.count) {
        icon(e, PHOTO, (cui_rect){area.x + area.w / 2 - 14, area.y + area.h / 2 - 14, 28, 28}, t.muted);
        return;
    }
    const Image *image = e->previewed == e->selected && e->preview.rgba ? &e->preview : &m->thumb;
    float scale = fminf((area.w - 30) / e->layout.w, (area.h - 28) / e->layout.h);
    float left = area.x + (area.w - e->layout.w * scale) / 2;
    float top = area.y + (area.h - e->layout.h * scale) / 2;
    for (size_t i = 0; i < e->layout.count; ++i) {
        const Output *o = &e->layout.items[i];
        cui_rect r = {roundf(left + o->x * scale) + 2, roundf(top + o->y * scale) + 2,
                      roundf(o->w * scale) - 4, roundf(o->h * scale) - 4};
        if (r.w < 2 || r.h < 2) continue;
        box(e, r, cui_color_mix_oklab(t.background, t.text, .11f), 3, t.border);
        if (image->rgba)
            if (m->w > 0 && m->h > 0) {
                double x, y, w, h;
                media_crop(&e->cfg, m, o->w, o->h, &x, &y, &w, &h);
                double sx = image->w / (m->w * m->sar), sy = image->h / (double)m->h;
                picture(e, (cui_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, image,
                        (cui_rect){(float)(x * sx), (float)(y * sy), (float)(w * sx), (float)(h * sy)}, 1);
            }
        if (r.w > 86 && r.h > 38) {
            const float length = fminf(r.w - 16, (float)strlen(o->name) * 6.4f + 17);
            box(e, (cui_rect){r.x + 7, r.y + r.h - 25, length, 18}, (cui_color){13, 14, 15, 212}, 3, (cui_color){0});
            text_at(e, (cui_rect){r.x + 7, r.y + r.h - 25, length, 18}, o->name, 10, rgb(0xffffff), true, false, true);
        }
    }
}

static cui_rect
cover(const Image *i, float w, float h)
{
    float scale = fmaxf(w / i->w, h / i->h);
    float cw = w / scale, ch = h / scale;
    return (cui_rect){(i->w - cw) / 2, (i->h - ch) / 2, cw, ch};
}

static void
tile(Editor *e, size_t index, cui_rect r)
{
    Media *m = &e->library.items[index];
    cui_theme t = e->theme;
    bool selected = index == e->selected;
    bool good = media_eligible(&e->cfg, &e->layout, m, NULL);
    bool active = !strcmp(m->path, e->active);
    cui_id id = cui_id_from(e->ui, m->path);
    cui_interaction prior = cui_interaction_state(e->ui, id);
    cui_color line = selected || prior.focus_visible ? t.primary : t.border;
    cui_color bg = prior.hovered ? cui_color_mix_oklab(t.surface, t.text, .10f) : t.surface;
    cui_begin_absolute_box(e->ui, id, (cui_box){
        .background = bg, .border = line, .border_width = selected ? 2 : 1, .radius = t.radius
    }, r);
    cui_interaction click = cui_interact(e->ui, id, CUI_ROLE_BUTTON, cui_string(media_title(m)), false, selected, 0);
    cui_set_cursor(e->ui, id, CUI_CURSOR_POINTER);
    float inset = selected ? 4 : 3, height = r.h - 25;
    cui_rect poster = {inset, inset, r.w - inset * 2, height - inset};
    if (m->thumb.rgba)
        picture(e, poster, &m->thumb, cover(&m->thumb, poster.w, poster.h), good ? 1 : .43f);
    else {
        box(e, poster, cui_color_mix_oklab(bg, t.text, .04f), fmaxf(0, t.radius - inset), (cui_color){0});
        icon(e, m->type == MEDIA_VIDEO ? PLAY : PHOTO,
             (cui_rect){r.w / 2 - 10, height / 2 - 10, 20, 20}, t.muted);
    }
    if (m->type == MEDIA_VIDEO || active) {
        bool right = active;
        float x = right ? r.w - 27 : 7;
        box(e, (cui_rect){x, 7, 20, 20}, active ? t.primary : (cui_color){13, 14, 15, 220}, 4, (cui_color){0});
        icon(e, active ? CHECK : PLAY, (cui_rect){x + 3, 10, 14, 14}, active ? t.on_primary : rgb(0xffffff));
    }
    text_at(e, (cui_rect){8, height + 2, r.w - 16, 22}, media_title(m), t.font_size - 2,
            good ? t.text : t.muted, false, false, false);
    cui_end_box(e->ui);
    if (click.activated && !selected) { e->selected = index; e->message[0] = '\0'; e->dirty = true; }
}

static bool
matches(const Editor *e, size_t index)
{
    if (index >= e->library.count) return false;
    const Media *m = &e->library.items[index];
    return media_matches(m, e->filter, e->search_text) &&
           (!e->suitable_only || media_eligible(&e->cfg, &e->layout, m, NULL));
}

static void
match_selection(Editor *e)
{
    if (matches(e, e->selected)) return;
    e->selected = SIZE_MAX;
    for (size_t i = 0; i < e->library.count; ++i)
        if (matches(e, i)) { e->selected = i; break; }
}

static void
browse(Editor *e)
{
    cui_theme t = e->theme;
    const float margin = 24, width = e->w - 2 * margin;
    const float preview_w = fmaxf(width * .62f, width - 290);
    const float panel_x = margin + preview_w + 22, panel_w = e->w - margin - panel_x;
    match_selection(e);
    Media *m = e->selected < e->library.count ? &e->library.items[e->selected] : NULL;
    char reason[ERROR_SIZE] = "";
    bool ready = m && media_eligible(&e->cfg, &e->layout, m, reason);
    bool active = m && !strcmp(m->path, e->active);
    const float reason_line = t.font_size + 4;
    const float reason_h = (panel_w < 200 ? 4 : 3) * reason_line;
    const float preview_h = fmaxf(fminf(258, fmaxf(145, e->h * .37f)), m && !ready ? 168 + reason_h : 0);
    const float top = 70, bar = top + preview_h + 16;
    const float settings_w = fmaxf(124, cui_measure_text(e->ui, cui_string("Settings…"), t.font_size).width + 42);
    const float effects_w = fmaxf(104, cui_measure_text(e->ui, cui_string("Effects…"), t.font_size).width + 42);
    icon(e, MONITOR, (cui_rect){margin, 24, 22, 22}, t.text);
    text_at(e, (cui_rect){margin + 31, 18, 240, 34}, "Desktop", t.font_size + 8, t.text, false, true, false);
    if (button(e, "effects", "Effects…", (cui_rect){e->w - margin - settings_w - effects_w - 6, 17, effects_w, 36},
               SPARKLES, GHOST, false)) {
        navigate(e, EFFECTS);
        return;
    }
    if (button(e, "settings", "Settings…", (cui_rect){e->w - margin - settings_w, 17, settings_w, 36},
               SETTINGS, GHOST, false)) {
        navigate(e, PREFERENCES);
        return;
    }
    preview(e, m, (cui_rect){margin, top, preview_w, preview_h});
    if (m) {
        const char *tag = active ? "CURRENT WALLPAPER" : m->type == MEDIA_VIDEO ? "VIDEO" : "IMAGE";
        text_at(e, (cui_rect){panel_x, top + 7, panel_w, 18}, tag, t.font_size - 3,
                active ? t.primary : t.muted, true, true, false);
        text_at(e, (cui_rect){panel_x, top + 29, panel_w, 27}, media_title(m), t.font_size + 4, t.text, false, true, false);
        if (m->w && m->h) {
            char dimensions[96];
            if (m->type == MEDIA_VIDEO && media_speed(m) != 1)
                snprintf(dimensions, sizeof dimensions, "%.3g× · %d × %d", media_speed(m), m->w, m->h);
            else snprintf(dimensions, sizeof dimensions, "%d × %d", m->w, m->h);
            text_at(e, (cui_rect){panel_x, top + 59, panel_w, 20}, dimensions,
                    t.font_size, t.muted, true, false, false);
        }
        if (!ready)
            wrapped_text_at(e, (cui_rect){panel_x, top + 84, panel_w, reason_h}, reason,
                            t.font_size - 1, reason_line, t.muted);
        else if (m->package && *m->package->tags && preview_h >= 189)
            text_at(e, (cui_rect){panel_x, top + 84, panel_w, 22}, m->package->tags,
                    t.font_size - 1, t.muted, false, false, false);
    } else if (!e->library.count) {
        text_at(e, (cui_rect){panel_x, top + 20, panel_w, 28}, e->worker.busy ? "Loading wallpapers" : "No wallpapers",
                t.font_size + 4, t.text, false, true, false);
        text_at(e, (cui_rect){panel_x, top + 53, panel_w, 22}, "Choose a folder in Settings.",
                t.font_size, t.muted, false, false, false);
    }
    if (m && !active && button(e, "apply", "Set wallpaper", (cui_rect){panel_x, top + preview_h - 80, panel_w, 36},
                                CHECK, PRIMARY, !ready))
        apply_selected(e);
    if (button(e, "shuffle", "Shuffle", (cui_rect){panel_x, top + preview_h - 39, panel_w, 36},
               SHUFFLE, active || !m ? PRIMARY : OUTLINED, !e->library.count))
        shuffle_now(e);

    static const char *const filters[] = {"All", "Images", "Videos"};
    float filter_x = margin;
    for (size_t i = 0; i < sizeof filters / sizeof *filters; ++i) {
        const char *label = filters[i];
        float button_w = fmaxf(44, ceilf(cui_measure_text(e->ui, cui_string(label), t.font_size).width) + 20);
        if (button(e, label, label, (cui_rect){filter_x, bar + 2, button_w, 30},
                   ICONS, e->filter == (MediaType)i ? ACTIVE : GHOST, false)) {
            e->filter = (MediaType)i;
            e->page = 0;
            e->dirty = true;
        }
        filter_x += button_w + 4;
    }
    const float pager_x = e->w - margin - 113;
    const float search_w = fminf(232, fminf(width * .37f, pager_x - filter_x - 8));
    const float search_x = pager_x - search_w - 12;
    const cui_str suitable_label = cui_string("Hide unsuitable");
    const float suitable_w = ceilf(cui_measure_text(e->ui, suitable_label, t.font_size).width) + 34;
    const bool suitable_in_toolbar = search_x - filter_x >= suitable_w + 16;
    const cui_rect suitable_rect = suitable_in_toolbar ? (cui_rect){filter_x + 8, bar + 2, suitable_w, 30} :
                                                       (cui_rect){e->w - margin - suitable_w, e->h - 34, suitable_w, 24};
    cui_begin_absolute_box(e->ui, 0, (cui_box){.align_y = CUI_ALIGN_CENTER}, suitable_rect);
    cui_id suitable_id = cui_id_from(e->ui, "hide unsuitable");
    if (cui_checkbox(e->ui, suitable_id, &t, suitable_label, &e->suitable_only, false)) {
        e->page = 0;
        e->dirty = true;
    }
    cui_set_cursor(e->ui, suitable_id, CUI_CURSOR_POINTER);
    cui_end_box(e->ui);
    char prior_search[sizeof e->search_text];
    strcpy(prior_search, e->search_text);
    input(e, "Find a wallpaper", (cui_rect){search_x, bar + 2, search_w, 30},
          e->search, &e->search_input, "Find a wallpaper");
    if (strcmp(prior_search, e->search_text)) { e->page = 0; e->dirty = true; }
    int columns = e->w >= 810 ? 6 : e->w >= 660 ? 5 : 4;
    int rows = e->h >= 565 ? 2 : 1;
    size_t step = (size_t)(columns * rows);
    size_t count = 0;
    for (size_t i = 0; i < e->library.count; ++i)
        if (matches(e, i)) ++count;
    e->pages = count ? (count - 1) / step + 1 : 1;
    if (e->page >= e->pages) e->page = e->pages - 1;
    if (button(e, "page previous", "Previous page", (cui_rect){pager_x, bar + 2, 30, 30},
               PREVIOUS, GHOST, e->page == 0)) { --e->page; e->dirty = true; }
    if (button(e, "page next", "Next page", (cui_rect){pager_x + 83, bar + 2, 30, 30},
               NEXT, GHOST, e->page + 1 >= e->pages)) { ++e->page; e->dirty = true; }
    char page[32];
    snprintf(page, sizeof page, "%u / %u", (unsigned)(e->page + 1), (unsigned)e->pages);
    text_at(e, (cui_rect){pager_x + 30, bar + 2, 53, 30}, page, t.font_size - 2, t.muted, true, false, true);
    const float grid_y = bar + 43, gap = 9;
    const float card_w = floorf((width - (columns - 1) * gap) / columns);
    const float card_h = fminf(136, floorf((e->h - grid_y - 41 - (rows - 1) * gap) / rows));
    size_t seen = 0;
    e->visible_count = 0;
    for (size_t i = 0; i < e->library.count && e->visible_count < step; ++i) {
        if (!matches(e, i)) continue;
        if (seen++ < e->page * step) continue;
        size_t cell = e->visible_count++;
        e->visible[cell] = (unsigned)i;
        tile(e, i, (cui_rect){margin + (float)(cell % (size_t)columns) * (card_w + gap),
                            grid_y + (float)(cell / (size_t)columns) * (card_h + gap), card_w, card_h});
    }
    if (!count && e->library.count)
        text_at(e, (cui_rect){margin, grid_y + 20, width, 28}, "No matching wallpapers", t.font_size, t.muted, false, false, true);
    if (*e->message) {
        float message_w = suitable_in_toolbar ? width : width - suitable_w - 12;
        text_at(e, (cui_rect){margin, e->h - 34, message_w, 22}, e->message, t.font_size - 1, t.muted, false, false, false);
    }
}

static void
save_preferences(Editor *e)
{
    bool effects = e->screen == EFFECTS;
    if (!effects && !*e->folder_text) {
        snprintf(e->message, sizeof e->message, "Enter a wallpaper folder.");
        e->dirty = true;
        return;
    }
    Config *c = &e->edit_cfg;
    Setting preferences[] = {
        {"library.directory", e->folder_text},
        {"rotation.mode", rotation_names[c->rotation]},
        {"rotation.minutes", e->minutes_text}
    };
    Setting motion[] = {
        {"blur.enabled", c->blur ? "true" : "false"},
        {"parallax.enabled", c->parallax ? "true" : "false"},
        {"parallax.empty_only", c->parallax_empty ? "true" : "false"},
        {"video.empty_only", c->video_empty ? "true" : "false"}
    };
    char error[ERROR_SIZE] = "";
    size_t count = effects ? sizeof motion / sizeof *motion : c->rotation == ROT_INTERVAL ? 3 : 2;
    if (!config_write(e->paths, effects ? motion : preferences, count, error) ||
        !config_read(e->paths, &e->cfg, error)) {
        snprintf(e->message, sizeof e->message, "%s", error);
        e->dirty = true;
        return;
    }
    navigate(e, BROWSE);
    if (!effects) { e->page = 0; e->scan = true; }
    if (action(e, "reload", NULL))
        snprintf(e->message, sizeof e->message, "%s saved.", effects ? "Effects" : "Settings");
}

static bool
switch_at(Editor *e, cui_rect r, const char *title, bool *value, size_t index, bool disabled)
{
    static const char *const ids[SWITCH_COUNT] = {"blur", "parallax", "parallax-empty", "video-empty"};
    cui_begin_absolute_box(e->ui, 0, (cui_box){.align_y = CUI_ALIGN_CENTER}, r);
    cui_id id = cui_id_from(e->ui, ids[index]);
    cui_switch_options options = cui_switch_options_default();
    options.size = index == BLUR_SWITCH || index == PARALLAX_SWITCH ? CUI_CONTROL_LARGE : CUI_CONTROL_MEDIUM;
    options.width = r.w;
    options.label_left = true;
    options.disabled = disabled;
    options.now_seconds = (double)now_ms() / 1000;
    options.reduced_motion = !e->cfg.animation_ms;
    bool changed = cui_switch_ex(e->ui, id, &e->theme, cui_string(title), value, &e->switches[index], options);
    if (!disabled) cui_set_cursor(e->ui, id, CUI_CURSOR_POINTER);
    cui_end_box(e->ui);
    return changed;
}

static bool
preferences_header(Editor *e, const char *title)
{
    if (button(e, "back", "Back to wallpapers", (cui_rect){24, 17, 35, 36}, BACK, GHOST, false)) {
        navigate(e, BROWSE);
        return false;
    }
    text_at(e, (cui_rect){67, 16, e->w - 92, 38}, title, e->theme.font_size + 8, e->theme.text, false, true, false);
    return true;
}

static void
preferences_footer(Editor *e, float margin, float width, const char *label)
{
    float button_w = fmaxf(140, cui_measure_text(e->ui, cui_string(label), e->theme.font_size).width + 42);
    if (*e->message)
        text_at(e, (cui_rect){margin, e->h - 55, width - button_w - 9, 35}, e->message,
                e->theme.font_size - 1, e->theme.muted, false, false, false);
    if (button(e, "save", label, (cui_rect){e->w - margin - button_w, e->h - 54, button_w, 36}, CHECK, PRIMARY, false))
        save_preferences(e);
}

static void
preferences(Editor *e)
{
    cui_theme t = e->theme;
    const float margin = fmaxf(24, (e->w - 580) / 2.0f), width = e->w - 2 * margin;
    const float gap = fminf(18, fmaxf(3, (e->h - 460) / 9.0f));
    const float top = 66, rotation = top + 72 + gap;
    if (!preferences_header(e, "Wallpaper settings")) return;
    text_at(e, (cui_rect){margin, top, width, 24}, "Wallpaper folder", t.font_size, t.text, false, true, false);
    input(e, "Wallpaper folder", (cui_rect){margin, top + 29, width, 36}, e->folder, &e->folder_input, "/path/to/wallpapers");

    box(e, (cui_rect){margin, rotation - 3, width, 1}, t.border, 0, (cui_color){0});
    text_at(e, (cui_rect){margin, rotation + 5, width, 22}, "Change wallpaper", t.font_size, t.text, false, true, false);
    static const char *const modes[] = {"Manually", "At login", "Every N minutes", "Daily"};
    const float segment = (width - 12) / 4;
    for (int i = 0; i < 4; ++i)
        if (button(e, modes[i], modes[i], (cui_rect){margin + i * (segment + 4), rotation + 34, segment, 33},
                   ICONS, (int)e->edit_cfg.rotation == i ? ACTIVE : OUTLINED, false)) {
            e->edit_cfg.rotation = (RotationMode)i;
            e->dirty = true;
        }
    if (e->edit_cfg.rotation == ROT_INTERVAL) {
        input(e, "Interval in minutes", (cui_rect){margin, rotation + 73, 76, 31}, e->minutes, &e->minutes_input, "30");
        text_at(e, (cui_rect){margin + 86, rotation + 73, width - 86, 31}, "minutes",
                t.font_size, t.muted, false, false, false);
    }

    preferences_footer(e, margin, width, "Save settings");
}

static void
effects(Editor *e)
{
    if (!preferences_header(e, "Wallpaper effects")) return;
    const cui_theme t = e->theme;
    const float margin = fmaxf(24, (e->w - 580) / 2.0f), width = e->w - 2 * margin;
    const float blur_y = 78, parallax_y = blur_y + 88, video_y = parallax_y + 90;
    e->dirty |= switch_at(e, (cui_rect){margin, blur_y, width, 34}, "Wallpaper blur",
                          &e->edit_cfg.blur, BLUR_SWITCH, false);
    text_at(e, (cui_rect){margin, blur_y + 34, width, 29}, "Softens the wallpaper behind open windows.",
            t.font_size - 1, t.muted, false, false, false);

    box(e, (cui_rect){margin, parallax_y - 10, width, 1}, t.border, 0, (cui_color){0});
    e->dirty |= switch_at(e, (cui_rect){margin, parallax_y, width, 34}, "Pointer parallax",
                          &e->edit_cfg.parallax, PARALLAX_SWITCH, false);
    e->dirty |= switch_at(e, (cui_rect){margin + 16, parallax_y + 35, width - 16, 34}, "Only on an empty desktop",
                          &e->edit_cfg.parallax_empty, PARALLAX_EMPTY_SWITCH, !e->edit_cfg.parallax);

    box(e, (cui_rect){margin, video_y - 10, width, 1}, t.border, 0, (cui_color){0});
    text_at(e, (cui_rect){margin, video_y, width, 34}, "Video playback", t.font_size, t.text, false, true, false);
    e->dirty |= switch_at(e, (cui_rect){margin + 16, video_y + 35, width - 16, 34}, "Only on an empty desktop",
                          &e->edit_cfg.video_empty, VIDEO_EMPTY_SWITCH, false);
    preferences_footer(e, margin, width, "Save effects");
}

static bool
resize_pixels(Editor *e)
{
    if (e->pixels && e->pixels->width == e->w && e->pixels->height == e->h) return true;
    if (e->pixels) XDestroyImage(e->pixels);
    e->pixels = XCreateImage(e->x.d, e->x.visual, (unsigned)e->x.depth, ZPixmap, 0, NULL,
                             (unsigned)e->w, (unsigned)e->h, 32, 0);
    if (!e->pixels) return false;
    e->pixels->data = malloc((size_t)e->pixels->bytes_per_line * e->h);
    return e->pixels->data && cui_software_resize(e->software, (unsigned)e->w, (unsigned)e->h);
}

static unsigned long
component(unsigned char c, unsigned long mask)
{
    if (!mask) return 0;
    unsigned q = (unsigned)__builtin_ctzl(mask);
    return ((c * (mask >> q) + 127) / 255) << q;
}

static void
present(Editor *e, cui_draw_list list)
{
    if (!resize_pixels(e) || !cui_software_render(e->software, &list, e->theme.background)) {
        if (getenv("DESKTOP_DEBUG_UI")) fprintf(stderr, "Could not render the frame.\n");
        return;
    }
    const unsigned char *rgba = cui_software_pixels(e->software, NULL, NULL);
    XImage *pixels = e->pixels;
    bool bgra = pixels->bits_per_pixel == 32 && pixels->byte_order == LSBFirst &&
                pixels->red_mask == 0xff0000 && pixels->green_mask == 0xff00 && pixels->blue_mask == 0xff;
    for (int y = 0; y < e->h; ++y) {
        unsigned char *line = (unsigned char *)pixels->data + (size_t)y * pixels->bytes_per_line;
        for (int x = 0; x < e->w; ++x, rgba += 4) {
            if (bgra) {
                line[x * 4] = rgba[2];
                line[x * 4 + 1] = rgba[1];
                line[x * 4 + 2] = rgba[0];
                line[x * 4 + 3] = 0;
            } else XPutPixel(pixels, x, y, component(rgba[0], pixels->red_mask) |
                         component(rgba[1], pixels->green_mask) | component(rgba[2], pixels->blue_mask));
        }
    }
    XPutImage(e->x.d, e->window, e->gc, pixels, 0, 0, 0, 0, (unsigned)e->w, (unsigned)e->h);
    cui_cursor cursor = cui_pointer_cursor(e->ui);
    if (cursor != e->shown_cursor && cursor <= CUI_CURSOR_HELP) {
        static const unsigned glyphs[] = {XC_left_ptr, XC_hand2, XC_xterm, XC_sb_h_double_arrow,
            XC_sb_v_double_arrow, XC_fleur, XC_X_cursor, XC_crosshair, XC_hand1, XC_hand2,
            XC_plus, XC_circle, XC_watch, XC_question_arrow};
        if (!e->cursor[cursor]) e->cursor[cursor] = XCreateFontCursor(e->x.d, glyphs[cursor]);
        XDefineCursor(e->x.d, e->window, e->cursor[cursor]);
        e->shown_cursor = cursor;
    }
    XFlush(e->x.d);
}

static void
frame(Editor *e, uint64_t now)
{
    e->dirty = false;
    if (!cui_begin_frame(e->ui, (float)e->w, (float)e->h, (float)(now - e->last_frame) / 1000)) return;
    e->last_frame = now;
    float t = e->cfg.animation_ms && e->entered ? fminf(1, (float)(now - e->entered) / e->cfg.animation_ms) : 1;
    float ease = 1 - (1 - t) * (1 - t) * (1 - t);
    if (t < 1) e->dirty = true;
    cui_begin_box(e->ui, 0, (cui_box){.width = (float)e->w, .height = (float)e->h});
    cui_begin_absolute_box(e->ui, 0, (cui_box){.opacity = .60f + .4f * ease, .custom_opacity = t < 1},
                           (cui_rect){0, 3 * (1 - ease), (float)e->w, (float)e->h});
    if (e->screen == BROWSE) browse(e);
    else if (e->screen == PREFERENCES) preferences(e);
    else effects(e);
    cui_end_box(e->ui);
    cui_end_box(e->ui);
    cui_draw_list list = cui_end_frame(e->ui);
    if (getenv("DESKTOP_DEBUG_UI"))
        fprintf(stderr, "frame: %zu commands, error %d\n", list.count, cui_last_error(e->ui));
    present(e, list);
}

static unsigned
modifiers(unsigned mask)
{
    return ((mask & ShiftMask) ? CUI_MOD_SHIFT : 0) | ((mask & ControlMask) ? CUI_MOD_CTRL : 0) |
           ((mask & Mod1Mask) ? CUI_MOD_ALT : 0) | ((mask & Mod4Mask) ? CUI_MOD_META : 0);
}

static cui_key
key_for(KeySym symbol)
{
    switch (symbol) {
    case XK_Tab: case XK_ISO_Left_Tab: return CUI_KEY_TAB;
    case XK_Return: case XK_KP_Enter: return CUI_KEY_ENTER;
    case XK_space: return CUI_KEY_SPACE;
    case XK_Escape: return CUI_KEY_ESCAPE;
    case XK_Left: return CUI_KEY_LEFT;
    case XK_Right: return CUI_KEY_RIGHT;
    case XK_Up: return CUI_KEY_UP;
    case XK_Down: return CUI_KEY_DOWN;
    case XK_Home: return CUI_KEY_HOME;
    case XK_End: return CUI_KEY_END;
    case XK_BackSpace: return CUI_KEY_BACKSPACE;
    case XK_Delete: return CUI_KEY_DELETE;
    case XK_Page_Up: return CUI_KEY_PAGEUP;
    case XK_Page_Down: return CUI_KEY_PAGEDOWN;
    default: return symbol <= 127 ? cui_key_from_character((char)symbol) : CUI_KEY_NONE;
    }
}

static bool
clipboard_set(void *user, cui_str text)
{
    Editor *e = user;
    if (text.length >= sizeof e->clipboard_text) return false;
    memcpy(e->clipboard_text, text.data, text.length);
    e->clipboard_text[text.length] = '\0';
    XSetSelectionOwner(e->x.d, e->clipboard, e->window, CurrentTime);
    return XGetSelectionOwner(e->x.d, e->clipboard) == e->window;
}

static void
selection_request(Editor *e, XSelectionRequestEvent *r)
{
    XSelectionEvent response = {.type = SelectionNotify, .display = r->display, .requestor = r->requestor,
        .selection = r->selection, .target = r->target, .time = r->time};
    Atom dest = r->property ? r->property : r->target;
    if (r->target == e->targets) {
        Atom supported[] = {e->targets, e->utf8, XA_STRING};
        XChangeProperty(e->x.d, r->requestor, dest, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)supported, 3);
        response.property = dest;
    } else if (r->target == e->utf8 || r->target == XA_STRING) {
        XChangeProperty(e->x.d, r->requestor, dest, r->target, 8, PropModeReplace,
                        (unsigned char *)e->clipboard_text, (int)strlen(e->clipboard_text));
        response.property = dest;
    }
    XEvent notify = {.xselection = response};
    XSendEvent(e->x.d, r->requestor, False, 0, &notify);
}

static void
selection_received(Editor *e, XSelectionEvent *r)
{
    if (!r->property) {
        if (r->target == e->utf8)
            XConvertSelection(e->x.d, e->clipboard, XA_STRING, e->paste, e->window, CurrentTime);
        return;
    }
    Atom kind;
    int format;
    unsigned long count, more;
    unsigned char *data = NULL;
    if (XGetWindowProperty(e->x.d, e->window, r->property, 0, PATH_MAX / 4, True, AnyPropertyType,
                           &kind, &format, &count, &more, &data) == Success &&
        (kind == e->utf8 || kind == XA_STRING) && format == 8 && !more && count) {
        cui_queue_event(e->ui, (cui_event){.type = CUI_EVENT_TEXT, .text_data = (char *)data, .text_length = count});
        e->dirty = true;
    }
    if (data) XFree(data);
}

static void
key_event(Editor *e, XKeyEvent *xkey, bool down)
{
    KeySym sym = XLookupKeysym(xkey, 0);
    unsigned mod = modifiers(xkey->state);
    if (down && sym == XK_Escape) {
        if (e->screen != BROWSE) navigate(e, BROWSE);
        else e->finish = true;
        return;
    }
    if (down && (mod & CUI_MOD_CTRL) && sym == XK_v && cui_focused(e->ui)) {
        XConvertSelection(e->x.d, e->clipboard, e->utf8, e->paste, e->window, CurrentTime);
        return;
    }
    if (down && e->screen == BROWSE) {
        if (sym == XK_f && (mod & CUI_MOD_CTRL)) {
            cui_focus(e->ui, cui_id_from(e->ui, "Find a wallpaper"));
            e->dirty = true;
            return;
        }
        if (sym == XK_Page_Up && e->page) { --e->page; e->dirty = true; return; }
        if (sym == XK_Page_Down && e->page + 1 < e->pages) { ++e->page; e->dirty = true; return; }
    }
    cui_key key = key_for(sym);
    if (key)
        cui_queue_event(e->ui, (cui_event){.type = down ? CUI_EVENT_KEY_DOWN : CUI_EVENT_KEY_UP,
                                          .modifiers = mod, .key = key});
    if (down && !(mod & (CUI_MOD_CTRL | CUI_MOD_META))) {
        char buffer[128];
        int n;
        if (e->ic) {
            Status status;
            n = Xutf8LookupString(e->ic, xkey, buffer, sizeof buffer, &sym, &status);
            if (status == XBufferOverflow || (status != XLookupChars && status != XLookupBoth)) n = 0;
        } else n = XLookupString(xkey, buffer, sizeof buffer, &sym, NULL);
        if (n > 0 && (unsigned char)buffer[0] >= 32 && buffer[0] != 127)
            cui_queue_event(e->ui, (cui_event){.type = CUI_EVENT_TEXT, .text_data = buffer, .text_length = (size_t)n});
    }
    e->dirty = true;
}

static void
window_event(Editor *e, XEvent *ev)
{
    if (XFilterEvent(ev, e->window)) return;
    if (x11_layout_event(&e->x, ev)) {
        x11_layout(&e->x, &e->layout);
        e->dirty = true;
        return;
    }
    cui_event input_event = {0};
    switch (ev->type) {
    case Expose: e->dirty = true; return;
    case ConfigureNotify:
        if (ev->xconfigure.window == e->window) {
            e->w = ev->xconfigure.width;
            e->h = ev->xconfigure.height;
            e->dirty = true;
        }
        return;
    case PropertyNotify:
        if (ev->xproperty.window == e->x.root && ev->xproperty.atom == XA_RESOURCE_MANAGER) {
            read_theme(e, false);
            e->dirty = true;
        }
        return;
    case DestroyNotify: if (ev->xdestroywindow.window == e->window) e->finish = true; return;
    case ClientMessage: if ((Atom)ev->xclient.data.l[0] == e->wm_delete) e->finish = true; return;
    case SelectionRequest: selection_request(e, &ev->xselectionrequest); return;
    case SelectionNotify: selection_received(e, &ev->xselection); return;
    case KeyPress: case KeyRelease: key_event(e, &ev->xkey, ev->type == KeyPress); return;
    case FocusIn: if (e->ic) XSetICFocus(e->ic); return;
    case FocusOut:
        if (e->ic) XUnsetICFocus(e->ic);
        input_event.type = CUI_EVENT_CANCEL;
        break;
    case EnterNotify: input_event.type = CUI_EVENT_POINTER_ENTER; break;
    case LeaveNotify: input_event.type = CUI_EVENT_POINTER_LEAVE; break;
    case MotionNotify:
        input_event = (cui_event){.type = CUI_EVENT_POINTER_MOVE, .x = ev->xmotion.x, .y = ev->xmotion.y};
        break;
    case ButtonPress: case ButtonRelease: {
        unsigned b = ev->xbutton.button;
        if (b == Button4 || b == Button5) {
            if (ev->type == ButtonPress && e->screen == BROWSE && ev->xbutton.y >= 220) {
                if (b == Button4 && e->page) --e->page;
                if (b == Button5 && e->page + 1 < e->pages) ++e->page;
                e->dirty = true;
            }
            return;
        }
        if (b != Button1) return;
        input_event = (cui_event){.type = ev->type == ButtonPress ? CUI_EVENT_POINTER_DOWN : CUI_EVENT_POINTER_UP,
            .x = ev->xbutton.x, .y = ev->xbutton.y, .modifiers = modifiers(ev->xbutton.state)};
        break;
    }
    default: return;
    }
    cui_queue_event(e->ui, input_event);
    e->dirty = true;
}

static bool
setup_editor(Editor *e)
{
    if (!e->font) return false;
    e->ui = cui_create((cui_config){.width = (float)e->w, .height = (float)e->h,
        .measure = cui_font_measure, .measure_styled = cui_font_measure_styled,
        .measure_user = e->font, .font_metrics = cui_font_get_metrics});
    e->software = cui_software_create(1, 1, e->font);
    e->search = cui_edit_create((cui_edit_config){.buffer = e->search_text, .capacity = sizeof e->search_text});
    e->folder = cui_edit_create((cui_edit_config){.buffer = e->folder_text, .capacity = sizeof e->folder_text});
    e->minutes = cui_edit_create((cui_edit_config){.buffer = e->minutes_text, .capacity = sizeof e->minutes_text});
    return e->ui && e->software && e->search && e->folder && e->minutes;
}

static void
open_window(Editor *e)
{
    Display *d = e->x.d;
    int mx, my;
    x11_pointer(&e->x, &mx, &my);
    Output output = e->layout.items[0];
    for (size_t i = 0; i < e->layout.count; ++i) {
        Output o = e->layout.items[i];
        if (mx >= o.x && mx < o.x + o.w && my >= o.y && my < o.y + o.h) output = o;
    }
    e->w = (int)fmin(e->cfg.width, fmax(300, output.w - 32));
    e->h = (int)fmin(e->cfg.height, fmax(360, output.h - 70));
    e->window = XCreateSimpleWindow(d, e->x.root, output.x + (output.w - e->w) / 2,
        output.y + (output.h - e->h) / 2, (unsigned)e->w, (unsigned)e->h, 0, 0,
        (unsigned long)(e->theme.background.r << 16 | e->theme.background.g << 8 | e->theme.background.b));
    e->gc = XCreateGC(d, e->window, 0, NULL);
    x11_window_class(d, e->window, "desktop", "Desktop");
    XStoreName(d, e->window, "Desktop");
    Atom dialog = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    XChangeProperty(d, e->window, e->x.type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&dialog, 1);
    e->wm_delete = XInternAtom(d, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(d, e->window, &e->wm_delete, 1);
    XSizeHints hints = {.flags = PMinSize | PMaxSize,
        .min_width = e->w < 560 ? e->w : 560, .min_height = e->h < 480 ? e->h : 480,
        .max_width = 1800, .max_height = 1200};
    XSetWMNormalHints(d, e->window, &hints);
    XSelectInput(d, e->window, ExposureMask | StructureNotifyMask | PointerMotionMask |
                 ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask | FocusChangeMask |
                 EnterWindowMask | LeaveWindowMask);
    XSelectInput(d, e->x.root, StructureNotifyMask | PropertyChangeMask);
    e->clipboard = XInternAtom(d, "CLIPBOARD", False);
    e->utf8 = XInternAtom(d, "UTF8_STRING", False);
    e->targets = XInternAtom(d, "TARGETS", False);
    e->paste = XInternAtom(d, "_DESKTOP_PASTE", False);
    e->instance = XInternAtom(d, "_DESKTOP_SETTINGS_INSTANCE", False);
    XSetSelectionOwner(d, e->instance, e->window, CurrentTime);
    Bool repeat;
    XkbSetDetectableAutoRepeat(d, True, &repeat);
    e->im = XOpenIM(d, NULL, NULL, NULL);
    if (e->im)
        e->ic = XCreateIC(e->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                          XNClientWindow, e->window, XNFocusWindow, e->window, NULL);
    e->platform = (cui_platform){.user = e, .capabilities = CUI_PLATFORM_CLIPBOARD, .clipboard_set = clipboard_set};
}

int
ui_run(const Paths *paths)
{
    Editor *e = calloc(1, sizeof *e);
    if (!e) return 1;
    e->paths = paths;
    e->selected = e->previewed = SIZE_MAX;
    setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");
    char error[ERROR_SIZE] = "";
    bool worker_ready = false;
    int rc = 1;
    if (!config_read(paths, &e->cfg, error) || !x11_open(&e->x, error)) goto done;
    Atom singleton = XInternAtom(e->x.d, "_DESKTOP_SETTINGS_INSTANCE", False);
    Window existing = XGetSelectionOwner(e->x.d, singleton);
    if (existing) {
        Atom active = XInternAtom(e->x.d, "_NET_ACTIVE_WINDOW", False);
        XEvent event = {.xclient = {.type = ClientMessage, .display = e->x.d, .window = existing,
            .message_type = active, .format = 32, .data.l = {2, CurrentTime, 0, 0, 0}}};
        XSendEvent(e->x.d, e->x.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
        XRaiseWindow(e->x.d, existing);
        XFlush(e->x.d);
        rc = 0;
        goto done;
    }
    if (!x11_layout(&e->x, &e->layout)) goto done;
    read_theme(e, true);
    open_window(e);
    if (!setup_editor(e)) {
        snprintf(error, sizeof error, "Could not initialize the CUI editor.");
        goto done;
    }
    for (size_t i = 0; i < ICONS; ++i)
        for (size_t p = 0; p < ICON_SIZE * ICON_SIZE; ++p) {
            unsigned char *target = e->icon_pixels[i] + p * 4;
            target[0] = target[1] = target[2] = 255;
            target[3] = icons_start[i * ICON_SIZE * ICON_SIZE + p];
        }
    if (!(worker_ready = worker_start(&e->worker))) {
        snprintf(error, sizeof error, "Could not start the wallpaper preview worker.");
        goto done;
    }
    State state;
    if (state_read(paths, &state)) strcpy(e->active, state.path);
    e->entered = e->last_frame = now_ms();
    e->dirty = e->scan = true;
    XMapRaised(e->x.d, e->window);
    while (!e->finish) {
        unsigned processed = 0;
        while (processed < 64 && XPending(e->x.d)) {
            XEvent ev;
            XNextEvent(e->x.d, &ev);
            window_event(e, &ev);
            ++processed;
        }
        uint64_t now = now_ms();
        double wake_switch = 0;
        if (e->screen == EFFECTS) for (size_t i = 0; i < SWITCH_COUNT; ++i)
            if (e->switches[i].next_wake_seconds > 0 &&
                (!wake_switch || e->switches[i].next_wake_seconds < wake_switch))
                wake_switch = e->switches[i].next_wake_seconds;
        if (wake_switch && wake_switch * 1000 <= now) e->dirty = true;
        if (e->dirty && (now - e->last_frame >= 15 || processed)) frame(e, now);
        worker_schedule(e);
        if (e->finish) break;
        if (XPending(e->x.d)) continue;
        struct pollfd fd[] = {{ConnectionNumber(e->x.d), POLLIN, 0}, {e->worker.signal[0], POLLIN, 0}};
        int timeout = e->dirty ? 16 : wake_switch ? (int)fmax(1, wake_switch * 1000 - now) : -1;
        if (poll(fd, 2, timeout) > 0 && (fd[1].revents & POLLIN)) worker_collect(e);
    }
    rc = 0;
done:
    if (rc) fprintf(stderr, "desktop: %s\n", *error ? error : "Could not open the editor.");
    if (e->x.d && e->window) {
        XUnmapWindow(e->x.d, e->window);
        XFlush(e->x.d);
    }
    if (worker_ready) worker_end(&e->worker);
    if (e->ic) XDestroyIC(e->ic);
    if (e->im) XCloseIM(e->im);
    if (e->pixels) XDestroyImage(e->pixels);
    if (e->search) cui_edit_destroy(e->search);
    if (e->folder) cui_edit_destroy(e->folder);
    if (e->minutes) cui_edit_destroy(e->minutes);
    if (e->software) cui_software_destroy(e->software);
    if (e->ui) cui_destroy(e->ui);
    if (e->font) cui_font_close(e->font);
    if (e->mono) cui_font_close(e->mono);
    if (e->medium) cui_font_close(e->medium);
    image_free(&e->preview);
    library_free(&e->library);
    x11_close(&e->x);
    free(e);
    return rc;
}
