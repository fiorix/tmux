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
#include <string.h>

#include "tmux.h"
#include "restart-codec-private.h"

#ifdef ENABLE_SIXEL
/*
 * Build the text an image is replaced by on a terminal that cannot draw it.
 * Sized from the image's cell extent, so it occupies the same area.
 */
static int
restart_image_fallback(char **ret, u_int sx, u_int sy,
    struct restart_budget *budget, char **cause)
{
	char	 label[64], *buf;
	size_t	 lsize, line, size;
	u_int	 py;
	int	 n;

	*ret = NULL;
	if (sy == 0) {
		if (restart_budget_charge(budget, 1, cause) != 0)
			return (-1);
		*ret = xmalloc(1);
		**ret = '\0';
		return (0);
	}
	n = snprintf(label, sizeof label, "SIXEL IMAGE (%ux%u)\r\n", sx, sy);
	if (n < 0 || (size_t)n >= sizeof label) {
		restart_set_cause(cause, "sixel fallback label is too large");
		return (-1);
	}
	lsize = n + 1;
	line = sx < lsize - 3 ? lsize - 1 : sx + 2;
	if (restart_size_add(sx, 2, &size) != 0 ||
	    restart_size_mul(size, sy - 1, &size) != 0 ||
	    restart_size_add(line, size, &size) != 0 ||
	    restart_size_add(size, 1, &size) != 0 ||
	    restart_budget_charge(budget, size, cause) != 0)
		return (-1);
	*ret = buf = xmalloc(size);
	if (sx < lsize - 3) {
		memcpy(buf, label, lsize);
		buf += lsize - 1;
	} else {
		memcpy(buf, label, lsize - 3);
		buf += lsize - 3;
		memset(buf, '+', sx - lsize + 3);
		buf += sx - lsize + 3;
		memcpy(buf, "\r\n", 2);
		buf += 2;
	}
	for (py = 1; py < sy; py++) {
		memset(buf, '+', sx);
		buf += sx;
		memcpy(buf, "\r\n", 2);
		buf += 2;
	}
	*buf = '\0';
	return (0);
}

/*
 * Turn the decoded images into a detached list. The cell extent is recomputed
 * from the parsed image and checked against the stored one, so a stream
 * cannot claim an area its pixels do not occupy.
 */
static int
restart_image_prepare(const struct restart_image_list *in,
    struct restart_budget *budget, struct images *out, char **cause)
{
	const struct restart_image	*item;
	struct image			*im;
	struct sixel_image		*si;
	u_int				 sx, sy;
	size_t				 i;

	TAILQ_INIT(out);
	for (i = 0; i < in->count; i++) {
		item = &in->items[i];
		if (restart_budget_charge(budget, sizeof *im, cause) != 0)
			goto fail;
		si = sixel_restart_parse(item->dcs.data, item->dcs.size,
		    item->xpixel, item->ypixel, item->psx, item->psy);
		if (si == NULL) {
			restart_set_cause(cause, "invalid restart sixel");
			goto fail;
		}
		sixel_size_in_cells(si, &sx, &sy);
		if (sx != item->sx || sy != item->sy) {
			sixel_free(si);
			restart_set_cause(cause, "sixel cell size mismatch");
			goto fail;
		}
		im = xcalloc(1, sizeof *im);
		im->data = si;
		im->px = item->px;
		im->py = item->py;
		im->sx = sx;
		im->sy = sy;
		if (restart_image_fallback(&im->fallback, sx, sy, budget,
		    cause) != 0) {
			sixel_free(si);
			free(im);
			goto fail;
		}
		im->list = out;
		TAILQ_INSERT_TAIL(out, im, entry);
	}
	return (0);

fail:
	image_restart_discard(out);
	return (-1);
}

/* Check that the images about to be published fit under the global limit. */
static int
restart_image_preflight(size_t count, char **cause)
{
	if (image_restart_room(count) != 0) {
		restart_set_cause(cause, "global image limit exceeded");
		return (-1);
	}
	return (0);
}
#endif

