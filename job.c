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

#include <stdio.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>

#ifdef __FreeBSD__
#include <sys/param.h>
#include <sys/jail.h>
#endif

#include "job.h"
#include "log.h"
#include "socket.h"
#include "timer.h"

static void job_dump(job_t job) {
	log_debug("job dump: label=%s state=%d", job->jm->label, job->state);
}

static int apply_resource_limits(const job_t job) {
	//TODO - SoftResourceLimits, HardResourceLimits
	//TODO - LowPriorityIO

	if (job->jm->nice != 0) {
		if (setpriority(PRIO_PROCESS, 0, job->jm->nice) < 0) {
			log_errno("setpriority(2) to nice=%d", job->jm->nice);
			return (-1);
		}
	}

	return (0);
}

static inline int modify_credentials(job_t const job, const struct passwd *pwent, const struct group *grent)
{
	if (getuid() != 0) return (0);

	log_debug("setting credentials: uid=%d gid=%d", pwent->pw_uid, grent->gr_gid);

	if (initgroups(job->jm->user_name, grent->gr_gid) < 0) {
		log_errno("initgroups");
		return (-1);
	}
	if (setgid(grent->gr_gid) < 0) {
		log_errno("setgid");
		return (-1);
	}
#ifndef __GLIBC__
	if (setlogin(job->jm->user_name) < 0) {
		log_errno("setlogin");
		return (-1);
	}
#endif
	if (setuid(pwent->pw_uid) < 0) {
		log_errno("setuid");
		return (-1);
	}
	return (0);
}

static inline cvec_t setup_environment_variables(const job_t job, const struct passwd *pwent)
{
	struct job_manifest_socket *jms;
	cvec_t env = NULL;
	char *curp, *buf = NULL;
	char *logname_var, *user_var;
	int i, j;
	bool found[] = { false, false, false, false, false, false };

	env = cvec_new();
	if (!env) goto err_out;

	if (asprintf(&logname_var, "LOGNAME=%s", job->jm->user_name) < 0) goto err_out;
	if (asprintf(&user_var, "USER=%s", job->jm->user_name) < 0) goto err_out;

	/* Convert the flat array into an array of key=value pairs */
	/* Follow the crontab(5) convention of overriding LOGNAME and USER
	 * and providing a default value for HOME, PATH, and SHELL */
	log_debug("job %s has %zu env var(s)\n", job->jm->label, cvec_length(job->jm->environment_variables) / 2);
	for (i = 0; i < cvec_length(job->jm->environment_variables); i += 2) {
		curp = cvec_get(job->jm->environment_variables, i);
		log_debug("evaluating %s", curp);
		if (strcmp(curp, "LOGNAME") == 0) {
			found[0] = true;
			if (cvec_push(env, logname_var) < 0) goto err_out;
		} else if (strcmp(curp, "USER") == 0) {
			found[1] = true;
			if (cvec_push(env, user_var) < 0) goto err_out;
		} else if (strcmp(curp, "HOME") == 0) {
			found[2] = true;
		} else if (strcmp(curp, "PATH") == 0) {
			found[3] = true;
		} else if (strcmp(curp, "SHELL") == 0) {
			found[4] = true;
		} else if (strcmp(curp, "TMPDIR") == 0) {
			found[5] = true;
		} else {
			char *keypair;
			char *value;
			value = cvec_get(job->jm->environment_variables, i + 1);
			if (!value)
				goto err_out;
			if (asprintf(&keypair, "%s=%s", curp, value) < 0)
				goto err_out;
			if (cvec_push(env, keypair) < 0) goto err_out;
			log_debug("set keypair: %s", keypair);
			free(keypair);
		}
	}
	if (!found[0]) {
		if (cvec_push(env, logname_var) < 0) goto err_out;
	}
	if (!found[1]) {
		if (cvec_push(env, user_var) < 0) goto err_out;
	}
	if (!found[2]) {
		if (asprintf(&buf, "HOME=%s", pwent->pw_dir) < 0) goto err_out;
		if (cvec_push(env, buf) < 0) goto err_out;
		free(buf);
	}
	if (!found[3]) {
#ifdef PREFIX
		if (cvec_push(env, "PATH=/usr/bin:/bin:" PREFIX "/bin:/usr/local/bin") < 0) goto err_out;
#else
		if (cvec_push(env, "PATH=/usr/bin:/bin:/usr/local/bin") < 0) goto err_out;
#endif
	}
	if (!found[4]) {
		if (asprintf(&buf, "SHELL=%s", pwent->pw_shell) < 0) goto err_out;
		if (cvec_push(env, buf) < 0) goto err_out;
		free(buf);
	}
	if (!found[5]) {
		if (cvec_push(env, "TMPDIR=/tmp") < 0) goto err_out;
	}
	free(logname_var);
	free(user_var);

	size_t offset = 0;
	SLIST_FOREACH(jms, &job->jm->sockets, entry) {
		job_manifest_socket_export(jms, env, offset++);
	}
	if (offset > 0) {
		if (asprintf(&buf, "LISTEN_FDS=%zu", offset) < 0) goto err_out;
		if (cvec_push(env, buf) < 0) goto err_out;
		free(buf);
		if (asprintf(&buf, "LISTEN_PID=%d", getpid()) < 0) goto err_out;
		if (cvec_push(env, buf) < 0) goto err_out;
		free(buf);
	}

	return (env);

err_out:
	free(logname_var);
	free(user_var);
	free(buf);
	cvec_free(env);
	return NULL;
}

