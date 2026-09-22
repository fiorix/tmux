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
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
#define SYSTEMD_BUS_NAME "org.freedesktop.systemd1"
#define SYSTEMD_MANAGER_PATH "/org/freedesktop/systemd1"
#define SYSTEMD_MANAGER_INTERFACE "org.freedesktop.systemd1.Manager"
#define SYSTEMD_UNIT_INTERFACE "org.freedesktop.systemd1.Unit"
#define SYSTEMD_SERVICE_INTERFACE "org.freedesktop.systemd1.Service"
#define SYSTEMD_SOCKET_INTERFACE "org.freedesktop.systemd1.Socket"
#define SYSTEMD_NOTIFY_TIMEOUT 5000000

#ifdef HAVE_SYSTEMD_RESTART
struct systemd_identity {
	char	*service_name;
	char	*service_path;
	char	*socket_name;
	char	*socket_path;
	char	*listener_path;
};
#endif

struct systemd_activation {
	enum restart_activation_type	 type;
	int				 state_fd;
	struct restart_fd	*panes;
	size_t				 pane_count;
#ifdef HAVE_SYSTEMD_RESTART
	int				 restart_prepared;
	int				 restart_available;
	struct systemd_identity		 identity;
#endif
};

#ifdef HAVE_SYSTEMD_RESTART
static const struct systemd_activation *systemd_restart_activation;
#endif

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
			    "stream socket",
			    fd);
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
				systemd_activation_error(cause, "descriptor %d "
				    "duplicates the server socket", fd);
				goto fail;
			}
			r = sd_is_socket_unix(fd, SOCK_STREAM, 1, NULL, 0);
			if (r <= 0) {
				systemd_activation_error(cause,
				    "descriptor %d is not a listening Unix "
				    "stream socket", fd);
				goto fail;
			}
			listener = fd;
			continue;
		}
		if (strcmp(names[i], SYSTEMD_STATE_FD_NAME) == 0) {
			if (state != -1) {
				systemd_activation_error(cause, "descriptor %d "
				    "duplicates the restart state", fd);
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
		systemd_activation_error(cause, "restart descriptors are "
		    "missing the server socket or state");
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

#ifdef HAVE_SYSTEMD_RESTART
static enum restart_activation_type
systemd_restart_activation_type(const struct restart_activation *base)
{
	const struct systemd_activation	*activation = (const void *)base;

	if (activation == NULL)
		return (RESTART_ACTIVATION_NONE);
	return (activation->type);
}

static int
systemd_restart_activation_state_fd(const struct restart_activation *base)
{
	const struct systemd_activation	*activation = (const void *)base;

	if (activation == NULL ||
	    activation->type != RESTART_ACTIVATION_RESTORE)
		return (-1);
	return (activation->state_fd);
}

static size_t
systemd_restart_activation_pane_count(const struct restart_activation *base)
{
	const struct systemd_activation	*activation = (const void *)base;

	if (activation == NULL ||
	    activation->type != RESTART_ACTIVATION_RESTORE)
		return (0);
	return (activation->pane_count);
}

static int
systemd_restart_activation_pane_at(const struct restart_activation *base,
    size_t index, struct restart_fd *pane)
{
	const struct systemd_activation	*activation = (const void *)base;

	if (activation == NULL || pane == NULL ||
	    activation->type != RESTART_ACTIVATION_RESTORE) {
		errno = EINVAL;
		return (-1);
	}
	if (index >= activation->pane_count) {
		errno = ERANGE;
		return (-1);
	}
	*pane = activation->panes[index];
	return (0);
}

static int
systemd_restart_activation_lookup(void *data, u_int pane_id, pid_t pid,
    int *fd)
{
	struct systemd_activation	*activation = data;
	size_t				 left, middle, right;

	if (activation == NULL || fd == NULL || pid <= 0 ||
	    activation->type != RESTART_ACTIVATION_RESTORE) {
		errno = EINVAL;
		return (-1);
	}
	left = 0;
	right = activation->pane_count;
	while (left < right) {
		middle = left + (right - left) / 2;
		if (activation->panes[middle].pane_id < pane_id)
			left = middle + 1;
		else
			right = middle;
	}
	if (left == activation->pane_count ||
	    activation->panes[left].pane_id != pane_id ||
	    activation->panes[left].pid != pid) {
		errno = ENOENT;
		return (-1);
	}
	*fd = activation->panes[left].fd;
	return (0);
}
#endif

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

#ifdef HAVE_SYSTEMD_RESTART
static void
systemd_restart_activation_close(struct restart_activation *base)
{
	systemd_activation_close_restart_fds((void *)base);
}
#endif

void
systemd_activation_free(struct systemd_activation *activation)
{
	if (activation == NULL)
		return;
#ifdef HAVE_SYSTEMD_RESTART
	if (systemd_restart_activation == activation)
		systemd_restart_activation = NULL;
	free(activation->identity.service_name);
	free(activation->identity.service_path);
	free(activation->identity.socket_name);
	free(activation->identity.socket_path);
	free(activation->identity.listener_path);
#endif
	systemd_activation_close_restart_fds(activation);
	free(activation->panes);
	free(activation);
}

int
systemd_ready(char **cause)
{
	int	r;

	if (cause != NULL)
		*cause = NULL;
	r = sd_notify(0, "READY=1");
	if (r > 0)
		return (0);
	if (cause != NULL) {
		if (r == 0) {
			xasprintf(cause, "systemd readiness error: "
			    "notification socket is unavailable");
		} else {
			xasprintf(cause, "systemd readiness error: %s",
			    strerror(-r));
		}
	}
	return (-1);
}

#ifdef HAVE_SYSTEMD_RESTART
static void systemd_restart_unavailable(char **, const char *, ...)
    printflike(2, 3);
static void systemd_fdstore_add_error(char **, const char *, const char *, ...)
    printflike(3, 4);
static void systemd_fdstore_cleanup_error(char **, const char *, ...)
    printflike(2, 3);
static void systemd_restart_request_error(char **, int, const char *, ...)
    printflike(3, 4);

static void
systemd_restart_unavailable(char **cause, const char *fmt, ...)
{
	va_list	 ap;
	char	*msg;

	if (cause == NULL)
		return;
	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);
	xasprintf(cause, "systemd restart unavailable: %s", msg);
	free(msg);
}

static void
systemd_fdstore_add_error(char **cause, const char *name, const char *fmt, ...)
{
	va_list	 ap;
	char	*msg;

	if (cause == NULL)
		return;
	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);
	xasprintf(cause, "systemd fdstore add failed for %s: %s", name, msg);
	free(msg);
}

