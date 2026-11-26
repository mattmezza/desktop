#include "desktop.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern const char defaults_start[], defaults_end[];

static int
connect_daemon(const Paths *p)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(p->socket) >= sizeof addr.sun_path) { errno = ENAMETOOLONG; return -1; }
    strcpy(addr.sun_path, p->socket);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (!connect(fd, (struct sockaddr *)&addr, sizeof addr)) return fd;
    close(fd);
    return -1;
}

bool
daemon_start(const Paths *p, char *error)
{
    int fd = connect_daemon(p);
    if (fd >= 0) { close(fd); return true; }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) goto failed;
    pid_t child = fork();
    if (child < 0) { close(pipefd[0]); close(pipefd[1]); goto failed; }
    if (!child) {
        close(pipefd[0]);
        if (setsid() < 0) _exit(1);
        pid_t daemon = fork();
        if (daemon != 0) _exit(daemon < 0);
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
            close(null);
        }
        _exit(daemon_run(p, pipefd[1]));
    }
    close(pipefd[1]);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    struct pollfd pollfd = {.fd = pipefd[0], .events = POLLIN};
    int status;
    do { status = poll(&pollfd, 1, 45000); } while (status < 0 && errno == EINTR);
    if (status > 0) {
        FILE *in = fdopen(pipefd[0], "r");
        if (in) {
            char line[8];
            bool ok = fgets(line, sizeof line, in) && line[0] == '0';
            if (!ok) {
                if (!fgets(error, ERROR_SIZE, in)) snprintf(error, ERROR_SIZE, "Desktop daemon exited at startup.");
                error[strcspn(error, "\n")] = '\0';
            }
            fclose(in);
            if (ok) return true;
            fd = connect_daemon(p);
            if (fd >= 0) { close(fd); return true; }
            return false;
        }
    }
    close(pipefd[0]);
    snprintf(error, ERROR_SIZE, "Timed out starting the Desktop daemon.");
    return false;
failed:
    snprintf(error, ERROR_SIZE, "Could not start the Desktop daemon (%s).", strerror(errno));
    return false;
}

int
command_run(const Paths *p, size_t argc, const char *const *argv, FILE *out, char *error)
{
    if (!argc || !argv[0]) return 1;
    int fd = connect_daemon(p);
    if (fd < 0 && strcmp(argv[0], "stop") && daemon_start(p, error))
        fd = connect_daemon(p);
    if (fd < 0) {
        if (!*error) snprintf(error, ERROR_SIZE, "The Desktop daemon is not running.");
        return 1;
    }
    for (size_t i = 0; i < argc; ++i) {
        const char *text = argv[i];
        size_t left = strlen(text) + 1;
        while (left) {
            ssize_t n = send(fd, text, left, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) goto failed;
            left -= (size_t)n;
            text += n;
        }
    }
    shutdown(fd, SHUT_WR);
    FILE *in = fdopen(fd, "r");
    if (!in) goto failed;
    char code[8], data[16384];
    int rc = 1;
    if (fgets(code, sizeof code, in) && code[0] == '0') {
        size_t n;
        while ((n = fread(data, 1, sizeof data, in)))
            if (out && fwrite(data, 1, n, out) != n) break;
        rc = ferror(in) || (out && ferror(out));
    } else if (fgets(error, ERROR_SIZE, in)) error[strcspn(error, "\n")] = '\0';
    if (rc && !*error) snprintf(error, ERROR_SIZE, "Desktop's daemon did not return a complete response.");
    fclose(in);
    return rc;
failed:
    snprintf(error, ERROR_SIZE, "Could not contact Desktop's daemon (%s).", strerror(errno));
    close(fd);
    return 1;
}

static volatile sig_atomic_t guarded_child;

static void
forward_lock_signal(int signal_number)
{
    if (guarded_child > 0) kill((pid_t)guarded_child, signal_number);
}

