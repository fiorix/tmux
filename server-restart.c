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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"
#include "restart-codec.h"
#include "restart-exec.h"
#include "server-restart-private.h"

/*
 * Drive a restart as a transaction. The server stops reading, writes its state
 * to a checkpoint the transport provides, hands the checkpoint and the pane
 * descriptors to the transport, and only then tells the clients. Everything
 * before the handover can be undone; nothing after it can.
 *
 * The transport is reached through an operation table so that this file has no
 * opinion about how the descriptors travel.
 */

static struct server_restart_context server_restart_ctx = {
	.phase = SERVER_RESTART_NORMAL,
	.activation = NULL
};

static const struct server_restart_ops	*server_restart_ops;

static void	server_restart_init_ops(void);
static short	server_restart_pane_events_get(const struct window_pane *);
static void	server_restart_pane_events_set(struct window_pane *, short);
static void	server_restart_recovery_wait(void);
static int	server_restart_key_compare(u_int, pid_t, u_int, pid_t);
static int	server_restart_pane_compare(const void *, const void *);
static int	server_restart_fd_compare(const void *, const void *);
static int	server_restart_preflight(struct server_restart_transaction *,
		    char **);
static void	server_restart_quiesce(struct server_restart_transaction *);
static int	server_restart_state_keys(const struct restart_state *,
		    struct restart_fd **, size_t *, char **);
static int	server_restart_compare_keys(const struct restart_fd *, size_t,
		    const struct restart_fd *, size_t, const char *, char **);
static int	server_restart_verify_encoded(
		    struct server_restart_transaction *, char **);
static int	server_restart_compare_descriptors(const struct restart_state *,
		    const struct restart_activation *, char **);
static int	server_restart_enable_panes(char **);
static int	server_restart_recover_published(
		    enum server_restart_restore_stage,
		    struct restart_activation *, char **);
static enum restart_store_result server_restart_handoff(
		    struct server_restart_transaction *, char **);
static enum restart_store_result server_restart_remove_handoff(
		    struct server_restart_transaction *, char **);
static enum restart_dispatch_result server_restart_dispatch(char **);
static void	server_restart_restore_io(struct server_restart_transaction *);
static void	server_restart_rollback(struct server_restart_transaction *,
		    int);
static void	server_restart_wake(int, short, void *);
static void	server_restart_commit(struct server_restart_transaction *, int,
		    const char *);
static void	server_restart_transaction_free(
		    struct server_restart_transaction *);
static int	server_restart_unknown_exit_status(void);

/*
 * The one place that names a transport. A second transport would choose here
 * rather than anywhere else in this file.
 */
static void
server_restart_init_ops(void)
{
	if (server_restart_ops == NULL)
		server_restart_ops = restart_exec_get_ops();
}

static short
server_restart_pane_events_get(const struct window_pane *wp)
{
	if (wp->event == NULL)
		return (0);
	return (bufferevent_get_enabled(wp->event));
}

static void
server_restart_pane_events_set(struct window_pane *wp, short events)
{
	if (wp->event == NULL)
		return;
	bufferevent_disable(wp->event, EV_READ|EV_WRITE);
	if (events != 0)
		bufferevent_enable(wp->event, events);
}

static void
server_restart_recovery_wait(void)
{
	struct timespec	ts = { 1, 0 };

	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		continue;
}

/*
 * A pane is named by its identifier and the process on the other end of it.
 * Sorting by both gives the two walks a common order so they can be compared.
 */
static int
server_restart_key_compare(u_int aid, pid_t apid, u_int bid, pid_t bpid)
{
	if (aid != bid)
		return (aid < bid ? -1 : 1);
	if (apid != bpid)
		return (apid < bpid ? -1 : 1);
	return (0);
}

static int
server_restart_pane_compare(const void *aa, const void *bb)
{
	const struct server_restart_pane	*a = aa;
	const struct server_restart_pane	*b = bb;

	return (server_restart_key_compare(a->handoff.pane_id, a->handoff.pid,
	    b->handoff.pane_id, b->handoff.pid));
}

