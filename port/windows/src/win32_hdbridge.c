/*
WIN32_HDBRIDGE.C

The HaloDoom bridge's operating-system services on Windows (hdb_bridge.h):
hdbridge.ini, the shared memory, starting UZDoom. Named win32_* so it sees
the Windows SDK only; it replaces posix_hdbridge.c (port.json).
*/

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#include "../../linux/src/hdb_bridge.h"
#include "../../linux/src/hdb_ini.h"

static HANDLE g_map = NULL;
static HANDLE g_job = NULL;
static HANDLE g_proc = NULL;
static char   g_exe_dir[HDB_PATH_MAX];

static const char* exe_dir(void) {
    if (!g_exe_dir[0]) {
        char buf[HDB_PATH_MAX];
        DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
        char* slash;
        buf[n < sizeof buf ? n : sizeof buf - 1] = 0;
        slash = strrchr(buf, '\\');
        if (slash) *slash = 0;
        hdb_ini_copy(g_exe_dir, sizeof g_exe_dir, buf);
    }
    return g_exe_dir;
}

static void resolve(char* path, size_t n) {
    char tmp[HDB_PATH_MAX];
    if (!path[0] || path[1] == ':' || path[0] == '\\' || path[0] == '/') return;
    snprintf(tmp, sizeof tmp, "%s\\%s", exe_dir(), path);
    hdb_ini_copy(path, n, tmp);
}

void hdb_os_log(const char* fmt, ...) {
    char msg[1024], path[HDB_PATH_MAX];
    va_list ap;
    FILE* f;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    OutputDebugStringA("[HaloDoomBridge] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    snprintf(path, sizeof path, "%s\\hdbridge.log", exe_dir());
    f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

static int file_exists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES;
}

/* A path in hdbridge.ini that names nothing: try the usual places for that
file, by its own name, before giving up (UZDoom unpacked straight into
HaloDoomBridge, or into HaloDoomBridge\uzdoom, or next to halo.exe). */
static void find_elsewhere(const char* setting, char* path, size_t n) {
    static const char* const folders[] = { "HaloDoomBridge", "HaloDoomBridge\\uzdoom", "", "uzdoom" };
    const char* name;
    char tried[HDB_PATH_MAX];
    int i;
    if (!path[0] || file_exists(path)) return;
    name = strrchr(path, '\\');
    if (!name) name = strrchr(path, '/');
    name = name ? name + 1 : path;
    for (i = 0; i < (int)(sizeof folders / sizeof folders[0]); i++) {
        if (folders[i][0]) snprintf(tried, sizeof tried, "%s\\%s\\%s", exe_dir(), folders[i], name);
        else snprintf(tried, sizeof tried, "%s\\%s", exe_dir(), name);
        if (file_exists(tried)) {
            hdb_os_log("%s not at %s; found it at %s", setting, path, tried);
            hdb_ini_copy(path, n, tried);
            return;
        }
    }
}

int hdb_os_load_config(hdb_config* cfg) {
    char path[HDB_PATH_MAX];
    int read;
    hdb_config_defaults(cfg);
    snprintf(path, sizeof path, "%s\\hdbridge.ini", exe_dir());
    read = hdb_config_read(cfg, path);
    if (read == 0) return 0;   /* no hdbridge.ini: the bridge stays off */
    hdb_os_log("--- settings from %s", path);
    if (read < 0)
        hdb_os_log("hdbridge.ini is saved as UTF-16 (\"Unicode\"), which can't be read: "
                   "save it again as UTF-8 or ANSI. Using the default settings.");
    resolve(cfg->uzdoom_exe, sizeof cfg->uzdoom_exe);
    resolve(cfg->iwad, sizeof cfg->iwad);
    resolve(cfg->halodoom_pk3, sizeof cfg->halodoom_pk3);
    resolve(cfg->bridge_pk3, sizeof cfg->bridge_pk3);
    resolve(cfg->doom_config, sizeof cfg->doom_config);
    find_elsewhere("sUZDoom", cfg->uzdoom_exe, sizeof cfg->uzdoom_exe);
    find_elsewhere("sIWAD", cfg->iwad, sizeof cfg->iwad);
    find_elsewhere("sHaloDoom", cfg->halodoom_pk3, sizeof cfg->halodoom_pk3);
    find_elsewhere("sBridgePk3", cfg->bridge_pk3, sizeof cfg->bridge_pk3);
    hdb_os_log("sUZDoom    = %s", cfg->uzdoom_exe);
    hdb_os_log("sIWAD      = %s", cfg->iwad);
    hdb_os_log("sHaloDoom  = %s", cfg->halodoom_pk3);
    hdb_os_log("sBridgePk3 = %s", cfg->bridge_pk3);
    return 1;
}

hdb_shared* hdb_os_map_shared(int* created_new) {
    void* p;
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                               (DWORD)sizeof(hdb_shared), HDB_SHM_NAME_A);
    if (!g_map) { hdb_os_log("CreateFileMapping failed (%lu)", GetLastError()); return NULL; }
    *created_new = GetLastError() != ERROR_ALREADY_EXISTS;
    p = MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hdb_shared));
    if (!p) { hdb_os_log("MapViewOfFile failed (%lu)", GetLastError()); return NULL; }
    return (hdb_shared*)p;
}

