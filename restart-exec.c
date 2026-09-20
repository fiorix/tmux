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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_MEMFD_CREATE
#include <sys/mman.h>
#endif

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
 */

/* Names the replacement reads its inherited descriptors from. */
#define RESTART_EXEC_LISTEN "TMUX_RESTART_LISTEN"
#define RESTART_EXEC_STATE "TMUX_RESTART_STATE"
#define RESTART_EXEC_PANES "TMUX_RESTART_PANES"
#define RESTART_EXEC_IMAGE "TMUX_RESTART_IMAGE"

#ifdef HAVE_MEMFD_CREATE
#define RESTART_EXEC_SEALS \
	(F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE)
#endif

/* The longest argument vector build_argv can produce, including the NULL. */
#define RESTART_EXEC_MAXARGV 16

struct restart_exec_activation {
	enum restart_activation_type	 type;
	int				 listen_fd;
	int				 state_fd;
	int				 image_fd;
	struct restart_fd		*panes;
	size_t				 pane_count;
};

static char			*restart_exec_path;
static int			 restart_exec_listen_fd = -1;
static int			 restart_exec_armed;
static int			 restart_exec_checkpoint_fd = -1;
static int			 restart_exec_image_fd = -1;
static struct restart_fd	*restart_exec_vector;
static size_t			 restart_exec_vector_count;

static void	 restart_exec_inherit(int);
static int	 restart_exec_parse_panes(const char *, struct restart_fd **,
		     size_t *, char **);
static size_t	 restart_exec_build_argv(char **, size_t, char *);
static int	 restart_exec_probe(const char *, char **);
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
	.checkpoint_seal = restart_checkpoint_seal,
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
 * Run the candidate image once as a child, so a replacement that cannot start
 * at all is found while this server can still refuse the restart. It says
 * nothing about whether the replacement can read the checkpoint; that is what
 * the fallback is for.
 */