static int
restart_palette_build(const struct restart_palette *in,
    struct colour_palette *palette, struct restart_budget *budget, char **cause)
{
	size_t i;

	colour_palette_init(palette);
	palette->fg = in->fg;
	palette->bg = in->bg;
	if (in->count == 0)
		return (0);
	if (restart_budget_charge(budget, 256 * sizeof *palette->palette,
	    cause) != 0)
		return (-1);
	palette->palette = malloc(256 * sizeof *palette->palette);
	if (palette->palette == NULL) {
		restart_set_cause(cause, "out of memory");
		return (-1);
	}
	for (i = 0; i < 256; i++)
		palette->palette[i] = -1;
	for (i = 0; i < in->count; i++)
		palette->palette[in->items[i].index] = in->items[i].value;
	return (0);
}

static int
restart_runtime_string(const char *value, char **out,
    struct restart_budget *budget, char **cause)
{
	size_t size = strlen(value) + 1;

	*out = NULL;
	if (restart_budget_charge(budget, size, cause) != 0)
		return (-1);
	*out = malloc(size);
	if (*out == NULL) {
		restart_set_cause(cause, "out of memory");
		return (-1);
	}
	memcpy(*out, value, size);
	return (0);
}

/* Build a grid from a decoded one. */
static int
restart_grid_build(const struct restart_grid *in, struct grid **out,
    char **cause)
{
	struct grid		*gd;
	struct grid_line	*gl;
	struct grid_cell	 gc;
	const struct restart_line *line;
	u_int			 x, y;

	*out = NULL;
	gd = grid_restart_create(in->sx, in->sy, in->hlimit, in->hsize);
	gd->hscrolled = in->hscrolled;
	gd->scroll_added = in->scroll_added;
	gd->scroll_collected = in->scroll_collected;
	gd->scroll_generation = in->scroll_generation;
	if (in->flags & GRID_HISTORY)
		gd->flags |= GRID_HISTORY;
	else
		gd->flags &= ~GRID_HISTORY;

	for (y = 0; y < in->line_count; y++) {
		line = &in->lines[y];
		for (x = 0; x < line->cellsize; x++) {
			if (restart_cell_build(&line->cells[x], &gc,
			    cause) != 0) {
				grid_destroy(gd);
				return (-1);
			}
			grid_restart_set_cell(gd, x, y, &gc);
		}
		gl = grid_get_line(gd, y);
		gl->cellused = line->cellused;
		gl->time = line->time;
		gl->osc133_data.prompt_col = line->prompt_col;
		gl->osc133_data.cmd_col = line->command_col;
		gl->osc133_data.out_start_col = line->output_start_col;
		gl->osc133_data.out_end_col = line->output_end_col;
		gl->osc133_data.exit_status = line->exit_status;
		gl->flags = 0;
		if (line->flags & 0x01)
			gl->flags |= GRID_LINE_WRAPPED;
		if (line->flags & 0x02)
			gl->flags |= GRID_LINE_START_PROMPT;
		if (line->flags & 0x04)
			gl->flags |= GRID_LINE_SECOND_PROMPT;
		if (line->flags & 0x08)
			gl->flags |= GRID_LINE_START_COMMAND;
		if (line->flags & 0x10)
			gl->flags |= GRID_LINE_START_OUTPUT;
		if (line->flags & 0x20)
			gl->flags |= GRID_LINE_END_OUTPUT;
	}

	*out = gd;
	return (0);
}

static int
restart_screen_build(const struct restart_screen *in, struct screen *screen,
    struct hyperlinks **links, struct restart_budget *budget, char **cause)
{
	struct grid		*grid = NULL, *saved_grid = NULL;
	struct hyperlinks	*prepared = NULL;
	size_t			 tabs = in->tabs.size, i;
	struct grid_cell	 saved_cell;

	memset(screen, 0, sizeof *screen);
#ifdef ENABLE_SIXEL
	TAILQ_INIT(&screen->images);
	TAILQ_INIT(&screen->saved_images);
#endif
	*links = NULL;
	if (restart_grid_build(&in->grid, &grid, cause) != 0 ||
	    (in->have_saved_grid &&
	    restart_grid_build(&in->saved_grid, &saved_grid, cause) != 0) ||
	    restart_cell_build(&in->saved.cell, &saved_cell, cause) != 0)
		goto fail;