static int
server_restart_fd_compare(const void *aa, const void *bb)
{
	const struct restart_fd	*a = aa;
	const struct restart_fd	*b = bb;

	return (server_restart_key_compare(a->pane_id, a->pid, b->pane_id,
	    b->pid));
}

static int
server_restart_preflight(struct server_restart_transaction *tr, char **cause)
{
	struct session		*s;
	struct winlink		*wl;
	struct window		*w;
	struct window_pane	*wp;
	struct client		*c;
	size_t			 i, total = 0;

	if (server_restart_ctx.phase != SERVER_RESTART_NORMAL) {
		xasprintf(cause, "a server restart is already in progress");
		return (-1);
	}
	if (server_restart_ctx.dirty) {
		xasprintf(cause, "an earlier restart left the descriptors "
		    "in an unknown state");
		return (-1);
	}
	if (!cfg_finished) {
		xasprintf(cause, "the configuration has not finished loading");
		return (-1);
	}

	/*
	 * A window kept alive by a reference other than a winlink is absent
	 * from the encoded state, so handing over its descriptors would leave
	 * the replacement with more received fds than the state describes.
	 * Panes in such a window are not preserved across the restart.
	 */
	RB_FOREACH(s, sessions, &sessions) {
		RB_FOREACH(wl, winlinks, &s->windows) {
			TAILQ_FOREACH(wp, &wl->window->panes, entry) {
				if (wp->fd != -1)
					total++;
			}
		}
	}
	TAILQ_FOREACH(c, &clients, entry)
		tr->client_count++;

	if (total != 0) {
		tr->panes = xcalloc(total, sizeof *tr->panes);
		tr->vector = xcalloc(total, sizeof *tr->vector);
	}
	if (tr->client_count != 0)
		tr->clients = xcalloc(tr->client_count, sizeof *tr->clients);

	RB_FOREACH(s, sessions, &sessions) {
		RB_FOREACH(wl, winlinks, &s->windows) {
			w = wl->window;
			TAILQ_FOREACH(wp, &w->panes, entry) {
				if (wp->fd == -1)
					continue;
				for (i = 0; i < tr->pane_count; i++) {
					if (tr->panes[i].wp == wp)
						break;
				}
				if (i != tr->pane_count)
					continue;
				i = tr->pane_count++;
				tr->panes[i].wp = wp;
				tr->panes[i].handoff.pane_id = wp->id;
				tr->panes[i].handoff.pid = wp->pid;
				tr->panes[i].handoff.fd = wp->fd;
			}
		}
	}
	if (tr->pane_count != 0) {
		qsort(tr->panes, tr->pane_count, sizeof *tr->panes,
		    server_restart_pane_compare);
		for (i = 0; i < tr->pane_count; i++)
			tr->vector[i] = tr->panes[i].handoff;
	}

	i = 0;
	TAILQ_FOREACH(c, &clients, entry)
		tr->clients[i++].client = c;

	if (server_restart_ops->preflight(server_restart_ctx.activation,
	    server_restart_ctx.flags, tr->vector, tr->pane_count, cause) != 0)
		return (-1);

	tr->checkpoint_fd = server_restart_ops->checkpoint_create(cause);
	if (tr->checkpoint_fd == -1)
		return (-1);
	return (0);
}

/*
 * Stop reading from the panes and the clients, remembering what each was doing
 * so that a refused restart can put it back exactly.
 */
static void
server_restart_quiesce(struct server_restart_transaction *tr)
{
	struct server_restart_client	*rc;
	struct server_restart_pane	*rp;
	size_t				 i;

	server_restart_ctx.phase = SERVER_RESTART_QUIESCED;
	server_remove_accept();

	for (i = 0; i < tr->client_count; i++) {
		rc = &tr->clients[i];
		rc->read_enabled = proc_peer_read_enabled(rc->client->peer);
		proc_set_peer_read(rc->client->peer, 0);
	}
	for (i = 0; i < tr->pane_count; i++) {
		rp = &tr->panes[i];
		rp->events = server_restart_pane_events_get(rp->wp);
		server_restart_pane_events_set(rp->wp, 0);
	}
}

