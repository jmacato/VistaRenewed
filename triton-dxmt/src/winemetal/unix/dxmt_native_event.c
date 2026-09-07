/*
 * Native Win32-style events, kqueue-backed.  See dxmt_native_event.h for the
 * model: the kqueue trigger is the signaled state, plus a lazily-created
 * pipe mirror for the cross-process pollable fd.
 *
 *  - manual-reset registers EVFILT_USER without EV_CLEAR (the trigger stays
 *    pending, every waiter/poller sees it until an explicit clear);
 *    auto-reset registers with EV_CLEAR (retrieval consumes the trigger).
 *  - clearing deletes and re-adds the registration — the only way to
 *    un-trigger EVFILT_USER.
 *  - the exported fd is a pipe: signal writes a token, clear drains it.
 *    pipe_mtx orders lazy creation against signal so a first-dup racing a
 *    signal never loses the edge.
 */

#include "dxmt_native_event.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>

#define EVT_MAGIC 0x54564544u /* 'DEVT' */

struct dxmt_evt_core {
  _Atomic uint32_t refs;
  int manual_reset; /* immutable after creation */
  int kq;           /* the signaled state; immutable after creation */
  pthread_mutex_t pipe_mtx;
  int pipe_r; /* read end handed out (dup'd) by dxmt_event_dup_fd */
  int pipe_w; /* write end poked alongside the kqueue trigger */
};

struct dxmt_evt {
  uint32_t magic;
  struct dxmt_evt_core *core;
};

static struct dxmt_evt *
as_event(void *h) {
  struct dxmt_evt *e = h;
  if (!e || e->magic != EVT_MAGIC)
    return NULL;
  return e;
}

static int
event_register(int kq, int manual_reset) {
  struct kevent ev;
  uint16_t flags = EV_ADD | EV_ENABLE;
  if (!manual_reset)
    flags |= EV_CLEAR;
  EV_SET(&ev, 1, EVFILT_USER, flags, 0, 0, NULL);
  return kevent(kq, &ev, 1, NULL, 0, NULL) == 0;
}

static void
set_cloexec_nonblock(int fd) {
  int fdflags = fcntl(fd, F_GETFD);
  if (fdflags >= 0)
    fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
  int stflags = fcntl(fd, F_GETFL);
  if (stflags >= 0)
    fcntl(fd, F_SETFL, stflags | O_NONBLOCK);
}

/* Peek at the signaled state.  Manual-reset: a zero-timeout read is
 * non-consuming (no EV_CLEAR).  Auto-reset: the read consumes the trigger,
 * so re-arm it with NOTE_TRIGGER — a waiter whose wakeup this stole is
 * re-woken by the re-trigger, and the state stays signaled throughout. */