static void
systemd_fdstore_cleanup_error(char **cause, const char *fmt, ...)
{
	const struct systemd_identity	*identity;
	va_list	 ap;
	char	*msg;

	if (cause == NULL)
		return;
	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);
	if (systemd_restart_activation == NULL ||
	    !systemd_restart_activation->restart_available) {
		systemd_restart_unavailable(cause, "%s", msg);
		free(msg);
		return;
	}
	identity = &systemd_restart_activation->identity;
	xasprintf(cause, "systemd fdstore cleanup could not be confirmed: %s\n"
	    "recovery is destructive; run these commands in order:\n"
	    "systemctl --user stop '%s'\n"
	    "systemctl --user stop '%s'\n"
	    "systemctl --user clean --what=fdstore '%s'\n"
	    "systemctl --user start '%s'", msg, identity->socket_name,
	    identity->service_name, identity->service_name,
	    identity->socket_name);
	free(msg);
}

/*
 * A request that was sent and not confirmed has already committed: the manager
 * holds the descriptors and this server is going away either way. Saying only
 * that the outcome is unknown invites the reader to assume nothing happened,
 * which is the one reading that is certainly wrong, so the commitment leads and
 * the uncertainty qualifies it.
 */
static void
systemd_restart_request_error(char **cause, int sent, const char *fmt, ...)
{
	const struct systemd_identity	*identity;
	va_list	 ap;
	char	*msg;

	if (cause == NULL)
		return;
	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);
	if (!sent) {
		xasprintf(cause, "systemd restart request was not sent: %s",
		    msg);
		free(msg);
		return;
	}
	if (systemd_restart_activation == NULL ||
	    !systemd_restart_activation->restart_available) {
		systemd_restart_unavailable(cause, "%s", msg);
		free(msg);
		return;
	}
	identity = &systemd_restart_activation->identity;
	xasprintf(cause, "systemd restart request was committed but its "
	    "outcome could not be confirmed: %s\n"
	    "this server handed off its panes and is exiting; the manager "
	    "restarts the service on failure, so a replacement should arrive "
	    "with the panes restored\n"
	    "if it does not, the start limit may be exhausted; check with:\n"
	    "systemctl --user status '%s'", msg, identity->service_name);
	free(msg);
}

static void
systemd_identity_free(struct systemd_identity *identity)
{
	free(identity->service_name);
	free(identity->service_path);
	free(identity->socket_name);
	free(identity->socket_path);
	free(identity->listener_path);
	memset(identity, 0, sizeof *identity);
}

static void
systemd_strv_free(char **values)
{
	char	**value;

	if (values == NULL)
		return;
	for (value = values; *value != NULL; value++)
		free(*value);
	free(values);
}

static int
systemd_bus_open(sd_bus **bus, char **cause)
{
	int	r;

	r = sd_bus_open_user(bus);
	if (r < 0)
		systemd_restart_unavailable(cause,
		    "failed to connect to user manager: %s", strerror(-r));
	return (r);
}

static int
systemd_get_unit_path(sd_bus *bus, const char *name, char **path, char **cause)
{
	sd_bus_error	 error = SD_BUS_ERROR_NULL;
	sd_bus_message	*reply = NULL;
	const char	*value;
	int		 r;

	r = sd_bus_call_method(bus, SYSTEMD_BUS_NAME, SYSTEMD_MANAGER_PATH,
	    SYSTEMD_MANAGER_INTERFACE, "GetUnit", &error, &reply, "s", name);
	if (r < 0) {
		systemd_restart_unavailable(cause, "cannot resolve unit %s: %s",
		    name, error.message != NULL ? error.message : strerror(-r));
		goto out;
	}
	r = sd_bus_message_read(reply, "o", &value);
	if (r != 1 || sd_bus_message_at_end(reply, 1) != 1) {
		systemd_restart_unavailable(cause,
		    "cannot parse object path for unit %s", name);
		r = -EINVAL;
		goto out;
	}
	*path = xstrdup(value);
	r = 0;

out:
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	return (r);
}

static int
systemd_property_error(char **cause, const char *kind, const char *unit,
    const char *property, const char *expected, int error,
    const sd_bus_error *bus_error)
{
	if (cause == NULL)
		return (-1);
	if (bus_error != NULL && bus_error->name != NULL &&
	    strstr(bus_error->name, "UnknownProperty") != NULL) {
		xasprintf(cause, "systemd %s unit %s has no %s property", kind,
		    unit, property);
	} else if (error == -ENXIO || error == -EBADMSG || error == -EINVAL) {
		xasprintf(cause, "systemd %s unit %s has %s=<invalid type>, "
		    "expected %s", kind, unit, property, expected);
	} else {
		systemd_restart_unavailable(cause,
		    "cannot read %s unit %s property %s: %s", kind, unit,
		    property, bus_error != NULL && bus_error->message != NULL ?
		    bus_error->message : strerror(-error));
	}
	return (-1);
}

static int
systemd_property_string(sd_bus *bus, const char *kind, const char *unit,
    const char *path, const char *interface, const char *property,
    const char *expected, char **cause)
{
	sd_bus_error	 error = SD_BUS_ERROR_NULL;
	char		*value = NULL;
	int		 r;

	r = sd_bus_get_property_string(bus, SYSTEMD_BUS_NAME, path, interface,
	    property, &error, &value);
	if (r < 0) {
		systemd_property_error(cause, kind, unit, property, expected, r,
		    &error);
		goto out;
	}
	if (strcmp(value, expected) != 0) {
		if (cause != NULL) {
			xasprintf(cause, "systemd %s unit %s has %s=%s, "
			    "expected %s", kind, unit, property, value,
			    expected);
		}
		r = -EINVAL;
		goto out;
	}
	r = 0;

out:
	free(value);
	sd_bus_error_free(&error);
	return (r);
}

