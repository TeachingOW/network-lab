// I/O backends: each runs one worker's event loop on its own listener.
#pragma once

#include "conn.h"

// Never returns.
void run_epoll(Worker& w, int lfd);

// Returns false if io_uring is unusable (caller falls back to epoll).
bool run_uring(Worker& w, int lfd);
