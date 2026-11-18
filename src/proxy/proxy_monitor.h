/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_MONITOR_H
#define PROXY_MONITOR_H

#include <stdbool.h>
#include <sys/types.h>

/* Process-wide child-exit monitor backed by one kqueue thread
 * (EVFILT_PROC / NOTE_EXIT).  Self-contained on purpose so tests can
 * build it without the rest of the proxy.
 *
 * The callback runs on the monitor thread while the monitor mutex is
 * held; it must be fast and must not call back into this API.  unwatch
 * blocks until any in-flight callback for the watch has finished, so a
 * destroy racing an exit event cannot touch freed memory. */
struct proxy_monitor_watch;

/* reaped is true only when the monitor's waitid() actually reaped the
 * child; the callback may run on a non-reaped (still-zombie) child. */
typedef void (*proxy_monitor_exit_fn)(void *udata, bool reaped);

/* Starts the monitor thread on first use; safe to call repeatedly. */
bool
proxy_monitor_start(void);

/* Stops the monitor thread; only safe when no watches are registered.
 * Mostly for tests so the process can exit cleanly. */
void
proxy_monitor_shutdown(void);

struct proxy_monitor_watch *
proxy_monitor_watch(pid_t pid, proxy_monitor_exit_fn cb, void *udata);

void
proxy_monitor_unwatch(struct proxy_monitor_watch *watch);

#endif /* PROXY_MONITOR_H */