	prepared = hyperlinks_restart_init(in->links.next_inner);
	for (i = 0; i < in->links.count; i++) {
		if (hyperlinks_restart_add(prepared, in->links.items[i].inner,
		    in->links.items[i].uri, in->links.items[i].internal_id,
		    in->links.items[i].external_id) != 0) {
			restart_set_cause(cause, "duplicate restart hyperlink");
			goto fail;
		}
	}
#ifdef ENABLE_SIXEL
	if (restart_image_prepare(&in->images, budget, &screen->images,
	    cause) != 0 ||
	    restart_image_prepare(&in->saved_images, budget,
	    &screen->saved_images, cause) != 0)
		goto fail;
#endif
	screen->grid = grid;
	grid = NULL;
	screen->saved_grid = saved_grid;
	saved_grid = NULL;
	if (restart_runtime_string(in->title, &screen->title, budget,
	    cause) != 0 ||
	    (in->path != NULL && restart_runtime_string(in->path,
	    &screen->path, budget, cause) != 0))
		goto fail_screen;
	for (i = 0; i < in->titles.count; i++)
		screen_add_title(screen, in->titles.items[i]);
	screen->cx = in->cx;
	screen->cy = in->cy;
	screen->cstyle = in->cstyle;
	screen->default_cstyle = in->default_cstyle;
	screen->ccolour = in->ccolour;
	screen->default_ccolour = in->default_ccolour;
	screen->rupper = in->rupper;
	screen->rlower = in->rlower;
	screen->mode = in->mode;
	screen->default_mode = in->default_mode;
	screen->saved_cx = in->saved.cx;
	screen->saved_cy = in->saved.cy;
	screen->saved_cell = saved_cell;
	screen->saved_flags = in->saved.grid_flags;
	if (restart_budget_charge(budget, tabs, cause) != 0)
		goto fail_screen;
	screen->tabs = malloc(tabs);
	if (screen->tabs == NULL) {
		restart_set_cause(cause, "out of memory");
		goto fail_screen;
	}
	memcpy(screen->tabs, in->tabs.data, tabs);
	screen->progress_bar.state = in->progress_state;
	screen->progress_bar.progress = in->progress;
	*links = prepared;
	return (0);

fail_screen:
#ifdef ENABLE_SIXEL
	image_restart_discard(&screen->images);
	image_restart_discard(&screen->saved_images);
	TAILQ_INIT(&screen->images);
	TAILQ_INIT(&screen->saved_images);
#endif
	screen_free(screen);
fail:
#ifdef ENABLE_SIXEL
	image_restart_discard(&screen->images);
	image_restart_discard(&screen->saved_images);
#endif
	if (grid != NULL)
		grid_destroy(grid);
	if (saved_grid != NULL)
		grid_destroy(saved_grid);
	hyperlinks_restart_discard(prepared);
	return (-1);
}

struct restart_terminal_prepared {
	const struct restart_terminal	*terminal;
	struct window_pane		*wp;
	struct screen			 screen;
	struct colour_palette		 palette;
	struct hyperlinks		*links;
	struct input_ctx		*parser;
	int				 preflighted;
};

