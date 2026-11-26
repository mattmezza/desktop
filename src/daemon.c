#include "x11.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    pid_t pid;
    int fd;
    Image frame;
    size_t bytes, progress;
    double credit, ramp;
    bool accelerating, failed;
} Player;

typedef struct {
    double amount, start, elapsed;
    bool occupied;
} BlurTransition;

typedef struct {
    const Paths *paths;
    Config cfg;
    X11 x;
    State state;
    Library library;
    Media current;
    Player players[MAX_OUTPUTS];
    double dx[MAX_OUTPUTS], dy[MAX_OUTPUTS], follow_x[MAX_OUTPUTS], follow_y[MAX_OUTPUTS];
    double parallax_speed[MAX_OUTPUTS], blur_radius[MAX_OUTPUTS];
    BlurTransition blur[MAX_OUTPUTS];
    uint64_t fade_start, layout_due, inspect_at, paint_at, last_tick, retry_at;
    uint32_t occupied;
    bool drifting, playing, blurring, blur_ready, blanked, logged_in, dirty, publish, stop;
    char notice[ERROR_SIZE];
} Daemon;

static volatile sig_atomic_t quitting;
static void
terminate(int signal_number)
{
    (void)signal_number;
    quitting = 1;
}

static bool
same_layout(const Layout *a, const Layout *b)
{
    if (a->w != b->w || a->h != b->h || a->count != b->count) return false;
    for (size_t i = 0; i < a->count; ++i) {
        const Output *u = &a->items[i], *v = &b->items[i];
        if (u->x != v->x || u->y != v->y || u->w != v->w || u->h != v->h || strcmp(u->name, v->name))
            return false;
    }
    return true;
}

static void
player_close(Player *p)
{
    if (p->pid > 0) close(p->fd);
    child_stop(p->pid);
    image_free(&p->frame);
    *p = (Player){0};
}

static void
clear_playback(Daemon *d)
{
    for (size_t i = 0; i < MAX_OUTPUTS; ++i) player_close(&d->players[i]);
    x11_clear_video(&d->x);
    d->playing = false;
}

static bool
player_open(Daemon *d, size_t i)
{
    Player *p = &d->players[i];
    if (p->pid > 0) return true;
    p->fd = video_spawn(&d->cfg, &d->current, &d->x.layout.items[i], &p->frame, &p->pid);
    if (p->fd >= 0) {
        p->bytes = (size_t)p->frame.w * p->frame.h * 4;
#ifdef F_SETPIPE_SZ
        (void)fcntl(p->fd, F_SETPIPE_SZ, 1024 * 1024);
#endif
        return true;
    }
    player_close(p);
    p->failed = true;
    snprintf(d->notice, sizeof d->notice, "Video playback could not start on %s.", d->x.layout.items[i].name);
    return false;
}

