/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Alexandre Fiori <fiorix@gmail.com>
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
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"
#include "restart-codec.h"
#include "restart-exec.h"

/*
 * Replace the server by executing its own image again, with the pane pty
 * masters, the listening socket and the checkpoint left open across the exec.
 * The process keeps its pid, so the pane processes stay its children and can
 * still be waited for.
 *
 * Nothing here is conditional on a service manager. The descriptors survive
 * because the process does, which is also the limit of it: a server killed
 * outright takes them with it.
 *
 * There is no way back to the old image once the exec has happened, so the new
 * image is first run as a child and asked to read the checkpoint. A restart is
 * only committed to once that has worked.
 */

/* Names the replacement reads its inherited descriptors from. */
#define RESTART_EXEC_LISTEN "TMUX_RESTART_LISTEN"
#define RESTART_EXEC_STATE "TMUX_RESTART_STATE"
#define RESTART_EXEC_PANES "TMUX_RESTART_PANES"

/* Set when an image is run only to check it can read the checkpoint. */
#define RESTART_EXEC_CHECK "TMUX_RESTART_CHECK"

/* The longest argument vector build_argv can produce, including the NULL. */
#define RESTART_EXEC_MAXARGV 16

/* How much of a failed check's explanation is kept. */
#define RESTART_EXEC_CHECK_CAUSE 256

struct restart_exec_activation {
	enum restart_activation_type	 type;
	int				 listen_fd;
	int				 state_fd;
	struct restart_fd		*panes;
	size_t				 pane_count;
};

static char			*restart_exec_path;
static int			 restart_exec_listen_fd = -1;
static int			 restart_exec_armed;
static int			 restart_exec_checkpoint_fd = -1;
static struct restart_fd	*restart_exec_vector;
static size_t			 restart_exec_vector_count;

static void	 restart_exec_inherit(int);
static int	 restart_exec_parse_panes(const char *, struct restart_fd **,
		     size_t *, char **);
static size_t	 restart_exec_build_argv(char **, size_t, char *);
static int	 restart_exec_probe(const char *, int, char **);
static void	 restart_exec_disarm(void);

static enum restart_activation_type restart_exec_activation_type(
		     const struct restart_activation *);
static int	 restart_exec_activation_state_fd(
		     const struct restart_activation *);
static size_t	 restart_exec_activation_pane_count(
		     const struct restart_activation *);
static int	 restart_exec_activation_pane_at(
		     const struct restart_activation *, size_t,
		     struct restart_fd *);
static int	 restart_exec_activation_lookup(void *, u_int, pid_t, int *);
static enum restart_store_result restart_exec_activation_remove(
		     const struct restart_activation *, char **);
static void	 restart_exec_activation_close(struct restart_activation *);
static int	 restart_exec_prepare(struct restart_activation *, char **);
static int	 restart_exec_preflight(const struct restart_activation *,
		     uint64_t, const struct restart_fd *, size_t, char **);
static enum restart_store_result restart_exec_store(int,
		     const struct restart_fd *, size_t, char **);
static enum restart_store_result restart_exec_remove(
		     const struct restart_fd *, size_t, char **);
static enum restart_dispatch_result restart_exec_restart(char **);
static int	 restart_exec_baseline_ready(char **);
static int	 restart_exec_ready(char **);

static const struct server_restart_ops restart_exec_ops = {
	.activation_type = restart_exec_activation_type,
	.activation_state_fd = restart_exec_activation_state_fd,
	.activation_pane_count = restart_exec_activation_pane_count,
	.activation_pane_at = restart_exec_activation_pane_at,
	.activation_lookup = restart_exec_activation_lookup,
	.activation_remove = restart_exec_activation_remove,
	.activation_close = restart_exec_activation_close,
	.prepare = restart_exec_prepare,
	.preflight = restart_exec_preflight,
	.checkpoint_create = restart_checkpoint_create,
	.checkpoint_write = restart_checkpoint_write,
	.checkpoint_read = restart_checkpoint_read,
	.store = restart_exec_store,
	.remove = restart_exec_remove,
	.restart = restart_exec_restart,
	.baseline_ready = restart_exec_baseline_ready,
	.ready = restart_exec_ready
};