static int
systemd_property_u32_get(sd_bus *bus, const char *kind, const char *unit,
    const char *path, const char *interface, const char *property,
    const char *expected, uint32_t *result, char **cause)
{
	sd_bus_error	 error = SD_BUS_ERROR_NULL;
	uint32_t	 value;
	int		 r;

	r = sd_bus_get_property_trivial(bus, SYSTEMD_BUS_NAME, path, interface,
	    property, &error, 'u', &value);
	if (r < 0) {
		systemd_property_error(cause, kind, unit, property, expected, r,
		    &error);
		goto out;
	}
	*result = value;
	r = 0;

out:
	sd_bus_error_free(&error);
	return (r);
}

static int
systemd_property_u32(sd_bus *bus, const char *kind, const char *unit,
    const char *path, const char *interface, const char *property,
    uint32_t expected, int minimum, uint32_t *result, char **cause)
{
	uint32_t value;
	char	 actual[32], wanted[48];

	if (minimum)
		snprintf(wanted, sizeof wanted, "at least %u", expected);
	else
		snprintf(wanted, sizeof wanted, "%u", expected);
	if (systemd_property_u32_get(bus, kind, unit, path, interface, property,
	    wanted, &value, cause) < 0)
		return (-1);
	if ((!minimum && value != expected) || (minimum && value < expected)) {
		snprintf(actual, sizeof actual, "%u", value);
		if (cause != NULL) {
			xasprintf(cause, "systemd %s unit %s has %s=%s, "
			    "expected %s", kind, unit, property, actual,
			    wanted);
		}
		return (-1);
	}
	if (result != NULL)
		*result = value;
	return (0);
}

static int
systemd_property_bool(sd_bus *bus, const char *kind, const char *unit,
    const char *path, const char *interface, const char *property,
    int expected, char **cause)
{
	sd_bus_error	 error = SD_BUS_ERROR_NULL;
	int		 value, r;
	const char	*wanted = expected ? "true" : "false";

	r = sd_bus_get_property_trivial(bus, SYSTEMD_BUS_NAME, path, interface,
	    property, &error, 'b', &value);
	if (r < 0) {
		systemd_property_error(cause, kind, unit, property, wanted, r,
		    &error);
		goto out;
	}
	if (!!value != !!expected) {
		if (cause != NULL) {
			xasprintf(cause, "systemd %s unit %s has %s=%s, "
			    "expected %s", kind, unit, property,
			    value ? "true" : "false", wanted);
		}
		r = -EINVAL;
		goto out;
	}
	r = 0;

out:
	sd_bus_error_free(&error);
	return (r);
}

static int
systemd_property_strv(sd_bus *bus, const char *kind, const char *unit,
    const char *path, const char *property, char ***values, char **cause)
{
	sd_bus_error	 error = SD_BUS_ERROR_NULL;
	int		 r;

	r = sd_bus_get_property_strv(bus, SYSTEMD_BUS_NAME, path,
	    SYSTEMD_UNIT_INTERFACE, property, &error, values);
	if (r < 0)
		systemd_property_error(cause, kind, unit, property,
		    "an array of unit names", r, &error);
	sd_bus_error_free(&error);
	return (r);
}

static int
systemd_strv_contains(char **values, const char *wanted)
{
	char	**value;

	if (values == NULL)
		return (0);
	for (value = values; *value != NULL; value++) {
		if (strcmp(*value, wanted) == 0)
			return (1);
	}
	return (0);
}

static int
systemd_property_contains(sd_bus *bus, const char *kind, const char *unit,
    const char *path, const char *property, const char *wanted, char **cause)
{
	char	**values = NULL, expected[256];
	int	  r;

	r = systemd_property_strv(bus, kind, unit, path, property, &values,
	    cause);
	if (r < 0)
		goto out;
	if (!systemd_strv_contains(values, wanted)) {
		snprintf(expected, sizeof expected, "an array containing %s",
		    wanted);
		if (cause != NULL) {
			xasprintf(cause, "systemd %s unit %s has %s="
			    "<missing %s>, expected %s", kind, unit, property,
			    wanted, expected);
		}
		r = -EINVAL;
	}

out:
	systemd_strv_free(values);
	return (r);
}

static int
systemd_socket_listen_matches(sd_bus *bus, const char *unit, const char *path,
    const char *listener, int report, char **cause)
{
	sd_bus_error	 error = SD_BUS_ERROR_NULL;
	sd_bus_message	*message = NULL;
	const char	*type, *address;
	size_t		 count = 0;
	int		 match = 0, r;

	r = sd_bus_get_property(bus, SYSTEMD_BUS_NAME, path,
	    SYSTEMD_SOCKET_INTERFACE, "Listen", &error, &message, "a(ss)");
	if (r < 0) {
		if (report) {
			systemd_property_error(cause, "socket", unit,
			    "Listen", "one Stream entry for the listener path",
			    r, &error);
		}
		goto out;
	}
	r = sd_bus_message_enter_container(message, 'a', "(ss)");
	if (r < 0)
		goto invalid;
	while ((r = sd_bus_message_read(message, "(ss)", &type,
	    &address)) > 0) {
		count++;
		if (strcmp(type, "Stream") == 0 &&
		    strcmp(address, listener) == 0)
			match++;
	}
	if (r < 0)
		goto invalid;
	if (count == 1 && match == 1) {
		r = 1;
		goto out;
	}
	if (report && cause != NULL) {
		xasprintf(cause, "systemd socket unit %s has "
		    "Listen=<%zu entries>, expected one Stream entry for %s",
		    unit, count, listener);
	}
	r = 0;
	goto out;

invalid:
	if (report)
		systemd_property_error(cause, "socket", unit, "Listen",
		    "one Stream entry for the listener path", r, &error);
	r = -1;

out:
	sd_bus_error_free(&error);
	sd_bus_message_unref(message);
	return (r);
}

