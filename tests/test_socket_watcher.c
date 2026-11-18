/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

/* Watches the same kqueue registration the context socket watcher uses
 * (server/render_context.c): EVFILT_READ with NOTE_LOWAT=INT_MAX must not
 * report ordinary socket data, while peer close must still report EV_EOF.
 * Also sanity-checks that the same socket does produce a read event with a
 * default low-water mark, so the first half cannot pass vacuously. */

#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                       \
   do {                                                                        \
      if (!(cond)) {                                                           \
         fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                  \
         fprintf(stderr, __VA_ARGS__);                                         \
         fprintf(stderr, "\n");                                                \
         failures++;                                                           \
      }                                                                        \
   } while (0)

static int
wait_event(int kq, int timeout_ms, struct kevent *out)
{
   const struct timespec ts = {
      .tv_sec = timeout_ms / 1000,
      .tv_nsec = (long)(timeout_ms % 1000) * 1000000,
   };
   return kevent(kq, NULL, 0, out, 1, &ts);
}

int
main(void)
{
   int fds[2];
   CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
         "socketpair: %s", strerror(errno));
   if (failures)
      return 1;

   int kq = kqueue();
   CHECK(kq >= 0, "kqueue: %s", strerror(errno));

   /* high low-water mark: data alone must not report */
   struct kevent kev;
   EV_SET(&kev, fds[0], EVFILT_READ, EV_ADD | EV_CLEAR, NOTE_LOWAT, INT_MAX,
          NULL);
   CHECK(kevent(kq, &kev, 1, NULL, 0, NULL) == 0,
         "register NOTE_LOWAT read: %s", strerror(errno));

   const char payload[256] = {0x42};
   CHECK(write(fds[1], payload, sizeof(payload)) == (ssize_t)sizeof(payload),
         "write payload: %s", strerror(errno));

   struct kevent out;
   int n = wait_event(kq, 100, &out);
   CHECK(n == 0, "lowat: data produced %d event(s), expected 0 "
         "(filter=0x%x flags=0x%x)", n, out.filter, out.flags);

   /* drain, then re-register with the default low-water mark: data must
    * report, proving the suppressed case was not a dead registration */
   CHECK(read(fds[0], (char[256]){0}, 256) == 256, "drain: %s",
         strerror(errno));
   EV_SET(&kev, fds[0], EVFILT_READ, EV_DELETE, 0, 0, NULL);
   CHECK(kevent(kq, &kev, 1, NULL, 0, NULL) == 0, "delete lowat: %s",
         strerror(errno));
   EV_SET(&kev, fds[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
   CHECK(kevent(kq, &kev, 1, NULL, 0, NULL) == 0, "register default: %s",
         strerror(errno));
   CHECK(write(fds[1], payload, sizeof(payload)) == (ssize_t)sizeof(payload),
         "write payload 2: %s", strerror(errno));
   n = wait_event(kq, 2000, &out);
   CHECK(n == 1 && out.filter == EVFILT_READ,
         "default lowat: got %d events filter=0x%x", n, out.filter);

   /* back to the watcher registration; peer close must still report EOF */
   EV_SET(&kev, fds[0], EVFILT_READ, EV_DELETE, 0, 0, NULL);
   CHECK(kevent(kq, &kev, 1, NULL, 0, NULL) == 0, "delete default: %s",
         strerror(errno));
   EV_SET(&kev, fds[0], EVFILT_READ, EV_ADD | EV_CLEAR, NOTE_LOWAT, INT_MAX,
          NULL);
   CHECK(kevent(kq, &kev, 1, NULL, 0, NULL) == 0, "re-register lowat: %s",
         strerror(errno));
   CHECK(close(fds[1]) == 0, "close peer: %s", strerror(errno));

   n = wait_event(kq, 2000, &out);
   CHECK(n == 1 && out.filter == EVFILT_READ && (out.flags & EV_EOF),
         "lowat: EOF got %d events filter=0x%x flags=0x%x", n, out.filter,
         out.flags);

   close(fds[0]);
   close(kq);

   if (failures) {
      fprintf(stderr, "test_socket_watcher: %d failure(s)\n", failures);
      return 1;
   }
   printf("test_socket_watcher: all checks passed\n");
   return 0;
}