static void
player_read(Daemon *d, size_t i)
{
    Player *p = &d->players[i];
    while (p->progress < p->bytes) {
        ssize_t n = read(p->fd, p->frame.rgba + p->progress, p->bytes - p->progress);
        if (n > 0) { p->progress += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return;
        player_close(p);
        p->failed = true;
        snprintf(d->notice, sizeof d->notice, "Video decoder stopped on %s.", d->x.layout.items[i].name);
        return;
    }
    if (p->credit >= 1) d->paint_at = 0;
}

static void
read_library(Daemon *d)
{
    Library refreshed;
    char error[ERROR_SIZE] = "";
    if (!library_read(&d->cfg, &refreshed, error)) {
        library_free(&d->library);
        snprintf(d->notice, sizeof d->notice, "%s", *error ? error : "Could not scan the wallpaper folder.");
        return;
    }
    for (size_t i = 0; i < refreshed.count; ++i) {
        for (size_t j = 0; j < d->library.count; ++j) {
            Media *old = &d->library.items[j], *new = &refreshed.items[i];
            if (!strcmp(old->path, new->path)) { new->used = old->used; break; }
        }
    }
    library_free(&d->library);
    d->library = refreshed;
    d->notice[0] = '\0';
}

static Media *
lookup(Daemon *d, const char *path)
{
    for (size_t i = 0; i < d->library.count; ++i)
        if (!strcmp(path, d->library.items[i].path)) return &d->library.items[i];
    return NULL;
}

static bool
select_media(Daemon *d, Media *media, bool update_state, bool transition, char *error)
{
    if (!media_eligible(&d->cfg, &d->x.layout, media, error)) return false;
    Image picture;
    if (!media_decode(&d->cfg, media, 0, &picture, error)) {
        media->failed = true;
        return false;
    }
    bool fade = transition && d->current.path && d->cfg.crossfade_ms &&
                strcmp(media->path, d->current.path) != 0;
    char *path = strdup(media->path);
    if (!path) { image_free(&picture); snprintf(error, ERROR_SIZE, "Out of memory."); return false; }
    if (fade) x11_capture(&d->x);
    bool ok = x11_upload(&d->x, &picture, &d->cfg);
    image_free(&picture);
    if (!ok) {
        free(path);
        snprintf(error, ERROR_SIZE, "Could not upload the wallpaper pixels.");
        return false;
    }
    clear_playback(d);
    free(d->current.path);
    d->current = *media;
    d->current.path = path;
    d->current.thumb = (Image){0};
    media->used = true;
    memset(d->dx, 0, sizeof d->dx);
    memset(d->dy, 0, sizeof d->dy);
    memset(d->follow_x, 0, sizeof d->follow_x);
    memset(d->follow_y, 0, sizeof d->follow_y);
    d->fade_start = fade ? now_ms() : 0;
    d->dirty = d->publish = true;
    d->notice[0] = '\0';
    if (d->cfg.blur && d->cfg.blur_radius && !d->x.convolution)
        snprintf(d->notice, sizeof d->notice, "This X11 display does not support wallpaper blur.");
    if (update_state) {
        strcpy(d->state.path, path);
        d->state.changed = time(NULL);
        if (!state_write(d->paths, &d->state))
            snprintf(d->notice, sizeof d->notice, "Wallpaper applied. Could not save session state.");
    }
    return true;
}

static bool
select_path(Daemon *d, const char *path, bool save, bool transition, char *error)
{
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        snprintf(error, ERROR_SIZE, "Cannot open %.290s (%s).", path, strerror(errno));
        return false;
    }
    Media media;
    Media *entry = lookup(d, resolved);
    if (entry) return select_media(d, entry, save, transition, error);
    if (!media_probe(&d->cfg, resolved, &media, error)) return false;
    bool ok = select_media(d, &media, save, transition, error);
    free(media.path);
    return ok;
}

static size_t
random_below(size_t limit)
{
    uint64_t r;
    if (getrandom(&r, sizeof r, 0) != sizeof r)
        r = (uint64_t)random() ^ now_ms();
    return (size_t)(r % limit);
}

static Media *
choose(Daemon *d, bool fresh, bool allow_current)
{
    size_t candidates = 0;
    Media *selected = NULL;
    for (size_t i = 0; i < d->library.count; ++i) {
        Media *m = &d->library.items[i];
        if (fresh && m->used) continue;
        const char *active = d->current.path ? d->current.path : d->state.path;
        if (!allow_current && !strcmp(active, m->path)) continue;
        if (!media_eligible(&d->cfg, &d->x.layout, m, NULL)) continue;
        if (!random_below(++candidates)) selected = m;
    }
    return selected;
}

static bool
shuffle(Daemon *d, bool save, bool transition, char *error)
{
    for (;;) {
        Media *m = choose(d, d->cfg.no_repeat, false);
        if (!m) {
            for (size_t i = 0; i < d->library.count; ++i) d->library.items[i].used = false;
            m = choose(d, false, false);
        }
        if (!m) m = choose(d, false, true);
        if (!m) {
            snprintf(error, ERROR_SIZE, "%s", *d->notice ? d->notice :
                     "No wallpaper is large enough for all active displays.");
            return false;
        }
        if (select_media(d, m, save, transition, error)) return true;
        m->failed = true;
    }
}

static void
after_set(Daemon *d)
{
    if (d->current.path)
        hook_run(d->cfg.after_set, d->current.path, d->current.video ? "video" : "image", false);
}