/*
 * The handover set and the encoded set are derived by separate walks, and past
 * this point a disagreement between them is the replacement's to discover with
 * the old server already gone.
 */
static int
server_restart_state_keys(const struct restart_state *state,
    struct restart_fd **out, size_t *outlen, char **cause)
{
	struct restart_fd	*keys;
	size_t			 count, i;
	u_int			 pane_id;
	pid_t			 pid;

	*out = NULL;
	*outlen = count = restart_state_descriptor_count(state);
	if (count == 0)
		return (0);

	keys = xcalloc(count, sizeof *keys);
	for (i = 0; i < count; i++) {
		if (restart_state_descriptor_at(state, i, &pane_id,
		    &pid) != 0) {
			xasprintf(cause, "cannot read restart state descriptor "
			    "%zu", i);
			free(keys);
			return (-1);
		}
		keys[i].pane_id = pane_id;
		keys[i].pid = pid;
		keys[i].fd = -1;
	}
	qsort(keys, count, sizeof *keys, server_restart_fd_compare);
	*out = keys;
	return (0);
}

static int
server_restart_compare_keys(const struct restart_fd *want, size_t want_count,
    const struct restart_fd *got, size_t got_count, const char *what,
    char **cause)
{
	size_t	i;

	if (want_count != got_count) {
		xasprintf(cause, "restart state describes %zu descriptors but "
		    "%zu %s", want_count, got_count, what);
		return (-1);
	}
	for (i = 0; i < want_count; i++) {
		if (want[i].pane_id == got[i].pane_id &&
		    want[i].pid == got[i].pid)
			continue;
		xasprintf(cause, "restart state describes pane %%%u process "
		    "%ld where pane %%%u process %ld %s", want[i].pane_id,
		    (long)want[i].pid, got[i].pane_id, (long)got[i].pid, what);
		return (-1);
	}
	return (0);
}

/*
 * Decode the state that was just encoded and check it names the same panes
 * that are about to be handed over. This is the last point at which the two
 * can be compared by the server that still owns both.
 */
static int
server_restart_verify_encoded(struct server_restart_transaction *tr,
    char **cause)
{
	struct restart_state	*state = NULL;
	struct restart_fd	*keys = NULL;
	size_t			 count;
	int			 retval = -1;

	if (restart_state_decode(ibuf_data(tr->encoded), ibuf_size(tr->encoded),
	    &state, cause) != 0)
		return (-1);

	if (server_restart_state_keys(state, &keys, &count, cause) == 0) {
		retval = server_restart_compare_keys(keys, count, tr->vector,
		    tr->pane_count, "are being handed over", cause);
	}

	free(keys);
	restart_state_free(state);
	return (retval);
}

static int
server_restart_compare_descriptors(const struct restart_state *state,
    const struct restart_activation *activation, char **cause)
{
	struct restart_fd	*keys = NULL, *got = NULL;
	size_t			 count, n, i;
	int			 retval = -1;

	if (server_restart_state_keys(state, &keys, &count, cause) != 0)
		return (-1);

	n = server_restart_ops->activation_pane_count(activation);
	if (n != 0) {
		got = xcalloc(n, sizeof *got);
		for (i = 0; i < n; i++) {
			if (server_restart_ops->activation_pane_at(activation,
			    i, &got[i]) != 0) {
				xasprintf(cause, "cannot read received "
				    "descriptor %zu", i);
				goto out;
			}
		}
		qsort(got, n, sizeof *got, server_restart_fd_compare);
	}
	retval = server_restart_compare_keys(keys, count, got, n,
	    "were received", cause);

out:
	free(got);
	free(keys);
	return (retval);
}

static int
server_restart_enable_panes(char **cause)
{
	struct window		*w;
	struct window_pane	*wp;

	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->fd == -1 || wp->event == NULL)
				continue;
			if (window_pane_restart_enable_event(wp, cause) != 0)
				return (-1);
		}
	}
	return (0);
}

