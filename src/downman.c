#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CONFIG_FILENAME  "downman.ini"
#define DOWNLOADS_DIR    "downloads"
#define MAX_CONFIG_LINE  1024
#define MAX_ARGS         64

#define FMT_VIDEO_AUDIO  "bestvideo[vcodec^=avc1]+bestaudio[acodec^=mp4a]/" \
                         "bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]"
#define FMT_VIDEO_ONLY   "bestvideo"
#define FMT_AUDIO_ONLY   "bestaudio[acodec^=mp4a]/bestaudio"

#define OUTPUT_WITH_ID   "%(title)s [%(id)s].%(ext)s"
#define OUTPUT_NO_ID     "%(title)s.%(ext)s"

typedef enum { MODE_NONE, MODE_VA, MODE_V, MODE_A } Mode;
typedef enum { PARSE_OK, PARSE_HELP, PARSE_ERROR } ParseResult;

static volatile sig_atomic_t g_child_pid = 0;

static void print_usage(const char *progname)
{
    fprintf(stderr,
            "Usage: %s [-va|-v|-a] URL\n"
            "  -va        Video + Audio (best, H.264/MP4)\n"
            "  -v         Video only (remuxed to MP4)\n"
            "  -a         Audio only (MP3)\n",
            progname);
}

static void print_help(FILE *out, const char *progname)
{
    fprintf(out,
            "%s - Download manager for yt-dlp\n\n"
            "Usage: %s [OPTIONS] URL\n\n"
            "Options:\n"
            "  -va               Download best video (H.264/MP4) + audio, merge to MP4\n"
            "  -v                Download best video only, remux to MP4\n"
            "  -a                Download best audio only, convert to MP3 (quality 0)\n"
            "  -h, --help        Show this help\n\n"
            "File Layout:\n"
            "  ./\n"
            "  ├── downman                   (this program)\n"
            "  ├── downman.ini               (required configuration)\n"
            "  └── downloads/                (all downloads go here)\n\n"
            "Config (downman.ini) - REQUIRED:\n"
            "  [Executables]\n"
            "  yt-dlp=files/yt-dlp\n"
            "  ffmpeg=files/ffmpeg\n"
            "  Both keys are mandatory. Paths may be relative (to this exe)\n"
            "  or absolute. Section and keys are case-insensitive. Save as\n"
            "  UTF-8 (UTF-16 is rejected).\n\n"
            "  [Options]\n"
            "  id=true            Append video ID to filename (default)\n"
            "  id=false           Omit video ID from filename\n\n"
            "Examples:\n"
            "  %s -va https://youtube.com/watch?v=123\n"
            "  %s -v https://youtube.com/watch?v=123\n"
            "  %s -a https://youtube.com/watch?v=123\n",
            progname, progname, progname, progname, progname);
}

static void on_term_signal(int sig)
{
    (void)sig;
    pid_t pid = (pid_t)g_child_pid;
    if (pid > 0) kill(pid, SIGTERM);
}

static bool get_exe_dir(char *buf, size_t bufsize)
{
    ssize_t len = readlink("/proc/self/exe", buf, bufsize - 1);
    if (len <= 0 || (size_t)len >= bufsize)
        return false;
    buf[len] = '\0';

    char *slash = strrchr(buf, '/');
    if (slash != NULL)
        *slash = '\0';
    else
        strcpy(buf, ".");
    return true;
}

static bool build_path(char *dest, size_t size, const char *exe_dir, const char *relative)
{
    int needed = snprintf(dest, size, "%s/%s", exe_dir, relative);
    if (needed < 0 || (size_t)needed >= size) {
        dest[0] = '\0';
        return false;
    }
    return true;
}

static bool is_absolute_path(const char *p)
{
    return p[0] == '/';
}

static bool resolve_path(char *dest, size_t size, const char *exe_dir, const char *rel)
{
    if (is_absolute_path(rel)) {
        if (strlen(rel) >= size) return false;
        strcpy(dest, rel);
        return true;
    }
    return build_path(dest, size, exe_dir, rel);
}

static const char *path_basename(const char *path)
{
    const char *base = strrchr(path, '/');
    return (base != NULL) ? base + 1 : path;
}

static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

