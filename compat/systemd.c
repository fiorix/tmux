/* $OpenBSD$ */

/*
 * Copyright (c) 2022 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/un.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-login.h>
#include <systemd/sd-id128.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"
#include "restart-exec.h"

#ifndef SD_ID128_UUID_FORMAT_STR
#define SD_ID128_UUID_FORMAT_STR \
	"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x"
#endif

#define SYSTEMD_SOCKET_FD_NAME "tmux-server"
#define SYSTEMD_STATE_FD_NAME "tmux.restart.state"
#define SYSTEMD_PANE_FD_PREFIX "tmux.p."

struct systemd_activation {
	enum restart_activation_type	 type;
	int				 state_fd;
	struct restart_fd	*panes;
	size_t				 pane_count;
};

static void systemd_activation_error(char **, const char *, ...)
    printflike(2, 3);

static void
systemd_activation_error(char **cause, const char *fmt, ...)
{
	va_list	 ap;
	char	*msg;

	if (cause == NULL)
		return;
	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);
	xasprintf(cause, "systemd activation error: %s", msg);
	free(msg);
}

static void
systemd_activation_close_fds(int fds)
{
	while (fds > 0)
		close(SD_LISTEN_FDS_START + --fds);
}

static void
systemd_activation_free_names(char **names, int count)
{
	int	 i;

	if (names == NULL)
		return;
	for (i = 0; i < count; i++)
		free(names[i]);
	free(names);
}

static int
systemd_activation_parse_number(const char *s, size_t len, uint64_t limit,
    uint64_t *value)
{
	uint64_t	 n = 0;
	size_t		 i;

	if (len == 0 || (len != 1 && s[0] == '0'))
		return (-1);
	for (i = 0; i < len; i++) {
		if (s[i] < '0' || s[i] > '9')
			return (-1);
		if (n > (limit - (s[i] - '0')) / 10)
			return (-1);
		n = n * 10 + (s[i] - '0');
	}
	*value = n;
	return (0);
}

static int
systemd_activation_parse_pane(const char *name, u_int *pane_id, pid_t *pid)
{
	const char	*first, *last;
	uint64_t	 number;

	if (strncmp(name, SYSTEMD_PANE_FD_PREFIX,
	    sizeof SYSTEMD_PANE_FD_PREFIX - 1) != 0)
		return (-1);
	first = name + sizeof SYSTEMD_PANE_FD_PREFIX - 1;
	last = strchr(first, '.');
	if (last == NULL || strchr(last + 1, '.') != NULL)
		return (-1);
	if (systemd_activation_parse_number(first, last - first, UINT_MAX,
	    &number) != 0)
		return (-1);
	*pane_id = number;
	first = last + 1;
	if (systemd_activation_parse_number(first, strlen(first), INT_MAX,
	    &number) != 0 || number == 0)
		return (-1);
	*pid = number;
	return (0);
}

static int
systemd_activation_pane_cmp(const void *lhs, const void *rhs)
{
	const struct restart_fd	*a = lhs;
	const struct restart_fd	*b = rhs;

	if (a->pane_id < b->pane_id)
		return (-1);
	if (a->pane_id > b->pane_id)
		return (1);
	return (0);
}

static int
systemd_activation_socket_path(int fd, char **path)
{
	struct sockaddr_un	 sa;
	socklen_t		 addrlen = sizeof sa;

	memset(&sa, 0, sizeof sa);
	if (getsockname(fd, (struct sockaddr *)&sa, &addrlen) == -1)
		return (-1);
	*path = xstrndup(sa.sun_path, sizeof sa.sun_path);
	return (0);
}

int
systemd_activated(void)
{
	return (sd_listen_fds(0) >= 1);
}

int
systemd_create_socket(int flags, struct systemd_activation **activation,
    char **cause)
{
	struct systemd_activation	*a = NULL;
	struct restart_fd	 pane;
	char			       **names = NULL, *path = NULL;
	size_t				 i, j;
	int				 candidate_fds, fd, fds, listener;
	int				 r, state;

	if (activation != NULL)
		*activation = NULL;
	if (cause != NULL)
		*cause = NULL;
	candidate_fds = sd_listen_fds(0);
	fds = sd_listen_fds_with_names(1, &names);
	if (fds < 0) {
		if (candidate_fds > 0)
			systemd_activation_close_fds(candidate_fds);
		systemd_activation_error(cause, "%s", strerror(-fds));
		return (-1);
	}
	if (fds == 0)
		return (server_create_socket(flags, cause));
	if (activation == NULL) {
		systemd_activation_error(cause, "missing activation output");
		goto fail;
	}

	a = xcalloc(1, sizeof *a);
	a->state_fd = -1;
	if (fds == 1) {
		fd = SD_LISTEN_FDS_START;
		r = sd_is_socket_unix(fd, SOCK_STREAM, 1, NULL, 0);
		if (r <= 0) {
			if (r < 0)
				errno = -r;
			else
				errno = EPFNOSUPPORT;
			systemd_activation_error(cause,
			    "descriptor %d is not a listening Unix "
			    "stream socket", fd);
			goto fail;
		}
		if (systemd_activation_socket_path(fd, &path) != 0) {
			systemd_activation_error(cause,
			    "descriptor %d has no socket path: %s", fd,
			    strerror(errno));
			goto fail;
		}
		a->type = RESTART_ACTIVATION_SOCKET;
		goto done;
	}

	listener = state = -1;
	for (i = 0; i < (size_t)fds; i++) {
		fd = SD_LISTEN_FDS_START + i;
		if (names[i] == NULL) {
			systemd_activation_error(cause,
			    "descriptor %d has no name", fd);
			goto fail;
		}
		if (strcmp(names[i], SYSTEMD_SOCKET_FD_NAME) == 0) {
			if (listener != -1) {
				systemd_activation_error(cause,
				    "descriptor %d duplicates the server "
				    "socket", fd);
				goto fail;
			}
			r = sd_is_socket_unix(fd, SOCK_STREAM, 1, NULL, 0);
			if (r <= 0) {
				systemd_activation_error(cause,
				    "descriptor %d is not a listening "
				    "Unix stream socket", fd);
				goto fail;
			}
			listener = fd;
			continue;
		}
		if (strcmp(names[i], SYSTEMD_STATE_FD_NAME) == 0) {
			if (state != -1) {
				systemd_activation_error(cause,
				    "descriptor %d duplicates the restart "
				    "state", fd);
				goto fail;
			}
			state = fd;
			continue;
		}
		if (systemd_activation_parse_pane(names[i], &pane.pane_id,
		    &pane.pid) != 0) {
			systemd_activation_error(cause,
			    "descriptor %d has an invalid name", fd);
			goto fail;
		}
		for (j = 0; j < a->pane_count; j++) {
			if (a->panes[j].pane_id == pane.pane_id) {
				systemd_activation_error(cause,
				    "descriptor %d duplicates a pane ID", fd);
				goto fail;
			}
			if (a->panes[j].pid == pane.pid) {
				systemd_activation_error(cause,
				    "descriptor %d duplicates a pane PID", fd);
				goto fail;
			}
		}
		pane.fd = fd;
		a->panes = xreallocarray(a->panes, a->pane_count + 1,
		    sizeof *a->panes);
		a->panes[a->pane_count++] = pane;
	}
	if (listener == -1 || state == -1) {
		systemd_activation_error(cause,
		    "restart descriptors are missing the server socket or "
		    "state");
		goto fail;
	}
	if (a->pane_count > 1) {
		qsort(a->panes, a->pane_count, sizeof *a->panes,
		    systemd_activation_pane_cmp);
	}
	if (systemd_activation_socket_path(listener, &path) != 0) {
		systemd_activation_error(cause,
		    "descriptor %d has no socket path: %s", listener,
		    strerror(errno));
		goto fail;
	}
	a->type = RESTART_ACTIVATION_RESTORE;
	a->state_fd = state;
	fd = listener;

done:
	systemd_activation_free_names(names, fds);
	socket_path = path;
	*activation = a;
	return (fd);

fail:
	systemd_activation_close_fds(fds);
	systemd_activation_free_names(names, fds);
	if (a != NULL) {
		free(a->panes);
		free(a);
	}
	free(path);
	return (-1);
}

int
systemd_activation_is_restart(const struct systemd_activation *activation)
{
	return (activation != NULL &&
	    activation->type == RESTART_ACTIVATION_RESTORE);
}

static void
systemd_activation_close_restart_fds(struct systemd_activation *activation)
{
	size_t	 i;

	if (activation == NULL)
		return;
	if (activation->state_fd != -1) {
		close(activation->state_fd);
		activation->state_fd = -1;
	}
	for (i = 0; i < activation->pane_count; i++) {
		if (activation->panes[i].fd != -1) {
			close(activation->panes[i].fd);
			activation->panes[i].fd = -1;
		}
	}
}

void
systemd_activation_free(struct systemd_activation *activation)
{
	if (activation == NULL)
		return;
	systemd_activation_close_restart_fds(activation);
	free(activation->panes);
	free(activation);
}

struct systemd_job_watch {
	const char	*path;
	int		 done;
};

static int
job_removed_handler(sd_bus_message *m, void *userdata,
    __unused sd_bus_error *ret_error)
{
	struct systemd_job_watch *watch = userdata;
	const char		 *path = NULL;
	uint32_t		 id;
	int			 r;

	/* This handler could be called during the sd_bus_call. */
	if (watch->path == NULL)
		return 0;

	r = sd_bus_message_read(m, "uo", &id, &path);
	if (r < 0)
		return (r);

	if (strcmp(path, watch->path) == 0)
		watch->done = 1;

	return (0);
}