/*
 * Retries the failed stage and then continues the sequence rather than
 * returning, because every later stage is still outstanding. No caller can
 * observe an intermediate state: accept is unarmed and the event base has not
 * been dispatched.
 */
static int
server_restart_recover_published(enum server_restart_restore_stage stage,
    struct restart_activation *activation, char **cause)
{
	for (;;) {
		if (*cause != NULL) {
			log_debug("restart recovery: %s", *cause);
			free(*cause);
			*cause = NULL;
		}

		switch (stage) {
		case SERVER_RESTART_RESTORE_PUBLISHED:
			if (server_restart_ops->activation_remove(activation,
			    cause) == RESTART_STORE_OK) {
				server_restart_ops->activation_close(
				    activation);
				stage = SERVER_RESTART_RESTORE_CLEAN;
				continue;
			}
			break;
		case SERVER_RESTART_RESTORE_CLEAN:
			if (server_restart_enable_panes(cause) == 0) {
				stage = SERVER_RESTART_RESTORE_ENABLED;
				continue;
			}
			break;
		case SERVER_RESTART_RESTORE_ENABLED:
			if (server_restart_ops->ready(cause) == 0)
				return (0);
			break;
		case SERVER_RESTART_RESTORE_DETACHED:
		case SERVER_RESTART_RESTORE_READY:
			return (0);
		}
		server_restart_recovery_wait();
	}
}

/*
 * The store and remove helpers hand over the whole batch rather than one
 * descriptor at a time, because a transport with a limit applies it to the
 * total: a per-pane call would be under the limit every time and over it in
 * total.
 */
static enum restart_store_result
server_restart_handoff(struct server_restart_transaction *tr, char **cause)
{
	return (server_restart_ops->store(tr->checkpoint_fd, tr->vector,
	    tr->pane_count, cause));
}

static enum restart_store_result
server_restart_remove_handoff(struct server_restart_transaction *tr,
    char **cause)
{
	return (server_restart_ops->remove(tr->vector, tr->pane_count, cause));
}

static enum restart_dispatch_result
server_restart_dispatch(char **cause)
{
	return (server_restart_ops->restart(cause));
}

static void
server_restart_restore_io(struct server_restart_transaction *tr)
{
	size_t	i;

	for (i = 0; i < tr->pane_count; i++) {
		server_restart_pane_events_set(tr->panes[i].wp,
		    tr->panes[i].events);
	}
	for (i = 0; i < tr->client_count; i++) {
		proc_set_peer_read(tr->clients[i].client->peer,
		    tr->clients[i].read_enabled);
	}
	server_restart_ctx.phase = SERVER_RESTART_NORMAL;
	server_add_accept(0);
}

static void
server_restart_rollback(struct server_restart_transaction *tr, int dirty)
{
	if (tr->checkpoint_fd != -1) {
		close(tr->checkpoint_fd);
		tr->checkpoint_fd = -1;
	}
	server_restart_restore_io(tr);
	if (dirty)
		server_restart_ctx.dirty = 1;
}

/*
 * Commit marks the clients but sends nothing: the shutdown goes out from
 * server_client_exit_loop, reached only from the committed branch of
 * server_loop, which proc_loop runs after event_loop(EVLOOP_ONCE) returns.
 * With the panes quiesced and the client waiting for a reply there may be
 * nothing left to fire, and the process parks without releasing anybody.
 * This schedules the one pass that starts the drain; the client closing its
 * peer and the timer armed by check_exit provide the rest.
 *
 * Assigned once per process: preflight refuses any phase but NORMAL and commit
 * sets COMMITTED before reaching here, so this cannot reassign an event that
 * is already active. Relaxing that guard breaks this.
 */
static struct event	 server_restart_wake_ev;

static void
server_restart_wake(__unused int fd, __unused short events, __unused void *arg)
{
}

