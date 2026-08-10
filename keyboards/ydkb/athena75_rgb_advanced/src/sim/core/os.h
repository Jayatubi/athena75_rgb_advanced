// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The few places the simulator has to leave ISO C: BSD sockets, the monotonic
// clock and sleeping. Winsock spells all of those differently -- errors arrive
// out of band instead of in errno, sockets are closed and switched to
// non-blocking with their own calls -- so the servers in net/ and dbg/ talk to
// this shim instead of to either platform directly.
//
// Sockets stay `int` with -1 for "none", which is what the servers were written
// against and what Winsock's INVALID_SOCKET truncates to anyway.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
// send()/recv() return int here, and MSVC has no ssize_t of its own.
typedef intptr_t ssize_t;
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <sys/select.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

// Once before any socket call: starts Winsock, ignores SIGPIPE where it exists.
void sock_startup(void);

int  sock_open_tcp(void);        // -1 on failure
int  sock_accept(int listen_fd); // -1 when there is nobody waiting
void sock_close(int fd);
void sock_set_nonblocking(int fd);
void sock_set_blocking(int fd);

// Whether the last failed send/recv/accept just meant "not ready yet", and
// whether it was only a signal interrupting the wait.
bool        sock_would_block(void);
bool        sock_interrupted(void);
const char *sock_lasterror(void);

uint64_t os_now_us(void);
void     os_sleep_us(uint64_t us);

// ---- where a packaged build finds its own files -----------------------------
//
// Started from the Dock or from Explorer, athena_sim gets no arguments at all and
// still has to come up with a firmware image, a layout and somewhere to keep its
// flash. These are what it uses to find them; a build run from a shell never calls
// them. Paths come back absolute and without a trailing separator.

// The directory holding the running executable. False when the platform will not
// say, which leaves the caller with nothing but its arguments.
bool os_exe_dir(char *buf, size_t n);

// A per-user writable directory for `app`, created if it does not exist:
//   macOS    ~/Library/Application Support/<app>
//   Windows  %LOCALAPPDATA%\<app>
//   else     $XDG_DATA_HOME/<app>, or ~/.local/share/<app>
bool os_state_dir(const char *app, char *buf, size_t n);

// The names -- not the paths -- of the entries of `dir` ending in `suffix`, in the
// order the filesystem gives them. Returns how many were written, at most `max`.
unsigned os_dir_list(const char *dir, const char *suffix, char (*names)[64], unsigned max);

// Windows only, and nothing at all elsewhere. A GUI-subsystem process starts with
// no stdio, so the window build would print nothing when run from a terminal.
// This hands stdout and stderr back to the console that launched it; launched
// from the shell there is one, double-clicked there is not.
void os_attach_console(void);

// ---- executable memory ------------------------------------------------------
//
// For the native block backends in jit/. Emit into a region, then call
// os_code_commit() over what was written before jumping to it.
//
// The pair looks redundant on x86, where the pages can simply stay writable and
// executable and the caches are coherent, and that is exactly what happens there:
// os_code_writable() is a no-op and os_code_commit() only flushes. It is not
// redundant on Apple silicon, where the kernel refuses a page that is writable and
// executable at the same time -- the region is mapped MAP_JIT and these two toggle
// per-thread write protection around the emission. Callers therefore have to
// bracket every emission, even though on most platforms it costs nothing.
//
// Both are per-block operations, so neither may be a system call on a path that
// runs hundreds of thousands of times a second. That is why the region is mapped
// once with its final permissions rather than mprotect()ed per block.
//
// The size is rounded up to whole pages internally. NULL means the platform would
// not give us executable memory, which is not fatal -- the caller falls back to the
// portable block executor.
void *os_code_alloc(size_t size);
void  os_code_free(void *p, size_t size);

bool os_code_writable(void *p, size_t size);

// Make written bytes runnable: drop write permission where the platform demands it
// and tell the instruction cache. The cache step is the reason this cannot be
// skipped on arm64, where instruction and data caches are not coherent and freshly
// written code is otherwise not guaranteed to be what executes.
bool os_code_commit(void *p, size_t size);
