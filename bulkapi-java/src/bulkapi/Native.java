package bulkapi;

import java.nio.ByteBuffer;

/** JNI shim (native/uring_jni.c, built on liburing). Only the io_uring backend loads it. */
final class Native {
    static { System.loadLibrary("bulkuring"); }

    // indexes into the array returned by ringSetup
    static final int R_FD = 0, R_SQ_HEAD = 1, R_SQ_TAIL = 2, R_SQ_MASK = 3, R_SQ_ENTRIES = 4, R_SQES = 5,
            R_CQ_HEAD = 6, R_CQ_TAIL = 7, R_CQ_MASK = 8, R_CQ_ENTRIES = 9, R_CQES = 10;
    // indexes into the array returned by constants
    static final int K_OP_ACCEPT = 0, K_OP_RECV = 1, K_OP_SEND = 2, K_ACCEPT_MULTISHOT = 3, K_CQE_F_MORE = 4,
            K_ENTER_GETEVENTS = 5, K_MSG_NOSIGNAL = 6, K_SOCK_CLOEXEC = 7,
            K_SQE_SIZE = 8, K_SQE_OPCODE = 9, K_SQE_IOPRIO = 10, K_SQE_FD = 11, K_SQE_ADDR = 12, K_SQE_LEN = 13,
            K_SQE_OPFLAGS = 14, K_SQE_USER_DATA = 15,
            K_CQE_SIZE = 16, K_CQE_USER_DATA = 17, K_CQE_RES = 18, K_CQE_FLAGS = 19,
            K_EINTR = 20, K_EAGAIN = 21, K_EBUSY = 22;

    /** Creates a ring (SINGLE_ISSUER | DEFER_TASKRUN). Returns {-errno} on failure. */
    static native long[] ringSetup(int entries, int cqEntries);
    /** io_uring_enter; returns the number of SQEs consumed or -errno. */
    static native int enter(int ringFd, int toSubmit, int minComplete, int flags);
    /** A direct buffer over native memory (the mapped rings). */
    static native ByteBuffer wrap(long address, long length);
    /** Native address of a direct buffer. */
    static native long address(ByteBuffer direct);
    /** SO_REUSEPORT listener with TCP_NODELAY (inherited by accepted sockets). Returns fd or -errno. */
    static native int listen(int port, int backlog);
    static native int close(int fd);
    static native int[] constants();
    static native String liburingVersion();
}