static int
systemd_validate_identity(sd_bus *bus, const struct systemd_identity *identity,
    uint32_t minimum_store, int require_empty, char **cause)
{
	char		*path = NULL;
	uint32_t	 pid;
	int		 r = -1;

	if (systemd_get_unit_path(bus, identity->service_name, &path,
	    cause) < 0)
		goto out;
	if (strcmp(path, identity->service_path) != 0) {
		systemd_restart_unavailable(cause, "service unit %s changed "
		    "object path", identity->service_name);
		goto out;
	}
	free(path);
	path = NULL;
	if (systemd_get_unit_path(bus, identity->socket_name, &path, cause) < 0)
		goto out;
	if (strcmp(path, identity->socket_path) != 0) {
		systemd_restart_unavailable(cause, "socket unit %s changed "
		    "object path", identity->socket_name);
		goto out;
	}
	free(path);
	path = NULL;

	pid = getpid();
	if (systemd_property_u32(bus, "service", identity->service_name,
	    identity->service_path, SYSTEMD_SERVICE_INTERFACE, "MainPID", pid,
	    0, NULL, cause) < 0 ||
	    systemd_property_string(bus, "service", identity->service_name,
	    identity->service_path, SYSTEMD_SERVICE_INTERFACE, "Type", "notify",
	    cause) < 0 ||
	    systemd_property_string(bus, "service", identity->service_name,
	    identity->service_path, SYSTEMD_SERVICE_INTERFACE, "NotifyAccess",
	    "main", cause) < 0 ||
	    systemd_property_string(bus, "service", identity->service_name,
	    identity->service_path, SYSTEMD_SERVICE_INTERFACE, "KillMode",
	    "process", cause) < 0 ||
	    systemd_property_string(bus, "service", identity->service_name,
	    identity->service_path, SYSTEMD_SERVICE_INTERFACE, "Restart",
	    "on-failure", cause) < 0 ||
	    systemd_property_string(bus, "service", identity->service_name,
	    identity->service_path, SYSTEMD_SERVICE_INTERFACE,
	    "FileDescriptorStorePreserve", "yes", cause) < 0 ||
	    systemd_property_u32(bus, "service", identity->service_name,
	    identity->service_path, SYSTEMD_SERVICE_INTERFACE,
	    "FileDescriptorStoreMax", minimum_store, 1, NULL, cause) < 0)
		goto out;
	if (require_empty && systemd_property_u32(bus, "service",
	    identity->service_name, identity->service_path,
	    SYSTEMD_SERVICE_INTERFACE, "NFileDescriptorStore", 0, 0, NULL,
	    cause) < 0)
		goto out;
	if (systemd_property_bool(bus, "socket", identity->socket_name,
	    identity->socket_path, SYSTEMD_SOCKET_INTERFACE, "Accept", 0,
	    cause) < 0 ||
	    systemd_property_string(bus, "socket", identity->socket_name,
	    identity->socket_path, SYSTEMD_SOCKET_INTERFACE,
	    "FileDescriptorName", SYSTEMD_SOCKET_FD_NAME, cause) < 0 ||
	    systemd_property_u32(bus, "socket", identity->socket_name,
	    identity->socket_path, SYSTEMD_SOCKET_INTERFACE, "SocketMode", 0600,
	    0, NULL, cause) < 0 ||
	    systemd_property_u32(bus, "socket", identity->socket_name,
	    identity->socket_path, SYSTEMD_SOCKET_INTERFACE, "DirectoryMode",
	    0700, 0, NULL, cause) < 0)
		goto out;
	if (systemd_socket_listen_matches(bus, identity->socket_name,
	    identity->socket_path, identity->listener_path, 1, cause) != 1)
		goto out;
	if (systemd_property_contains(bus, "service", identity->service_name,
	    identity->service_path, "Requires", identity->socket_name,
	    cause) < 0 ||
	    systemd_property_contains(bus, "service", identity->service_name,
	    identity->service_path, "After", identity->socket_name,
	    cause) < 0 ||
	    systemd_property_contains(bus, "service", identity->service_name,
	    identity->service_path, "TriggeredBy", identity->socket_name,
	    cause) < 0 ||
	    systemd_property_contains(bus, "socket", identity->socket_name,
	    identity->socket_path, "Triggers", identity->service_name,
	    cause) < 0)
		goto out;
	r = 0;

out:
	free(path);
	return (r);
}

static int
systemd_socket_candidate(sd_bus *bus, const char *name, const char *path,
    const char *listener)
{
	sd_bus_error	 error = SD_BUS_ERROR_NULL;
	char		*fdname = NULL;
	int		 r, match = 0;

	r = sd_bus_get_property_string(bus, SYSTEMD_BUS_NAME, path,
	    SYSTEMD_SOCKET_INTERFACE, "FileDescriptorName", &error, &fdname);
	if (r >= 0 && strcmp(fdname, SYSTEMD_SOCKET_FD_NAME) == 0 &&
	    systemd_socket_listen_matches(bus, name, path, listener, 0,
	    NULL) == 1)
		match = 1;
	free(fdname);
	sd_bus_error_free(&error);
	return (match);
}

static int
systemd_restart_prepare(struct restart_activation *base, char **cause)
{
	struct systemd_activation *activation = (void *)base;
	struct systemd_identity	 identity = { 0 };
	sd_bus			*bus = NULL;
	char		       **units = NULL, **unit;
	char			*path = NULL;
	size_t			 matches = 0;
	int			 r = -1;

	if (cause != NULL)
		*cause = NULL;
	if (activation == NULL ||
	    (activation->type != RESTART_ACTIVATION_SOCKET &&
	    activation->type != RESTART_ACTIVATION_RESTORE)) {
		systemd_restart_unavailable(cause,
		    "server socket is not manager-owned");
		return (-1);
	}
	if (activation->restart_prepared) {
		if (activation->restart_available)
			return (0);
		systemd_restart_unavailable(cause,
		    "service and socket identity was not validated at startup");
		return (-1);
	}
	activation->restart_prepared = 1;

	r = sd_pid_get_user_unit(getpid(), &identity.service_name);
	if (r < 0) {
		systemd_restart_unavailable(cause,
		    "cannot identify owning user service: %s", strerror(-r));
		goto fail;
	}
	r = -1;
	if (socket_path == NULL || *socket_path == '\0') {
		systemd_restart_unavailable(cause,
		    "manager-owned listener has no filesystem path");
		goto fail;
	}
	identity.listener_path = xstrdup(socket_path);
	if (systemd_bus_open(&bus, cause) < 0 ||
	    systemd_get_unit_path(bus, identity.service_name,
	    &identity.service_path, cause) < 0 ||
	    systemd_property_strv(bus, "service", identity.service_name,
	    identity.service_path, "TriggeredBy", &units, cause) < 0)
		goto fail;

	for (unit = units; unit != NULL && *unit != NULL; unit++) {
		size_t len = strlen(*unit);

		if (len < 7 || strcmp(*unit + len - 7, ".socket") != 0)
			continue;
		if (systemd_get_unit_path(bus, *unit, &path, cause) < 0)
			goto fail;
		if (systemd_socket_candidate(bus, *unit, path,
		    identity.listener_path)) {
			matches++;
			if (matches == 1) {
				identity.socket_name = xstrdup(*unit);
				identity.socket_path = path;
				path = NULL;
			}
		}
		free(path);
		path = NULL;
	}
	if (matches != 1) {
		systemd_restart_unavailable(cause,
		    "expected one matching socket unit, found %zu", matches);
		goto fail;
	}
	if (systemd_validate_identity(bus, &identity, 1, 0, cause) < 0)
		goto fail;

	activation->identity = identity;
	memset(&identity, 0, sizeof identity);
	activation->restart_available = 1;
	systemd_restart_activation = activation;
	r = 0;

fail:
	free(path);
	systemd_strv_free(units);
	sd_bus_unref(bus);
	systemd_identity_free(&identity);
	return (r);
}