static int
guard_motion(const Paths *paths, char **command)
{
    const char *sleep_fd = getenv("XSS_SLEEP_LOCK_FD");
    char *end;
    long fd = sleep_fd ? strtol(sleep_fd, &end, 10) : -1;
    struct sigaction signals = {.sa_handler = forward_lock_signal};
    sigemptyset(&signals.sa_mask);
    const int forwarded[] = {SIGTERM, SIGINT, SIGHUP};
    sigset_t waiting, original;
    sigemptyset(&waiting);
    for (size_t i = 0; i < sizeof forwarded / sizeof *forwarded; ++i) {
        sigaddset(&waiting, forwarded[i]);
        sigaction(forwarded[i], &signals, NULL);
    }
    sigprocmask(SIG_BLOCK, &waiting, &original);
    pid_t child = fork();
    if (!child) {
        sigprocmask(SIG_SETMASK, &original, NULL);
        execvp(command[0], command);
        _exit(127);
    }
    guarded_child = child;
    sigprocmask(SIG_SETMASK, &original, NULL);
    if (sleep_fd && end != sleep_fd && !*end && fd >= 3 && fd <= INT_MAX)
        close((int)fd);
    if (child < 0) return 1;

    char error[ERROR_SIZE] = "";
    const char *hold[] = {"inhibit"};
    bool inhibited = command_run(paths, 1, hold, NULL, error) == 0;
    int result;
    pid_t done;
    do { done = waitpid(child, &result, 0); } while (done < 0 && errno == EINTR);
    guarded_child = 0;
    if (inhibited) {
        const char *release[] = {"uninhibit"};
        command_run(paths, 1, release, NULL, error);
    }
    if (done < 0) return 1;
    return WIFEXITED(result) ? WEXITSTATUS(result) : 128 + WTERMSIG(result);
}

static void
usage(FILE *f)
{
    fputs("Usage: desktop [COMMAND]\n"
          "  desktop                    Open wallpaper settings\n"
          "  desktop launch             Choose a wallpaper action in Launch\n"
          "  desktop daemon | login     Run the session daemon or initialize it at login\n"
          "  desktop next | set PATH    Shuffle or apply a wallpaper file or package\n"
          "  desktop pack FILE DIR      Copy a wallpaper into a new package with a thumbnail\n"
          "  desktop status [--json | --path]  Show session state or the active package path\n"
          "  desktop list [--json]      Show wallpapers, metadata, and eligibility\n"
          "  desktop pause | resume     Pause or resume automatic rotation\n"
          "  desktop inhibit | uninhibit   Suspend or restore motion\n"
          "  desktop inhibit -- CMD ...  Suspend motion while CMD runs (e.g. a locker)\n"
          "  desktop reapply | reload | stop\n"
          "  desktop config path | defaults | check\n"
          "  desktop config set SECTION.KEY VALUE\n", f);
}