static int
restart_exec_probe(const char *path, char **cause)
{
	pid_t	pid, got;
	int	status, devnull;

	pid = fork();
	if (pid == -1) {
		xasprintf(cause, "cannot probe %s: %s", path, strerror(errno));
		return (-1);
	}
	if (pid == 0) {
		devnull = open(_PATH_DEVNULL, O_RDWR);
		if (devnull != -1) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
		closefrom(STDERR_FILENO + 1);
		execl(path, "tmux", "-V", (char *)NULL);
		_exit(127);
	}
	while ((got = waitpid(pid, &status, 0)) == -1 && errno == EINTR)
		continue;
	if (got != pid) {
		xasprintf(cause, "cannot probe %s: %s", path, strerror(errno));
		return (-1);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		xasprintf(cause, "%s did not start", path);
		return (-1);
	}
	return (0);
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
	if (ea->image_fd != -1) {
		close(ea->image_fd);
		ea->image_fd = -1;
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
	unsetenv(RESTART_EXEC_IMAGE);
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
#ifdef HAVE_FEXECVE
	if (restart_exec_probe(restart_exec_path, cause) != 0)
		return (-1);
	if (restart_exec_image_fd == -1) {
		xasprintf(cause, "cannot keep the running image for recovery");
		return (-1);
	}
#else
	xasprintf(cause, "restart recovery requires fexecve");
	return (-1);
#endif
	return (0);
}

/*
 * Create the file the state is written to. On Linux this is an anonymous
 * descriptor that can be sealed; elsewhere it is a temporary file, unlinked as
 * soon as it exists, so the state is briefly nameable on disk but never
 * readable by another user. Either way it is carried by descriptor and never
 * by path.
 */
int
restart_checkpoint_create(char **cause)
{
	int	 fd;
#ifndef HAVE_MEMFD_CREATE
	char	*path;
#endif

#ifdef HAVE_MEMFD_CREATE
	fd = memfd_create("tmux-restart-state", MFD_CLOEXEC|MFD_ALLOW_SEALING);
	if (fd == -1) {
		xasprintf(cause, "cannot create restart checkpoint: %s",
		    strerror(errno));
		return (-1);
	}
#else
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
#endif
	return (fd);
}

/*
 * Write the state and make the checkpoint read-only, so that a descriptor the
 * replacement inherits cannot be changed after this server has approved it.
 * Sealing needs memfd_create; without it the guarantee is weaker, and the
 * decoder's own checks are what catch a damaged checkpoint.
 */
int
restart_checkpoint_seal(int fd, const struct ibuf *buf, char **cause)
{
	const u_char	*data = ibuf_data((struct ibuf *)buf);
	size_t		 left = ibuf_size((struct ibuf *)buf);
	ssize_t		 n;
#ifdef HAVE_MEMFD_CREATE
	int		 seals;
#endif

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

#ifdef HAVE_MEMFD_CREATE
	if (fcntl(fd, F_ADD_SEALS, RESTART_EXEC_SEALS) == -1) {
		xasprintf(cause, "cannot seal restart checkpoint: %s",
		    strerror(errno));
		return (-1);
	}
	seals = fcntl(fd, F_GET_SEALS);
	if (seals == -1) {
		xasprintf(cause, "cannot read restart checkpoint seals: %s",
		    strerror(errno));
		return (-1);
	}
	if ((seals & RESTART_EXEC_SEALS) != RESTART_EXEC_SEALS) {
		xasprintf(cause, "restart checkpoint is not fully sealed");
		return (-1);
	}
#endif
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
#ifdef HAVE_MEMFD_CREATE
	int		 seals;
#endif

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

#ifdef HAVE_MEMFD_CREATE
	seals = fcntl(fd, F_GET_SEALS);
	if (seals == -1) {
		xasprintf(cause, "cannot read restart checkpoint seals: %s",
		    strerror(errno));
		return (-1);
	}
	if ((seals & RESTART_EXEC_SEALS) != RESTART_EXEC_SEALS) {
		xasprintf(cause, "restart checkpoint is not fully sealed");
		return (-1);
	}
#endif

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
 * Keep the checkpoint and the descriptors until the exec. The checkpoint is
 * duplicated because the caller closes its own copy once the state is stored.
 */
static enum restart_store_result
restart_exec_store(int checkpoint_fd, const struct restart_fd *fds,
    size_t count, char **cause)
{
	int	fd;

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
	const char			*listen, *state, *panes, *image;

	a = xcalloc(1, sizeof *a);
	a->type = RESTART_ACTIVATION_NONE;
	a->listen_fd = -1;
	a->state_fd = -1;
	a->image_fd = -1;
	*out = (struct restart_activation *)a;

	if (restart_exec_path == NULL && tmux_path != NULL)
		restart_exec_path = xstrdup(tmux_path);
#ifdef HAVE_FEXECVE
	if (restart_exec_image_fd == -1 && restart_exec_path != NULL)
		restart_exec_image_fd = open(restart_exec_path, O_RDONLY);
#endif

	listen = getenv(RESTART_EXEC_LISTEN);
	if (listen == NULL) {
		restart_exec_listen_fd = server_create_socket(flags, cause);
		return (restart_exec_listen_fd);
	}

	state = getenv(RESTART_EXEC_STATE);
	panes = getenv(RESTART_EXEC_PANES);
	image = getenv(RESTART_EXEC_IMAGE);
	if (state == NULL || panes == NULL) {
		xasprintf(cause, "restart handoff is incomplete");
		return (-1);
	}

	a->type = RESTART_ACTIVATION_RESTORE;
	a->listen_fd = atoi(listen);
	a->state_fd = atoi(state);
	a->image_fd = (image == NULL ? -1 : atoi(image));
	if (restart_exec_parse_panes(panes, &a->panes, &a->pane_count,
	    cause) != 0)
		return (-1);

	log_debug("%s: inherited listen %d state %d image %d, %zu panes",
	    __func__, a->listen_fd, a->state_fd, a->image_fd, a->pane_count);
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

#ifdef HAVE_FEXECVE
/*
 * Return to the image that handed the descriptors over, when the replacement
 * has them but cannot use them. The predecessor is executed from the
 * descriptor it left open, so this works even if the path has since been
 * replaced, which is the case an upgrade creates. On success it does not
 * return.
 */
void
restart_exec_fallback(struct restart_activation *a)
{
	struct restart_exec_activation	*ea = (void *)a;
	char	*argv0, *argv[RESTART_EXEC_MAXARGV];
	size_t	 i;

	if (a == NULL || ea->image_fd == -1)
		return;

	unsetenv(RESTART_EXEC_IMAGE);
	restart_exec_inherit(ea->listen_fd);
	restart_exec_inherit(ea->state_fd);
	for (i = 0; i < ea->pane_count; i++)
		restart_exec_inherit(ea->panes[i].fd);

	argv0 = xstrdup(restart_exec_path != NULL ? restart_exec_path :
	    getprogname());
	restart_exec_build_argv(argv, nitems(argv), argv0);

	log_debug("%s: returning to the previous image", __func__);
	log_close();
	fexecve(ea->image_fd, argv, environ);
	log_open("server");
	log_debug("%s: fexecve failed: %s", __func__, strerror(errno));
	free(argv0);
}
#else
void
restart_exec_fallback(__unused struct restart_activation *a)
{
}
#endif

/*
 * Execute the replacement, after the clients have gone and in place of exit.
 * The log is closed first because log_open does not set close-on-exec and the
 * replacement opens its own. On success this does not return, and the pane
 * processes keep the same parent.
 */
void
restart_exec_finish(void)
{
	char	*listen = NULL, *state = NULL, *panes = NULL, *image = NULL;
	char	*old, *argv0, *argv[RESTART_EXEC_MAXARGV];
	size_t	 i;

	if (!restart_exec_armed)
		return;

	restart_exec_inherit(restart_exec_listen_fd);
	restart_exec_inherit(restart_exec_checkpoint_fd);
	restart_exec_inherit(restart_exec_image_fd);

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
	xasprintf(&image, "%d", restart_exec_image_fd);
	setenv(RESTART_EXEC_LISTEN, listen, 1);
	setenv(RESTART_EXEC_STATE, state, 1);
	setenv(RESTART_EXEC_PANES, panes, 1);
	setenv(RESTART_EXEC_IMAGE, image, 1);
	free(listen);
	free(state);
	free(image);

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
}