static int
systemd_pane_name(const struct restart_fd *pane, char **name)
{
	if (pane->pid <= 0)
		return (-1);
	xasprintf(name, "%s%u.%ld", SYSTEMD_PANE_FD_PREFIX, pane->pane_id,
	    (long)pane->pid);
	if (strlen(*name) > 255) {
		free(*name);
		*name = NULL;
		return (-1);
	}
	return (0);
}

static int
systemd_validate_panes(const struct restart_fd *panes,
    size_t pane_count, int state_fd, char **cause)
{
	size_t	 i, j;
	char	*name = NULL;

	if (pane_count == SIZE_MAX || pane_count > UINT32_MAX - 1) {
		systemd_restart_unavailable(cause,
		    "descriptor count overflows");
		return (-1);
	}
	if (pane_count != 0 && panes == NULL) {
		systemd_restart_unavailable(cause,
		    "pane descriptor vector is missing");
		return (-1);
	}
	if (state_fd >= 0 && fcntl(state_fd, F_GETFD) == -1) {
		systemd_restart_unavailable(cause,
		    "checkpoint descriptor is invalid: %s", strerror(errno));
		return (-1);
	}
	for (i = 0; i < pane_count; i++) {
		if (panes[i].fd < 0 || fcntl(panes[i].fd, F_GETFD) == -1 ||
		    (uintmax_t)panes[i].pid > INT_MAX) {
			systemd_restart_unavailable(cause,
			    "pane descriptor %zu is invalid", i);
			return (-1);
		}
		if (state_fd >= 0 && panes[i].fd == state_fd) {
			systemd_restart_unavailable(cause,
			    "pane descriptor %zu duplicates the checkpoint", i);
			return (-1);
		}
		if (systemd_pane_name(&panes[i], &name) < 0) {
			systemd_restart_unavailable(cause,
			    "pane descriptor %zu has an invalid key", i);
			return (-1);
		}
		free(name);
		name = NULL;
		for (j = 0; j < i; j++) {
			if (panes[i].pane_id == panes[j].pane_id ||
			    panes[i].pid == panes[j].pid ||
			    panes[i].fd == panes[j].fd) {
				systemd_restart_unavailable(cause,
				    "pane descriptor vector has duplicate keys "
				    "or descriptors");
				return (-1);
			}
		}
	}
	return (0);
}

static int
systemd_name_budget(const struct restart_fd *panes,
    size_t pane_count, char **cause)
{
	long	 page_size;
	size_t	 i, limit, total;
	char	*name = NULL;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0 || (uintmax_t)page_size > SIZE_MAX / 32) {
		systemd_restart_unavailable(cause,
		    "cannot determine descriptor-name environment limit");
		return (-1);
	}
	limit = (size_t)page_size * 32;
	total = sizeof "LISTEN_FDNAMES=" - 1;
	if (total > SIZE_MAX - strlen(SYSTEMD_SOCKET_FD_NAME) ||
	    (total += strlen(SYSTEMD_SOCKET_FD_NAME)) > SIZE_MAX - 1 ||
	    ++total > SIZE_MAX - strlen(SYSTEMD_STATE_FD_NAME))
		goto overflow;
	total += strlen(SYSTEMD_STATE_FD_NAME);
	for (i = 0; i < pane_count; i++) {
		if (systemd_pane_name(&panes[i], &name) < 0)
			goto overflow;
		if (total > SIZE_MAX - 1 || ++total > SIZE_MAX - strlen(name)) {
			free(name);
			goto overflow;
		}
		total += strlen(name);
		free(name);
		name = NULL;
	}
	if (total == SIZE_MAX)
		goto overflow;
	total++;
	if (total > limit) {
		systemd_restart_unavailable(cause,
		    "descriptor-name environment needs %zu bytes, limit is %zu",
		    total, limit);
		return (-1);
	}
	return (0);

overflow:
	systemd_restart_unavailable(cause,
	    "descriptor-name environment length overflows");
	return (-1);
}

static int
systemd_restart_preflight(const struct restart_activation *base,
    uint64_t flags, const struct restart_fd *panes, size_t pane_count,
    char **cause)
{
	const struct systemd_activation	*activation = (const void *)base;
	sd_bus	*bus = NULL;
	int	 r = -1;

	if (cause != NULL)
		*cause = NULL;
	/*
	 * Both activation types are manager-owned and both carry a validated
	 * identity.  A server started by the socket unit is SOCKET; a
	 * replacement is RESTART, because it inherits the listener alongside
	 * the checkpoint and pane descriptors.  A replacement is as
	 * restartable as the server it replaced.
	 */
	if (activation == NULL ||
	    (activation->type != RESTART_ACTIVATION_SOCKET &&
	    activation->type != RESTART_ACTIVATION_RESTORE) ||
	    !activation->restart_available ||
	    systemd_restart_activation != activation) {
		systemd_restart_unavailable(cause,
		    "validated manager-owned socket identity is unavailable");
		return (-1);
	}
	if (~flags & CLIENT_NOFORK) {
		systemd_restart_unavailable(cause,
		    "server was not started with client no-fork mode");
		return (-1);
	}
	if (systemd_validate_panes(panes, pane_count, -1, cause) < 0 ||
	    systemd_name_budget(panes, pane_count, cause) < 0)
		return (-1);
	if (systemd_bus_open(&bus, cause) < 0)
		goto out;
	if (systemd_validate_identity(bus, &activation->identity,
	    pane_count + 1, 1, cause) < 0)
		goto out;
	r = 0;

out:
	sd_bus_unref(bus);
	return (r);
}