static bool
reapply(Daemon *d, bool force, char *error)
{
    Layout layout;
    if (!x11_layout(&d->x, &layout)) {
        snprintf(error, ERROR_SIZE, "Could not query the X11 monitor layout.");
        return false;
    }
    bool unchanged = same_layout(&layout, &d->x.layout);
    if (!force && unchanged) {
        if (d->current.path && !x11_owns_background(&d->x)) d->dirty = d->publish = true;
        return true;
    }
    if (!x11_scene(&d->x, &layout, &d->cfg)) {
        snprintf(error, ERROR_SIZE, "The combined monitor arrangement is too large.");
        return false;
    }
    clear_playback(d);
    d->fade_start = 0;
    d->inspect_at = 0;
    if (!unchanged) d->blur_ready = false;
    memset(d->dx, 0, sizeof d->dx);
    memset(d->dy, 0, sizeof d->dy);
    memset(d->follow_x, 0, sizeof d->follow_x);
    memset(d->follow_y, 0, sizeof d->follow_y);
    memset(d->parallax_speed, 0, sizeof d->parallax_speed);
    const char *wanted = *d->state.path ? d->state.path : d->current.path;
    if (wanted && d->current.path && !strcmp(wanted, d->current.path) && !force &&
        media_eligible(&d->cfg, &layout, &d->current, NULL)) {
        d->dirty = d->publish = true;
        return true;
    }
    if (wanted && select_path(d, wanted, false, false, error)) return true;
    if (wanted && shuffle(d, false, false, error)) return true;
    if (!wanted) return true;
    if (d->current.path) d->dirty = d->publish = true;
    return false;
}

static bool
rotation_due(const Daemon *d, time_t now)
{
    if (d->state.paused || d->state.inhibited || !d->logged_in) return false;
    if (d->cfg.rotation == ROT_INTERVAL)
        return !d->state.changed || difftime(now, d->state.changed) >= d->cfg.minutes * 60.0;
    if (d->cfg.rotation == ROT_DAILY) {
        struct tm day, last;
        if (!d->state.changed) return true;
        if (now < d->state.changed || !localtime_r(&now, &day) || !localtime_r(&d->state.changed, &last))
            return false;
        return day.tm_year != last.tm_year || day.tm_yday != last.tm_yday;
    }
    return false;
}

static void
maintain_rotation(Daemon *d, uint64_t now)
{
    if (now < d->retry_at || !rotation_due(d, time(NULL))) return;
    d->retry_at = now + 30000;
    read_library(d);
    char error[ERROR_SIZE];
    if (shuffle(d, true, true, error)) after_set(d);
    else snprintf(d->notice, sizeof d->notice, "%s", error);
}

static double
approach(double value, double target, double dt, int duration)
{
    if (!duration || fabs(target - value) <= 0.001) return target;
    double ratio = 1 - exp(-4.6 * dt / duration);
    return value + (target - value) * ratio;
}

static double
video_rate(const Player *p, int power)
{
    return p->accelerating ? pow(p->ramp, power) : 1 - pow(1 - p->ramp, power);
}

static double
video_time(Player *p, const Config *cfg, double delta, bool active)
{
    int duration = cfg->video_ramp_ms, power = cfg->video_ramp_power;
    if (!duration) {
        p->ramp = active ? 1 : 0;
        p->accelerating = active;
        return active ? delta : 0;
    }
    if (p->accelerating != active) {
        // Rebase the opposite curve at the current speed when a window toggles mid-ramp.
        double rate = video_rate(p, power), root = 1.0 / power;
        p->ramp = active ? pow(rate, root) : 1 - pow(1 - rate, root);
        p->accelerating = active;
        return rate * delta;
    }
    double phase = active ? p->ramp : 1 - p->ramp;
    double next = fmin(1, phase + delta / duration);
    p->ramp = active ? next : 1 - next;
    // Integrate the speed to get elapsed video time, including any time at full speed.
    double span = (next - phase) * duration;
    double area = (pow(next, power + 1) - pow(phase, power + 1)) * duration / (power + 1);
    return active ? area + fmax(0, delta - span) : fmax(0, span - area);
}

static bool
advance_video(Daemon *d, size_t i, double delta, bool active, bool suspended)
{
    Player *p = &d->players[i];
    if (p->failed) return false;
    if (suspended) {
        p->ramp = 0;
        p->credit = 0;
        return false;
    }
    double elapsed = video_time(p, &d->cfg, delta, active);
    if (!active && p->ramp == 0) {
        p->credit = fmin(p->credit, 0.99);
        return false;
    }
    if (!player_open(d, i)) return false;
    p->credit = fmin(1.5, p->credit + elapsed * d->cfg.video_fps / 1000);
    // Keep one decoded frame ready so the ramp can control presentation timing.
    if (p->credit >= 1 && p->progress == p->bytes) {
        if (!x11_video_frame(&d->x, i, &p->frame, &d->cfg)) {
            player_close(p);
            p->failed = true;
            snprintf(d->notice, sizeof d->notice, "Could not upload the video frame for %s.", d->x.layout.items[i].name);
            return false;
        }
        p->progress = 0;
        p->credit -= 1;
    }
    return true;
}

