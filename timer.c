/*
 * Copyright (c) 2015 Mark Heily <mark@heily.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "vendor/FreeBSD/sys/queue.h"
#if HAVE_SYS_LIMITS_H
#include <sys/limits.h>
#else
#include <limits.h>
#endif
#include <sys/types.h>
#ifdef __linux__
#include <kqueue/sys/event.h>
#else
#include <sys/event.h>
#endif
#include <sys/time.h>
#include <time.h>

#include "log.h"
#include "manager.h"
#include "timer.h"
#include "job.h"

/* The main kqueue descriptor used by launchd */
static int parent_kqfd;

#define TIMER_TYPE_CONSTANT_INTERVAL (1)

static SLIST_HEAD(, job) start_interval_list;

static uint32_t min_interval = UINT_MAX;

/* Find the smallest interval that we can wait before waking up at least one job */
static void update_min_interval()
{
	struct kevent kev;
	job_t job, job_tmp;
	uint32_t old_value = min_interval;

	SLIST_FOREACH_SAFE(job, &start_interval_list, start_interval_sle, job_tmp) {
		if (job->jm->start_interval < min_interval)
			min_interval = job->jm->start_interval;
	}
/*
	if (old_value != min_interval) {
		EV_SET(&kev, TIMER_TYPE_CONSTANT_INTERVAL, EVFILT_TIMER, EV_ADD, 0, 0, &setup_timers);
		if (kevent(parent_kqfd, &kev, 1, NULL, 0, NULL) < 0) {
			log_errno("kevent(2)");
			return -1;
		}
	}
*/
}

static inline time_t current_time() {
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
		log_errno("clock_gettime(2)");
		abort();
	}
	return now.tv_sec;
}

static inline void update_job_interval(job_t job)
{
	job->next_scheduled_start = current_time() + job->jm->start_interval;
}

int setup_timers(int kqfd)
{
	struct kevent kev;
	parent_kqfd = kqfd;
	SLIST_INIT(&start_interval_list);
	return 0;
}

int timer_register_constant_interval(struct job *job)
{
	struct kevent kev;

	//TODO: keep the list sorted
	SLIST_INSERT_HEAD(&start_interval_list, job, start_interval_sle);
	update_min_interval();
	update_job_interval(job);

	EV_SET(&kev, TIMER_TYPE_CONSTANT_INTERVAL, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, (1000 * min_interval), &setup_timers);
	if (kevent(parent_kqfd, &kev, 1, NULL, 0, NULL) < 0) {
		log_errno("kevent(2)");
		return -1;
	}
	log_debug("registered a timer for %s; interval=%u", job->jm->label, min_interval);
	return 0;
}

/* TODO:
 *
int timer_unregister_constant_interval(struct job *job)
{
	SLIST_REMOVE(&start_interval_list, job, start_interval_sle);
	update_min_interval();
	if (SLIST_EMPTY(&start_interval_list)) {
		EV_SET(&kev, TIMER_TYPE_CONSTANT_INTERVAL, EVFILT_TIMER, EV_ADD | EV_DISABLE, 0, 0, &setup_timers);
		if (kevent(parent_kqfd, &kev, 1, NULL, 0, NULL) < 0) {
			log_errno("kevent(2)");
			return -1;
		}
	}
	return 0;
}
*/

int timer_handler()
{
	job_t job;
	time_t now;

	log_debug("waking up after %u seconds", min_interval);
	SLIST_FOREACH(job, &start_interval_list, start_interval_sle) {
		if (now >= job->next_scheduled_start) {
			log_debug("job %s starting due to timer interval", job->jm->label);
			update_job_interval(job);
			(void) manager_wake_job(job); //FIXME: error handling
		}
	}
	return 0;
}