static int
event_peek_signaled(struct dxmt_evt_core *c) {
  struct kevent out;
  struct timespec zero = {0, 0};
  if (kevent(c->kq, NULL, 0, &out, 1, &zero) <= 0)
    return 0;
  if (!c->manual_reset) {
    struct kevent ev;
    EV_SET(&ev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kevent(c->kq, &ev, 1, NULL, 0, NULL);
  }
  return 1;
}

static void
pipe_write_token(int w) {
  if (w < 0)
    return;
  const uint8_t b = 1;
  ssize_t n;
  do {
    n = write(w, &b, 1);
  } while (n < 0 && errno == EINTR);
  /* EAGAIN: a token is already pending — the "signaled" edge is preserved. */
}

static void
pipe_drain(int r) {
  if (r < 0)
    return;
  uint8_t buf[64];
  ssize_t n;
  do {
    n = read(r, buf, sizeof(buf));
  } while (n > 0 || (n < 0 && errno == EINTR));
}

static int
event_ensure_pipe(struct dxmt_evt_core *c) {
  pthread_mutex_lock(&c->pipe_mtx);
  if (c->pipe_r >= 0) {
    pthread_mutex_unlock(&c->pipe_mtx);
    return 1;
  }
  int fds[2];
  if (pipe(fds) != 0) {
    pthread_mutex_unlock(&c->pipe_mtx);
    return 0;
  }
  set_cloexec_nonblock(fds[0]);
  set_cloexec_nonblock(fds[1]);
  c->pipe_r = fds[0];
  c->pipe_w = fds[1];
  /* Prime the pipe if the event is already signaled so a dup taken after
   * the signal still reports readable. */
  if (event_peek_signaled(c))
    pipe_write_token(c->pipe_w);
  pthread_mutex_unlock(&c->pipe_mtx);
  return 1;
}

static void
event_signal_core(struct dxmt_evt_core *c) {
  struct kevent ev;
  EV_SET(&ev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  kevent(c->kq, &ev, 1, NULL, 0, NULL);
  /* Trigger is set; take the lock only to poke the pipe.  A first-dup that
   * publishes the pipe after this trigger observes it via its post-creation
   * peek, so the token is never lost. */
  pthread_mutex_lock(&c->pipe_mtx);
  pipe_write_token(c->pipe_w);
  pthread_mutex_unlock(&c->pipe_mtx);
}

static void
event_clear_core(struct dxmt_evt_core *c) {
  struct kevent ev;
  EV_SET(&ev, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
  kevent(c->kq, &ev, 1, NULL, 0, NULL);
  event_register(c->kq, c->manual_reset);
  pthread_mutex_lock(&c->pipe_mtx);
  pipe_drain(c->pipe_r);
  pthread_mutex_unlock(&c->pipe_mtx);
}

static void
core_unref(struct dxmt_evt_core *c) {
  if (atomic_fetch_sub_explicit(&c->refs, 1, memory_order_acq_rel) != 1)
    return;
  if (c->pipe_r >= 0)
    close(c->pipe_r);
  if (c->pipe_w >= 0)
    close(c->pipe_w);
  close(c->kq);
  pthread_mutex_destroy(&c->pipe_mtx);
  free(c);
}

void *
dxmt_event_create(int manual_reset, int initial_state) {
  int kq = kqueue();
  if (kq < 0)
    return NULL;
  int fdflags = fcntl(kq, F_GETFD);
  if (fdflags >= 0)
    fcntl(kq, F_SETFD, fdflags | FD_CLOEXEC);
  if (!event_register(kq, manual_reset)) {
    close(kq);
    return NULL;
  }

  struct dxmt_evt_core *c = calloc(1, sizeof(*c));
  if (!c) {
    close(kq);
    return NULL;
  }
  atomic_store_explicit(&c->refs, 1, memory_order_relaxed);
  c->manual_reset = manual_reset != 0;
  c->kq = kq;
  pthread_mutex_init(&c->pipe_mtx, NULL);
  c->pipe_r = -1;
  c->pipe_w = -1;
  if (initial_state)
    event_signal_core(c);

  struct dxmt_evt *e = malloc(sizeof(*e));
  if (!e) {
    core_unref(c);
    return NULL;
  }
  e->magic = EVT_MAGIC;
  e->core = c;
  return e;
}

void
dxmt_event_signal(void *handle) {
  struct dxmt_evt *e = as_event(handle);
  if (e)
    event_signal_core(e->core);
}

void
dxmt_event_clear(void *handle) {
  struct dxmt_evt *e = as_event(handle);
  if (e)
    event_clear_core(e->core);
}

void
dxmt_event_close(void *handle) {
  struct dxmt_evt *e = as_event(handle);
  if (!e)
    return;
  e->magic = 0;
  core_unref(e->core);
  free(e);
}

int
dxmt_event_dup_fd(void *handle) {
  struct dxmt_evt *e = as_event(handle);
  if (!e)
    return -1;
  if (!event_ensure_pipe(e->core))
    return -1;
  pthread_mutex_lock(&e->core->pipe_mtx);
  int fd = e->core->pipe_r >= 0 ? dup(e->core->pipe_r) : -1;
  pthread_mutex_unlock(&e->core->pipe_mtx);
  return fd;
}

dxmt_evt_wait_status
dxmt_event_wait(void *handle, uint64_t timeout_ns) {
  struct dxmt_evt *e = as_event(handle);
  if (!e)
    return DXMT_EVT_WAIT_FAILED;

  uint64_t deadline = 0;
  if (timeout_ns != DXMT_EVT_WAIT_INFINITE)
    deadline = clock_gettime_nsec_np(CLOCK_MONOTONIC) + timeout_ns;

  for (;;) {
    struct timespec ts;
    struct timespec *tsp = NULL;
    if (timeout_ns != DXMT_EVT_WAIT_INFINITE) {
      uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
      uint64_t left = deadline > now ? deadline - now : 0;
      ts.tv_sec = (time_t)(left / 1000000000ull);
      ts.tv_nsec = (long)(left % 1000000000ull);
      tsp = &ts;
    }
    struct kevent out;
    int n = kevent(e->core->kq, NULL, 0, &out, 1, tsp);
    if (n > 0) {
      /* Auto-reset: this waiter consumed the single trigger, so the
       * pollable pipe must clear too.  Manual-reset stays signaled.
       * The drain can also eat the token of a second signal that landed
       * after our kevent consumed the trigger; if the event is signaled
       * again already, put a token back so the exported fd stays in
       * sync with the kqueue state. */
      if (!e->core->manual_reset) {
        pthread_mutex_lock(&e->core->pipe_mtx);
        pipe_drain(e->core->pipe_r);
        if (event_peek_signaled(e->core))
          pipe_write_token(e->core->pipe_w);
        pthread_mutex_unlock(&e->core->pipe_mtx);
      }
      return DXMT_EVT_WAIT_SIGNALED;
    }
    if (n == 0)
      return DXMT_EVT_WAIT_TIMEOUT;
    if (errno != EINTR)
      return DXMT_EVT_WAIT_FAILED;
  }
}

struct dxmt_evt_core *
dxmt_evt_core_retain(void *handle) {
  struct dxmt_evt *e = as_event(handle);
  if (!e)
    return NULL;
  atomic_fetch_add_explicit(&e->core->refs, 1, memory_order_relaxed);
  return e->core;
}

void
dxmt_evt_core_signal_release(struct dxmt_evt_core *core) {
  if (!core)
    return;
  event_signal_core(core);
  core_unref(core);
}