static bool read_config(const char *path, char *ytdlp, size_t ytdlp_size,
                        char *ffmpeg, size_t ffmpeg_size, bool *id_enabled)
{
    if (access(path, F_OK) != 0) {
        fprintf(stderr, "Error: configuration file not found: %s\n", path);
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open configuration file: %s\n", path);
        return false;
    }

    enum { SEC_NONE, SEC_EXECUTABLES, SEC_OPTIONS } section = SEC_NONE;
    bool got_ytdlp = false, got_ffmpeg = false;
    bool first = true;
    char line[MAX_CONFIG_LINE];
    *id_enabled = true;

    while (fgets(line, sizeof(line), f)) {
        size_t flen = strlen(line);

        if (flen == sizeof(line) - 1 && line[flen - 1] != '\n' && !feof(f)) {
            fprintf(stderr, "Error: line too long in %s\n", path);
            fclose(f);
            return false;
        }

        if (first) {
            first = false;
            if (flen >= 3 &&
                (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF) {
                memmove(line, line + 3, flen - 3 + 1);
            } else if (flen >= 2 &&
                       (((unsigned char)line[0] == 0xFF &&
                         (unsigned char)line[1] == 0xFE) ||
                        ((unsigned char)line[0] == 0xFE &&
                         (unsigned char)line[1] == 0xFF))) {
                fprintf(stderr,
                        "Error: %s appears to be UTF-16; save as UTF-8.\n",
                        path);
                fclose(f);
                return false;
            }
        }

        trim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
            continue;

        if (line[0] == '[') {
            if (strcasecmp(line, "[Executables]") == 0)
                section = SEC_EXECUTABLES;
            else if (strcasecmp(line, "[Options]") == 0)
                section = SEC_OPTIONS;
            else
                section = SEC_NONE;
            continue;
        }

        if (section == SEC_NONE) continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (section == SEC_EXECUTABLES) {
            if (strcasecmp(key, "yt-dlp") == 0) {
                if (strlen(val) >= ytdlp_size) {
                    fprintf(stderr,
                            "Error: yt-dlp path in %s is too long (max %d chars)\n",
                            path, (int)(ytdlp_size - 1));
                    fclose(f);
                    return false;
                }
                strcpy(ytdlp, val);
                got_ytdlp = true;
            } else if (strcasecmp(key, "ffmpeg") == 0) {
                if (strlen(val) >= ffmpeg_size) {
                    fprintf(stderr,
                            "Error: ffmpeg path in %s is too long (max %d chars)\n",
                            path, (int)(ffmpeg_size - 1));
                    fclose(f);
                    return false;
                }
                strcpy(ffmpeg, val);
                got_ffmpeg = true;
            }
        } else if (section == SEC_OPTIONS) {
            if (strcasecmp(key, "id") == 0) {
                if (strcasecmp(val, "true") == 0)
                    *id_enabled = true;
                else if (strcasecmp(val, "false") == 0)
                    *id_enabled = false;
                else {
                    fprintf(stderr,
                            "Error: invalid value for 'id' in %s "
                            "(expected true or false)\n", path);
                    fclose(f);
                    return false;
                }
            }
        }
    }

    fclose(f);

    if (!got_ytdlp)
        fprintf(stderr, "Error: missing 'yt-dlp' key in %s\n", path);
    if (!got_ffmpeg)
        fprintf(stderr, "Error: missing 'ffmpeg' key in %s\n", path);
    return got_ytdlp && got_ffmpeg;
}

#define PUSH_ARG(a) do { \
    if (idx >= MAX_ARGS) { \
        fprintf(stderr, "Too many arguments\n"); \
        return false; \
    } \
    args[idx++] = (char *)(a); \
} while (0)

static bool build_ytdlp_args(Mode mode, bool id_enabled,
                             const char *ytdlp, const char *ffmpeg, const char *url,
                             char *args[], int *out_count)
{
    int idx = 0;

    PUSH_ARG(ytdlp);
    PUSH_ARG("--ffmpeg-location");
    PUSH_ARG(ffmpeg);
    PUSH_ARG("--no-warnings");

    switch (mode) {
    case MODE_VA:
        PUSH_ARG("-f");
        PUSH_ARG(FMT_VIDEO_AUDIO);
        PUSH_ARG("--merge-output-format");
        PUSH_ARG("mp4");
        break;
    case MODE_V:
        PUSH_ARG("-f");
        PUSH_ARG(FMT_VIDEO_ONLY);
        PUSH_ARG("--remux-video");
        PUSH_ARG("mp4");
        break;
    case MODE_A:
        PUSH_ARG("-f");
        PUSH_ARG(FMT_AUDIO_ONLY);
        PUSH_ARG("--extract-audio");
        PUSH_ARG("--audio-format");
        PUSH_ARG("mp3");
        PUSH_ARG("--audio-quality");
        PUSH_ARG("0");
        break;
    default:
        return false;
    }

    PUSH_ARG("-o");
    PUSH_ARG(id_enabled ? OUTPUT_WITH_ID : OUTPUT_NO_ID);
    PUSH_ARG(url);

    args[idx] = NULL;
    *out_count = idx;
    return true;
}

#undef PUSH_ARG

static int ensure_downloads_dir(const char *exe_dir, char *downloads, size_t size)
{
    if (!build_path(downloads, size, exe_dir, DOWNLOADS_DIR)) {
        fprintf(stderr, "Path too long for downloads directory\n");
        return -1;
    }

    if (mkdir(downloads, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create downloads directory '%s': %s\n",
                downloads, strerror(errno));
        return -1;
    }

    struct stat st;
    if (stat(downloads, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "'%s' exists but is not a directory\n", downloads);
        return -1;
    }
    return 0;
}