void hdb_os_unmap_shared(hdb_shared* shm) {
    if (shm) UnmapViewOfFile(shm);
    if (g_map) { CloseHandle(g_map); g_map = NULL; }
}

int hdb_os_process_alive(uint32_t pid) {
    HANDLE h;
    int alive;
    if (!pid) return 0;
    h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return 0;
    alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

int hdb_os_launch_doom(const hdb_config* cfg, uint32_t view_w, uint32_t view_h) {
    char cmd[4096];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    if (view_w > HDB_OVERLAY_MAX_W) view_w = HDB_OVERLAY_MAX_W;
    if (view_h > HDB_OVERLAY_MAX_H) view_h = HDB_OVERLAY_MAX_H;
    snprintf(cmd, sizeof cmd,
        "\"%s\" %s -config \"%s\" -iwad \"%s\" -file \"%s\" \"%s\" -width %u -height %u "
        "+vid_fullscreen 0 +map HDBVOID %s",
        cfg->uzdoom_exe, cfg->doom_visible ? "-hdbridge -hdbridge-visible" : "-hdbridge",
        cfg->doom_config, cfg->iwad, cfg->halodoom_pk3, cfg->bridge_pk3, view_w, view_h, cfg->extra_args);

    /* Tie UZDoom's lifetime to Halo's. */
    g_job = CreateJobObjectA(NULL, NULL);
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        ZeroMemory(&li, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    /* every file it needs, named in the log if missing (the usual problem) */
    {
        const char* names[5] = { "sUZDoom", "sIWAD", "sHaloDoom", "sBridgePk3", NULL };
        const char* paths[5] = { cfg->uzdoom_exe, cfg->iwad, cfg->halodoom_pk3, cfg->bridge_pk3, NULL };
        int i, missing = 0;
        for (i = 0; names[i]; i++) {
            if (!file_exists(paths[i])) {
                hdb_os_log("%s not found: %s", names[i], paths[i]);
                missing = 1;
            }
        }
        if (missing) {
            hdb_os_log("UZDoom not started: fix the paths above in hdbridge.ini "
                       "(relative paths start from %s)", exe_dir());
            return 0;
        }
    }
    hdb_os_log("starting: %s", cmd);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, exe_dir(), &si, &pi)) {
        hdb_os_log("could not start UZDoom (error %lu): %s", GetLastError(), cfg->uzdoom_exe);
        return 0;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    hdb_os_log("started UZDoom (pid %lu)", pi.dwProcessId);
    return 1;
}

void hdb_os_kill_doom(void) {
    if (g_job) { CloseHandle(g_job); g_job = NULL; }   /* KILL_ON_JOB_CLOSE */
    if (g_proc) { CloseHandle(g_proc); g_proc = NULL; }
}

int hdb_os_doom_exited(long* exit_code) {
    DWORD code;
    if (!g_proc || WaitForSingleObject(g_proc, 0) != WAIT_OBJECT_0) return 0;
    if (!GetExitCodeProcess(g_proc, &code)) code = 0;
    *exit_code = (long)code;
    CloseHandle(g_proc);
    g_proc = NULL;
    return 1;
}

uint32_t hdb_os_pid(void) { return (uint32_t)GetCurrentProcessId(); }
uint64_t hdb_os_ms(void) { return (uint64_t)GetTickCount64(); }