static int
restart_terminal_target_valid(const struct window_pane *wp, char **cause)
{
	if (wp == NULL || wp->options == NULL || wp->screen != &wp->base ||
	    wp->base.grid == NULL || wp->base.tabs == NULL ||
	    wp->base.hyperlinks == NULL || wp->status_screen.grid == NULL ||
	    wp->status_screen.tabs == NULL ||
	    wp->status_screen.hyperlinks == NULL) {
		restart_set_cause(cause, "terminal target is not initialized");
		return (-1);
	}
	if (!TAILQ_EMPTY(&wp->modes) || !TAILQ_EMPTY(&wp->resize_queue) ||
	    wp->base.sel != NULL || wp->base.write_list != NULL ||
	    wp->status_screen.sel != NULL ||
	    wp->status_screen.write_list != NULL || wp->wait_item != NULL ||
	    wp->editor != NULL || wp->searchstr != NULL ||
	    wp->searchregex != 0 || wp->prompt != NULL ||
	    wp->prompt_data != NULL || wp->prompt_cx != 0 ||
	    wp->sync_dirty != NULL || wp->sync_dirty_size != 0 ||
	    wp->r.ranges != NULL || wp->r.used != 0 || wp->r.size != 0 ||
	    wp->pipe_fd != -1 || wp->pipe_event != NULL || wp->pipe_pid != 0 ||
	    wp->offset.used != 0 || wp->base_offset != 0 ||
	    wp->pipe_offset.used != 0) {
		restart_set_cause(cause, "terminal target has transient state");
		return (-1);
	}
	if (wp->event != NULL && bufferevent_get_enabled(wp->event) != 0) {
		restart_set_cause(cause, "terminal target event is enabled");
		return (-1);
	}
	return (0);
}

int
restart_terminal_prepare(const struct restart_terminal *terminal,
    struct window_pane *wp, struct restart_budget *budget,
    struct restart_terminal_prepared **out, char **cause)
{
	struct restart_terminal_prepared *prepared;
	struct input_parser_state	  ips;
	struct evbuffer *input, *output;

	*out = NULL;
	if (cause != NULL)
		*cause = NULL;
	if (terminal == NULL || budget == NULL) {
		restart_set_cause(cause,
		    "invalid terminal preparation arguments");
		return (-1);
	}
	if (restart_terminal_target_valid(wp, cause) != 0)
		return (-1);
#ifdef ENABLE_SIXEL
	if (!TAILQ_EMPTY(&wp->base.images) ||
	    !TAILQ_EMPTY(&wp->base.saved_images)) {
		restart_set_cause(cause,
		    "target terminal images are not empty");
		return (-1);
	}
#endif
	prepared = restart_calloc(budget, 1, sizeof *prepared, cause);
	if (prepared == NULL)
		return (-1);
	prepared->terminal = terminal;
	prepared->wp = wp;
	if (wp->event == NULL &&
	    (terminal->pty_input.size != 0 || terminal->pty_output.size != 0)) {
		restart_set_cause(cause, "terminal buffers require an event");
		goto fail;
	}
	if (wp->event != NULL) {
		input = wp->event->input;
		output = wp->event->output;
		if (EVBUFFER_LENGTH(input) != 0 ||
		    EVBUFFER_LENGTH(output) != 0) {
			restart_set_cause(cause,
			    "target terminal buffers are not empty");
			goto fail;
		}
		if (restart_budget_charge(budget, terminal->pty_input.size,
		    cause) != 0 || restart_budget_charge(budget,
		    terminal->pty_output.size, cause) != 0)
			goto fail;
	}
	if (restart_palette_build(&terminal->palette, &prepared->palette,
	    budget, cause) != 0)
		goto fail;
	if (restart_screen_build(&terminal->screen, &prepared->screen,
	    &prepared->links, budget, cause) != 0) {
		colour_palette_free(&prepared->palette);
		goto fail;
	}
	if (restart_parser_build(&terminal->parser, &ips, cause) != 0 ||
	    input_restore(&ips, &prepared->parser, cause) != 0) {
		restart_terminal_discard(prepared);
		return (-1);
	}
	*out = prepared;
	return (0);

fail:
	free(prepared);
	return (-1);
}


/*
 * Armed once where an apply begins, and never cleared in between.
 *
 * Clearing it on commit or discard looked equivalent and was not: discard
 * takes a single prepared terminal and runs per pane, so a rollback over N
 * panes re-armed it N times and the guard only caught two consecutive calls
 * with nothing at all between them. Once per transaction and once per pane
 * are different scopes, and the difference is invisible when the clearing is
 * described by which function does it rather than by how often it happens.
 */
static int	restart_terminal_preflight_done;

void
restart_terminal_preflight_rearm(void)
{
	restart_terminal_preflight_done = 0;
}