static bool
advance_blur(Daemon *d, size_t i, double delta, bool occupied, bool suspended)
{
    const Config *cfg = &d->cfg;
    bool enabled = cfg->blur && cfg->blur_radius && d->x.convolution;
    occupied &= enabled;
    double target = occupied ? 1 : 0;
    BlurTransition *b = &d->blur[i];
    int duration = cfg->blur_transition_ms;
    if (!enabled || !d->blur_ready || (!suspended && !duration))
        *b = (BlurTransition){.amount = target, .start = target, .occupied = occupied};
    else if (!suspended) {
        if (b->occupied != occupied) {
            // Start the reverse transition from the last radius that was drawn.
            b->occupied = occupied;
            b->start = b->amount;
            b->elapsed = 0;
        } else if (b->amount != target) {
            b->elapsed = fmin(duration, b->elapsed + delta);
            double change = pow(b->elapsed / duration, cfg->blur_easing_power);
            b->amount = occupied ? fmin(target, b->start + change) : fmax(target, b->start - change);
        }
    }
    d->blur_radius[i] = b->amount * cfg->blur_radius;
    return !suspended && b->amount != target;
}

static double
follow_pointer(double position, double *follow, double target, double phase, double retention)
{
    // Two cascaded low-pass stages respond gently to reversals without overshoot.
    double next = target + (position - target + phase * (*follow - target)) * retention;
    *follow = target + (*follow - target) * retention;
    if (fabs(*follow - target) < .025 && fabs(next - target) < .025)
        next = *follow = target;
    return next;
}

static bool
advance_parallax(Daemon *d, size_t i, double delta, int px, int py, bool active, bool suspended)
{
    const Output *o = &d->x.layout.items[i];
    const Config *cfg = &d->cfg;
    double speed = suspended ? 0 : approach(d->parallax_speed[i], active ? 1 : 0, delta, cfg->slowdown_ms);
    d->parallax_speed[i] = speed;
    bool inside = px >= o->x && py >= o->y && px < o->x + o->w && py < o->y + o->h;
    double nx = inside ? ((px - o->x) * 2.0 / o->w - 1) * cfg->travel * speed : 0;
    double ny = inside ? ((py - o->y) * 2.0 / o->h - 1) * cfg->travel * speed : 0;
    // 4.74 puts a stationary target at about 95% of the response by ease_ms.
    double phase = cfg->ease_ms ? 4.74 * delta / cfg->ease_ms : 0;
    double retention = cfg->ease_ms ? exp(-phase) : 0;
    double dx = follow_pointer(d->dx[i], &d->follow_x[i], nx, phase, retention);
    double dy = follow_pointer(d->dy[i], &d->follow_y[i], ny, phase, retention);
    if (cfg->parallax_max_speed) {
        double x_step = dx - d->dx[i], y_step = dy - d->dy[i];
        double distance = hypot(x_step, y_step), limit = cfg->parallax_max_speed * delta / 1000;
        if (distance > limit) {
            dx = d->dx[i] + x_step * (limit / distance);
            dy = d->dy[i] + y_step * (limit / distance);
        }
    }
    d->dx[i] = dx;
    d->dy[i] = dy;
    return speed > .001 || dx != 0 || dy != 0;
}

