/*
POSIX_HDBRIDGE.C

The HaloDoom bridge's operating-system services on Linux (hdb_bridge.h):
hdbridge.ini, the shared memory, starting UZDoom. Named posix_* so it is
built with glibc's own ABI.
*/

#ifndef __ANDROID__

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "hdb_bridge.h"
#include "hdb_ini.h"

static pid_t g_doom = 0;
static char  g_exe_dir[HDB_PATH_MAX];

static const char* exe_dir(void) {
    if (!g_exe_dir[0]) {
        char buf[HDB_PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) {
            char* slash;
            buf[n] = 0;
            slash = strrchr(buf, '/');
            if (slash) *slash = 0;
            hdb_ini_copy(g_exe_dir, sizeof g_exe_dir, buf);
        } else {
            hdb_ini_copy(g_exe_dir, sizeof g_exe_dir, ".");
        }
    }
    return g_exe_dir;
}

static void resolve(char* path, size_t n) {
    char tmp[HDB_PATH_MAX];
    if (!path[0] || path[0] == '/') return;
    snprintf(tmp, sizeof tmp, "%s/%s", exe_dir(), path);
    hdb_ini_copy(path, n, tmp);
}

void hdb_os_log(const char* fmt, ...) {
    char path[HDB_PATH_MAX];
    va_list ap;
    FILE* f;
    va_start(ap, fmt);
    fprintf(stderr, "[HaloDoomBridge] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);

    snprintf(path, sizeof path, "%s/hdbridge.log", exe_dir());
    f = fopen(path, "a");
    if (f) {
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        fputc('\n', f);
        va_end(ap);
        fclose(f);
    }
}

int hdb_os_load_config(hdb_config* cfg) {
    char path[HDB_PATH_MAX];
    hdb_config_defaults(cfg);
    snprintf(path, sizeof path, "%s/hdbridge.ini", exe_dir());
    if (hdb_config_read(cfg, path) == 0) return 0;
    resolve(cfg->uzdoom_exe, sizeof cfg->uzdoom_exe);
    resolve(cfg->iwad, sizeof cfg->iwad);
    resolve(cfg->halodoom_pk3, sizeof cfg->halodoom_pk3);
    resolve(cfg->bridge_pk3, sizeof cfg->bridge_pk3);
    resolve(cfg->doom_config, sizeof cfg->doom_config);
    return 1;
}

hdb_shared* hdb_os_map_shared(int* created_new) {
    void* p;
    int fd = shm_open(HDB_SHM_NAME_POSIX, O_RDWR | O_CREAT | O_EXCL, 0600);
    *created_new = fd >= 0;
    if (fd < 0 && errno == EEXIST) fd = shm_open(HDB_SHM_NAME_POSIX, O_RDWR, 0600);
    if (fd < 0) { hdb_os_log("shm_open failed: %s", strerror(errno)); return NULL; }
    if (ftruncate(fd, (off_t)sizeof(hdb_shared)) != 0) {
        hdb_os_log("ftruncate failed: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    p = mmap(NULL, sizeof(hdb_shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { hdb_os_log("mmap failed: %s", strerror(errno)); return NULL; }
    return (hdb_shared*)p;
}

void hdb_os_unmap_shared(hdb_shared* shm) {
    if (shm) munmap(shm, sizeof(hdb_shared));
    shm_unlink(HDB_SHM_NAME_POSIX);
}

int hdb_os_process_alive(uint32_t pid) {
    return pid && kill((pid_t)pid, 0) == 0;
}

int hdb_os_launch_doom(const hdb_config* cfg, uint32_t view_w, uint32_t view_h) {
    char w[16], h[16];
    const char* argv[32];   /* 20 fixed + up to 11 extra + NULL */
    int n = 0;
    pid_t pid;

    if (view_w > HDB_OVERLAY_MAX_W) view_w = HDB_OVERLAY_MAX_W;
    if (view_h > HDB_OVERLAY_MAX_H) view_h = HDB_OVERLAY_MAX_H;
    snprintf(w, sizeof w, "%u", view_w);
    snprintf(h, sizeof h, "%u", view_h);

    argv[n++] = cfg->uzdoom_exe;
    argv[n++] = "-hdbridge";
    if (cfg->doom_visible) argv[n++] = "-hdbridge-visible";
    argv[n++] = "-iwad";   argv[n++] = cfg->iwad;
    argv[n++] = "-file";   argv[n++] = cfg->halodoom_pk3; argv[n++] = cfg->bridge_pk3;
    argv[n++] = "-config"; argv[n++] = cfg->doom_config;
    argv[n++] = "-width";  argv[n++] = w;
    argv[n++] = "-height"; argv[n++] = h;
    argv[n++] = "+vid_fullscreen"; argv[n++] = "0";
    {   /* UZDoom's log beside hdbridge.log */
        static char log_path[HDB_PATH_MAX];
        snprintf(log_path, sizeof log_path, "%s/uzdoom.log", exe_dir());
        argv[n++] = "+logfile"; argv[n++] = log_path;
    }
    argv[n++] = "+map";    argv[n++] = "HDBVOID";
    /* sExtraArgs: split on spaces (no quoting; use paths without spaces). */
    {
        static char extra[HDB_PATH_MAX];
        char* tok;
        hdb_ini_copy(extra, sizeof extra, cfg->extra_args);
        for (tok = strtok(extra, " \t"); tok && n < 31; tok = strtok(NULL, " \t")) argv[n++] = tok;
    }
    argv[n] = NULL;

    if (access(cfg->uzdoom_exe, X_OK) != 0) {
        hdb_os_log("sUZDoom not found: %s", cfg->uzdoom_exe);
        return 0;
    }
    pid = fork();
    if (pid < 0) { hdb_os_log("fork failed: %s", strerror(errno)); return 0; }
    if (pid == 0) {
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGTERM);    /* UZDoom quits when Halo does */
#endif
        execv(cfg->uzdoom_exe, (char* const*)argv);
        _exit(127);
    }
    g_doom = pid;
    hdb_os_log("started UZDoom (pid %d)", (int)pid);
    return 1;
}

void hdb_os_kill_doom(void) {
    if (g_doom > 0) {
        kill(g_doom, SIGTERM);
        waitpid(g_doom, NULL, WNOHANG);
        g_doom = 0;
    }
}

int hdb_os_doom_exited(long* exit_code) {
    int status;
    if (g_doom <= 0 || waitpid(g_doom, &status, WNOHANG) != g_doom) return 0;
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
    g_doom = 0;
    return 1;
}

uint32_t hdb_os_pid(void) { return (uint32_t)getpid(); }

#ifdef HDB_TEST
static uint64_t g_test_clock_offset;
void hdb_test_advance_clock(unsigned ms) { g_test_clock_offset += ms; }
#endif

uint64_t hdb_os_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
#ifdef HDB_TEST
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000) + g_test_clock_offset;
#else
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

#endif /* __ANDROID__ */