static int
systemd_fdstore_count(sd_bus *bus, uint32_t *count, char **cause)
{
	const struct systemd_identity *identity;

	if (systemd_restart_activation == NULL ||
	    !systemd_restart_activation->restart_available) {
		systemd_restart_unavailable(cause,
		    "validated service identity is unavailable");
		return (-1);
	}
	identity = &systemd_restart_activation->identity;
	return (systemd_property_u32_get(bus, "service", identity->service_name,
	    identity->service_path, SYSTEMD_SERVICE_INTERFACE,
	    "NFileDescriptorStore", "an unsigned count", count, cause));
}

static int
systemd_deadline_start(struct timespec *deadline)
{
	if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0)
		return (-errno);
	deadline->tv_sec += SYSTEMD_NOTIFY_TIMEOUT / 1000000;
	deadline->tv_nsec += (SYSTEMD_NOTIFY_TIMEOUT % 1000000) * 1000;
	if (deadline->tv_nsec >= 1000000000) {
		deadline->tv_sec++;
		deadline->tv_nsec -= 1000000000;
	}
	return (0);
}

static int
systemd_deadline_remaining(const struct timespec *deadline,
    uint64_t *remaining)
{
	struct timespec	 now;
	uint64_t	 seconds;
	long		 nanoseconds;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return (-errno);
	if (now.tv_sec > deadline->tv_sec ||
	    (now.tv_sec == deadline->tv_sec &&
	    now.tv_nsec >= deadline->tv_nsec))
		return (-ETIMEDOUT);
	seconds = deadline->tv_sec - now.tv_sec;
	nanoseconds = deadline->tv_nsec - now.tv_nsec;
	if (nanoseconds < 0) {
		seconds--;
		nanoseconds += 1000000000;
	}
	*remaining = seconds * 1000000 + nanoseconds / 1000;
	if (*remaining == 0)
		return (-ETIMEDOUT);
	return (0);
}

static int
systemd_notify_barrier_wait(char **cause)
{
	struct timespec	 deadline;
	uint64_t	 remaining;
	int		 r;

	if (cause != NULL)
		*cause = NULL;
	r = systemd_deadline_start(&deadline);
	if (r < 0)
		goto fail;
	for (;;) {
		r = systemd_deadline_remaining(&deadline, &remaining);
		if (r < 0)
			goto fail;
		r = sd_notify_barrier(0, remaining);
		if (r != -EINTR)
			break;
	}
	if (r > 0)
		return (0);
	if (r == 0) {
		if (cause != NULL)
			*cause = xstrdup("notification socket is unavailable");
		return (-1);
	}

fail:
	if (cause != NULL) {
		if (r == -ETIMEDOUT) {
			*cause = xstrdup("timed out waiting for "
			    "notification barrier");
		}
		else
			xasprintf(cause, "%s", strerror(-r));
	}
	return (-1);
}

static int
systemd_fdstore_remove_names(sd_bus *bus,
    const struct restart_fd *panes, size_t pane_count, char **cause)
{
	char		*name = NULL, *state = NULL, *first = NULL;
	size_t		 i;
	uint32_t	 count = 0;
	int		 r;

	for (i = 0; i < pane_count; i++) {
		if (systemd_pane_name(&panes[i], &name) < 0) {
			if (first == NULL)
				first = xstrdup("invalid pane descriptor key");
			continue;
		}
		xasprintf(&state, "FDSTOREREMOVE=1\nFDNAME=%s", name);
		r = sd_notify(0, state);
		if (r <= 0 && first == NULL) {
			xasprintf(&first, "%s removal was not sent: %s", name,
			    r == 0 ? "notification socket is unavailable" :
			    strerror(-r));
		}
		free(state);
		free(name);
		state = name = NULL;
	}
	xasprintf(&state, "FDSTOREREMOVE=1\nFDNAME=%s", SYSTEMD_STATE_FD_NAME);
	r = sd_notify(0, state);
	if (r <= 0 && first == NULL) {
		xasprintf(&first, "%s removal was not sent: %s",
		    SYSTEMD_STATE_FD_NAME, r == 0 ?
		    "notification socket is unavailable" : strerror(-r));
	}
	free(state);
	state = NULL;

	r = systemd_notify_barrier_wait(&state);
	if (r < 0 && first == NULL) {
		xasprintf(&first, "notification barrier failed: %s",
		    state == NULL ? "unknown error" : state);
	}
	free(state);
	state = NULL;
	if (systemd_fdstore_count(bus, &count, &state) < 0) {
		if (first == NULL) {
			first = state;
			state = NULL;
		}
		free(state);
	} else if (count != 0 && first == NULL) {
		xasprintf(&first, "fdstore contains %u descriptors", count);
	}
	if (first != NULL) {
		systemd_fdstore_cleanup_error(cause, "%s", first);
		free(first);
		return (-1);
	}
	return (0);
}

static enum restart_store_result
systemd_restart_remove(const struct restart_fd *panes,
    size_t pane_count, char **cause)
{
	sd_bus	*bus = NULL;
	char	*detail = NULL;
	int	 r;

	if (cause != NULL)
		*cause = NULL;
	if (systemd_restart_activation == NULL ||
	    !systemd_restart_activation->restart_available) {
		systemd_fdstore_cleanup_error(cause,
		    "validated service identity is unavailable");
		return (RESTART_STORE_ERROR_DIRTY);
	}
	if (systemd_validate_panes(panes, pane_count, -1, &detail) < 0) {
		systemd_fdstore_cleanup_error(cause, "%s",
		    detail == NULL ? "invalid descriptor vector" : detail);
		free(detail);
		return (RESTART_STORE_ERROR_DIRTY);
	}
	if (systemd_bus_open(&bus, &detail) < 0) {
		systemd_fdstore_cleanup_error(cause, "%s",
		    detail == NULL ? "cannot connect to user manager" : detail);
		free(detail);
		return (RESTART_STORE_ERROR_DIRTY);
	}
	r = systemd_fdstore_remove_names(bus, panes, pane_count, cause);
	sd_bus_unref(bus);
	if (r < 0)
		return (RESTART_STORE_ERROR_DIRTY);
	return (RESTART_STORE_OK);
}