int
systemd_move_to_new_cgroup(char **cause)
{
	sd_bus_error		 error = SD_BUS_ERROR_NULL;
	sd_bus_message		*m = NULL, *reply = NULL;
	sd_bus 			*bus = NULL;
	sd_bus_slot		*slot = NULL;
	char			*name, *desc, *slice, *unit;
	sd_id128_t		 uuid;
	int			 r;
	uint64_t		 elapsed_usec;
	pid_t			 pid, parent_pid;
	struct timeval		 start, now;
	struct systemd_job_watch watch = {};

	gettimeofday(&start, NULL);

	/* Connect to the session bus. */
	r = sd_bus_default_user(&bus);
	if (r < 0) {
		xasprintf(cause, "failed to connect to session bus: %s",
		    strerror(-r));
		goto finish;
	}

	/* Start watching for JobRemoved events */
	r = sd_bus_match_signal(bus, &slot,
	    "org.freedesktop.systemd1",
	    "/org/freedesktop/systemd1",
	    "org.freedesktop.systemd1.Manager",
	    "JobRemoved",
	    job_removed_handler,
	    &watch);
	if (r < 0) {
		xasprintf(cause, "failed to create match signal: %s",
		    strerror(-r));
		goto finish;
	}

	/* Start building the method call. */
	r = sd_bus_message_new_method_call(bus, &m,
	    "org.freedesktop.systemd1",
	    "/org/freedesktop/systemd1",
	    "org.freedesktop.systemd1.Manager",
	    "StartTransientUnit");
	if (r < 0) {
		xasprintf(cause, "failed to create bus message: %s",
		    strerror(-r));
		goto finish;
	}

	/* Generate a unique name for the new scope, to avoid collisions. */
	r = sd_id128_randomize(&uuid);
	if (r < 0) {
		xasprintf(cause, "failed to generate uuid: %s", strerror(-r));
		goto finish;
	}
	xasprintf(&name, "tmux-spawn-" SD_ID128_UUID_FORMAT_STR ".scope",
	    SD_ID128_FORMAT_VAL(uuid));
	r = sd_bus_message_append(m, "s", name);
	free(name);
	if (r < 0) {
		xasprintf(cause, "failed to append to bus message: %s",
		    strerror(-r));
		goto finish;
	}

	/* Mode: fail if there's a queued unit with the same name. */
	r = sd_bus_message_append(m, "s", "fail");
	if (r < 0) {
		xasprintf(cause, "failed to append to bus message: %s",
		    strerror(-r));
		goto finish;
	}

	/* Start properties array. */
	r = sd_bus_message_open_container(m, 'a', "(sv)");
	if (r < 0) {
		xasprintf(cause, "failed to start properties array: %s",
		    strerror(-r));
		goto finish;
	}

	pid = getpid();
	parent_pid = getppid();
	xasprintf(&desc, "tmux child pane %ld launched by process %ld",
	    (long)pid, (long)parent_pid);
	r = sd_bus_message_append(m, "(sv)", "Description", "s", desc);
	free(desc);
	if (r < 0) {
		xasprintf(cause, "failed to append to properties: %s",
		    strerror(-r));
		goto finish;
	}

	/*
	 * Make sure that the session shells are terminated with SIGHUP since
	 * bash and friends tend to ignore SIGTERM.
	 */
	r = sd_bus_message_append(m, "(sv)", "SendSIGHUP", "b", 1);
	if (r < 0) {
		xasprintf(cause, "failed to append to properties: %s",
		    strerror(-r));
		goto finish;
	}

	/*
	 * Inherit the slice from the parent process, or default to
	 * "app-tmux.slice" if that fails.
	 */
	r = sd_pid_get_user_slice(parent_pid, &slice);
	if (r < 0) {
		slice = xstrdup("app-tmux.slice");
	}
	r = sd_bus_message_append(m, "(sv)", "Slice", "s", slice);
	free(slice);
	if (r < 0) {
		xasprintf(cause, "failed to append to properties: %s",
		    strerror(-r));
		goto finish;
	}

	/* PIDs to add to the scope: length - 1 array of uint32_t. */
	r = sd_bus_message_append(m, "(sv)", "PIDs", "au", 1, pid);
	if (r < 0) {
		xasprintf(cause, "failed to append to properties: %s",
		    strerror(-r));
		goto finish;
	}

	/* Clean up the scope even if it fails. */
	r = sd_bus_message_append(m, "(sv)", "CollectMode", "s",
	    "inactive-or-failed");
	if (r < 0) {
		xasprintf(cause, "failed to append to properties: %s",
		    strerror(-r));
		goto finish;
	}

	/*
	 * Try locating systemd unit that started the server, and mark pane units
	 * as dependent on it. Use "Before" to make sure systemd will not try to
	 * kill them first.
	 */
	if (sd_pid_get_user_unit(parent_pid, &unit) == 0 ||
	    sd_pid_get_unit(parent_pid, &unit) == 0) {
		r = sd_bus_message_append(m, "(sv)", "Before", "as", 1, unit);
		if (r >= 0) {
			r = sd_bus_message_append(m, "(sv)", "PartOf", "as", 1,
			    unit);
		}
		free(unit);
		if (r < 0) {
			xasprintf(cause, "failed to append to properties: %s",
			    strerror(-r));
			goto finish;
		}
	}

	/* End properties array. */
	r = sd_bus_message_close_container(m);
	if (r < 0) {
		xasprintf(cause, "failed to end properties array: %s",
		    strerror(-r));
		goto finish;
	}

	/* aux is currently unused and should be passed an empty array. */
	r = sd_bus_message_append(m, "a(sa(sv))", 0);
	if (r < 0) {
		xasprintf(cause, "failed to append to bus message: %s",
		    strerror(-r));
		goto finish;
	}

	/* Call the method with a timeout of 1 second = 1e6 us. */
	r = sd_bus_call(bus, m, 1000000, &error, &reply);
	if (r < 0) {
		if (error.message != NULL) {
			/* We have a specific error message from sd-bus. */
			xasprintf(cause, "StartTransientUnit call failed: %s",
			    error.message);
		} else {
			xasprintf(cause, "StartTransientUnit call failed: %s",
			    strerror(-r));
		}
		goto finish;
	}

	/* Get the job (object path) from the reply */
	r = sd_bus_message_read(reply, "o", &watch.path);
	if (r < 0) {
		xasprintf(cause, "failed to parse method reply: %s",
		    strerror(-r));
		goto finish;
	}

	while (!watch.done) {
		/* Process events including callbacks. */
		r = sd_bus_process(bus, NULL);
		if (r < 0) {
			xasprintf(cause,
			    "failed waiting for cgroup allocation: %s",
			    strerror(-r));
			goto finish;
		}

		/*
		 * A positive return means we handled an event and should keep
		 * processing; zero indicates no events available, so wait.
		 */
		if (r > 0)
			continue;

		gettimeofday(&now, NULL);
		elapsed_usec = (now.tv_sec - start.tv_sec) * 1000000 +
		    now.tv_usec - start.tv_usec;

		if (elapsed_usec >= 1000000) {
			xasprintf(cause,
			    "timeout waiting for cgroup allocation");
			goto finish;
		}

		r = sd_bus_wait(bus, 1000000 - elapsed_usec);
		if (r < 0) {
			xasprintf(cause,
			    "failed waiting for cgroup allocation: %s",
			    strerror(-r));
			goto finish;
		}
	}

finish:
	sd_bus_error_free(&error);
	sd_bus_message_unref(m);
	sd_bus_message_unref(reply);
	sd_bus_slot_unref(slot);
	sd_bus_unref(bus);

	return (r);
}