int
restart_terminal_preflight_batch(struct restart_terminal_prepared **items,
    size_t count, char **cause)
{
	struct restart_terminal_prepared *prepared;
	struct evbuffer *input, *output;
	size_t hyperlinks = 0, i, j;
#ifdef ENABLE_SIXEL
	size_t images = 0;
#endif

	if (cause != NULL)
		*cause = NULL;
	if (count != 0 && items == NULL) {
		restart_set_cause(cause, "missing prepared terminal batch");
		return (-1);
	}

	/*
	 * This is the only enforcement of either global limit and it
	 * accumulates across the batch before a single check, so calling it
	 * once per pane passes every element individually and still exceeds
	 * the total. Nothing about such a loop looks wrong at the call site,
	 * and during apply the global counts start at zero so each element
	 * clears the limit trivially. Dying here makes the loop fail on its
	 * second element, as loudly as forgetting the call fails at commit.
	 */
	if (restart_terminal_preflight_done)
		fatalx("restart terminal preflight called more than once");
	restart_terminal_preflight_done = 1;
	for (i = 0; i < count; i++) {
		prepared = items[i];
		if (prepared == NULL || prepared->preflighted) {
			restart_set_cause(cause,
			    "invalid prepared terminal batch");
			return (-1);
		}
		for (j = 0; j < i; j++) {
			if (items[j]->wp == prepared->wp) {
				restart_set_cause(cause,
				    "duplicate prepared terminal target");
				return (-1);
			}
		}
		if (restart_size_add(hyperlinks,
		    prepared->terminal->screen.links.count, &hyperlinks) != 0) {
			restart_set_cause(cause,
			    "terminal hyperlink count overflow");
			return (-1);
		}
#ifdef ENABLE_SIXEL
		if (restart_size_add(images,
		    prepared->terminal->screen.images.count, &images) != 0 ||
		    restart_size_add(images,
		    prepared->terminal->screen.saved_images.count,
		    &images) != 0) {
			restart_set_cause(cause,
			    "terminal image count overflow");
			return (-1);
		}
#endif
	}
	if (hyperlinks_restart_preflight(hyperlinks, cause) != 0)
		return (-1);
#ifdef ENABLE_SIXEL
	if (restart_image_preflight(images, cause) != 0)
		return (-1);
#endif
	for (i = 0; i < count; i++) {
		prepared = items[i];
		if (prepared->wp->event == NULL)
			continue;
		input = prepared->wp->event->input;
		output = prepared->wp->event->output;
		if (EVBUFFER_LENGTH(input) != 0 ||
		    EVBUFFER_LENGTH(output) != 0) {
			restart_set_cause(cause,
			    "prepared terminal buffers are not empty");
			return (-1);
		}
		if (evbuffer_unfreeze(input, 0) != 0) {
			restart_set_cause(cause,
			    "could not unfreeze prepared terminal input");
			return (-1);
		}
		if (evbuffer_expand(input,
		    prepared->terminal->pty_input.size) != 0) {
			evbuffer_freeze(input, 0);
			restart_set_cause(cause, "out of memory");
			return (-1);
		}
		if (evbuffer_freeze(input, 0) != 0 ||
		    evbuffer_expand(output,
		    prepared->terminal->pty_output.size) != 0) {
			restart_set_cause(cause, "out of memory");
			return (-1);
		}
	}
	for (i = 0; i < count; i++) {
		prepared = items[i];
		if (restart_parser_timer_state(
		    prepared->terminal->parser.state) &&
		    input_restart_arm_ground_timer(prepared->parser,
		        cause) != 0)
			return (-1);
	}
	for (i = 0; i < count; i++)
		items[i]->preflighted = 1;
	return (0);
}

void
restart_terminal_commit(struct restart_terminal_prepared *prepared,
    struct window_pane *wp)
{
	const struct restart_terminal *terminal = prepared->terminal;
	struct screen old_screen;
	struct colour_palette old_palette;
	struct evbuffer *input, *output;