static inline int exec_job(const job_t job, const struct passwd *pwent) {
	int rv;
	char *path;
	char **argv, **envp;
	cvec_t final_env;

	final_env = setup_environment_variables(job, pwent);
	if (final_env == NULL) {
		log_error("unable to set environment vars");
		return (-1);
	}
	envp = cvec_to_array(final_env);

	argv = cvec_to_array(job->jm->program_arguments);
	path = argv[0];
	if (job->jm->program) {
		path = job->jm->program;
	} else {
		path = argv[0];
	}
	if (job->jm->enable_globbing) {
    	//TODO: globbing
    }
	log_debug("exec: %s", path);
#if DEBUG
	log_debug("argv[]:");
	for (char **item = argv; *item; item++) {
		log_debug(" - arg: %s", *item);
	}
	log_debug("envp[]:");
	for (char **item = envp; *item; item++) {
		log_debug(" - env: %s", *item);
	}
#endif

	fclose(logfile);
    rv = execve(path, argv, envp);
    if (rv < 0) {
    	log_error("failed to call execve(2)");
    	return (-1);
    }
    log_notice("executed job");
	cvec_free(final_env);
    return (0);
}

static int start_child_process(const job_t job, const struct passwd *pwent, const struct group *grent)
{
	int rv;

#ifdef __FreeBSD__
	if (job->jm->jail_name) {
		log_debug("entering jail %s", job->jm->jail_name);
		/* XXX-FIXME: hardcoded to JID #1, should lookup the JID from name */
		if (jail_attach(1) < 0) {
			log_errno("jail_attach(2)");
			return -1;
		}
	}
#endif

#ifndef NOFORK
	if (setsid() < 0) {
		log_errno("setsid");
		goto err_out;
	}
#endif
	if (apply_resource_limits(job) < 0) {
		log_error("unable to apply resource limits");
		goto err_out;
	}
	if (job->jm->working_directory) {
		if (chdir(job->jm->working_directory) < 0) {
			log_error("unable to chdir to %s", job->jm->working_directory);
			goto err_out;
		}
	}
	if (job->jm->root_directory && getuid() == 0) {
		if (chroot(job->jm->root_directory) < 0) {
			log_error("unable to chroot to %s", job->jm->root_directory);
			goto err_out;
		}
	}
	if (modify_credentials(job, pwent, grent) < 0) {
    	log_error("unable to modify credentials");
    	goto err_out;
	}
    if (job->jm->umask) {
    	//TODO: umask
    }
    if (job->jm->stdin_path) {
    	log_debug("setting stdin path to %s", job->jm->stdin_path);
    	int fd = open(job->jm->stdin_path, O_RDONLY);
    	if (fd < 0) goto err_out;
    	if (dup2(fd, 0) < 0) goto err_out;
    	if (close(fd) < 0) goto err_out;
    }
    if (job->jm->stdout_path) {
    	log_debug("setting stdout path to %s", job->jm->stdout_path);
    	int fd = open(job->jm->stdout_path, O_CREAT | O_WRONLY, 0600);
		if (fd < 0) goto err_out;
    	if (dup2(fd, 1) < 0) goto err_out;
    	if (close(fd) < 0) goto err_out;
    }
    if (job->jm->stderr_path) {
    	/* FIXME: this shares a fd with launchd's logger. Fix this by moving launchd's logger to fd #3 */
    	log_debug("setting stderr path to %s", job->jm->stderr_path);
    	int fd = open(job->jm->stderr_path, O_CREAT | O_WRONLY, 0600);
    	if (fd < 0) goto err_out;
    	if (dup2(fd, 2) < 0) goto err_out;
    	if (close(fd) < 0) goto err_out;
    }

    if (exec_job(job, pwent) < 0) {
    	log_error("exec_job() failed");
    	goto err_out;
    }

    return (0);

err_out:
	log_error("job %s failed to start; see previous log message for details", job->jm->label);
	return (-1);
}