static void
server_restart_commit(struct server_restart_transaction *tr, int exit_status,
    const char *cause)
{
	struct window		*w;
	struct window_pane	*wp;
	struct client		*c, *c1;

	server_restart_ctx.phase = SERVER_RESTART_COMMITTED;
	server_restart_ctx.exit_status = exit_status;

	if (tr->checkpoint_fd != -1) {
		close(tr->checkpoint_fd);
		tr->checkpoint_fd = -1;
	}

	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			window_pane_wait_cancel(wp);
			spawn_editor_discard(wp);
			window_pane_close_pipe(wp);
		}
	}

	TAILQ_FOREACH_SAFE(c, &clients, entry, c1) {
		if (c->flags & CLIENT_SUSPENDED) {
			server_client_lost(c);
			continue;
		}
		c->flags |= CLIENT_EXIT;
		if (c->exit_type == CLIENT_EXIT_RETURN && c->retval != 0)
			;
		else if (exit_status == 0)
			c->exit_type = CLIENT_EXIT_SHUTDOWN;
		else {
			/*
			 * A shutdown message carries no payload, so a client
			 * released that way learns only that the server went
			 * away. The return path carries a status and a string,
			 * which is the same machinery an ordinary command error
			 * uses, so the reason reaches the person who asked.
			 *
			 * Every client gets it, including those that did not
			 * issue the restart: a bystander whose session vanished
			 * needs the explanation more than it needs a zero
			 * status.
			 */
			c->exit_type = CLIENT_EXIT_RETURN;
			c->retval = exit_status;
			free(c->exit_message);
			if (cause != NULL)
				c->exit_message = xstrdup(cause);
			else
				c->exit_message = NULL;
		}
		c->session = NULL;
		proc_set_peer_read(c->peer, 1);
	}

	evtimer_set(&server_restart_wake_ev, server_restart_wake, NULL);
	event_active(&server_restart_wake_ev, EV_TIMEOUT, 1);
}

static void
server_restart_transaction_free(struct server_restart_transaction *tr)
{
	if (tr->checkpoint_fd != -1) {
		close(tr->checkpoint_fd);
		tr->checkpoint_fd = -1;
	}
	ibuf_free(tr->encoded);
	tr->encoded = NULL;
	free(tr->panes);
	tr->panes = NULL;
	free(tr->vector);
	tr->vector = NULL;
	free(tr->clients);
	tr->clients = NULL;
	tr->pane_count = 0;
	tr->client_count = 0;
}

/*
 * A dispatch that may or may not have been delivered cannot be treated as
 * either, so the clients are released with a failure they can see.
 */
static int
server_restart_unknown_exit_status(void)
{
	return (1);
}

int
server_restart_is_quiesced(void)
{
	return (server_restart_ctx.phase == SERVER_RESTART_QUIESCED);
}

int
server_restart_is_committed(void)
{
	return (server_restart_ctx.phase == SERVER_RESTART_COMMITTED);
}

int
server_restart_exit_status(void)
{
	return (server_restart_ctx.exit_status);
}

/*
 * Called once at startup. A server that was not handed a checkpoint has
 * nothing to restore and returns at once; one that was rebuilds itself from it
 * before any client can connect.
 */
