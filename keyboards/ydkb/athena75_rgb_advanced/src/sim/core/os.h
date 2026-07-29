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
