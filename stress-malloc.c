/*
 * Copyright (C) 2013-2016 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * This code is a complete clean re-write of the stress tool by
 * Colin Ian King <colin.king@canonical.com> and attempts to be
 * backwardly compatible with the stress tool by Amos Waterland
 * <apw@rossby.metr.ou.edu> but has more stress tests and more
 * functionality.
 *
 */
#define _GNU_SOURCE

#include "stress-ng.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#if defined(STRESS_MALLOPT)
#include <malloc.h>
#endif

static size_t opt_malloc_bytes = DEFAULT_MALLOC_BYTES;
static bool set_malloc_bytes = false;

static size_t opt_malloc_max = DEFAULT_MALLOC_MAX;
static bool set_malloc_max = false;

#if defined(STRESS_MALLOPT)
static int opt_malloc_threshold = DEFAULT_MALLOC_THRESHOLD;
static bool set_malloc_threshold = false;
#endif

void stress_set_malloc_bytes(const char *optarg)
{
	set_malloc_bytes = true;
	opt_malloc_bytes = (size_t)get_uint64_byte(optarg);
	check_range("malloc-bytes", opt_malloc_bytes,
		MIN_MALLOC_BYTES, MAX_MALLOC_BYTES);
}

void stress_set_malloc_max(const char *optarg)
{
	set_malloc_max = true;
	opt_malloc_max = (size_t)get_uint64_byte(optarg);
	check_range("malloc-max", opt_malloc_max,
		MIN_MALLOC_MAX, MAX_MALLOC_MAX);
}

#if defined(STRESS_MALLOPT)
void stress_set_malloc_threshold(const char *optarg)
{
	set_malloc_threshold = true;
	opt_malloc_threshold = (size_t)get_uint64_byte(optarg);
	check_range("malloc-threshold", opt_malloc_threshold,
		MIN_MALLOC_THRESHOLD, MAX_MALLOC_THRESHOLD);
}
#endif

/*
 *  stress_alloc_size()
 *	get a new allocation size, ensuring
 *	it is never zero bytes.
 */
static inline size_t stress_alloc_size(void)
{
	size_t len = mwc64() % opt_malloc_bytes;

	return len ? len : 1;
}

/*
 *  stress_malloc()
 *	stress malloc by performing a mix of
 *	allocation and frees
 */
int stress_malloc(
	uint64_t *const counter,
	const uint32_t instance,
	const uint64_t max_ops,
	const char *name)
{
	pid_t pid;
	uint32_t restarts = 0, nomems = 0;

	if (!set_malloc_bytes) {
		if (opt_flags & OPT_FLAGS_MAXIMIZE)
			opt_malloc_bytes = MAX_MALLOC_BYTES;
		if (opt_flags & OPT_FLAGS_MINIMIZE)
			opt_malloc_bytes = MIN_MALLOC_BYTES;
	}

	if (!set_malloc_max) {
		if (opt_flags & OPT_FLAGS_MAXIMIZE)
			opt_malloc_max = MAX_MALLOC_MAX;
		if (opt_flags & OPT_FLAGS_MINIMIZE)
			opt_malloc_max = MIN_MALLOC_MAX;
	}

#if defined(STRESS_MALLOPT)
	if (set_malloc_threshold)
		mallopt(M_MMAP_THRESHOLD, opt_malloc_threshold);
#endif

again:
	pid = fork();
	if (pid < 0) {
		if (opt_do_run && (errno == EAGAIN))
			goto again;
		pr_err(stderr, "%s: fork failed: errno=%d: (%s)\n",
			name, errno, strerror(errno));
	} else if (pid > 0) {
		int status, ret;

		(void)setpgid(pid, pgrp);
		stress_parent_died_alarm();

		/* Parent, wait for child */
		ret = waitpid(pid, &status, 0);
		if (ret < 0) {
			if (errno != EINTR)
				pr_dbg(stderr, "%s: waitpid(): errno=%d (%s)\n",
					name, errno, strerror(errno));
			(void)kill(pid, SIGTERM);
			(void)kill(pid, SIGKILL);
			(void)waitpid(pid, &status, 0);
		} else if (WIFSIGNALED(status)) {
			pr_dbg(stderr, "%s: child died: %s (instance %d)\n",
				name, stress_strsignal(WTERMSIG(status)),
				instance);
			/* If we got killed by OOM killer, re-start */
			if (WTERMSIG(status) == SIGKILL) {
				log_system_mem_info();
				pr_dbg(stderr, "%s: assuming killed by OOM "
					"killer, restarting again "
					"(instance %d)\n",
					name, instance);
				restarts++;
				goto again;
			}
		}
	} else if (pid == 0) {
		void *addr[opt_malloc_max];
		size_t j;

		(void)setpgid(0, pgrp);
		memset(addr, 0, sizeof(addr));

		/* Make sure this is killable by OOM killer */
		set_oom_adjustment(name, true);

		do {
			unsigned int rnd = mwc32();
			unsigned int i = rnd % opt_malloc_max;
			unsigned int action = (rnd >> 12) & 1;
			unsigned int do_calloc = (rnd >> 14) & 0x1f;

			/*
			 * With many instances running it is wise to
			 * double check before the next allocation as
			 * sometimes process start up is delayed for
			 * some time and we should bail out before
			 * exerting any more memory pressure
			 */
			if (!opt_do_run)
				goto abort;

			if (addr[i]) {
				/* 50% free, 50% realloc */
				if (action) {
					free(addr[i]);
					addr[i] = NULL;
					(*counter)++;
				} else {
					void *tmp;
					size_t len = stress_alloc_size();

					tmp = realloc(addr[i], len);
					if (tmp) {
						addr[i] = tmp;
						(void)mincore_touch_pages(addr[i], len);
						(*counter)++;
					}
				}
			} else {
				/* 50% free, 50% alloc */
				if (action) {
					size_t len = stress_alloc_size();

					if (do_calloc == 0) {
						size_t n = ((rnd >> 15) % 17) + 1;
						addr[i] = calloc(n, len / n);
						len = n * (len / n);
					} else {
						addr[i] = malloc(len);
					}
					if (addr[i]) {
						(*counter)++;
						(void)mincore_touch_pages(addr[i], len);
					}
				}
			}
		} while (opt_do_run && (!max_ops || *counter < max_ops));
abort:
		for (j = 0; j < opt_malloc_max; j++) {
			free(addr[j]);
		}
	}
	if (restarts + nomems > 0)
		pr_dbg(stderr, "%s: OOM restarts: %" PRIu32
			", out of memory restarts: %" PRIu32 ".\n",
			name, restarts, nomems);

	return EXIT_SUCCESS;
}
