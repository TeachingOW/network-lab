// Socket setup shared by both I/O backends.
#pragma once

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

// SO_REUSEPORT listener on 0.0.0.0:port (one per worker). Returns -1 on error.
int make_listener(int port);

inline void nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}