int
server_restart_restore(struct restart_activation *activation, uint64_t flags,
    char **cause)
{
	struct restart_state		*state = NULL;
	enum restart_activation_type	 type = RESTART_ACTIVATION_NONE;
	void				*bytes = NULL;
	size_t				 len = 0;
	char				*local = NULL;
	int				 fd;

	server_restart_init_ops();

	*cause = NULL;
	server_restart_ctx.activation = activation;
	server_restart_ctx.flags = flags;

	if (activation != NULL)
		type = server_restart_ops->activation_type(activation);

	if (type != RESTART_ACTIVATION_RESTORE) {
		if (type == RESTART_ACTIVATION_SOCKET &&
		    server_restart_ops->prepare(activation, &local) != 0) {
			if (local != NULL)
				log_debug("restart unavailable: %s", local);
			free(local);
			local = NULL;
		}
		if (server_restart_ops->baseline_ready(&local) != 0 &&
		    local != NULL)
			log_debug("baseline readiness: %s", local);
		free(local);
		return (0);
	}

	if (server_restart_ops->prepare(activation, cause) != 0)
		return (-1);

	if (!TAILQ_EMPTY(&clients) || !RB_EMPTY(&sessions) ||
	    !RB_EMPTY(&windows) || !RB_EMPTY(&all_window_panes)) {
		xasprintf(cause, "restart restore requires an empty server");
		return (-1);
	}

	fd = server_restart_ops->activation_state_fd(activation);
	if (fd == -1) {
		xasprintf(cause, "restart state descriptor was not received");
		return (-1);
	}
	if (server_restart_ops->checkpoint_read(fd, &bytes, &len, cause) != 0)
		return (-1);

	if (restart_state_decode(bytes, len, &state, cause) != 0 ||
	    server_restart_compare_descriptors(state, activation, cause) != 0 ||
	    restart_state_apply(state,
	    server_restart_ops->activation_lookup, activation, cause) != 0) {
		if (state != NULL)
			restart_state_free(state);
		free(bytes);
		return (-1);
	}
	restart_state_free(state);
	free(bytes);

	if (server_restart_ops->activation_remove(activation, cause) !=
	    RESTART_STORE_OK) {
		return (server_restart_recover_published(
		    SERVER_RESTART_RESTORE_PUBLISHED, activation, cause));
	}
	server_restart_ops->activation_close(activation);

	if (server_restart_enable_panes(cause) != 0) {
		return (server_restart_recover_published(
		    SERVER_RESTART_RESTORE_CLEAN, activation, cause));
	}

	if (server_restart_ops->ready(cause) != 0) {
		return (server_restart_recover_published(
		    SERVER_RESTART_RESTORE_ENABLED, activation, cause));
	}
	return (0);
}

/*
 * Ask for a restart. Returns -1 with a reason and the server untouched, or 0
 * with the clients released and the replacement due to run once they have
 * gone.
 */
int
server_restart_start(char **cause)
{
	struct server_restart_transaction	 tr;
	enum restart_store_result		 stored, removed;
	enum restart_dispatch_result		 dispatched;
	char					*dispatch_cause = NULL;
	char					*cleanup_cause = NULL;
	int					 dirty = 0;

	server_restart_init_ops();

	*cause = NULL;
	memset(&tr, 0, sizeof tr);
	tr.checkpoint_fd = -1;

	if (server_restart_preflight(&tr, cause) != 0) {
		server_restart_transaction_free(&tr);
		return (-1);
	}

	server_restart_quiesce(&tr);

	if (restart_state_encode(&tr.encoded, cause) != 0 ||
	    server_restart_verify_encoded(&tr, cause) != 0 ||
	    server_restart_ops->checkpoint_seal(tr.checkpoint_fd, tr.encoded,
	    cause) != 0)
		goto rollback;
	ibuf_free(tr.encoded);
	tr.encoded = NULL;

	stored = server_restart_handoff(&tr, cause);
	if (stored != RESTART_STORE_OK) {
		dirty = (stored != RESTART_STORE_ERROR_CLEAN);
		goto rollback;
	}

	dispatched = server_restart_dispatch(&dispatch_cause);
	if (dispatched == RESTART_DISPATCH_NOT_SENT) {
		removed = server_restart_remove_handoff(&tr, &cleanup_cause);
		if (removed == RESTART_STORE_OK) {
			free(cleanup_cause);
			*cause = dispatch_cause;
			goto rollback;
		}
		free(dispatch_cause);
		*cause = cleanup_cause;
		dirty = 1;
		goto rollback;
	}

	if (dispatched == RESTART_DISPATCH_QUEUED)
		server_restart_commit(&tr, 0, NULL);
	else
		server_restart_commit(&tr,
		    server_restart_unknown_exit_status(), dispatch_cause);
	free(dispatch_cause);
	server_restart_transaction_free(&tr);
	return (0);

rollback:
	server_restart_rollback(&tr, dirty);
	server_restart_transaction_free(&tr);
	return (-1);
}
