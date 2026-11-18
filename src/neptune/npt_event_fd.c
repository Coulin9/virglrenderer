/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Platform backend for Win32-style events, shared by the guest event
 * proxies (npt_event.c) and the fence-feedback arm events
 * (npt_feedback.c).  See npt_event.h for the contract.
 */

#include "npt_event.h"

#include "npt_library.h"
#include "npt_renderer.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif

int
npt_event_fd_create(struct npt_event_fd *out)
{
#if defined(__linux__)
   out->fd = eventfd(0, EFD_CLOEXEC);
   return out->fd < 0 ? -1 : 0;
#elif defined(__APPLE__)
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_event_create) {
      npt_log("event: backend event API unavailable");
      return -1;
   }
   /* Manual-reset matches Win32's default for guest-created HANDLEs
    * (we don't auto-clear on read). */
   out->handle = lib->pfn_event_create(/*manual_reset=*/1, /*initial_state=*/0);
   return out->handle ? 0 : -1;
#else
   /* Prefer pipe2(O_CLOEXEC) where available (Linux, FreeBSD) to
    * avoid the fd-leak window between pipe() and fcntl().  Fall back
    * to pipe() + fcntl() on platforms without pipe2. */
   int fds[2];
#ifdef HAVE_PIPE2
   if (pipe2(fds, O_CLOEXEC) == 0) {
      out->read_fd = fds[0];
      out->write_fd = fds[1];
      return 0;
   }
   if (errno != ENOSYS)
      return -1;
#endif
   if (pipe(fds) != 0)
      return -1;
   /* Best-effort CLOEXEC: non-fatal, fds remain usable on failure. */
   for (int i = 0; i < 2; i++) {
      int flags = fcntl(fds[i], F_GETFD);
      if (flags >= 0)
         fcntl(fds[i], F_SETFD, flags | FD_CLOEXEC);
   }
   out->read_fd = fds[0];
   out->write_fd = fds[1];
   return 0;
#endif
}

void
npt_event_fd_destroy(struct npt_event_fd *fd)
{
#if defined(__linux__)
   if (fd->fd >= 0) {
      close(fd->fd);
      fd->fd = -1;
   }
#elif defined(__APPLE__)
   if (fd->handle) {
      struct npt_d3d_library *lib = npt_renderer_get_library();
      if (lib && lib->pfn_event_close)
         lib->pfn_event_close(fd->handle);
      fd->handle = NULL;
   }
#else
   if (fd->read_fd >= 0) {
      close(fd->read_fd);
      fd->read_fd = -1;
   }
   if (fd->write_fd >= 0) {
      close(fd->write_fd);
      fd->write_fd = -1;
   }
#endif
}

/* The value handed to the host as the HANDLE; SetEvent acts on it.
 * An fd cast to a pointer on eventfd/pipe platforms, the backend's
 * event handle on darwin. */
void *
npt_event_fd_signal_handle(const struct npt_event_fd *fd)
{
#if defined(__linux__)
   return fd->fd >= 0 ? (void *)(uintptr_t)fd->fd : NULL;
#elif defined(__APPLE__)
   return fd->handle;
#else
   return fd->write_fd >= 0 ? (void *)(uintptr_t)fd->write_fd : NULL;
#endif
}

/* Caller polls + closes the returned dup.
 *
 * Always non-blocking: a ring's gate event is shared by every gate on
 * it, so the sync worker cannot know how many wakeups are queued and
 * has to drain with a read loop that ends on EAGAIN.  A blocking fd
 * would park that worker in read() with the whole queue behind it. */
int
npt_event_fd_dup_wait_fd(const struct npt_event_fd *fd)
{
#if defined(__linux__)
   int wait_fd = dup(fd->fd);
#elif defined(__APPLE__)
   struct npt_d3d_library *lib = npt_renderer_get_library();
   int wait_fd = (lib && lib->pfn_event_dup_fd)
                    ? lib->pfn_event_dup_fd(fd->handle) : -1;
#else
   int wait_fd = dup(fd->read_fd);
#endif
   if (wait_fd < 0)
      return -1;

   const int flags = fcntl(wait_fd, F_GETFL);
   if (flags < 0 || fcntl(wait_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      npt_log("event: O_NONBLOCK on the wait fd failed: %s", strerror(errno));
      close(wait_fd);
      return -1;
   }
   return wait_fd;
}

/* Take every queued wakeup off a wait fd.  The fd is level-triggered
 * and may be shared across fires, so a token left behind would look
 * like a fire that never happened.  The wait fd is non-blocking, so
 * the loop ends on EAGAIN.  Returns whether anything was actually
 * there: a poll that reports readable and then yields nothing is a
 * dead fd (POLLERR/POLLHUP), not a fire. */
bool
npt_event_fd_drain_wait_fd(int fd)
{
   if (fd < 0)
      return false;

   bool drained = false;
   uint64_t token;
   ssize_t n;
   do {
      n = read(fd, &token, sizeof(token));
      drained |= n > 0;
   } while (n > 0 || (n < 0 && errno == EINTR));

   return drained;
}

void
npt_event_fd_init_invalid(struct npt_event_fd *fd)
{
#if defined(__linux__)
   fd->fd = -1;
#elif defined(__APPLE__)
   fd->handle = NULL;
#else
   fd->read_fd = -1;
   fd->write_fd = -1;
#endif
}

bool
npt_event_fd_is_valid(const struct npt_event_fd *fd)
{
#if defined(__linux__)
   return fd->fd >= 0;
#elif defined(__APPLE__)
   return fd->handle != NULL;
#else
   return fd->read_fd >= 0;
#endif
}
