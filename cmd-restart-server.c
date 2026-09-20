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

#include <stdlib.h>

#include "tmux.h"

/*
 * Replace the server with a new copy of its own image, keeping the panes.
 */

static enum cmd_retval	cmd_restart_server_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_restart_server_entry = {
	.name = "restart-server",
	.alias = NULL,

	.args = { "", 0, 0, NULL },
	.usage = "",

	.flags = 0,
	.exec = cmd_restart_server_exec
};

static enum cmd_retval
cmd_restart_server_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	char	*cause = NULL;

	if (server_restart_start(&cause) != 0) {
		cmdq_error(item, "%s", cause);
		free(cause);
		return (CMD_RETURN_ERROR);
	}

	/*
	 * Past this point the replacement is due to run and this process is
	 * releasing its clients, so the item never completes here.
	 */
	return (CMD_RETURN_WAIT);
}
