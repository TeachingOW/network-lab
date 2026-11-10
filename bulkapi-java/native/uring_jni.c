/* JNI shim for bulkapi.Native: ring setup through liburing, io_uring_enter, and the few socket
 * calls the io_uring backend needs. Java reads and writes the rings directly, so this is all
 * the native code there is. */
#include <jni.h>

#include <errno.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

JNIEXPORT jlongArray JNICALL Java_bulkapi_Native_ringSetup(JNIEnv *env, jclass cls, jint entries, jint cq_entries) {
  (void)cls;
  struct io_uring *ring = calloc(1, sizeof *ring); /* lives as long as the process */
  struct io_uring_params p = {0};
  p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
  p.cq_entries = (unsigned)cq_entries;
  int rc = ring ? io_uring_queue_init_params((unsigned)entries, ring, &p) : -ENOMEM;
  if (rc < 0) {
    jlong err = rc;
    jlongArray a = (*env)->NewLongArray(env, 1);
    if (a) (*env)->SetLongArrayRegion(env, a, 0, 1, &err);
    free(ring);
    return a;
  }
  jlong v[11] = {
      ring->ring_fd,
      (jlong)(uintptr_t)ring->sq.khead, (jlong)(uintptr_t)ring->sq.ktail, ring->sq.ring_mask, ring->sq.ring_entries,
      (jlong)(uintptr_t)ring->sq.sqes,
      (jlong)(uintptr_t)ring->cq.khead, (jlong)(uintptr_t)ring->cq.ktail, ring->cq.ring_mask, ring->cq.ring_entries,
      (jlong)(uintptr_t)ring->cq.cqes,
  };
  jlongArray a = (*env)->NewLongArray(env, 11);
  if (a) (*env)->SetLongArrayRegion(env, a, 0, 11, v);
  return a;
}

JNIEXPORT jint JNICALL Java_bulkapi_Native_enter(JNIEnv *env, jclass cls, jint fd, jint to_submit, jint min_complete, jint flags) {
  (void)env; (void)cls;
  return io_uring_enter((unsigned)fd, (unsigned)to_submit, (unsigned)min_complete, (unsigned)flags, NULL);
}

JNIEXPORT jobject JNICALL Java_bulkapi_Native_wrap(JNIEnv *env, jclass cls, jlong addr, jlong len) {
  (void)cls;
  return (*env)->NewDirectByteBuffer(env, (void *)(uintptr_t)addr, len);
}

JNIEXPORT jlong JNICALL Java_bulkapi_Native_address(JNIEnv *env, jclass cls, jobject buf) {
  (void)cls;
  return (jlong)(uintptr_t)(*env)->GetDirectBufferAddress(env, buf);
}

JNIEXPORT jint JNICALL Java_bulkapi_Native_listen(JNIEnv *env, jclass cls, jint port, jint backlog) {
  (void)env; (void)cls;
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -errno;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); /* inherited by accepted sockets */
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, backlog) < 0) {
    int e = errno;
    close(fd);
    return -e;
  }
  return fd;
}

JNIEXPORT jint JNICALL Java_bulkapi_Native_close(JNIEnv *env, jclass cls, jint fd) {
  (void)env; (void)cls;
  return close(fd);
}

/* Order must match the K_* indexes in Native.java. */
JNIEXPORT jintArray JNICALL Java_bulkapi_Native_constants(JNIEnv *env, jclass cls) {
  (void)cls;
  jint v[] = {
      IORING_OP_ACCEPT, IORING_OP_RECV, IORING_OP_SEND, IORING_ACCEPT_MULTISHOT, IORING_CQE_F_MORE,
      IORING_ENTER_GETEVENTS, MSG_NOSIGNAL, SOCK_CLOEXEC,
      sizeof(struct io_uring_sqe), offsetof(struct io_uring_sqe, opcode), offsetof(struct io_uring_sqe, ioprio),
      offsetof(struct io_uring_sqe, fd), offsetof(struct io_uring_sqe, addr), offsetof(struct io_uring_sqe, len),
      offsetof(struct io_uring_sqe, msg_flags), offsetof(struct io_uring_sqe, user_data),
      sizeof(struct io_uring_cqe), offsetof(struct io_uring_cqe, user_data), offsetof(struct io_uring_cqe, res),
      offsetof(struct io_uring_cqe, flags),
      EINTR, EAGAIN, EBUSY,
  };
  jsize n = (jsize)(sizeof v / sizeof v[0]);
  jintArray a = (*env)->NewIntArray(env, n);
  if (a) (*env)->SetIntArrayRegion(env, a, 0, n, v);
  return a;
}

JNIEXPORT jstring JNICALL Java_bulkapi_Native_liburingVersion(JNIEnv *env, jclass cls) {
  (void)cls;
  char s[32];
  snprintf(s, sizeof s, "%d.%d", io_uring_major_version(), io_uring_minor_version());
  return (*env)->NewStringUTF(env, s);
}
