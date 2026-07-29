// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "os.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <errno.h>
#    include <fcntl.h>
#    include <signal.h>
#    include <sys/mman.h>
#    include <time.h>
#    include <unistd.h>
#    if defined(__APPLE__)
#        include <libkern/OSCacheControl.h>
#        include <pthread.h>
#    endif
#endif

void sock_startup(void) {
#ifdef _WIN32
    static bool started = false;
    if (started) return;
    WSADATA wsa;
    started = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#else
    // A client that hangs up mid-write is normal here; it must fail the write,
    // not kill the process.
    signal(SIGPIPE, SIG_IGN);
#endif
}

int sock_open_tcp(void) {
    sock_startup();
    return (int)socket(AF_INET, SOCK_STREAM, 0);
}

int sock_accept(int listen_fd) {
#ifdef _WIN32
    return (int)accept((SOCKET)listen_fd, NULL, NULL);
#else
    return accept(listen_fd, NULL, NULL);
#endif
}

void sock_close(int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
    close(fd);
#endif
}

void sock_set_nonblocking(int fd) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &on);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

void sock_set_blocking(int fd) {
#ifdef _WIN32
    u_long off = 0;
    ioctlsocket((SOCKET)fd, FIONBIO, &off);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
#endif
}

bool sock_would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

bool sock_interrupted(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

const char *sock_lasterror(void) {
#ifdef _WIN32
    static char buf[192];
    int         err = WSAGetLastError();
    // Winsock messages come with a trailing CRLF that would break log lines.
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                             (DWORD)err, 0, buf, sizeof buf, NULL);
    while (n && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = '\0';
    if (!n) snprintf(buf, sizeof buf, "winsock error %d", err);
    return buf;
#else
    return strerror(errno);
#endif
}

uint64_t os_now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000ll) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

void os_sleep_us(uint64_t us) {
#ifdef _WIN32
    // Sleep()'s granularity is a millisecond, and the pacing loop asks for tens
    // of them at a time; a sub-millisecond request just yields.
    DWORD ms = (DWORD)(us / 1000ull);
    Sleep(ms ? ms : (us ? 1 : 0));
#else
    struct timespec ts = {.tv_sec = (time_t)(us / 1000000ull),
                          .tv_nsec = (long)((us % 1000000ull) * 1000ull)};
    nanosleep(&ts, NULL);
#endif
}

// ---- executable memory ------------------------------------------------------
//
// The region is mapped once with the permissions it will keep, because the caller
// commits a block at a time and there can be hundreds of thousands of those a
// second -- an mprotect() per block would cost more than the block saves.
//
// That works everywhere except Apple silicon, which will not map a page writable
// and executable at once. There the mapping is MAP_JIT and write permission is
// toggled per thread instead, which is a cheap userspace operation rather than a
// system call. Getting it wrong shows up as a SIGBUS on the first store, not as an
// error from mmap.

static size_t page_round(size_t n) {
#ifdef _WIN32
    static size_t page;
    if (!page) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        page = si.dwPageSize;
    }
#else
    static size_t page;
    if (!page) page = (size_t)sysconf(_SC_PAGESIZE);
#endif
    if (!page) page = 4096u;
    return (n + page - 1u) & ~(page - 1u);
}

void *os_code_alloc(size_t size) {
    if (!size) return NULL;
    size = page_round(size);
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    int prot  = PROT_READ | PROT_WRITE | PROT_EXEC;
    int flags = MAP_PRIVATE | MAP_ANON;
#    if defined(__APPLE__) && defined(__aarch64__)
    // The one platform where PROT_WRITE | PROT_EXEC is a lie the kernel will not
    // tell: MAP_JIT accepts it and then enforces one or the other per thread.
    flags |= MAP_JIT;
#    endif
    void *p = mmap(NULL, size, prot, flags, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

void os_code_free(void *p, size_t size) {
    if (!p) return;
#ifdef _WIN32
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, page_round(size));
#endif
}

bool os_code_writable(void *p, size_t size) {
    (void)size;
    if (!p) return false;
#if defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(0);
#endif
    return true;
}

// Not page_round()ed: the range is what was just written, and both the icache
// maintenance and FlushInstructionCache take arbitrary byte ranges. Rounding out to
// pages here would flush a whole page for a fifty-byte block.
bool os_code_commit(void *p, size_t size) {
    if (!p) return false;
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), p, size);
#elif defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(p, size);
#elif defined(__GNUC__)
    // A no-op on x86, the necessary cache maintenance on arm64.
    __builtin___clear_cache((char *)p, (char *)p + size);
#endif
    return true;
}