int
launch_menu(const Paths *paths)
{
    static const char *const labels[] = {
        "Open Desktop…", "Shuffle wallpaper", "Pause rotation", "Resume rotation"
    };
    static const char *const commands[] = {NULL, "next", "pause", "resume"};
    int input[2], output[2];
    if (pipe2(input, O_CLOEXEC)) return 1;
    if (pipe2(output, O_CLOEXEC)) { close(input[0]); close(input[1]); return 1; }
    pid_t pid = fork();
    if (!pid) {
        dup2(input[0], STDIN_FILENO);
        dup2(output[1], STDOUT_FILENO);
        execlp("launch", "launch", "--dmenu", "-i", "-p", "Desktop", (char *)NULL);
        execlp("dmenu", "dmenu", "-i", "-p", "Desktop", (char *)NULL);
        _exit(127);
    }
    close(input[0]);
    close(output[1]);
    if (pid < 0) { close(input[1]); close(output[0]); return 1; }
    FILE *items = fdopen(input[1], "w");
    if (items) {
        for (size_t i = 0; i < sizeof labels / sizeof *labels; ++i) fprintf(items, "%s\n", labels[i]);
        fclose(items);
    } else close(input[1]);
    char choice[128] = "";
    FILE *selected = fdopen(output[0], "r");
    if (selected) { (void)fgets(choice, sizeof choice, selected); fclose(selected); }
    else close(output[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) return ui_run(paths);
    choice[strcspn(choice, "\r\n")] = '\0';
    for (size_t i = 0; i < sizeof labels / sizeof *labels; ++i) {
        if (strcmp(choice, labels[i])) continue;
        if (!commands[i]) return ui_run(paths);
        char error[ERROR_SIZE] = "";
        int rc = command_run(paths, 1, &commands[i], stdout, error);
        if (rc) fprintf(stderr, "desktop: %s\n", error);
        return rc;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        puts("desktop " DESKTOP_VERSION);
        return 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        usage(stdout);
        return 0;
    }
    Paths paths;
    char error[ERROR_SIZE] = "";
    if (!paths_init(&paths, error)) goto failed;
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "open"))) return ui_run(&paths);
    if (argc == 2 && !strcmp(argv[1], "daemon")) return daemon_run(&paths, -1);
    if (argc == 2 && !strcmp(argv[1], "launch")) return launch_menu(&paths);
    const char *cmd = argv[1];
    if (argc == 4 && !strcmp(cmd, "pack")) {
        Config cfg;
        if (!config_read(&paths, &cfg, error) || !package_create(&cfg, argv[2], argv[3], error)) goto failed;
        puts(argv[3]);
        return 0;
    }
    if (argc >= 4 && !strcmp(cmd, "inhibit") && !strcmp(argv[2], "--"))
        return guard_motion(&paths, argv + 3);
    if (!strcmp(cmd, "config")) {
        if (argc == 3 && !strcmp(argv[2], "defaults"))
            return fwrite(defaults_start, 1, (size_t)((uintptr_t)defaults_end - (uintptr_t)defaults_start), stdout) ? 0 : 1;
        if (argc == 3 && !strcmp(argv[2], "path")) { puts(paths.config); return 0; }
        if (argc == 3 && !strcmp(argv[2], "check")) {
            Config cfg;
            if (!config_read(&paths, &cfg, error)) goto failed;
            return 0;
        }
        if (argc == 5 && !strcmp(argv[2], "set")) {
            Setting change = {argv[3], argv[4]};
            if (!config_write(&paths, &change, 1, error)) goto failed;
            int fd = connect_daemon(&paths);
            if (fd < 0) return 0;
            close(fd);
            const char *reload[] = {"reload"};
            if (command_run(&paths, 1, reload, stdout, error)) goto failed;
            return 0;
        }
    }
    const char *single[] = {"login", "next", "pause", "resume", "toggle", "inhibit", "uninhibit", "stop", "reload", "reapply"};
    bool valid = false;
    for (size_t i = 0; i < sizeof single / sizeof *single; ++i)
        if (argc == 2 && !strcmp(cmd, single[i])) valid = true;
    if ((!strcmp(cmd, "status") || !strcmp(cmd, "list")) &&
        (argc == 2 || (argc == 3 && !strcmp(argv[2], "--json"))))
        valid = true;
    if (argc == 3 && !strcmp(cmd, "status") && !strcmp(argv[2], "--path")) valid = true;
    if (argc == 3 && !strcmp(cmd, "set")) {
        char absolute[PATH_MAX];
        if (!realpath(argv[2], absolute)) {
            snprintf(error, ERROR_SIZE, "Cannot resolve %.300s (%s).", argv[2], strerror(errno));
            goto failed;
        }
        const char *args[] = {"set", absolute};
        if (command_run(&paths, 2, args, stdout, error)) goto failed;
        return 0;
    }
    if (!valid) { usage(stderr); return 2; }
    if (command_run(&paths, (size_t)(argc - 1), (const char *const *)(argv + 1), stdout, error)) goto failed;
    return 0;
failed:
    fprintf(stderr, "desktop: %s\n", error);
    return 1;
}
