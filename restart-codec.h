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

#ifndef TMUX_RESTART_CODEC_H
#define TMUX_RESTART_CODEC_H

#include <sys/types.h>

#include <stdint.h>

struct ibuf;
struct window_pane;

struct restart_terminal;
struct restart_state;

int restart_terminal_encode(const struct window_pane *, struct ibuf **,
    char **);
int restart_terminal_decode(const void *, size_t, struct restart_terminal **,
    char **);
void restart_terminal_free(struct restart_terminal *);

typedef int (*restart_fd_lookup_cb)(void *, u_int, pid_t, int *);

size_t restart_codec_max_size(void);

int restart_state_encode(struct ibuf **, char **);
int restart_state_decode(const void *, size_t, struct restart_state **,
    char **);
uint64_t restart_state_features(const struct restart_state *);
size_t restart_state_descriptor_count(const struct restart_state *);
int restart_state_descriptor_at(const struct restart_state *, size_t,
    u_int *, pid_t *);
int restart_state_apply(const struct restart_state *, restart_fd_lookup_cb,
    void *, char **);
void restart_state_free(struct restart_state *);

int restart_terminal_apply(const struct restart_terminal *,
    struct window_pane *, char **);

#endif
