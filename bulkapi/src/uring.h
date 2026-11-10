// Minimal io_uring wrapper on raw syscalls, used by the single-shot backend
// (backend_uring.cpp). Only liburing's ABI header is used here, not the library.
#pragma once

#include <liburing/io_uring.h>  // current io_uring ABI, shipped with liburing (see Makefile)
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>

struct Uring {
    int fd = -1;
    unsigned entries = 0, local_tail = 0, pending = 0;
    unsigned *sq_head, *sq_tail, *sq_mask, *sq_array, *cq_head, *cq_tail, *cq_mask;
    io_uring_sqe* sqes;
    io_uring_cqe* cqes;
    const char* mode = "";

    bool init(unsigned ent, unsigned cq_ent) {
        struct Try { unsigned flags; const char* name; };
        Try tries[] = {
            {IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN, "single-issuer+defer-taskrun"},
            {IORING_SETUP_CQSIZE | IORING_SETUP_COOP_TASKRUN, "coop-taskrun"},
            {IORING_SETUP_CQSIZE, "plain"},
        };
        io_uring_params p;
        for (auto& t : tries) {
            memset(&p, 0, sizeof p);
            p.flags = t.flags;
            p.cq_entries = cq_ent;
            fd = (int)syscall(__NR_io_uring_setup, ent, &p);
            if (fd >= 0) { mode = t.name; break; }
        }
        if (fd < 0) return false;
        size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
        size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
        void* sq_ptr; void* cq_ptr;
        if (p.features & IORING_FEAT_SINGLE_MMAP) {
            sq_sz = cq_sz = std::max(sq_sz, cq_sz);
            sq_ptr = cq_ptr = mmap(nullptr, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
        } else {
            sq_ptr = mmap(nullptr, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
            cq_ptr = mmap(nullptr, cq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
        }
        sqes = (io_uring_sqe*)mmap(nullptr, p.sq_entries * sizeof(io_uring_sqe), PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
        if (sq_ptr == MAP_FAILED || cq_ptr == MAP_FAILED || sqes == MAP_FAILED) { close(fd); fd = -1; return false; }
        char* s = (char*)sq_ptr; char* q = (char*)cq_ptr;
        sq_head = (unsigned*)(s + p.sq_off.head);
        sq_tail = (unsigned*)(s + p.sq_off.tail);
        sq_mask = (unsigned*)(s + p.sq_off.ring_mask);
        sq_array = (unsigned*)(s + p.sq_off.array);
        cq_head = (unsigned*)(q + p.cq_off.head);
        cq_tail = (unsigned*)(q + p.cq_off.tail);
        cq_mask = (unsigned*)(q + p.cq_off.ring_mask);
        cqes = (io_uring_cqe*)(q + p.cq_off.cqes);
        entries = p.sq_entries;
        for (unsigned i = 0; i < entries; i++) sq_array[i] = i;
        local_tail = *sq_tail;
        return true;
    }

    // Push queued SQEs to the kernel; optionally wait for >= 1 completion.
    int enter(bool wait) {
        __atomic_store_n(sq_tail, local_tail, __ATOMIC_RELEASE);
        for (;;) {
            unsigned flags = wait ? IORING_ENTER_GETEVENTS : 0;
            long r = syscall(__NR_io_uring_enter, fd, pending, wait ? 1u : 0u, flags, nullptr, 0);
            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EBUSY) return 0;  // CQ pressure: caller drains
                return -1;
            }
            pending -= (unsigned)r;
            if (pending && !wait) continue;
            return 0;
        }
    }

    io_uring_sqe* get_sqe() {
        for (;;) {
            unsigned head = __atomic_load_n(sq_head, __ATOMIC_ACQUIRE);
            if (local_tail - head < entries) break;
            enter(false);  // SQ full: flush
        }
        io_uring_sqe* e = &sqes[local_tail & *sq_mask];
        local_tail++;
        pending++;
        memset(e, 0, sizeof *e);
        return e;
    }
};
