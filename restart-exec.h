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

#ifndef TMUX_RESTART_EXEC_H
#define TMUX_RESTART_EXEC_H

#include <sys/types.h>

#include <stdint.h>

struct ibuf;
struct restart_activation;

/*
 * How this server was started. A server that was handed descriptors by its
 * predecessor restores from them; one that was handed only a listening socket
 * has nothing to restore.
 */
enum restart_activation_type {
	RESTART_ACTIVATION_NONE,
	RESTART_ACTIVATION_SOCKET,
	RESTART_ACTIVATION_RESTORE
};

/*
 * Whether a failed store left anything behind. CLEAN means the caller may
 * continue as if the restart had never been asked for; DIRTY means it must
 * roll back.
 */
enum restart_store_result {
	RESTART_STORE_OK,
	RESTART_STORE_ERROR_CLEAN,
	RESTART_STORE_ERROR_DIRTY
};

/*
 * Whether the replacement was asked for. UNKNOWN means the request may or may
 * not have been delivered, so the server can neither continue nor exit.
 */
enum restart_dispatch_result {
	RESTART_DISPATCH_NOT_SENT,
	RESTART_DISPATCH_QUEUED,
	RESTART_DISPATCH_UNKNOWN
};

/* A pane's pty master, named so the replacement can match it to its pane. */
struct restart_fd {
	u_int	 pane_id;
	pid_t	 pid;
	int	 fd;
};

/*
 * How a server hands its descriptors to a replacement. Every member is a
 * property of the transport, so none of them names a platform type.
 */
struct server_restart_ops {
	enum restart_activation_type (*activation_type)(
	    const struct restart_activation *);
	int (*activation_state_fd)(const struct restart_activation *);
	size_t (*activation_pane_count)(const struct restart_activation *);
	int (*activation_pane_at)(const struct restart_activation *, size_t,
	    struct restart_fd *);
	int (*activation_lookup)(void *, u_int, pid_t, int *);
	enum restart_store_result (*activation_remove)(
	    const struct restart_activation *, char **);
	void (*activation_close)(struct restart_activation *);
	int (*prepare)(struct restart_activation *, char **);
	int (*preflight)(const struct restart_activation *, uint64_t,
	    const struct restart_fd *, size_t, char **);
	int (*checkpoint_create)(char **);
	int (*checkpoint_seal)(int, const struct ibuf *, char **);
	int (*checkpoint_read)(int, void **, size_t *, char **);
	enum restart_store_result (*store)(int, const struct restart_fd *,
	    size_t, char **);
	enum restart_store_result (*remove)(const struct restart_fd *, size_t,
	    char **);
	enum restart_dispatch_result (*restart)(char **);
	int (*baseline_ready)(char **);
	int (*ready)(char **);
};

const struct server_restart_ops *restart_exec_get_ops(void);

int	 restart_checkpoint_create(char **);
int	 restart_checkpoint_seal(int, const struct ibuf *, char **);
int	 restart_checkpoint_read(int, void **, size_t *, char **);

int	 restart_exec_activated(void);
int	 restart_exec_create_socket(uint64_t, struct restart_activation **,
	     char **);
void	 restart_exec_activation_free(struct restart_activation *);
void	 restart_exec_fallback(struct restart_activation *);
void	 restart_exec_finish(void);

#endif