job_t job_new(job_manifest_t jm)
{
	job_t j;

	j = calloc(1, sizeof(*j));
	if (!j) return NULL;
	j->jm = jm;
	j->state = JOB_STATE_DEFINED;
	return (j);
}

void job_free(job_t job)
{
	if (job == NULL) return;
	free(job->jm);
	free(job);
}

int job_load(job_t job)
{
	struct job_manifest_socket *jms;

	/* TODO: This is the place to setup on-demand watches for the following keys:
			WatchPaths
			QueueDirectories
			StartCalendarInterval
	*/
	if (!SLIST_EMPTY(&job->jm->sockets)) {
		SLIST_FOREACH(jms, &job->jm->sockets, entry) {
			if (job_manifest_socket_open(job, jms) < 0) {
				log_error("failed to open socket");
				return (-1);
			}
		}
		log_debug("job %s sockets created", job->jm->label);
		job->state = JOB_STATE_WAITING;
		return (0);
	}

	if (job->jm->start_interval > 0) {
		if (timer_register_constant_interval(job) < 0) {
			log_error("failed to register the interval timer");
			return -1;
		}
	}

	job->state = JOB_STATE_LOADED;
	log_debug("loaded %s", job->jm->label);
	job_dump(job);
	return (0);
}

int job_unload(job_t job)
{
	if (job->state == JOB_STATE_RUNNING) {
		log_debug("sending SIGTERM to process group %d", job->pid);
		if (killpg(job->pid, SIGTERM) < 0) {
			log_errno("killpg(2) of pid %d", job->pid);
			/* not sure how to handle the error, we still want to clean up */
		}
		job->state = JOB_STATE_KILLED;
		//TODO: start a timer to send a SIGKILL if it doesn't die gracefully
		return 0;
	} else {
		//TODO: update the timer interval in timer.c?
		job->state = JOB_STATE_DEFINED;
	}

	return 0;
}

int job_run(job_t job)
{
	struct job_manifest_socket *jms;
	struct passwd *pwent;
	struct group *grent;
	pid_t pid;

	if ((pwent = getpwnam(job->jm->user_name)) == NULL) {
		log_errno("getpwnam");
		return (-1);
	}

	if ((grent = getgrnam(job->jm->group_name)) == NULL) {
		log_errno("getgrnam");
		return (-1);
	}

	// temporary for debugging
#ifdef NOFORK
	(void) start_child_process(job, pwent, grent);
#else
    pid = fork();
    if (pid < 0) {
    	return (-1);
    } else if (pid == 0) {
    	if (start_child_process(job, pwent, grent) < 0) {
    		//TODO: report failures to the parent
    		exit(127);
    	}
    } else {
    	log_debug("running %s", job->jm->label);
    	job->pid = pid;
    	job->state = JOB_STATE_RUNNING;
	SLIST_FOREACH(jms, &job->jm->sockets, entry) {
		job_manifest_socket_close(jms);
	}
    }
#endif
	return (0);
}