static void
draw(Daemon *d, uint64_t now, bool force)
{
    if (!d->current.path) return;
    double delta = fmin(now - d->last_tick, 250);
    d->last_tick = now;
    const Config *cfg = &d->cfg;
    bool video = d->current.video;
    bool blank = cfg->idle_pause && d->blanked;
    bool suspended = blank || d->state.inhibited;
    bool drift_enabled = cfg->parallax && cfg->travel && !suspended && !video;
    int px = -1, py = -1;
    if (drift_enabled || d->drifting) x11_pointer(&d->x, &px, &py);
    d->drifting = d->playing = d->blurring = false;
    for (size_t i = 0; i < d->x.layout.count; ++i) {
        bool occupied = d->occupied & (UINT32_C(1) << i);
        d->blurring |= advance_blur(d, i, delta, occupied, suspended);
        if (video) d->playing |= advance_video(d, i, delta, !cfg->video_empty || !occupied, suspended);
        bool drift_active = drift_enabled && (!cfg->parallax_empty || !occupied);
        d->drifting |= advance_parallax(d, i, delta, px, py, drift_active, suspended || video);
    }
    d->blur_ready = true;
    double fade = 0;
    if (d->fade_start) {
        double t = cfg->crossfade_ms ? (now - d->fade_start) / (double)cfg->crossfade_ms : 1;
        if (t >= 1 || blank || force) d->fade_start = 0;
        else fade = 1 - t * t * (3 - 2 * t);
        d->dirty = true;
    }
    x11_paint(&d->x, cfg, &d->current, d->dx, d->dy, d->blur_radius, fade, d->dirty || force);
    if (d->publish && !d->fade_start) {
        x11_publish(&d->x);
        d->publish = false;
    }
    d->dirty = false;
}

static void
status_json(Daemon *d, FILE *out)
{
    double parallax_speed = 0, video_speed = 0;
    for (size_t i = 0; i < d->x.layout.count; ++i) {
        parallax_speed = fmax(parallax_speed, d->parallax_speed[i]);
        video_speed = fmax(video_speed, video_rate(&d->players[i], d->cfg.video_ramp_power));
    }
    fputs("{\"schema\":1,\"wallpaper\":", out);
    if (d->current.path) json_string(out, d->current.path); else fputs("null", out);
    fputs(",\"selection\":", out);
    if (*d->state.path) json_string(out, d->state.path); else fputs("null", out);
    fputs(",\"kind\":", out);
    if (d->current.path) json_string(out, d->current.video ? "video" : "image"); else fputs("null", out);
    fprintf(out, ",\"paused\":%s,\"inhibited\":%s,\"occupied\":%s,\"blanked\":%s,"
                 "\"video_speed\":%.3f,\"parallax_speed\":%.3f,\"changed_at\":%lld,\"rotation\":",
            d->state.paused ? "true" : "false", d->state.inhibited ? "true" : "false",
            d->occupied ? "true" : "false", d->blanked ? "true" : "false",
            video_speed, parallax_speed, (long long)d->state.changed);
    json_string(out, rotation_names[d->cfg.rotation]);
    fprintf(out, ",\"minutes\":%d,\"outputs\":[", d->cfg.minutes);
    for (size_t i = 0; i < d->x.layout.count; ++i) {
        const Output *o = &d->x.layout.items[i];
        if (i) fputc(',', out);
        fputs("{\"name\":", out);
        json_string(out, o->name);
        fprintf(out, ",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,\"occupied\":%s,"
                     "\"parallax_speed\":%.3f,\"video_speed\":%.3f,\"blur_radius_px\":%.3f}",
                o->x, o->y, o->w, o->h, d->occupied & (UINT32_C(1) << i) ? "true" : "false",
                d->parallax_speed[i], video_rate(&d->players[i], d->cfg.video_ramp_power), d->blur_radius[i]);
    }
    fputs("],\"notice\":", out);
    if (*d->notice) json_string(out, d->notice); else fputs("null", out);
    fputs("}\n", out);
}

static void
list_media(Daemon *d, FILE *out, bool json)
{
    if (json) fputs("{\"schema\":1,\"wallpapers\":[", out);
    for (size_t i = 0; i < d->library.count; ++i) {
        Media *m = &d->library.items[i];
        char reason[ERROR_SIZE];
        bool ready = media_eligible(&d->cfg, &d->x.layout, m, reason);
        if (!json) {
            fprintf(out, "%s\t%s%s\n", m->path, ready ? "ready" : "", ready ? "" : reason);
            continue;
        }
        if (i) fputc(',', out);
        fputs("{\"path\":", out);
        json_string(out, m->path);
        fprintf(out, ",\"kind\":\"%s\",\"width\":%d,\"height\":%d,\"eligible\":%s,\"reason\":",
                m->video ? "video" : "image", m->w, m->h, ready ? "true" : "false");
        if (ready) fputs("null", out); else json_string(out, reason);
        fputc('}', out);
    }
    if (json) fputs("]}\n", out);
}