	if (!prepared->preflighted || wp != prepared->wp)
		fatalx("terminal commit without batch preflight");
	if (wp->event != NULL) {
		input = wp->event->input;
		output = wp->event->output;
		if (terminal->pty_input.size != 0) {
			if (evbuffer_unfreeze(input, 0) != 0 ||
			    evbuffer_add(input, terminal->pty_input.data,
			    terminal->pty_input.size) != 0 ||
			    evbuffer_freeze(input, 0) != 0) {
				fatalx("preflighted terminal input append "
				    "failed");
			}
		}
		if (terminal->pty_output.size != 0 &&
		    evbuffer_add(output, terminal->pty_output.data,
		    terminal->pty_output.size) != 0)
			fatalx("preflighted terminal output append failed");
	}
	if (wp->ictx != NULL) {
		input_free(wp->ictx);
		wp->ictx = NULL;
	}
	old_screen = wp->base;
#ifdef ENABLE_SIXEL
	TAILQ_INIT(&old_screen.images);
	TAILQ_INIT(&old_screen.saved_images);
#endif
	old_palette = wp->palette;
	prepared->palette.default_palette = old_palette.default_palette;
	old_palette.default_palette = NULL;
	hyperlinks_restart_commit(prepared->links);
	prepared->screen.hyperlinks = prepared->links;
	wp->base = prepared->screen;
	wp->screen = &wp->base;
	wp->palette = prepared->palette;
#ifdef ENABLE_SIXEL
	image_restart_commit(&wp->base, &wp->base.images, 0);
	image_restart_commit(&wp->base, &wp->base.saved_images, 1);
#endif
	input_restart_bind(prepared->parser, wp, wp->event, &wp->palette);
	wp->ictx = prepared->parser;
	screen_free(&old_screen);
	colour_palette_free(&old_palette);
	wp->offset.used = 0;
	wp->base_offset = 0;
	wp->pipe_offset.used = 0;
	free(prepared);
}

/*
 * Resize a prepared terminal's detached screen to the pane's final geometry.
 *
 * Only the prepared copy is touched; the pane's own screens are not reached
 * from here, and nothing process-global is either, which is what makes this
 * safe before the commit boundary. The target size is the pane's own
 * geometry rather than anything read from the stream, so this allocates
 * against no budget.
 */
int
restart_terminal_resize_prepared(struct restart_terminal_prepared *prepared,
    u_int sx, u_int sy, char **cause)
{
	if (prepared == NULL) {
		restart_set_cause(cause, "invalid prepared terminal resize");
		return (-1);
	}
	screen_restart_resize(&prepared->screen, sx, sy, 1);
	return (0);
}

void
restart_terminal_discard(struct restart_terminal_prepared *prepared)
{
	if (prepared == NULL)
		return;
	input_restart_discard(prepared->parser);
	hyperlinks_restart_discard(prepared->links);
#ifdef ENABLE_SIXEL
	image_restart_discard(&prepared->screen.images);
	image_restart_discard(&prepared->screen.saved_images);
	TAILQ_INIT(&prepared->screen.images);
	TAILQ_INIT(&prepared->screen.saved_images);
#endif
	screen_free(&prepared->screen);
	colour_palette_free(&prepared->palette);
	free(prepared);
}

int
restart_terminal_apply(const struct restart_terminal *terminal,
    struct window_pane *wp, char **cause)
{
	struct restart_budget budget = { 0 };
	struct restart_terminal_prepared *prepared;

	/*
	 * This is an apply too: it prepares, preflights and commits one pane
	 * as a complete transaction, so the latch arms here for the same
	 * reason it arms at the graph apply. A loop over this function is
	 * safe where a loop over the batch preflight is not, because each
	 * iteration commits, so the global counts the next preflight reads
	 * already include everything committed before it.
	 */
	restart_terminal_preflight_rearm();

	if (restart_terminal_prepare(terminal, wp, &budget, &prepared,
	    cause) != 0)
		return (-1);
	if (restart_terminal_preflight_batch(&prepared, 1, cause) != 0) {
		restart_terminal_discard(prepared);
		return (-1);
	}
	restart_terminal_commit(prepared, wp);
	return (0);
}