const struct server_restart_ops *
restart_exec_get_ops(void)
{
	return (&restart_exec_ops);
}

/*
 * Clear close-on-exec so the descriptor reaches the replacement.
 * systemd_activated must not run between this and exec; it sets the flag.
 */
static void
restart_exec_inherit(int fd)
{
	int	flags;

	if (fd == -1)
		return;
	flags = fcntl(fd, F_GETFD);
	if (flags != -1 && (flags & FD_CLOEXEC))
		fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
}

static int
restart_exec_parse_panes(const char *s, struct restart_fd **out, size_t *outlen,
    char **cause)
{
	struct restart_fd	*panes = NULL;
	size_t			 count = 0;
	char			*copy, *token, *next;
	unsigned long		 id;
	long long		 pid;
	long			 fd;

	*out = NULL;
	*outlen = 0;
	if (*s == '\0')
		return (0);

	copy = xstrdup(s);
	next = copy;
	while ((token = strsep(&next, ",")) != NULL) {
		if (*token == '\0')
			continue;
		if (sscanf(token, "%lu:%lld:%ld", &id, &pid, &fd) != 3) {
			xasprintf(cause, "cannot parse restart descriptor "
			    "\"%s\"", token);
			free(panes);
			free(copy);
			return (-1);
		}
		panes = xreallocarray(panes, count + 1, sizeof *panes);
		panes[count].pane_id = (u_int)id;
		panes[count].pid = (pid_t)pid;
		panes[count].fd = (int)fd;
		count++;
	}
	free(copy);
	*out = panes;
	*outlen = count;
	return (0);
}

/*
 * Build the argument vector for a replacement server. argv[0] must be the path
 * rather than a bare name: the replacement resolves its own image from what it
 * is handed, and a bare name would be searched on PATH, so a server started
 * from a build directory would restart into the installed binary.
 */
static size_t
restart_exec_build_argv(char **argv, size_t size, char *argv0)
{
	size_t	i, n = 0;

	argv[n++] = argv0;
	argv[n++] = (char *)"-D";
	argv[n++] = (char *)"-S";
	argv[n++] = (char *)socket_path;
	for (i = 0; i < (size_t)log_get_level() && n < size - 1; i++)
		argv[n++] = (char *)"-v";
	argv[n] = NULL;
	return (n);
}

/*
 * Run the candidate image as a child and have it read the checkpoint, so that
 * a replacement that cannot start, or cannot restore what this server wrote, is
 * found while this server can still refuse the restart. The checkpoint is its
 * standard input and what it writes to standard error becomes the reason.
 *
 * An image that knows the check never looks at its arguments. They are for one
 * that does not, such as an older tmux: they make it fail at once without
 * starting or reaching a server, where no arguments would have it connect to
 * this one, which has stopped accepting, and wait for ever.
 */