static bool
request(Daemon *d, const char *cmd, const char *arg, char *error)
{
    if (!strcmp(cmd, "stop")) { d->stop = true; return true; }
    if (!strcmp(cmd, "reapply")) return reapply(d, false, error);
    if (!strcmp(cmd, "reload")) {
        Config cfg;
        if (!config_read(d->paths, &cfg, error)) return false;
        d->cfg = cfg;
        read_library(d);
        bool ok = reapply(d, true, error);
        d->retry_at = 0;
        return ok;
    }
    if (!strcmp(cmd, "inhibit") || !strcmp(cmd, "uninhibit")) {
        d->state.inhibited = !strcmp(cmd, "inhibit");
        return true;
    }
    if (!strcmp(cmd, "pause") || !strcmp(cmd, "resume") || !strcmp(cmd, "toggle")) {
        bool paused = !strcmp(cmd, "toggle") ? !d->state.paused : !strcmp(cmd, "pause");
        State changed = d->state;
        changed.paused = paused;
        if (!state_write(d->paths, &changed)) {
            snprintf(error, ERROR_SIZE, "Could not save the rotation state.");
            return false;
        }
        d->state = changed;
        return true;
    }
    if (!strcmp(cmd, "set") && arg && *arg) {
        if (!select_path(d, arg, true, true, error)) return false;
        after_set(d);
        return true;
    }
    if (!strcmp(cmd, "next")) {
        read_library(d);
        if (!shuffle(d, true, true, error)) return false;
        after_set(d);
        return true;
    }
    if (!strcmp(cmd, "login")) {
        if (d->logged_in) return true;
        d->logged_in = true;
        if (!d->state.paused && (d->cfg.rotation == ROT_LOGIN || rotation_due(d, time(NULL))))
            if (!shuffle(d, true, false, error) && !d->current.path) return false;
        if (d->current.path) {
            draw(d, now_ms(), true);
            if (!hook_run(d->cfg.after_login, d->current.path, d->current.video ? "video" : "image", true)) {
                snprintf(error, ERROR_SIZE, "Wallpaper restored. The login hook exited with an error.");
                return false;
            }
        }
        return true;
    }
    snprintf(error, ERROR_SIZE, "Unknown command.");
    return false;
}

static void
serve(Daemon *d, int server)
{
    int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return;
    const struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    char buf[MAX_REQUEST];
    size_t n = 0;
    ssize_t got = 0;
    while (n < sizeof buf && (got = read(fd, buf + n, sizeof buf - n)) > 0)
        n += (size_t)got;
    if (got < 0 || !n || n == sizeof buf || buf[n - 1] != '\0') { close(fd); return; }
    FILE *out = fdopen(fd, "w");
    if (!out) { close(fd); return; }
    char *cmd = buf;
    size_t skip = strlen(cmd) + 1;
    char *arg = skip < n ? buf + skip : NULL;
    bool json = arg && !strcmp(arg, "--json");
    if (!strcmp(cmd, "status")) {
        fputs("0\n", out);
        if (json) status_json(d, out);
        else fprintf(out, "%s\n", d->current.path ? d->current.path : "");
    } else if (!strcmp(cmd, "list")) {
        fputs("0\n", out);
        list_media(d, out, json);
    } else {
        char error[ERROR_SIZE] = "";
        bool ok = request(d, cmd, arg, error);
        if (!ok) fprintf(out, "1\n%s\n", *error ? error : "Command failed.");
        else fputs("0\n", out);
    }
    fclose(out);
}

static int
bind_socket(const Paths *paths, int *lockfd, char *error)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(paths->socket) >= sizeof addr.sun_path || !make_parents(paths->socket)) {
        snprintf(error, ERROR_SIZE, "Runtime socket path is too long or inaccessible.");
        return -1;
    }
    *lockfd = open(paths->lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (*lockfd < 0 || flock(*lockfd, LOCK_EX | LOCK_NB)) {
        snprintf(error, ERROR_SIZE, "Another Desktop daemon owns this X11 session.");
        return -1;
    }
    strcpy(addr.sun_path, paths->socket);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        unlink(paths->socket);
        mode_t old = umask(0077);
        int result = bind(fd, (struct sockaddr *)&addr, sizeof addr);
        umask(old);
        if (!result && !listen(fd, 12)) return fd;
        close(fd);
    }
    snprintf(error, ERROR_SIZE, "Could not bind the Desktop socket (%s).", strerror(errno));
    return -1;
}

