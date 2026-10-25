// ps5-homebrew-ui - Process-level runtime shims for the OpenGL runtime.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from ps5-opengl native-app/runtime_shims.c: the app log receipt,
// the never-return main policy, and libc entry points the clean-room libc
// shim does not provide but the statically linked Mesa runtime references.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern int sceKernelUsleep(uint32_t microseconds);
extern uint64_t sceKernelGetProcessTime(void);

/* The log goes into the title's own storage (a sandbox has no /data). While
 * the title runs, FTP reads it at
 * /mnt/sandbox/<TITLE_ID>_000/download0/hui/dev/app.log. */
#define HUI_PARENT_DIR "/download0/hui"
#define HUI_DATA_DIR HUI_PARENT_DIR "/dev"
#define HUI_LOG_PATH HUI_DATA_DIR "/app.log"
/* The crash markers, mirrored with open()/write() instead of stdout: if the
 * freopen below is what this firmware refuses, the markers still arrive. */
#define HUI_STEP_PATH HUI_DATA_DIR "/steps.log"

__attribute__((constructor)) static void hui_open_log(void)
{
    mkdir(HUI_PARENT_DIR, 0755);
    mkdir(HUI_DATA_DIR, 0755);
    /* Keep the previous launch's log for post-close inspection. */
    rename(HUI_LOG_PATH, HUI_DATA_DIR "/app.prev.log");
    rename(HUI_STEP_PATH, HUI_DATA_DIR "/steps.prev.log");
    FILE *stream = freopen(HUI_LOG_PATH, "w", stdout);
    /* Start a fresh receipt, then make both streams append-only and unbuffered
     * so the log survives a shell close or a GPU fail-stop. */
    if (stream != NULL)
        stream = freopen(HUI_LOG_PATH, "a", stdout);
    if (stream != NULL)
        setvbuf(stream, NULL, _IONBF, 0);
    stream = freopen(HUI_LOG_PATH, "a", stderr);
    if (stream != NULL)
        setvbuf(stream, NULL, _IONBF, 0);
}

int hui_step_write(const char *text, int length)
{
    if (text == NULL || length <= 0)
        return -1;
    /* A second copy outside the sandbox, which is what makes a crash
     * readable: the sandbox is torn down the moment the title dies, and the
     * failures worth reading happen on the way down. Two candidates are tried
     * because /app0 is mounted read-only on some setups and the title's own
     * install directory is not visible from inside the sandbox on others.
     * Best effort either way - the sandbox log stays the primary one. */
    /* Two of these turn out to be the same file, and every line then lands in
     * it twice, which makes the order of a crash unreadable. One is enough.
     * /app0 is read-only on most setups and fails harmlessly. */
    static const char *const kMirrorPaths[] = {"/app0/hui-steps.log",
                                               "/data/homebrew/PPSA99102/hui-steps.log"};
    for (size_t i = 0; i < sizeof kMirrorPaths / sizeof kMirrorPaths[0]; i++)
    {
        const int mirror = open(kMirrorPaths[i], O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (mirror >= 0)
        {
            (void)!write(mirror, text, (size_t)length);
            close(mirror);
        }
    }
    const int fd = open(HUI_STEP_PATH, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0)
        return -1;
    const ssize_t written = write(fd, text, (size_t)length);
    close(fd);
    return written == (ssize_t)length ? 0 : -1;
}

/* Returning from main or calling exit() crashes a native title; stay alive
 * until the shell closes the title. */
__attribute__((noreturn)) void catchReturnFromMain(int status)
{
    printf("[HUI] main returned status=%d\n", status);
    fflush(NULL);
    for (;;)
        sceKernelUsleep(100000);
}

void hui_glapi_tls_context_init(void) __asm__("_ZTH23_mesa_glapi_tls_Context");

void hui_glapi_tls_context_init(void)
{
}

__attribute__((noreturn)) void __assert(const char *function, const char *file, int line,
                                        const char *expression)
{
    fprintf(stderr, "[HUI] assertion failed: %s (%s:%d, %s)\n", expression, file, line, function);
    abort();
}

/* The OpenGL runtime's shader cache (PS5_SHADER_CACHE_DIR) writes through
 * mkstemp(). The SDK binds mkstemp and isatty to libScePosixForWebKit, a
 * system library a native title does not load: a call jumps to address 0, and
 * an app that sets the cache folder stops at its first shader. These
 * definitions are linked instead of those imports. */
int mkstemps(char *template_name, int suffix_length)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static unsigned counter;
    const size_t length = template_name ? strlen(template_name) : 0;

    if (suffix_length < 0 || length < (size_t)suffix_length + 6u)
    {
        errno = EINVAL;
        return -1;
    }
    char *name = template_name + length - (size_t)suffix_length - 6u;
    for (int i = 0; i < 6; ++i)
    {
        if (name[i] != 'X')
        {
            errno = EINVAL;
            return -1;
        }
    }
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        uint64_t value = sceKernelGetProcessTime() +
                         (uint64_t)__atomic_add_fetch(&counter, 1u, __ATOMIC_RELAXED) *
                             UINT64_C(0x9E3779B97F4A7C15);
        for (int i = 0; i < 6; ++i)
        {
            name[i] = letters[value % 36u];
            value /= 36u;
        }
        const int file = open(template_name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (file >= 0 || errno != EEXIST)
            return file;
    }
    errno = EEXIST;
    return -1;
}

int mkstemp(char *template_name)
{
    return mkstemps(template_name, 0);
}

int isatty(int descriptor)
{
    (void)descriptor;
    errno = ENOTTY;
    return 0;
}

/* The console's splash picture stays up until the app has presented its first
 * frame. The OpenGL runtime asks to hide it as soon as the display opens,
 * which leaves a black screen while programs build and fonts load (several
 * seconds at 4K). The build routes every such call here (--wrap), and
 * sys::hide_splash_screen() lets them through. */
extern int __real_sceSystemServiceHideSplashScreen(void);
static int hui_splash_released;

void hui_release_splash(void)
{
    hui_splash_released = 1;
}

int __wrap_sceSystemServiceHideSplashScreen(void)
{
    return hui_splash_released ? __real_sceSystemServiceHideSplashScreen() : 0;
}

void openlog(const char *identifier, int option, int facility)
{
    (void)identifier;
    (void)option;
    (void)facility;
}

FILE *popen(const char *command, const char *mode)
{
    (void)command;
    (void)mode;
    errno = ENOSYS;
    return NULL;
}

int pclose(FILE *stream)
{
    (void)stream;
    errno = ENOSYS;
    return -1;
}