static int
restart_exec_probe(const char *path, int checkpoint_fd, char **cause)
{
	char		 buf[RESTART_EXEC_CHECK_CAUSE], chunk[128];
	const char	*what;
	size_t		 len = 0;
	ssize_t		 n;
	pid_t		 pid, got;
	int		 status, devnull, p[2];
	sigset_t	 set, oldset;

	if (pipe(p) != 0) {
		xasprintf(cause, "cannot check %s: %s", path, strerror(errno));
		return (-1);
	}
	sigfillset(&set);
	sigprocmask(SIG_BLOCK, &set, &oldset);
	pid = fork();
	if (pid == -1) {
		sigprocmask(SIG_SETMASK, &oldset, NULL);
		xasprintf(cause, "cannot check %s: %s", path, strerror(errno));
		close(p[0]);
		close(p[1]);
		return (-1);
	}
	if (pid == 0) {
		proc_clear_signals(server_proc, 1);
		sigprocmask(SIG_SETMASK, &oldset, NULL);
		close(p[0]);
		if (dup2(checkpoint_fd, STDIN_FILENO) == -1 ||
		    dup2(p[1], STDERR_FILENO) == -1)
			_exit(127);
		devnull = open(_PATH_DEVNULL, O_RDWR);
		if (devnull != -1) {
			dup2(devnull, STDOUT_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
		closefrom(STDERR_FILENO + 1);
		setenv(RESTART_EXEC_CHECK, "1", 1);
		execl(path, path, "-S", _PATH_DEVNULL, "has-session",
		    (char *)NULL);
		fprintf(stderr, "%s\n", strerror(errno));
		_exit(127);
	}
	sigprocmask(SIG_SETMASK, &oldset, NULL);
	close(p[1]);

	/* Keep the start of the explanation but read to the end of it. */
	while ((n = read(p[0], chunk, sizeof chunk)) != 0) {
		if (n == -1) {
			if (errno == EINTR)
				continue;
			break;
		}
		if ((size_t)n > sizeof buf - 1 - len)
			n = sizeof buf - 1 - len;
		memcpy(buf + len, chunk, n);
		len += n;
	}
	close(p[0]);
	buf[len] = '\0';
	buf[strcspn(buf, "\n")] = '\0';

	while ((got = waitpid(pid, &status, 0)) == -1 && errno == EINTR)
		continue;
	if (got != pid) {
		xasprintf(cause, "cannot check %s: %s", path, strerror(errno));
		return (-1);
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return (0);

	if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
		what = "did not start";
	else
		what = "cannot restore this server";
	if (*buf != '\0')
		xasprintf(cause, "%s %s: %s", path, what, buf);
	else
		xasprintf(cause, "%s %s", path, what);
	return (-1);
}

static enum restart_activation_type
restart_exec_activation_type(const struct restart_activation *a)
{
	const struct restart_exec_activation	*ea = (const void *)a;

	if (a == NULL)
		return (RESTART_ACTIVATION_NONE);
	return (ea->type);
}

static int
restart_exec_activation_state_fd(const struct restart_activation *a)
{
	const struct restart_exec_activation	*ea = (const void *)a;

	if (a == NULL)
		return (-1);
	return (ea->state_fd);
}

static size_t
restart_exec_activation_pane_count(const struct restart_activation *a)
{
	const struct restart_exec_activation	*ea = (const void *)a;

	if (a == NULL)
		return (0);
	return (ea->pane_count);
}

static int
restart_exec_activation_pane_at(const struct restart_activation *a, size_t i,
    struct restart_fd *out)
{
	const struct restart_exec_activation	*ea = (const void *)a;

	if (a == NULL || i >= ea->pane_count)
		return (-1);
	*out = ea->panes[i];
	return (0);
}

/*
 * Find the pty master for a pane. Both the identifier and the process are
 * matched so a checkpoint that no longer describes these descriptors cannot
 * attach a pane to the wrong terminal.
 */
static int
restart_exec_activation_lookup(void *arg, u_int pane_id, pid_t pid, int *fd)
{
	struct restart_exec_activation	*a = arg;
	size_t				 i;

	if (a == NULL)
		return (-1);
	for (i = 0; i < a->pane_count; i++) {
		if (a->panes[i].pane_id != pane_id || a->panes[i].pid != pid)
			continue;
		*fd = a->panes[i].fd;
		return (0);
	}
	return (-1);
}

/*
 * Nothing to remove: this transport leaves no descriptors anywhere but in the
 * process, which is about to be replaced or to carry on.
 */
static enum restart_store_result
restart_exec_activation_remove(__unused const struct restart_activation *a,
    __unused char **cause)
{
	return (RESTART_STORE_OK);
}

static void
restart_exec_activation_close(struct restart_activation *a)
{
	struct restart_exec_activation	*ea = (void *)a;
	size_t				 i;

	if (a == NULL)
		return;
	if (ea->state_fd != -1) {
		close(ea->state_fd);
		ea->state_fd = -1;
	}
	for (i = 0; i < ea->pane_count; i++) {
		if (ea->panes[i].fd != -1) {
			close(ea->panes[i].fd);
			ea->panes[i].fd = -1;
		}
	}
	unsetenv(RESTART_EXEC_LISTEN);
	unsetenv(RESTART_EXEC_STATE);
	unsetenv(RESTART_EXEC_PANES);
}

/* The descriptors are open in this process, so there is nothing to prepare. */
static int
restart_exec_prepare(__unused struct restart_activation *a,
    __unused char **cause)
{
	return (0);
}

static int
restart_exec_preflight(__unused const struct restart_activation *a,
    __unused uint64_t flags, __unused const struct restart_fd *fds,
    __unused size_t count, char **cause)
{
	if (restart_exec_listen_fd == -1) {
		xasprintf(cause,
		    "this server was not started by the restart transport");
		return (-1);
	}
	if (restart_exec_path == NULL) {
		xasprintf(cause, "cannot find the server image to restart");
		return (-1);
	}
	if (socket_path == NULL) {
		xasprintf(cause, "cannot find the server socket to restart");
		return (-1);
	}
	return (0);
}

/*
 * Create the file the state is written to: a temporary file beside the socket,
 * unlinked as soon as it exists, so the state is briefly nameable on disk but
 * never readable by another user. It is carried by descriptor and never by
 * path.
 */
int
restart_checkpoint_create(char **cause)
{
	int	 fd;
	char	*path;

	if (socket_path != NULL && strchr(socket_path, '/') != NULL)
		xasprintf(&path, "%s.restart.XXXXXX", socket_path);
	else
		path = xstrdup(_PATH_TMP "tmux-restart.XXXXXX");
	fd = mkstemp(path);
	if (fd == -1) {
		xasprintf(cause, "cannot create restart checkpoint: %s",
		    strerror(errno));
		free(path);
		return (-1);
	}
	unlink(path);
	free(path);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return (fd);
}

/*
 * Write the state and rewind. Only this process and its children hold the
 * descriptor, and the decoder checks what it reads, so nothing more is needed
 * to trust it.
 */
int
restart_checkpoint_write(int fd, const struct ibuf *buf, char **cause)
{
	const u_char	*data = ibuf_data((struct ibuf *)buf);
	size_t		 left = ibuf_size((struct ibuf *)buf);
	ssize_t		 n;

	while (left != 0) {
		n = write(fd, data, left);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			xasprintf(cause, "cannot write restart checkpoint: %s",
			    strerror(errno));
			return (-1);
		}
		data += n;
		left -= n;
	}

	if (lseek(fd, 0, SEEK_SET) == -1) {
		xasprintf(cause, "cannot rewind restart checkpoint: %s",
		    strerror(errno));
		return (-1);
	}
	return (0);
}

int
restart_checkpoint_read(int fd, void **out, size_t *outlen, char **cause)
{
	struct stat	 sb;
	u_char		*buf;
	size_t		 size, left;
	off_t		 off;
	ssize_t		 n;

	*out = NULL;
	*outlen = 0;

	if (fstat(fd, &sb) == -1) {
		xasprintf(cause, "cannot stat restart checkpoint: %s",
		    strerror(errno));
		return (-1);
	}
	if (!S_ISREG(sb.st_mode)) {
		xasprintf(cause, "restart checkpoint is not a regular file");
		return (-1);
	}
	if (sb.st_size <= 0) {
		xasprintf(cause, "restart checkpoint is empty");
		return (-1);
	}
	/*
	 * The decoder refuses anything over this too. Checking here as well
	 * means an implausible size is refused before it is allocated.
	 */
	if ((uintmax_t)sb.st_size > (uintmax_t)restart_codec_max_size()) {
		xasprintf(cause, "restart checkpoint is %ju bytes, over the "
		    "%zu byte limit", (uintmax_t)sb.st_size,
		    restart_codec_max_size());
		return (-1);
	}

	size = (size_t)sb.st_size;
	buf = xmalloc(size);
	off = 0;
	left = size;
	while (left != 0) {
		n = pread(fd, buf + off, left, off);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			xasprintf(cause, "cannot read restart checkpoint: %s",
			    strerror(errno));
			free(buf);
			return (-1);
		}
		if (n == 0) {
			xasprintf(cause, "restart checkpoint ended after "
			    "%ju of %zu bytes", (uintmax_t)off, size);
			free(buf);
			return (-1);
		}
		off += n;
		left -= n;
	}

	*out = buf;
	*outlen = size;
	return (0);
}

/*
 * Check the new image can read the checkpoint, then keep the checkpoint and the
 * descriptors until the exec. The checkpoint is duplicated because the caller
 * closes its own copy once the state is stored.
 */
static enum restart_store_result
restart_exec_store(int checkpoint_fd, const struct restart_fd *fds,
    size_t count, char **cause)
{
	int	fd;

	if (restart_exec_probe(restart_exec_path, checkpoint_fd, cause) != 0)
		return (RESTART_STORE_ERROR_CLEAN);

	fd = dup(checkpoint_fd);
	if (fd == -1) {
		xasprintf(cause, "cannot keep restart checkpoint: %s",
		    strerror(errno));
		return (RESTART_STORE_ERROR_CLEAN);
	}
	restart_exec_checkpoint_fd = fd;
	if (count != 0) {
		restart_exec_vector = xcalloc(count, sizeof *fds);
		memcpy(restart_exec_vector, fds, count * sizeof *fds);
	}
	restart_exec_vector_count = count;
	restart_exec_armed = 1;
	return (RESTART_STORE_OK);
}

static void
restart_exec_disarm(void)
{
	restart_exec_armed = 0;
	if (restart_exec_checkpoint_fd != -1) {
		close(restart_exec_checkpoint_fd);
		restart_exec_checkpoint_fd = -1;
	}
	free(restart_exec_vector);
	restart_exec_vector = NULL;
	restart_exec_vector_count = 0;
}

static enum restart_store_result
restart_exec_remove(__unused const struct restart_fd *fds,
    __unused size_t count, __unused char **cause)
{
	restart_exec_disarm();
	return (RESTART_STORE_OK);
}

/*
 * The replacement is not started here but after the clients have gone, so
 * there is no state in which the request has been half delivered and this
 * never reports RESTART_DISPATCH_UNKNOWN.
 */
static enum restart_dispatch_result
restart_exec_restart(char **cause)
{
	if (!restart_exec_armed) {
		xasprintf(cause, "no restart is armed");
		return (RESTART_DISPATCH_NOT_SENT);
	}
	return (RESTART_DISPATCH_QUEUED);
}

/*
 * A service manager has to be asked whether it is able to restart this unit at
 * all, and again once it is listening. Executing an image needs neither.
 */
static int
restart_exec_baseline_ready(__unused char **cause)
{
	return (0);
}

static int
restart_exec_ready(__unused char **cause)
{
	return (0);
}

int
restart_exec_activated(void)
{
	return (getenv(RESTART_EXEC_LISTEN) != NULL);
}

int
restart_exec_checking(void)
{
	return (getenv(RESTART_EXEC_CHECK) != NULL);
}

/*
 * Return the socket the server should listen on, and describe how it was
 * started. A replacement inherits the socket already bound, so the path is
 * never unlinked and rebound and the backlog is never dropped; a server
 * started any other way binds it as usual.
 */
int
restart_exec_create_socket(uint64_t flags, struct restart_activation **out,
    char **cause)
{
	struct restart_exec_activation	*a;
	const char			*listen, *state, *panes;

	a = xcalloc(1, sizeof *a);
	a->type = RESTART_ACTIVATION_NONE;
	a->listen_fd = -1;
	a->state_fd = -1;
	*out = (struct restart_activation *)a;

	if (restart_exec_path == NULL && tmux_path != NULL)
		restart_exec_path = xstrdup(tmux_path);

	listen = getenv(RESTART_EXEC_LISTEN);
	if (listen == NULL) {
		restart_exec_listen_fd = server_create_socket(flags, cause);
		return (restart_exec_listen_fd);
	}

	state = getenv(RESTART_EXEC_STATE);
	panes = getenv(RESTART_EXEC_PANES);
	if (state == NULL || panes == NULL) {
		xasprintf(cause, "restart handoff is incomplete");
		return (-1);
	}

	a->type = RESTART_ACTIVATION_RESTORE;
	a->listen_fd = atoi(listen);
	a->state_fd = atoi(state);
	if (restart_exec_parse_panes(panes, &a->panes, &a->pane_count,
	    cause) != 0)
		return (-1);

	log_debug("%s: inherited listen %d state %d, %zu panes", __func__,
	    a->listen_fd, a->state_fd, a->pane_count);
	restart_exec_listen_fd = a->listen_fd;
	return (a->listen_fd);
}

void
restart_exec_activation_free(struct restart_activation *a)
{
	struct restart_exec_activation	*ea = (void *)a;

	if (a == NULL)
		return;
	free(ea->panes);
	free(ea);
}

/*
 * Execute the replacement, after the clients have gone and in place of exit.
 * The log is closed first because log_open does not set close-on-exec and the
 * replacement opens its own. On success this does not return, and the pane
 * processes keep the same parent.
 *
 * If the exec fails this process still has everything, so it returns -1 and the
 * server carries on. The descriptors keep close-on-exec cleared, which is
 * harmless because every child the server starts closes them first.
 */
int
restart_exec_finish(void)
{
	char	*listen = NULL, *state = NULL, *panes = NULL;
	char	*old, *argv0, *argv[RESTART_EXEC_MAXARGV];
	size_t	 i;

	if (!restart_exec_armed)
		return (0);

	restart_exec_inherit(restart_exec_listen_fd);
	restart_exec_inherit(restart_exec_checkpoint_fd);

	panes = xstrdup("");
	for (i = 0; i < restart_exec_vector_count; i++) {
		restart_exec_inherit(restart_exec_vector[i].fd);
		old = panes;
		xasprintf(&panes, "%s%s%u:%lld:%d", old,
		    (i == 0 ? "" : ","), restart_exec_vector[i].pane_id,
		    (long long)restart_exec_vector[i].pid,
		    restart_exec_vector[i].fd);
		free(old);
	}

	xasprintf(&listen, "%d", restart_exec_listen_fd);
	xasprintf(&state, "%d", restart_exec_checkpoint_fd);
	setenv(RESTART_EXEC_LISTEN, listen, 1);
	setenv(RESTART_EXEC_STATE, state, 1);
	setenv(RESTART_EXEC_PANES, panes, 1);
	free(listen);
	free(state);

	argv0 = xstrdup(restart_exec_path);
	restart_exec_build_argv(argv, nitems(argv), argv0);

	log_debug("%s: exec %s, listen %d state %d panes %s", __func__,
	    restart_exec_path, restart_exec_listen_fd,
	    restart_exec_checkpoint_fd, panes);
	free(panes);
	log_close();
	execv(restart_exec_path, argv);
	log_open("server");
	log_debug("%s: exec %s failed: %s", __func__, restart_exec_path,
	    strerror(errno));
	free(argv0);

	unsetenv(RESTART_EXEC_LISTEN);
	unsetenv(RESTART_EXEC_STATE);
	unsetenv(RESTART_EXEC_PANES);
	restart_exec_disarm();
	return (-1);
}

/*
 * Run instead of a client or server when this image is being checked by a
 * server about to restart into it: read the checkpoint on standard input as a
 * replacement would and report whether it can be restored.
 */
int
restart_exec_check(void)
{
	struct restart_state	*state = NULL;
	void			*bytes = NULL;
	size_t			 len;
	char			*cause = NULL;

	if (restart_checkpoint_read(STDIN_FILENO, &bytes, &len, &cause) != 0 ||
	    restart_state_decode(bytes, len, &state, &cause) != 0) {
		fprintf(stderr, "%s\n", cause);
		return (1);
	}
	restart_state_free(state);
	free(bytes);
	return (0);
}
