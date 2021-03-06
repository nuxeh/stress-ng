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

#if defined(STRESS_RMAP)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

#define RMAP_CHILD_MAX		(16)
#define MAPPINGS_MAX		(64)
#define MAPPING_PAGES		(16)

/*
 *  [ MAPPING 0 ]
 *  [ page ][ MAPPING 1 ]
 *  [ page ][page ][ MAPPING 2]
 *  [ page ][page ][ page ][ MAPPING 3]
 *
 *  file size = ((MAPPINGS_MAX - 1) + MAPPING_PAGES) * page_size;
 */

/*
 *  stress_rmap_handler()
 *      rmap signal handler
 */
static void MLOCKED stress_rmap_handler(int dummy)
{
	(void)dummy;

	kill(getppid(), SIGALRM);
	exit(0);
}

static void stress_rmap_child(
	uint64_t *const counter,
	const uint64_t max_ops,
	const size_t page_size,
	uint8_t *mappings[MAPPINGS_MAX])
{
	const size_t sz = MAPPING_PAGES * page_size;

	do {
		ssize_t i;
		const int sync_flag = mwc8() ? MS_ASYNC : MS_SYNC;

		switch (mwc32() & 3) {
		case 0: for (i = 0; opt_do_run && (i < MAPPINGS_MAX); i++) {
				memset(mappings[i], mwc8(), sz);
				msync(mappings[i], sz, sync_flag);
			}
			break;
		case 1: for (i = MAPPINGS_MAX - 1; opt_do_run && (i >= 0); i--) {
				memset(mappings[i], mwc8(), sz);
				msync(mappings[i], sz, sync_flag);
			}
			break;
		case 2: for (i = 0; opt_do_run && (i < MAPPINGS_MAX); i++) {
				size_t j = mwc32() % MAPPINGS_MAX;
				memset(mappings[j], mwc8(), sz);
				msync(mappings[j], sz, sync_flag);
			}
			break;
		case 3: for (i = 0; opt_do_run && (i < MAPPINGS_MAX - 1); i++) {
				memcpy(mappings[i], mappings[i + 1], sz);
				msync(mappings[i], sz, sync_flag);
			}
			break;
		}
		(*counter)++;
	} while (opt_do_run && (!max_ops || *counter < max_ops));

	exit(0);
}

/*
 *  stress_rmap()
 *	stress mmap
 */
int stress_rmap(
	uint64_t *const counter,
	const uint32_t instance,
	const uint64_t max_ops,
	const char *name)
{
	const size_t page_size = stress_get_pagesize();
	const size_t sz = ((MAPPINGS_MAX - 1) + MAPPING_PAGES) * page_size;
	const size_t counters_sz =
		(page_size + sizeof(uint64_t) * RMAP_CHILD_MAX) & ~(page_size - 1);
	const pid_t mypid = getpid();
	int fd = -1;
	size_t i;
	ssize_t rc;
	pid_t pids[RMAP_CHILD_MAX];
	uint8_t *mappings[MAPPINGS_MAX];
	uint8_t *paddings[MAPPINGS_MAX];
	uint64_t *counters;
	char filename[PATH_MAX];

	counters = (uint64_t *)mmap(NULL, counters_sz, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (counters == MAP_FAILED) {
		pr_err(stderr, "%s: mmap failed: errno=%d (%s)\n",
			name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}
	memset(counters, 0, counters_sz);
	memset(pids, 0, sizeof(pids));
	memset(mappings, 0, sizeof(mappings));

	/* Make sure this is killable by OOM killer */
	set_oom_adjustment(name, true);

	rc = stress_temp_dir_mk(name, mypid, instance);
	if (rc < 0)
		return exit_status(-rc);

	(void)stress_temp_filename(filename, sizeof(filename),
		name, mypid, instance, mwc32());

	(void)umask(0077);
	if ((fd = open(filename, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR)) < 0) {
		rc = exit_status(errno);
		pr_fail_err(name, "open");
		(void)unlink(filename);
		(void)stress_temp_dir_rm(name, mypid, instance);
		(void)munmap((void *)counters, counters_sz);

		return rc;
	}
	(void)unlink(filename);

	if (posix_fallocate(fd, 0, sz) < 0) {
		pr_fail_err(name, "posix_fallocate");
		(void)close(fd);
		(void)stress_temp_dir_rm(name, mypid, instance);
		(void)munmap((void *)counters, counters_sz);

		return EXIT_FAILURE;
	}

	for (i = 0; i < MAPPINGS_MAX; i++) {
		off_t offset = i * page_size;
		mappings[i] =
			mmap(0, MAPPING_PAGES * page_size, PROT_READ | PROT_WRITE,
				MAP_SHARED, fd, offset);
		/* Squeeze at least a page in between each mapping */
		paddings[i] =
			mmap(0, page_size, PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	}

	/*
	 *  Spawn of children workers
	 */
	for (i = 0; i < RMAP_CHILD_MAX; i++) {
		pids[i] = fork();
		if (pids[i] < 0) {
			pr_err(stderr, "%s: fork failed: errno=%d: (%s)\n",
				name, errno, strerror(errno));
			goto cleanup;
		} else if (pids[i] == 0) {
			if (stress_sighandler(name, SIGALRM,
			    stress_rmap_handler, NULL) < 0)
				exit(EXIT_FAILURE);

			(void)setpgid(0, pgrp);
			stress_parent_died_alarm();

			/* Make sure this is killable by OOM killer */
			set_oom_adjustment(name, true);
			stress_rmap_child(&counters[i], max_ops / RMAP_CHILD_MAX,
				page_size, mappings);
		} else {
			(void)setpgid(pids[i], pgrp);
		}
	}

	/*
	 *  Wait for SIGINT or SIGALRM
	 */
	do {
		(void)select(0, NULL, NULL, NULL, NULL);
		for (i = 0; i < RMAP_CHILD_MAX; i++)
			*counter += counters[i];
	} while (opt_do_run && (!max_ops || *counter < max_ops));

cleanup:
	/*
	 *  Kill and wait for children
	 */
	for (i = 0; i < RMAP_CHILD_MAX; i++) {
		int status, ret;

		if (pids[i] <= 0)
			continue;

		(void)kill(pids[i], SIGKILL);
		ret = waitpid(pids[i], &status, 0);
		if (ret < 0) {
			if (errno != EINTR)
				pr_dbg(stderr, "%s: waitpid(): errno=%d (%s)\n",
					name, errno, strerror(errno));
			(void)kill(pids[i], SIGTERM);
			(void)kill(pids[i], SIGKILL);
			(void)waitpid(pids[i], &status, 0);
		}
	}

	(void)munmap((void *)counters, counters_sz);
	(void)close(fd);
	(void)stress_temp_dir_rm(name, mypid, instance);

	for (i = 0; i < MAPPINGS_MAX; i++) {
		if (mappings[i] != MAP_FAILED)
			munmap(mappings[i], MAPPING_PAGES * page_size);
		if (paddings[i] != MAP_FAILED)
			munmap(paddings[i], page_size);
	}

	return EXIT_SUCCESS;
}

#endif