static enum restart_store_result
systemd_restart_activation_remove(const struct restart_activation *base,
    char **cause)
{
	const struct systemd_activation	*activation = (const void *)base;

	if (cause != NULL)
		*cause = NULL;
	if (activation == NULL ||
	    activation->type != RESTART_ACTIVATION_RESTORE) {
		systemd_fdstore_cleanup_error(cause,
		    "incoming restart activation is unavailable");
		return (RESTART_STORE_ERROR_DIRTY);
	}
	return (systemd_restart_remove(activation->panes,
	    activation->pane_count, cause));
}

static enum restart_store_result
systemd_restart_store(int state_fd, const struct restart_fd *panes,
    size_t pane_count, char **cause)
{
	sd_bus		*bus = NULL;
	char		*detail = NULL, *name = NULL, *state = NULL;
	size_t		 i;
	uint32_t	 count;
	int		 r;

	if (cause != NULL)
		*cause = NULL;
	if (state_fd < 0) {
		systemd_restart_unavailable(&detail,
		    "checkpoint descriptor is invalid");
	}
	if (detail != NULL ||
	    systemd_validate_panes(panes, pane_count, state_fd, &detail) < 0 ||
	    systemd_name_budget(panes, pane_count, &detail) < 0) {
		systemd_fdstore_add_error(cause, SYSTEMD_STATE_FD_NAME, "%s",
		    detail == NULL ? "invalid descriptor vector" : detail);
		free(detail);
		return (RESTART_STORE_ERROR_CLEAN);
	}
	if (systemd_bus_open(&bus, &detail) < 0) {
		systemd_fdstore_cleanup_error(cause, "%s",
		    detail == NULL ? "cannot connect to user manager" : detail);
		free(detail);
		sd_bus_unref(bus);
		return (RESTART_STORE_ERROR_DIRTY);
	}
	if (systemd_fdstore_count(bus, &count, &detail) < 0) {
		systemd_fdstore_cleanup_error(cause, "%s",
		    detail == NULL ? "cannot query fdstore" : detail);
		free(detail);
		sd_bus_unref(bus);
		return (RESTART_STORE_ERROR_DIRTY);
	}
	if (count != 0) {
		xasprintf(&detail, "systemd fdstore contains %u descriptors, "
		    "expected 0", count);
		goto cleanup;
	}

	for (i = 0; i < pane_count; i++) {
		systemd_pane_name(&panes[i], &name);
		xasprintf(&state, "FDSTORE=1\nFDNAME=%s\nFDPOLL=0", name);
		r = sd_pid_notify_with_fds(0, 0, state, &panes[i].fd, 1);
		free(state);
		state = NULL;
		if (r <= 0) {
			systemd_fdstore_add_error(&detail, name, "%s",
			    r == 0 ? "notification socket is unavailable" :
			    strerror(-r));
			free(name);
			name = NULL;
			goto cleanup;
		}
		free(name);
		name = NULL;
	}
	xasprintf(&state, "FDSTORE=1\nFDNAME=%s\nFDPOLL=0",
	    SYSTEMD_STATE_FD_NAME);
	r = sd_pid_notify_with_fds(0, 0, state, &state_fd, 1);
	free(state);
	state = NULL;
	if (r <= 0) {
		systemd_fdstore_add_error(&detail, SYSTEMD_STATE_FD_NAME,
		    "%s", r == 0 ? "notification socket is unavailable" :
		    strerror(-r));
		goto cleanup;
	}
	r = systemd_notify_barrier_wait(&state);
	if (r < 0) {
		systemd_fdstore_add_error(&detail, SYSTEMD_STATE_FD_NAME,
		    "notification barrier failed: %s",
		    state == NULL ? "unknown error" : state);
		free(state);
		state = NULL;
		goto cleanup;
	}
	if (systemd_fdstore_count(bus, &count, &detail) < 0) {
		state = detail;
		detail = NULL;
		systemd_fdstore_add_error(&detail, SYSTEMD_STATE_FD_NAME, "%s",
		    state == NULL ? "cannot query fdstore" : state);
		free(state);
		state = NULL;
		goto cleanup;
	}
	if (count != pane_count + 1) {
		if (detail != NULL)
			free(detail);
		xasprintf(&detail, "systemd fdstore contains %u descriptors, "
		    "expected %zu", count, pane_count + 1);
		goto cleanup;
	}
	sd_bus_unref(bus);
	return (RESTART_STORE_OK);

cleanup:
	if (systemd_fdstore_remove_names(bus, panes, pane_count, &state) == 0) {
		if (cause != NULL)
			*cause = detail;
		else
			free(detail);
		free(state);
		sd_bus_unref(bus);
		return (RESTART_STORE_ERROR_CLEAN);
	}
	free(detail);
	if (cause != NULL)
		*cause = state;
	else
		free(state);
	sd_bus_unref(bus);
	return (RESTART_STORE_ERROR_DIRTY);
}

static int
systemd_restart_ready(char **cause)
{
	char	*detail = NULL;
	int	 r;

	if (cause != NULL)
		*cause = NULL;
	if (systemd_ready(cause) != 0)
		return (-1);
	r = systemd_notify_barrier_wait(&detail);
	if (r < 0) {
		if (cause != NULL)
			xasprintf(cause, "systemd readiness error: %s",
			    detail == NULL ? "unknown error" : detail);
		free(detail);
		return (-1);
	}
	return (0);
}

struct systemd_restart_watch {
	int	 done;
	int	 queued;
	char	*detail;
};

static int
systemd_restart_reply(sd_bus_message *message, void *data,
    __unused sd_bus_error *ret_error)
{
	struct systemd_restart_watch	*watch = data;
	const sd_bus_error		*error;
	const char			*path;
	int				 r;

	watch->done = 1;
	if (sd_bus_message_is_method_error(message, NULL)) {
		error = sd_bus_message_get_error(message);
		watch->detail = xstrdup(error != NULL &&
		    error->message != NULL ? error->message :
		    "manager returned a method error");
		return (0);
	}
	r = sd_bus_message_read(message, "o", &path);
	if (r != 1 || sd_bus_message_at_end(message, 1) != 1) {
		watch->detail = xstrdup("manager returned a malformed reply");
		return (0);
	}
	watch->queued = 1;
	return (0);
}