static ParseResult parse_args(int argc, char *argv[], const char *progname,
                              Mode *mode_out, const char **url_out)
{
    Mode mode = MODE_NONE;
    const char *url = NULL;
    int url_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(stdout, progname);
            return PARSE_HELP;
        }

        if (strcmp(argv[i], "-va") == 0 || strcmp(argv[i], "-v") == 0 ||
            strcmp(argv[i], "-a") == 0) {
            if (mode != MODE_NONE) {
                fprintf(stderr, "Error: multiple mode flags specified.\n");
                return PARSE_ERROR;
            }
            if (strcmp(argv[i], "-va") == 0) mode = MODE_VA;
            else if (strcmp(argv[i], "-v") == 0) mode = MODE_V;
            else mode = MODE_A;
        } else if (argv[i][0] != '-') {
            url = argv[i];
            url_count++;
        } else {
            print_usage(progname);
            return PARSE_ERROR;
        }
    }

    if (url_count > 1) {
        fprintf(stderr, "Error: only one URL is allowed.\n");
        return PARSE_ERROR;
    }
    if (mode == MODE_NONE || url == NULL || url[0] == '\0') {
        print_usage(progname);
        return PARSE_ERROR;
    }

    *mode_out = mode;
    *url_out = url;
    return PARSE_OK;
}

static int run_ytdlp(char *const argv[], const char *workdir)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("Running yt-dlp...\n");
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return 1;
    }

    if (pid == 0) {
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        if (chdir(workdir) != 0) {
            fprintf(stderr, "chdir '%s' failed: %s\n", workdir, strerror(errno));
            _exit(127);
        }
        execv(argv[0], argv);
        fprintf(stderr, "execv '%s' failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    g_child_pid = (sig_atomic_t)pid;

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
            g_child_pid = 0;
            return 1;
        }
    }
    g_child_pid = 0;

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

int main(int argc, char *argv[])
{
    char progname_buf[PATH_MAX];
    snprintf(progname_buf, sizeof(progname_buf), "%s",
             argc > 0 ? argv[0] : "downman");
    const char *progname = path_basename(progname_buf);

    Mode mode;
    const char *url;
    ParseResult pr = parse_args(argc, argv, progname, &mode, &url);
    if (pr == PARSE_HELP) return 0;
    if (pr == PARSE_ERROR) return 1;

    char exe_dir[PATH_MAX];
    if (!get_exe_dir(exe_dir, sizeof(exe_dir))) {
        fprintf(stderr, "Failed to determine executable directory.\n");
        return 1;
    }

    char config_path[PATH_MAX];
    if (!build_path(config_path, sizeof(config_path), exe_dir, CONFIG_FILENAME)) {
        fprintf(stderr, "Path too long for config file\n");
        return 1;
    }

    char ytdlp_rel[PATH_MAX], ffmpeg_rel[PATH_MAX];
    bool id_enabled = true;
    if (!read_config(config_path, ytdlp_rel, sizeof(ytdlp_rel),
                     ffmpeg_rel, sizeof(ffmpeg_rel), &id_enabled))
        return 1;

    char ytdlp_path[PATH_MAX], ffmpeg_path[PATH_MAX];
    if (!resolve_path(ytdlp_path, sizeof(ytdlp_path), exe_dir, ytdlp_rel)) {
        fprintf(stderr, "Path too long for yt-dlp\n");
        return 1;
    }
    if (!resolve_path(ffmpeg_path, sizeof(ffmpeg_path), exe_dir, ffmpeg_rel)) {
        fprintf(stderr, "Path too long for ffmpeg\n");
        return 1;
    }

    if (access(ytdlp_path, X_OK) != 0 || access(ffmpeg_path, X_OK) != 0) {
        fprintf(stderr,
                "Required executables not found or not executable.\n"
                "  yt-dlp: %s\n"
                "  ffmpeg: %s\n",
                ytdlp_path, ffmpeg_path);
        return 1;
    }

    char *args[MAX_ARGS + 1];
    int arg_count = 0;
    if (!build_ytdlp_args(mode, id_enabled, ytdlp_path, ffmpeg_path, url,
                          args, &arg_count))
        return 1;

    char downloads[PATH_MAX];
    if (ensure_downloads_dir(exe_dir, downloads, sizeof(downloads)) != 0)
        return 1;

    int exit_code = run_ytdlp(args, downloads);
    if (exit_code == 0)
        printf("Download completed. Files saved in: %s\n", downloads);
    else
        fprintf(stderr, "yt-dlp exited with code: %d\n", exit_code);
    return exit_code;
}
