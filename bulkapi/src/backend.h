// I/O backends: each runs one worker's event loop on its own listener.
#pragma once

#include "conn.h"

// Never returns.
void run_epoll(Worker& w, int lfd);

// io_uring with single-shot receive (Linux 6.1+). Returns false if the ring cannot be set up.
bool run_uring(Worker& w, int lfd);

// io_uring with multishot receive into a provided-buffer ring (Linux 6.0+).
// Returns false if the ring or the buffer ring cannot be set up.
bool run_uring_ms(Worker& w, int lfd);