static enum restart_dispatch_result
systemd_restart_service(char **cause)
{
	struct systemd_restart_watch	 watch = { 0 };
	sd_bus				*bus = NULL;
	sd_bus_message			*message = NULL;
	sd_bus_slot			*slot = NULL;
	struct timespec			 deadline;
	uint64_t			 remaining;
	int				 r;

	if (cause != NULL)
		*cause = NULL;
	if (systemd_restart_activation == NULL ||
	    !systemd_restart_activation->restart_available) {
		systemd_restart_request_error(cause, 0,
		    "validated service identity is unavailable");
		goto not_sent;
	}
	r = sd_bus_open_user(&bus);
	if (r < 0) {
		systemd_restart_request_error(cause, 0,
		    "failed to connect to user manager: %s", strerror(-r));
		goto not_sent;
	}
	r = sd_bus_message_new_method_call(bus, &message, SYSTEMD_BUS_NAME,
	    SYSTEMD_MANAGER_PATH, SYSTEMD_MANAGER_INTERFACE, "RestartUnit");
	if (r < 0) {
		systemd_restart_request_error(cause, 0,
		    "cannot construct RestartUnit call: %s", strerror(-r));
		goto not_sent;
	}
	r = sd_bus_message_append(message, "ss",
	    systemd_restart_activation->identity.service_name, "replace");
	if (r < 0) {
		systemd_restart_request_error(cause, 0,
		    "cannot construct RestartUnit arguments: %s", strerror(-r));
		goto not_sent;
	}
	r = systemd_deadline_start(&deadline);
	if (r < 0) {
		systemd_restart_request_error(cause, 0,
		    "cannot read monotonic clock: %s", strerror(-r));
		goto not_sent;
	}
	r = sd_bus_call_async(bus, &slot, message, systemd_restart_reply,
	    &watch, SYSTEMD_NOTIFY_TIMEOUT);
	if (r < 0) {
		systemd_restart_request_error(cause, 0,
		    "asynchronous submission failed: %s", strerror(-r));
		goto not_sent;
	}
	while (!watch.done) {
		r = systemd_deadline_remaining(&deadline, &remaining);
		if (r == -ETIMEDOUT) {
			watch.detail = xstrdup("timed out waiting for "
			    "manager reply");
			break;
		}
		if (r < 0) {
			xasprintf(&watch.detail, "cannot read monotonic "
			    "clock: %s", strerror(-r));
			break;
		}
		r = sd_bus_process(bus, NULL);
		if (r < 0) {
			xasprintf(&watch.detail, "bus processing failed: %s",
			    strerror(-r));
			break;
		}
		if (r > 0)
			continue;
		r = systemd_deadline_remaining(&deadline, &remaining);
		if (r == -ETIMEDOUT) {
			watch.detail = xstrdup("timed out waiting for "
			    "manager reply");
			break;
		}
		if (r < 0) {
			xasprintf(&watch.detail, "cannot read monotonic "
			    "clock: %s", strerror(-r));
			break;
		}
		r = sd_bus_wait(bus, remaining);
		if (r < 0) {
			xasprintf(&watch.detail, "bus wait failed: %s",
			    strerror(-r));
			break;
		}
	}
	if (watch.queued) {
		sd_bus_slot_unref(slot);
		sd_bus_message_unref(message);
		sd_bus_unref(bus);
		return (RESTART_DISPATCH_QUEUED);
	}
	systemd_restart_request_error(cause, 1, "%s",
	    watch.detail == NULL ? "manager reply was not confirmed" :
	    watch.detail);
	free(watch.detail);
	sd_bus_slot_unref(slot);
	sd_bus_message_unref(message);
	sd_bus_unref(bus);
	return (RESTART_DISPATCH_UNKNOWN);

not_sent:
	free(watch.detail);
	sd_bus_slot_unref(slot);
	sd_bus_message_unref(message);
	sd_bus_unref(bus);
	return (RESTART_DISPATCH_NOT_SENT);
}

static const struct server_restart_ops systemd_restart_ops = {
	.activation_type = systemd_restart_activation_type,
	.activation_state_fd = systemd_restart_activation_state_fd,
	.activation_pane_count = systemd_restart_activation_pane_count,
	.activation_pane_at = systemd_restart_activation_pane_at,
	.activation_lookup = systemd_restart_activation_lookup,
	.activation_remove = systemd_restart_activation_remove,
	.activation_close = systemd_restart_activation_close,
	.prepare = systemd_restart_prepare,
	.preflight = systemd_restart_preflight,
	.checkpoint_create = restart_checkpoint_create,
	.checkpoint_seal = restart_checkpoint_seal,
	.checkpoint_read = restart_checkpoint_read,
	.store = systemd_restart_store,
	.remove = systemd_restart_remove,
	.restart = systemd_restart_service,
	.baseline_ready = systemd_ready,
	.ready = systemd_restart_ready
};

const struct server_restart_ops *
systemd_restart_get_ops(void)
{
	return (&systemd_restart_ops);
}
#endif

#ifdef ENABLE_CGROUPS
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
	char			*name, *desc, *slice, *unit = NULL;
	char			*partof = NULL;
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
#ifdef HAVE_SYSTEMD_RESTART
	if (systemd_restart_activation != NULL &&
	    systemd_restart_activation->restart_available) {
		unit = xstrdup(
		    systemd_restart_activation->identity.service_name);
		partof = xstrdup(
		    systemd_restart_activation->identity.socket_name);
	}
#endif
	if (unit == NULL && (sd_pid_get_user_unit(parent_pid, &unit) == 0 ||
	    sd_pid_get_unit(parent_pid, &unit) == 0))
		partof = xstrdup(unit);
	if (unit != NULL) {
		r = sd_bus_message_append(m, "(sv)", "Before", "as", 1, unit);
		if (r >= 0) {
			r = sd_bus_message_append(m, "(sv)", "PartOf", "as", 1,
			    partof);
		}
		free(unit);
		free(partof);
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
#endif
