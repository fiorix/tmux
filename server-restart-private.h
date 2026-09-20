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

#ifndef SERVER_RESTART_PRIVATE_H
#define SERVER_RESTART_PRIVATE_H

/* Include after tmux.h, restart-codec.h and restart-exec.h. */

/*
 * A restart is a transaction. NORMAL is the only phase a restart may be asked
 * for in; QUIESCED means input has been stopped and can still be given back;
 * COMMITTED means the clients have been told and there is no way back.
 */
enum server_restart_phase {
	SERVER_RESTART_NORMAL,
	SERVER_RESTART_QUIESCED,
	SERVER_RESTART_COMMITTED
};

/*
 * How far the restoring server got before something failed. Recovery resumes
 * at the stage that failed rather than starting again, because the stages
 * before it have already happened.
 */
enum server_restart_restore_stage {
	SERVER_RESTART_RESTORE_DETACHED,
	SERVER_RESTART_RESTORE_PUBLISHED,
	SERVER_RESTART_RESTORE_CLEAN,
	SERVER_RESTART_RESTORE_ENABLED,
	SERVER_RESTART_RESTORE_READY
};

/* A pane and the reading state it had before the restart stopped it. */
struct server_restart_pane {
	struct window_pane	*wp;
	short			 events;
	struct restart_fd	 handoff;
};

/* A client and the reading state it had before the restart stopped it. */
struct server_restart_client {
	struct client	*client;
	int		 read_enabled;
};

struct server_restart_transaction {
	struct server_restart_pane	*panes;
	struct restart_fd		*vector;
	size_t				 pane_count;
	struct server_restart_client	*clients;
	size_t				 client_count;
	struct ibuf			*encoded;
	int				 checkpoint_fd;
};

struct server_restart_context {
	enum server_restart_phase phase;
	int			 dirty;
	int			 exit_status;
	struct restart_activation *activation;
	uint64_t		 flags;
};

#endif /* SERVER_RESTART_PRIVATE_H */