static void
events(Daemon *d, uint64_t now)
{
    while (XPending(d->x.d)) {
        XEvent ev;
        XNextEvent(d->x.d, &ev);
        if (x11_layout_event(&d->x, &ev)) d->layout_due = now + 180;
        if (ev.type == Expose && ev.xexpose.window == d->x.live && !ev.xexpose.count)
            d->dirty = true;
        if (ev.type == MapNotify || ev.type == UnmapNotify || ev.type == ConfigureNotify ||
            (ev.type == PropertyNotify && ev.xproperty.atom == d->x.clients))
            d->inspect_at = 0;
    }
}

int
daemon_run(const Paths *paths, int ready)
{
    Daemon d = {.paths = paths};
    char error[ERROR_SIZE] = "Could not initialize the wallpaper renderer.";
    int lockfd = -1, server = -1, rc = 1;
    bool owns_socket = false;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, terminate);
    signal(SIGTERM, terminate);
    if (!config_read(paths, &d.cfg, error) ||
        !x11_open(&d.x, error)) goto done;
    if ((server = bind_socket(paths, &lockfd, error)) < 0) goto done;
    owns_socket = true;
    state_read(paths, &d.state);
    read_library(&d);
    if (!reapply(&d, false, error)) snprintf(d.notice, sizeof d.notice, "%s", error);
    if (!d.x.canvas) goto done;
    d.occupied = x11_occupied(&d.x);
    d.blanked = x11_blanked(&d.x);
    d.last_tick = now_ms();
    draw(&d, d.last_tick, true);
    if (ready >= 0) { (void)write(ready, "0\n", 2); close(ready); ready = -1; }
    while (!quitting && !d.stop) {
        uint64_t now = now_ms();
        events(&d, now);
        if (d.layout_due && now >= d.layout_due) {
            d.layout_due = 0;
            if (!reapply(&d, false, error)) snprintf(d.notice, sizeof d.notice, "%s", error);
        }
        if (now >= d.inspect_at) {
            uint32_t occupied = x11_occupied(&d.x);
            bool blanked = x11_blanked(&d.x);
            if (occupied != d.occupied || blanked != d.blanked) d.paint_at = 0;
            d.occupied = occupied;
            d.blanked = blanked;
            d.inspect_at = now + 300;
            maintain_rotation(&d, now);
            while (waitpid(-1, NULL, WNOHANG) > 0) {}
        }
        if (now >= d.paint_at || d.dirty) {
            draw(&d, now, false);
            int delay = (d.playing || d.fade_start) ? 12 : d.blurring ? 16 : d.drifting ? 1000 / d.cfg.parallax_fps : 100;
            d.paint_at = now + (unsigned)delay;
        }
        uint64_t next = d.paint_at < d.inspect_at ? d.paint_at : d.inspect_at;
        if (d.layout_due && d.layout_due < next) next = d.layout_due;
        uint64_t clock = now_ms();
        struct pollfd p[2 + MAX_OUTPUTS] = {{server, POLLIN, 0}, {ConnectionNumber(d.x.d), POLLIN, 0}};
        size_t count = d.x.layout.count;
        for (size_t i = 0; i < count; ++i) {
            Player *player = &d.players[i];
            bool pending = player->pid > 0 && player->progress < player->bytes && player->ramp > 0;
            p[i + 2] = (struct pollfd){pending ? player->fd : -1, POLLIN, 0};
        }
        if (XPending(d.x.d)) continue;
        int wake = next > clock ? (int)(next - clock) : 0;
        if (poll(p, 2 + count, wake) > 0) {
            if (p[0].revents & POLLIN) serve(&d, server);
            for (size_t i = 0; i < count; ++i)
                if (p[i + 2].revents && d.players[i].pid > 0 && d.players[i].fd == p[i + 2].fd) player_read(&d, i);
        }
    }
    rc = 0;
done:
    if (rc && ready >= 0) { dprintf(ready, "1\n%s\n", error); close(ready); }
    else if (rc) fprintf(stderr, "desktop: %s\n", error);
    if (server >= 0) close(server);
    if (owns_socket) unlink(paths->socket);
    if (lockfd >= 0) close(lockfd);
    clear_playback(&d);
    library_free(&d.library);
    free(d.current.path);
    x11_close(&d.x);
    return rc;
}
