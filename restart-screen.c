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

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "restart-codec-private.h"

#define RESTART_MODE_MASK 0x1fffffU
#define RESTART_ATTR_MASK 0x7fffU
#define RESTART_CELL_FLAG_MASK 0x0fU
#define RESTART_LINE_FLAG_MASK 0x3fU

enum restart_palette_record {
	RESTART_PALETTE_FG = 1,
	RESTART_PALETTE_BG,
	RESTART_PALETTE_OVERRIDE
};

enum restart_screen_record {
	RESTART_SCREEN_TITLE = 1,
	RESTART_SCREEN_PATH,
	RESTART_SCREEN_TITLE_STACK,
	RESTART_SCREEN_GRID,
	RESTART_SCREEN_CURSOR,
	RESTART_SCREEN_DEFAULT_CURSOR,
	RESTART_SCREEN_SCROLL_REGION,
	RESTART_SCREEN_MODES,
	RESTART_SCREEN_SAVED,
	RESTART_SCREEN_SAVED_GRID,
	RESTART_SCREEN_TABS,
	RESTART_SCREEN_HYPERLINKS,
	RESTART_SCREEN_IMAGES,
	RESTART_SCREEN_SAVED_IMAGES,
	RESTART_SCREEN_PROGRESS
};

enum restart_grid_record {
	RESTART_GRID_DIMENSIONS = 1,
	RESTART_GRID_FLAGS,
	RESTART_GRID_COUNTERS,
	RESTART_GRID_LINE
};

enum restart_line_record {
	RESTART_LINE_INDEX = 1,
	RESTART_LINE_FLAGS,
	RESTART_LINE_TIME,
	RESTART_LINE_OSC133,
	RESTART_LINE_CELLUSED,
	RESTART_LINE_CELLSIZE,
	RESTART_LINE_CELL
};

enum restart_link_record {
	RESTART_LINKS_NEXT_INNER = 1,
	RESTART_LINKS_ENTRY
};

enum restart_link_entry_record {
	RESTART_LINK_INNER = 1,
	RESTART_LINK_INTERNAL_ID,
	RESTART_LINK_EXTERNAL_ID,
	RESTART_LINK_URI
};

#ifdef ENABLE_SIXEL
enum restart_image_record {
	RESTART_IMAGE_POSITION = 1,
	RESTART_IMAGE_CELL_SIZE,
	RESTART_IMAGE_PIXEL_CELL,
	RESTART_IMAGE_DCS,
	RESTART_IMAGE_PIXEL_EXTENT
};
#endif

enum restart_parser_record {
	RESTART_PARSER_STATE = 1,
	RESTART_PARSER_CELL,
	RESTART_PARSER_OLD_CELL,
	RESTART_PARSER_OLD_CURSOR,
	RESTART_PARSER_OLD_MODE,
	RESTART_PARSER_INTERMEDIATE,
	RESTART_PARSER_PARAMETER,
	RESTART_PARSER_INPUT,
	RESTART_PARSER_INPUT_END,
	RESTART_PARSER_UTF8,
	RESTART_PARSER_UTF8_STARTED,
	RESTART_PARSER_LAST_UTF8,
	RESTART_PARSER_FLAGS
};

/* Encode a signed value as unsigned. */
static uint32_t
restart_s32_encode(int32_t value)
{
	if (value < 0)
		return (((uint32_t)(-(value + 1)) << 1) | 1);
	return ((uint32_t)value << 1);
}

/* Decode an unsigned value as signed. */
static int32_t
restart_s32_decode(uint32_t value)
{
	if (value & 1)
		return (-(int32_t)(value >> 1) - 1);
	return ((int32_t)(value >> 1));
}

/* Store a 16 bit value. */
static void
restart_put16(u_char *data, uint16_t value)
{
	data[0] = value >> 8;
	data[1] = value;
}

/* Store a 32 bit value. */
static void
restart_put32(u_char *data, uint32_t value)
{
	data[0] = value >> 24;
	data[1] = value >> 16;
	data[2] = value >> 8;
	data[3] = value;
}

/* Load a 16 bit value. */
static uint16_t
restart_get16(const u_char *data)
{
	return (((uint16_t)data[0] << 8) | data[1]);
}

/* Load a 32 bit value. */
static uint32_t
restart_get32(const u_char *data)
{
	return (((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
	    ((uint32_t)data[2] << 8) | data[3]);
}

/* Check a colour is one tmux can hold. */
static int
restart_colour_valid(int32_t colour)
{
	uint32_t value = colour;

	if (colour == -1 || (colour >= 0 && colour <= 255))
		return (1);
	if ((value & 0xffffff00U) == COLOUR_FLAG_256)
		return (1);
	if ((value & 0xff000000U) == COLOUR_FLAG_RGB)
		return (1);
	if ((value & 0xffffff00U) == COLOUR_FLAG_THEME &&
	    (value & 0xff) < COLOUR_THEME_COUNT)
		return (1);
	return (0);
}

/* Check a boolean is 0 or 1. */
static int
restart_bool_valid(uint8_t value)
{
	return (value == 0 || value == 1);
}

/* Capture a cell. */
int
restart_cell_capture(const struct grid_cell *gc, struct restart_cell *out,
    char **cause)
{
	struct grid_cell normalized;
	u_int i;

	memset(out, 0, sizeof *out);
	if ((gc->flags & GRID_FLAG_PADDING) && gc->data.size == 0) {
		normalized = *gc;
		normalized.data.data[0] = '!';
		normalized.data.have = normalized.data.size = 1;
		normalized.data.width = 0;
		gc = &normalized;
	}
	if (gc->data.size == 0 || gc->data.size > UTF8_SIZE ||
	    ((gc->flags & GRID_FLAG_TAB) ? gc->data.width != gc->data.size :
	    (gc->data.width > 2 && gc->data.width != 0xff)) ||
	    (gc->attr & ~RESTART_ATTR_MASK) != 0 ||
	    !restart_colour_valid(gc->fg) || !restart_colour_valid(gc->bg) ||
	    !restart_colour_valid(gc->us) ||
	    ((gc->flags & GRID_FLAG_TAB) &&
	    (gc->flags & (GRID_FLAG_PADDING|GRID_FLAG_CLEARED)) != 0) ||
	    ((gc->flags & GRID_FLAG_PADDING) &&
	    (gc->flags & GRID_FLAG_CLEARED))) {
		restart_set_cause(cause, "invalid grid cell");
		return (-1);
	}
	if (gc->flags & GRID_FLAG_TAB) {
		for (i = 0; i < gc->data.size; i++) {
			if (gc->data.data[i] != ' ') {
				restart_set_cause(cause, "invalid tab cell");
				return (-1);
			}
		}
	}
	out->size = gc->data.size;
	out->width = gc->data.width;
	memcpy(out->data, gc->data.data, out->size);
	if (gc->flags & GRID_FLAG_PADDING)
		out->flags |= 0x01;
	if (gc->flags & GRID_FLAG_NOPALETTE)
		out->flags |= 0x02;
	if (gc->flags & GRID_FLAG_CLEARED)
		out->flags |= 0x04;
	if (gc->flags & GRID_FLAG_TAB)
		out->flags |= 0x08;
	out->attr = gc->attr;
	out->fg = gc->fg;
	out->bg = gc->bg;
	out->us = gc->us;
	out->link = gc->link;
	return (0);
}

/* Build a cell from a captured one. */
int
restart_cell_build(const struct restart_cell *in, struct grid_cell *gc,
    char **cause)
{
	u_int i;
	if (in->size == 0 || in->size > UTF8_SIZE) {
		restart_set_cause(cause, "invalid restart cell");
		return (-1);
	}

	for (i = 0; i < in->size;) {
		u_int n, j;
		u_char ch = in->data[i];

		if (ch <= 0x7f) {
			i++;
			continue;
		}
		if (ch >= 0xc2 && ch <= 0xdf)
			n = 2;
		else if (ch >= 0xe0 && ch <= 0xef)
			n = 3;
		else if (ch >= 0xf0 && ch <= 0xf4)
			n = 4;
		else
			goto invalid;
		if (i + n > in->size)
			goto invalid;
		if ((ch == 0xe0 && in->data[i + 1] < 0xa0) ||
		    (ch == 0xed && in->data[i + 1] > 0x9f) ||
		    (ch == 0xf0 && in->data[i + 1] < 0x90) ||
		    (ch == 0xf4 && in->data[i + 1] > 0x8f))
			goto invalid;
		for (j = 1; j < n; j++) {
			if ((in->data[i + j] & 0xc0) != 0x80)
				goto invalid;
		}
		i += n;
	}

	if (in->size == 0 || in->size > UTF8_SIZE ||
	    ((in->flags & 0x08) ? in->width != in->size :
	    (in->width > 2 && in->width != 0xff)) ||
	    (in->flags & ~RESTART_CELL_FLAG_MASK) != 0 ||
	    (in->attr & ~RESTART_ATTR_MASK) != 0 ||
	    !restart_colour_valid(in->fg) || !restart_colour_valid(in->bg) ||
	    !restart_colour_valid(in->us) ||
	    ((in->flags & 0x01) && (in->size != 1 || in->width != 0 ||
	    in->data[0] != '!' || (in->flags & 0x0c) != 0)) ||
	    ((in->flags & 0x08) && (in->flags & 0x05) != 0)) {
		restart_set_cause(cause, "invalid restart cell");
		return (-1);
	}
	if (in->flags & 0x08) {
		for (i = 0; i < in->size; i++) {
			if (in->data[i] != ' ') {
				restart_set_cause(cause,
				    "invalid restart tab cell");
				return (-1);
			}
		}
	}
	memset(gc, 0, sizeof *gc);
	gc->data.have = gc->data.size = in->size;
	gc->data.width = in->width;
	memcpy(gc->data.data, in->data, in->size);
	if (in->flags & 0x01)
		gc->flags |= GRID_FLAG_PADDING;
	if (in->flags & 0x02)
		gc->flags |= GRID_FLAG_NOPALETTE;
	if (in->flags & 0x04)
		gc->flags |= GRID_FLAG_CLEARED;
	if (in->flags & 0x08)
		gc->flags |= GRID_FLAG_TAB;
	gc->attr = in->attr;
	gc->fg = in->fg;
	gc->bg = in->bg;
	gc->us = in->us;
	gc->link = in->link;
	return (0);
invalid:
	restart_set_cause(cause, "invalid restart cell UTF-8");
	return (-1);
}

/* Free captured bytes. */
static void
restart_bytes_free(struct restart_bytes *bytes)
{
	free(bytes->data);
	memset(bytes, 0, sizeof *bytes);
}

/* Free a captured string list. */
static void
restart_string_list_free(struct restart_string_list *list)
{
	size_t i;

	for (i = 0; i < list->count; i++)
		free(list->items[i]);
	free(list->items);
	memset(list, 0, sizeof *list);
}

/* Free a captured grid. */
static void
restart_grid_free(struct restart_grid *grid)
{
	size_t i;

	for (i = 0; i < grid->line_count; i++)
		free(grid->lines[i].cells);
	free(grid->lines);
	memset(grid, 0, sizeof *grid);
}

/* Free captured hyperlinks. */
static void
restart_links_free(struct restart_links *links)
{
	size_t i;

	for (i = 0; i < links->count; i++) {
		free(links->items[i].internal_id);
		free(links->items[i].external_id);
		free(links->items[i].uri);
	}
	free(links->items);
	memset(links, 0, sizeof *links);
}

/* Free captured images. */
#ifdef ENABLE_SIXEL
static void
restart_image_list_free(struct restart_image_list *images)
{
	size_t i;

	for (i = 0; i < images->count; i++)
		restart_bytes_free(&images->items[i].dcs);
	free(images->items);
	memset(images, 0, sizeof *images);
}
#endif

/* Free a captured parser. */
static void
restart_parser_free(struct restart_parser *parser)
{
	free(parser->state);
	restart_bytes_free(&parser->intermediate);
	restart_bytes_free(&parser->parameter);
	restart_bytes_free(&parser->input);
	memset(parser, 0, sizeof *parser);
}

/* Free a captured screen. */
static void
restart_screen_free(struct restart_screen *screen)
{
	free(screen->title);
	free(screen->path);
	restart_string_list_free(&screen->titles);
	restart_grid_free(&screen->grid);
	if (screen->have_saved_grid)
		restart_grid_free(&screen->saved_grid);
	restart_bytes_free(&screen->tabs);
	restart_links_free(&screen->links);
#ifdef ENABLE_SIXEL
	restart_image_list_free(&screen->images);
	restart_image_list_free(&screen->saved_images);
#endif
	memset(screen, 0, sizeof *screen);
}

/* Free a captured terminal. */
void
restart_terminal_free(struct restart_terminal *terminal)
{
	if (terminal == NULL)
		return;
	free(terminal->palette.items);
	restart_screen_free(&terminal->screen);
	restart_parser_free(&terminal->parser);
	restart_bytes_free(&terminal->pty_input);
	restart_bytes_free(&terminal->pty_output);
	free(terminal);
}

/* Capture a string. */
static int
restart_capture_string(const char *value, char **out,
    struct restart_budget *budget, char **cause)
{
	size_t size = strlen(value);

	if (size > RESTART_MAX_FIELD) {
		restart_set_cause(cause, "restart string is too large");
		return (-1);
	}
	*out = restart_alloc(budget, size + 1, cause);
	if (*out == NULL)
		return (-1);
	memcpy(*out, value, size + 1);
	return (0);
}

/* Capture bytes. */
static int
restart_capture_bytes(const void *data, size_t size,
    struct restart_bytes *out, struct restart_budget *budget, char **cause)
{
	memset(out, 0, sizeof *out);
	if (size == 0)
		return (0);
	out->data = restart_alloc(budget, size, cause);
	if (out->data == NULL)
		return (-1);
	memcpy(out->data, data, size);
	out->size = size;
	return (0);
}

/* Capture the title stack. */
static int
restart_titles_capture(const struct screen *s, struct restart_string_list *out,
    struct restart_budget *budget, char **cause)
{
	const char	*title;
	u_int		 i;

	memset(out, 0, sizeof *out);
	if (s->ntitles > RESTART_MAX_TITLES) {
		restart_set_cause(cause, "too many screen titles");
		return (-1);
	}
	if (s->ntitles == 0)
		return (0);
	out->items = restart_calloc(budget, s->ntitles, sizeof *out->items,
	    cause);
	if (out->items == NULL)
		return (-1);
	for (i = 0; i < s->ntitles; i++) {
		title = screen_title_at(s, i);
		if (title == NULL) {
			restart_set_cause(cause, "invalid screen title stack");
			goto fail;
		}
		if (restart_capture_string(title, &out->items[i], budget,
		    cause) != 0)
			goto fail;
		out->count = i + 1;
	}
	return (0);

fail:
	restart_string_list_free(out);
	return (-1);
}

/* Capture the hyperlinks. */
static int
restart_links_capture(struct hyperlinks *hl, struct restart_links *out,
    struct restart_budget *budget, char **cause)
{
	struct hyperlinks_uri	*hlu;
	struct restart_link	*link;
	const char		*internal_id, *external_id, *uri;
	void			*new;

	memset(out, 0, sizeof *out);
	if (hl == NULL) {
		restart_set_cause(cause, "pane has no hyperlinks");
		return (-1);
	}
	out->next_inner = hyperlinks_next_inner(hl);
	for (hlu = hyperlinks_first(hl); hlu != NULL;
	    hlu = hyperlinks_next(hlu)) {
		if (out->count == hyperlinks_limit()) {
			restart_set_cause(cause, "too many hyperlinks");
			goto fail;
		}
		new = restart_grow(budget, out->items, out->count,
		    out->count + 1, sizeof *out->items, cause);
		if (new == NULL)
			goto fail;
		out->items = new;
		link = &out->items[out->count++];
		memset(link, 0, sizeof *link);
		link->inner = hyperlinks_entry(hlu, &uri, &internal_id,
		    &external_id);
		if (restart_capture_string(internal_id, &link->internal_id,
		    budget, cause) != 0 ||
		    restart_capture_string(external_id, &link->external_id,
		    budget, cause) != 0 ||
		    restart_capture_string(uri, &link->uri, budget, cause) != 0)
			goto fail;
	}
	return (0);

fail:
	restart_links_free(out);
	return (-1);
}

/* Capture the images. */
#ifdef ENABLE_SIXEL
static int
restart_images_capture(const struct screen *s, int saved,
    struct restart_image_list *out, struct restart_budget *budget,
    char **cause)
{
	const struct images	*list;
	struct image		*im;
	struct restart_image	*item;
	char			*dcs;
	size_t			 size;

	memset(out, 0, sizeof *out);
	list = saved ? &s->saved_images : &s->images;
	TAILQ_FOREACH(im, (struct images *)list, entry) {
		if (out->count == image_limit() ||
		    budget->images == image_limit()) {
			restart_set_cause(cause, "too many images");
			goto fail;
		}
		item = restart_grow(budget, out->items, out->count,
		    out->count + 1, sizeof *out->items, cause);
		if (item == NULL)
			goto fail;
		out->items = item;
		item = &out->items[out->count++];
		budget->images++;
		memset(item, 0, sizeof *item);
		item->px = im->px;
		item->py = im->py;
		item->sx = im->sx;
		item->sy = im->sy;
		sixel_cell_size(im->data, &item->xpixel, &item->ypixel);
		sixel_size_in_pixels(im->data, &item->psx, &item->psy);
		if (item->xpixel == 0 || item->ypixel == 0 ||
		    item->psx == 0 || item->psy == 0) {
			restart_set_cause(cause, "invalid image size");
			goto fail;
		}
		dcs = sixel_restart_print(im->data, &size);
		if (dcs == NULL) {
			restart_set_cause(cause, "could not print image");
			goto fail;
		}
		if (restart_budget_charge(budget, size, cause) != 0) {
			free(dcs);
			goto fail;
		}
		item->dcs.data = dcs;
		item->dcs.size = size;
	}
	return (0);

fail:
	restart_image_list_free(out);
	return (-1);
}
#endif

/* Capture a grid. */
static int
restart_grid_capture(const struct grid *gd, struct restart_grid *out,
    struct restart_budget *budget, char **cause)
{
	const struct grid_line *gl;
	struct restart_line *line;
	struct grid_cell gc;
	size_t line_count, y;
	u_int x;

	memset(out, 0, sizeof *out);
	if (gd->sx < 1 || gd->sx > RESTART_MAX_DIMENSION || gd->sy < 1 ||
	    gd->sy > RESTART_MAX_DIMENSION || gd->hscrolled > gd->hsize ||
	    restart_size_add(gd->hsize, gd->sy, &line_count) != 0 ||
	    line_count > RESTART_MAX_RECORDS) {
		restart_set_cause(cause, "invalid grid dimensions");
		return (-1);
	}
	out->sx = gd->sx;
	out->sy = gd->sy;
	out->hsize = gd->hsize;
	out->hlimit = gd->hlimit;
	out->hscrolled = gd->hscrolled;
	out->flags = gd->flags & GRID_HISTORY;
	out->scroll_added = gd->scroll_added;
	out->scroll_collected = gd->scroll_collected;
	out->scroll_generation = gd->scroll_generation;
	out->lines = restart_calloc(budget, line_count, sizeof *out->lines,
	    cause);
	if (out->lines == NULL)
		return (-1);
	out->line_count = line_count;
	for (y = 0; y < line_count; y++) {
		gl = &gd->linedata[y];
		line = &out->lines[y];
		if (gl->flags & GRID_LINE_DEAD) {
			restart_set_cause(cause,
			    "dead grid line in restart state");
			return (-1);
		}
		if (gl->flags & GRID_LINE_WRAPPED)
			line->flags |= 0x01;
		if (gl->flags & GRID_LINE_START_PROMPT)
			line->flags |= 0x02;
		if (gl->flags & GRID_LINE_SECOND_PROMPT)
			line->flags |= 0x04;
		if (gl->flags & GRID_LINE_START_COMMAND)
			line->flags |= 0x08;
		if (gl->flags & GRID_LINE_START_OUTPUT)
			line->flags |= 0x10;
		if (gl->flags & GRID_LINE_END_OUTPUT)
			line->flags |= 0x20;
		line->time = gl->time;
		line->prompt_col = gl->osc133_data.prompt_col;
		line->command_col = gl->osc133_data.cmd_col;
		line->output_start_col = gl->osc133_data.out_start_col;
		line->output_end_col = gl->osc133_data.out_end_col;
		line->exit_status = gl->osc133_data.exit_status;
		line->cellused = gl->cellused;
		line->cellsize = gl->cellsize;
		if (line->cellused > line->cellsize ||
		    line->cellsize > gd->sx) {
			restart_set_cause(cause, "invalid grid line size");
			return (-1);
		}
		if (line->cellsize == 0)
			continue;
		line->cells = restart_calloc(budget, line->cellsize,
		    sizeof *line->cells, cause);
		if (line->cells == NULL)
			return (-1);
		for (x = 0; x < line->cellsize; x++) {
			grid_get_cell((struct grid *)gd, x, y, &gc);
			gc.flags &= ~GRID_FLAG_SELECTED;
			if (restart_cell_capture(&gc, &line->cells[x],
			    cause) != 0)
				return (-1);
		}
	}
	return (0);
}

/* Check a captured hyperlink number is present. */
static int
restart_captured_link_exists(const struct restart_links *links, uint32_t inner)
{
	size_t left = 0, right = links->count, middle;

	while (left < right) {
		middle = left + (right - left) / 2;
		if (links->items[middle].inner == inner)
			return (1);
		if (links->items[middle].inner < inner)
			left = middle + 1;
		else
			right = middle;
	}
	return (0);
}

/* Drop cell links with no hyperlink behind them. */
static void
restart_grid_normalize_links(struct restart_grid *grid,
    const struct restart_links *links)
{
	size_t y;
	u_int x;

	for (y = 0; y < grid->line_count; y++) {
		for (x = 0; x < grid->lines[y].cellsize; x++) {
			if (grid->lines[y].cells[x].link != 0 &&
			    !restart_captured_link_exists(links,
			    grid->lines[y].cells[x].link))
				grid->lines[y].cells[x].link = 0;
		}
	}
}

/* Capture a pane's screen. */
static int
restart_screen_capture(const struct window_pane *wp,
    struct restart_screen *out, struct restart_budget *budget, char **cause)
{
	const struct screen *s = &wp->base;
	size_t tabsize;
	u_int i;

	memset(out, 0, sizeof *out);
	if (restart_capture_string(s->title, &out->title, budget, cause) != 0)
		return (-1);
	if (s->path != NULL &&
	    restart_capture_string(s->path, &out->path, budget, cause) != 0)
		return (-1);
	if (restart_titles_capture(s, &out->titles, budget, cause) != 0 ||
	    restart_grid_capture(s->grid, &out->grid, budget, cause) != 0)
		return (-1);
	out->cx = s->cx;
	out->cy = s->cy;
	out->cstyle = s->cstyle;
	out->default_cstyle = s->default_cstyle;
	out->ccolour = s->ccolour;
	out->default_ccolour = s->default_ccolour;
	out->rupper = s->rupper;
	out->rlower = s->rlower;
	out->mode = s->mode;
	out->default_mode = s->default_mode;
	out->saved.cx = s->saved_cx;
	out->saved.cy = s->saved_cy;
	if ((s->saved_cx == UINT_MAX) != (s->saved_cy == UINT_MAX)) {
		restart_set_cause(cause, "invalid saved cursor");
		return (-1);
	}
	if (restart_cell_capture(s->saved_cx == UINT_MAX ? &grid_default_cell :
	    &s->saved_cell, &out->saved.cell, cause) != 0)
		return (-1);
	out->saved.grid_flags = s->saved_flags & GRID_HISTORY;
	if (s->saved_grid != NULL) {
		out->have_saved_grid = 1;
		if (restart_grid_capture(s->saved_grid, &out->saved_grid,
		    budget, cause) != 0)
			return (-1);
	}
	tabsize = (s->grid->sx + 7) / 8;
	if (restart_capture_bytes(s->tabs, tabsize, &out->tabs, budget,
	    cause) != 0)
		return (-1);
	if (tabsize != 0 && s->grid->sx % 8 != 0) {
		u_char mask = (1U << (s->grid->sx % 8)) - 1;
		out->tabs.data[tabsize - 1] &= mask;
	}
	if (restart_links_capture(s->hyperlinks, &out->links, budget,
	    cause) != 0)
		return (-1);
	restart_grid_normalize_links(&out->grid, &out->links);
	if (out->have_saved_grid)
		restart_grid_normalize_links(&out->saved_grid, &out->links);
#ifdef ENABLE_SIXEL
	if (restart_images_capture(s, 0, &out->images, budget, cause) != 0 ||
	    restart_images_capture(s, 1, &out->saved_images, budget,
	    cause) != 0)
		return (-1);
#endif
	out->progress_state = s->progress_bar.state;
	out->progress = s->progress_bar.progress;
	for (i = 0; i < out->links.count; i++) {
		if (out->links.items[i].inner >= out->links.next_inner) {
			restart_set_cause(cause, "invalid hyperlink next ID");
			return (-1);
		}
	}
	return (0);
}

/* Capture a parser for a pane with none. */
static int
restart_parser_capture_ground(struct restart_parser *out,
    struct restart_budget *budget, char **cause)
{
	memset(out, 0, sizeof *out);
	if (restart_capture_string("ground", &out->state, budget, cause) != 0 ||
	    restart_cell_capture(&grid_default_cell, &out->cell.cell,
	    cause) != 0 ||
	    restart_cell_capture(&grid_default_cell, &out->old_cell.cell,
	    cause) != 0)
		return (-1);
	return (0);
}

/* Capture a UTF-8 character. */
static void
restart_utf8_capture(const struct utf8_data *from, struct restart_utf8 *to)
{
	memset(to, 0, sizeof *to);
	to->have = from->have;
	to->size = from->size;
	to->width = from->width;
	memcpy(to->data, from->data, from->have);
}

/* Check a parser state is one that arms the ground timer. */
int
restart_parser_timer_state(const char *state)
{
	return (strncmp(state, "dcs_", 4) == 0 ||
	    strcmp(state, "osc_string") == 0 ||
	    strcmp(state, "apc_string") == 0 ||
	    strcmp(state, "rename_string") == 0 ||
	    strcmp(state, "consume_st") == 0);
}

/* Build a live parser state from a decoded one. */
int
restart_parser_build(const struct restart_parser *in,
    struct input_parser_state *out, char **cause)
{
	memset(out, 0, sizeof *out);
	out->state = in->state;
	if (restart_cell_build(&in->cell.cell, &out->cell.cell, cause) != 0 ||
	    restart_cell_build(&in->old_cell.cell, &out->old_cell.cell,
	    cause) != 0)
		return (-1);
	out->cell.set = in->cell.set;
	out->cell.g0set = in->cell.g0set;
	out->cell.g1set = in->cell.g1set;
	out->old_cell.set = in->old_cell.set;
	out->old_cell.g0set = in->old_cell.g0set;
	out->old_cell.g1set = in->old_cell.g1set;
	out->old_cx = in->old_cx;
	out->old_cy = in->old_cy;
	out->old_mode = in->old_mode;

	if (in->intermediate.size != 0) {
		memcpy(out->interm_buf, in->intermediate.data,
		    in->intermediate.size);
	}
	out->interm_len = in->intermediate.size;
	if (in->parameter.size != 0)
		memcpy(out->param_buf, in->parameter.data, in->parameter.size);
	out->param_len = in->parameter.size;
	out->input_buf = in->input.data;
	out->input_len = in->input.size;
	out->input_end = in->input_end;

	out->utf8data.have = in->utf8.have;
	out->utf8data.size = in->utf8.size;
	out->utf8data.width = in->utf8.width;
	memcpy(out->utf8data.data, in->utf8.data, in->utf8.have);
	out->utf8started = in->utf8_started;
	out->last.have = in->last_utf8.have;
	out->last.size = in->last_utf8.size;
	out->last.width = in->last_utf8.width;
	memcpy(out->last.data, in->last_utf8.data, in->last_utf8.have);

	out->flags = in->flags;
	return (0);
}

/* Capture a pane's parser. */
static int
restart_parser_capture(const struct input_ctx *ictx,
    struct restart_parser *out, struct restart_budget *budget, char **cause)
{
	struct input_parser_state	 ips;
	int				 error = -1;

	memset(out, 0, sizeof *out);
	input_save_parser(ictx, &ips);

	if (restart_capture_string(ips.state, &out->state, budget, cause) != 0)
		goto out;
	if (restart_cell_capture(&ips.cell.cell, &out->cell.cell, cause) != 0 ||
	    restart_cell_capture(&ips.old_cell.cell, &out->old_cell.cell,
	    cause) != 0)
		goto out;
	out->cell.set = ips.cell.set;
	out->cell.g0set = ips.cell.g0set;
	out->cell.g1set = ips.cell.g1set;
	out->old_cell.set = ips.old_cell.set;
	out->old_cell.g0set = ips.old_cell.g0set;
	out->old_cell.g1set = ips.old_cell.g1set;
	out->old_cx = ips.old_cx;
	out->old_cy = ips.old_cy;
	out->old_mode = ips.old_mode;
	if (restart_capture_bytes(ips.interm_buf, ips.interm_len,
	    &out->intermediate, budget, cause) != 0 ||
	    restart_capture_bytes(ips.param_buf, ips.param_len,
	    &out->parameter, budget, cause) != 0 ||
	    restart_capture_bytes(ips.input_buf, ips.input_len, &out->input,
	    budget, cause) != 0)
		goto out;
	out->input_end = ips.input_end;
	out->utf8_started = ips.utf8started;
	if (ips.utf8started)
		restart_utf8_capture(&ips.utf8data, &out->utf8);
	restart_utf8_capture(&ips.last, &out->last_utf8);
	out->flags = ips.flags;
	error = 0;

out:
	input_free_parser_state(&ips);
	return (error);
}

/* Capture a pane's terminal. */
static int
restart_terminal_capture(const struct window_pane *wp,
    struct restart_terminal **out, struct restart_budget *budget, char **cause)
{
	struct restart_terminal		*terminal;
	struct restart_palette_entry	*item;
	struct evbuffer			*input, *output;
	const u_char			*input_data = NULL;
	size_t				 input_size, used;
	u_int				 i, count = 0;

	*out = NULL;
	terminal = restart_calloc(budget, 1, sizeof *terminal, cause);
	if (terminal == NULL)
		return (-1);
	terminal->palette.fg = wp->palette.fg;
	terminal->palette.bg = wp->palette.bg;
	if (!restart_colour_valid(terminal->palette.fg) ||
	    !restart_colour_valid(terminal->palette.bg))
		goto invalid;
	if (wp->palette.palette != NULL) {
		for (i = 0; i < 256; i++) {
			if (wp->palette.palette[i] != -1)
				count++;
		}
	}
	if (count != 0) {
		terminal->palette.items = restart_calloc(budget, count,
		    sizeof *terminal->palette.items, cause);
		if (terminal->palette.items == NULL)
			goto fail;
		for (i = 0; i < 256; i++) {
			if (wp->palette.palette[i] == -1)
				continue;
			item = &terminal->palette.items[
			    terminal->palette.count];
			item->index = i;
			item->value = wp->palette.palette[i];
			if (!restart_colour_valid(item->value))
				goto invalid;
			terminal->palette.count++;
		}
	}
	if (restart_screen_capture(wp, &terminal->screen, budget, cause) != 0)
		goto fail;
	if (wp->ictx == NULL) {
		if (restart_parser_capture_ground(&terminal->parser, budget,
		    cause) != 0)
			goto fail;
	} else if (restart_parser_capture(wp->ictx, &terminal->parser, budget,
	    cause) != 0)
		goto fail;
	if (wp->event != NULL) {
		input = wp->event->input;
		output = wp->event->output;
		input_size = EVBUFFER_LENGTH(input);
		if (wp->offset.used < wp->base_offset)
			goto invalid;
		used = wp->offset.used - wp->base_offset;
		if (used > input_size)
			goto invalid;
		if (input_size > used)
			input_data = EVBUFFER_DATA(input) + used;
		if (restart_capture_bytes(input_data,
		    input_size - used, &terminal->pty_input, budget,
		    cause) != 0 ||
		    restart_capture_bytes(EVBUFFER_DATA(output),
		    EVBUFFER_LENGTH(output), &terminal->pty_output, budget,
		    cause) != 0)
			goto fail;
	}
#ifdef ENABLE_SIXEL
	if (terminal->screen.images.count != 0 ||
	    terminal->screen.saved_images.count != 0)
		terminal->features |= RESTART_FEATURE_SIXEL;
#endif
	*out = terminal;
	return (0);

invalid:
	restart_set_cause(cause, "invalid live terminal state");
fail:
	restart_terminal_free(terminal);
	return (-1);
}

/* Write a cell. */
static int
restart_write_cell(struct restart_writer *rw,
    struct restart_write_scope *scope, uint16_t type,
    const struct restart_cell *cell, char **cause)
{
	u_char data[24 + UTF8_SIZE];

	data[0] = cell->size;
	data[1] = cell->width;
	data[2] = cell->flags;
	data[3] = 0;
	restart_put16(data + 4, cell->attr);
	restart_put16(data + 6, 0);
	restart_put32(data + 8, restart_s32_encode(cell->fg));
	restart_put32(data + 12, restart_s32_encode(cell->bg));
	restart_put32(data + 16, restart_s32_encode(cell->us));
	restart_put32(data + 20, cell->link);
	memcpy(data + 24, cell->data, cell->size);
	return (restart_write_record(rw, scope, type, RESTART_RECORD_REQUIRED,
	    data, 24 + cell->size, cause));
}

/* Write a string list. */
static int
restart_write_string_list(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type,
    const struct restart_string_list *list, const char *what, char **cause)
{
	struct restart_write_scope scope;
	size_t i;

	if (restart_write_container_begin(rw, parent, type,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0)
		return (-1);
	for (i = 0; i < list->count; i++) {
		if (restart_write_string(rw, &scope, 1, list->items[i], what,
		    cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

/* Write a grid. */
static int
restart_write_grid(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type,
    const struct restart_grid *grid, char **cause)
{
	struct restart_write_scope scope, line_scope;
	u_char data[20];
	u_char osc[9];
	u_char *cell_data;
	size_t y;
	u_int x;

	if (restart_write_container_begin(rw, parent, type,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0)
		return (-1);
	restart_put32(data, grid->sx);
	restart_put32(data + 4, grid->sy);
	restart_put32(data + 8, grid->hsize);
	restart_put32(data + 12, grid->hlimit);
	restart_put32(data + 16, grid->hscrolled);
	if (restart_write_record(rw, &scope, RESTART_GRID_DIMENSIONS,
	    RESTART_RECORD_REQUIRED, data, 20, cause) != 0 ||
	    restart_write_u32(rw, &scope, RESTART_GRID_FLAGS, grid->flags,
	    cause) != 0)
		return (-1);
	restart_put32(data, grid->scroll_added);
	restart_put32(data + 4, grid->scroll_collected);
	restart_put32(data + 8, grid->scroll_generation);
	if (restart_write_record(rw, &scope, RESTART_GRID_COUNTERS,
	    RESTART_RECORD_REQUIRED, data, 12, cause) != 0)
		return (-1);
	for (y = 0; y < grid->line_count; y++) {
		const struct restart_line *line = &grid->lines[y];

		if (restart_write_container_begin(rw, &scope,
		    RESTART_GRID_LINE, RESTART_RECORD_REQUIRED, &line_scope,
		    cause) != 0 ||
		    restart_write_u32(rw, &line_scope, RESTART_LINE_INDEX, y,
		    cause) != 0 ||
		    restart_write_u16(rw, &line_scope, RESTART_LINE_FLAGS,
		    line->flags, cause) != 0 ||
		    restart_write_u32(rw, &line_scope, RESTART_LINE_TIME,
		    line->time, cause) != 0)
			return (-1);
		restart_put16(osc, line->prompt_col);
		restart_put16(osc + 2, line->command_col);
		restart_put16(osc + 4, line->output_start_col);
		restart_put16(osc + 6, line->output_end_col);
		osc[8] = line->exit_status;
		if (restart_write_record(rw, &line_scope, RESTART_LINE_OSC133,
		    RESTART_RECORD_REQUIRED, osc, sizeof osc, cause) != 0 ||
		    restart_write_u16(rw, &line_scope, RESTART_LINE_CELLUSED,
		    line->cellused, cause) != 0 ||
		    restart_write_u16(rw, &line_scope, RESTART_LINE_CELLSIZE,
		    line->cellsize, cause) != 0)
			return (-1);
		for (x = 0; x < line->cellsize; x++) {
			cell_data = malloc(4 + 24 + line->cells[x].size);
			if (cell_data == NULL) {
				restart_set_cause(cause, "out of memory");
				return (-1);
			}
			restart_put32(cell_data, x);
			cell_data[4] = line->cells[x].size;
			cell_data[5] = line->cells[x].width;
			cell_data[6] = line->cells[x].flags;
			cell_data[7] = 0;
			restart_put16(cell_data + 8, line->cells[x].attr);
			restart_put16(cell_data + 10, 0);
			restart_put32(cell_data + 12,
			    restart_s32_encode(line->cells[x].fg));
			restart_put32(cell_data + 16,
			    restart_s32_encode(line->cells[x].bg));
			restart_put32(cell_data + 20,
			    restart_s32_encode(line->cells[x].us));
			restart_put32(cell_data + 24, line->cells[x].link);
			memcpy(cell_data + 28, line->cells[x].data,
			    line->cells[x].size);
			if (restart_write_record(rw, &line_scope,
			    RESTART_LINE_CELL, RESTART_RECORD_REQUIRED,
			    cell_data,
			    28 + line->cells[x].size, cause) != 0) {
				free(cell_data);
				return (-1);
			}
			free(cell_data);
		}
		if (restart_write_container_end(rw, &line_scope, cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

/* Write the hyperlinks. */
static int
restart_write_links(struct restart_writer *rw,
    struct restart_write_scope *parent, const struct restart_links *links,
    char **cause)
{
	struct restart_write_scope scope, entry;
	size_t i;

	if (restart_write_container_begin(rw, parent,
	    RESTART_SCREEN_HYPERLINKS, RESTART_RECORD_REQUIRED, &scope,
	    cause) != 0 ||
	    restart_write_u32(rw, &scope, RESTART_LINKS_NEXT_INNER,
	    links->next_inner, cause) != 0)
		return (-1);
	for (i = 0; i < links->count; i++) {
		if (restart_write_container_begin(rw, &scope,
		    RESTART_LINKS_ENTRY, RESTART_RECORD_REQUIRED, &entry,
		    cause) != 0 ||
		    restart_write_u32(rw, &entry, RESTART_LINK_INNER,
		    links->items[i].inner, cause) != 0 ||
		    restart_write_string(rw, &entry, RESTART_LINK_INTERNAL_ID,
		    links->items[i].internal_id, "hyperlink id", cause) != 0 ||
		    restart_write_string(rw, &entry, RESTART_LINK_EXTERNAL_ID,
		    links->items[i].external_id, "hyperlink external id",
		    cause) != 0 ||
		    restart_write_string(rw, &entry, RESTART_LINK_URI,
		    links->items[i].uri, "hyperlink URI", cause) != 0 ||
		    restart_write_container_end(rw, &entry, cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

/* Write the images. */
#ifdef ENABLE_SIXEL
static int
restart_write_images(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type,
    const struct restart_image_list *images, char **cause)
{
	struct restart_write_scope list, image;
	u_char data[8];
	size_t i;

	if (images->count == 0)
		return (0);
	if (restart_write_container_begin(rw, parent, type,
	    RESTART_RECORD_REQUIRED, &list, cause) != 0)
		return (-1);
	for (i = 0; i < images->count; i++) {
		if (restart_write_container_begin(rw, &list, 1,
		    RESTART_RECORD_REQUIRED, &image, cause) != 0)
			return (-1);
		restart_put32(data, images->items[i].px);
		restart_put32(data + 4, images->items[i].py);
		if (restart_write_record(rw, &image, RESTART_IMAGE_POSITION,
		    RESTART_RECORD_REQUIRED, data, sizeof data, cause) != 0)
			return (-1);
		restart_put32(data, images->items[i].sx);
		restart_put32(data + 4, images->items[i].sy);
		if (restart_write_record(rw, &image, RESTART_IMAGE_CELL_SIZE,
		    RESTART_RECORD_REQUIRED, data, sizeof data, cause) != 0)
			return (-1);
		restart_put32(data, images->items[i].xpixel);
		restart_put32(data + 4, images->items[i].ypixel);
		if (restart_write_record(rw, &image, RESTART_IMAGE_PIXEL_CELL,
		    RESTART_RECORD_REQUIRED, data, sizeof data, cause) != 0 ||
		    restart_write_stream(rw, &image, RESTART_IMAGE_DCS,
		    images->items[i].dcs.data, images->items[i].dcs.size,
		    "image", cause) != 0)
			return (-1);
		restart_put32(data, images->items[i].psx);
		restart_put32(data + 4, images->items[i].psy);
		if (restart_write_record(rw, &image, RESTART_IMAGE_PIXEL_EXTENT,
		    RESTART_RECORD_REQUIRED, data, sizeof data, cause) != 0 ||
		    restart_write_container_end(rw, &image, cause) != 0)
			return (-1);
	}
	rw->features |= RESTART_FEATURE_SIXEL;
	return (restart_write_container_end(rw, &list, cause));
}
#endif

/* Write a screen. */
static int
restart_write_screen(struct restart_writer *rw,
    struct restart_write_scope *parent, const struct restart_screen *screen,
    char **cause)
{
	struct restart_write_scope scope, saved;
	u_char data[16];

	if (restart_write_container_begin(rw, parent, RESTART_TERMINAL_SCREEN,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0 ||
	    restart_write_string(rw, &scope, RESTART_SCREEN_TITLE,
	    screen->title, "pane title", cause) != 0)
		return (-1);
	if (screen->path != NULL && restart_write_string(rw, &scope,
	    RESTART_SCREEN_PATH, screen->path, "pane path", cause) != 0)
		return (-1);
	if (restart_write_string_list(rw, &scope, RESTART_SCREEN_TITLE_STACK,
	    &screen->titles, "saved pane title", cause) != 0 ||
	    restart_write_grid(rw, &scope, RESTART_SCREEN_GRID, &screen->grid,
	    cause) != 0)
		return (-1);
	restart_put32(data, screen->cx);
	restart_put32(data + 4, screen->cy);
	data[8] = screen->cstyle;
	restart_put32(data + 9, restart_s32_encode(screen->ccolour));
	if (restart_write_record(rw, &scope, RESTART_SCREEN_CURSOR,
	    RESTART_RECORD_REQUIRED, data, 13, cause) != 0)
		return (-1);
	data[0] = screen->default_cstyle;
	restart_put32(data + 1, restart_s32_encode(screen->default_ccolour));
	if (restart_write_record(rw, &scope, RESTART_SCREEN_DEFAULT_CURSOR,
	    RESTART_RECORD_REQUIRED, data, 5, cause) != 0)
		return (-1);
	restart_put32(data, screen->rupper);
	restart_put32(data + 4, screen->rlower);
	if (restart_write_record(rw, &scope, RESTART_SCREEN_SCROLL_REGION,
	    RESTART_RECORD_REQUIRED, data, 8, cause) != 0)
		return (-1);
	restart_put32(data, screen->mode);
	restart_put32(data + 4, screen->default_mode);
	if (restart_write_record(rw, &scope, RESTART_SCREEN_MODES,
	    RESTART_RECORD_REQUIRED, data, 8, cause) != 0 ||
	    restart_write_container_begin(rw, &scope, RESTART_SCREEN_SAVED,
	    RESTART_RECORD_REQUIRED, &saved, cause) != 0 ||
	    restart_write_u32(rw, &saved, 1, screen->saved.cx, cause) != 0 ||
	    restart_write_u32(rw, &saved, 2, screen->saved.cy, cause) != 0 ||
	    restart_write_cell(rw, &saved, 3, &screen->saved.cell,
	    cause) != 0 ||
	    restart_write_u32(rw, &saved, 4, screen->saved.grid_flags,
	    cause) != 0 || restart_write_container_end(rw, &saved, cause) != 0)
		return (-1);
	if (screen->have_saved_grid && restart_write_grid(rw, &scope,
	    RESTART_SCREEN_SAVED_GRID, &screen->saved_grid, cause) != 0)
		return (-1);
	if (restart_write_blob(rw, &scope, RESTART_SCREEN_TABS,
	    screen->tabs.data, screen->tabs.size, "tab stops", cause) != 0 ||
	    restart_write_links(rw, &scope, &screen->links, cause) != 0)
		return (-1);
#ifdef ENABLE_SIXEL
	if (restart_write_images(rw, &scope, RESTART_SCREEN_IMAGES,
	    &screen->images, cause) != 0 || restart_write_images(rw, &scope,
	    RESTART_SCREEN_SAVED_IMAGES, &screen->saved_images, cause) != 0)
		return (-1);
#endif
	data[0] = screen->progress_state;
	restart_put32(data + 1, restart_s32_encode(screen->progress));
	if (restart_write_record(rw, &scope, RESTART_SCREEN_PROGRESS,
	    RESTART_RECORD_REQUIRED, data, 5, cause) != 0)
		return (-1);
	return (restart_write_container_end(rw, &scope, cause));
}

/* Write a parser cell. */
static int
restart_write_parser_cell(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type,
    const struct restart_parser_cell *cell, char **cause)
{
	struct restart_write_scope scope;

	if (restart_write_container_begin(rw, parent, type,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0 ||
	    restart_write_cell(rw, &scope, 1, &cell->cell, cause) != 0 ||
	    restart_write_u8(rw, &scope, 2, cell->set, cause) != 0 ||
	    restart_write_u8(rw, &scope, 3, cell->g0set, cause) != 0 ||
	    restart_write_u8(rw, &scope, 4, cell->g1set, cause) != 0)
		return (-1);
	return (restart_write_container_end(rw, &scope, cause));
}

/* Write a UTF-8 character. */
static int
restart_write_utf8(struct restart_writer *rw,
    struct restart_write_scope *scope, uint16_t type,
    const struct restart_utf8 *utf8, char **cause)
{
	u_char data[4 + UTF8_SIZE];

	data[0] = utf8->have;
	data[1] = utf8->size;
	data[2] = utf8->width;
	data[3] = 0;
	memcpy(data + 4, utf8->data, utf8->have);
	return (restart_write_record(rw, scope, type, RESTART_RECORD_REQUIRED,
	    data, 4 + utf8->have, cause));
}

/* Write a parser. */
static int
restart_write_parser(struct restart_writer *rw,
    struct restart_write_scope *parent, const struct restart_parser *parser,
    char **cause)
{
	struct restart_write_scope scope;
	u_char data[8];

	if (restart_write_container_begin(rw, parent, RESTART_TERMINAL_PARSER,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0 ||
	    restart_write_string(rw, &scope, RESTART_PARSER_STATE,
	    parser->state, "parser state name", cause) != 0 ||
	    restart_write_parser_cell(rw, &scope, RESTART_PARSER_CELL,
	    &parser->cell, cause) != 0 ||
	    restart_write_parser_cell(rw, &scope, RESTART_PARSER_OLD_CELL,
	    &parser->old_cell, cause) != 0)
		return (-1);
	restart_put32(data, parser->old_cx);
	restart_put32(data + 4, parser->old_cy);
	if (restart_write_record(rw, &scope, RESTART_PARSER_OLD_CURSOR,
	    RESTART_RECORD_REQUIRED, data, 8, cause) != 0 ||
	    restart_write_u32(rw, &scope, RESTART_PARSER_OLD_MODE,
	    parser->old_mode, cause) != 0 ||
	    restart_write_blob(rw, &scope, RESTART_PARSER_INTERMEDIATE,
	    parser->intermediate.data, parser->intermediate.size,
	    "parser intermediate", cause) != 0 ||
	    restart_write_blob(rw, &scope, RESTART_PARSER_PARAMETER,
	    parser->parameter.data, parser->parameter.size,
	    "parser parameter", cause) != 0 ||
	    restart_write_stream(rw, &scope, RESTART_PARSER_INPUT,
	    parser->input.data, parser->input.size, "parser input",
	    cause) != 0 ||
	    restart_write_u8(rw, &scope, RESTART_PARSER_INPUT_END,
	    parser->input_end, cause) != 0 ||
	    restart_write_utf8(rw, &scope, RESTART_PARSER_UTF8,
	    &parser->utf8, cause) != 0 ||
	    restart_write_u8(rw, &scope, RESTART_PARSER_UTF8_STARTED,
	    parser->utf8_started, cause) != 0 ||
	    restart_write_utf8(rw, &scope, RESTART_PARSER_LAST_UTF8,
	    &parser->last_utf8, cause) != 0 ||
	    restart_write_u32(rw, &scope, RESTART_PARSER_FLAGS,
	    parser->flags, cause) != 0)
		return (-1);
	return (restart_write_container_end(rw, &scope, cause));
}

/* Write a palette. */
static int
restart_write_palette(struct restart_writer *rw,
    struct restart_write_scope *parent, const struct restart_palette *palette,
    char **cause)
{
	struct restart_write_scope scope;
	u_char data[6];
	size_t i;

	if (restart_write_container_begin(rw, parent, RESTART_TERMINAL_PALETTE,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0 ||
	    restart_write_s32(rw, &scope, RESTART_PALETTE_FG, palette->fg,
	    cause) != 0 || restart_write_s32(rw, &scope, RESTART_PALETTE_BG,
	    palette->bg, cause) != 0)
		return (-1);
	for (i = 0; i < palette->count; i++) {
		restart_put16(data, palette->items[i].index);
		restart_put32(data + 2,
		    restart_s32_encode(palette->items[i].value));
		if (restart_write_record(rw, &scope, RESTART_PALETTE_OVERRIDE,
		    RESTART_RECORD_REQUIRED, data, sizeof data, cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

/* Write the records of a terminal. */
static int
restart_terminal_write_payload(struct restart_writer *rw,
    const struct restart_terminal *terminal, char **cause)
{
	if (restart_write_palette(rw, &rw->top, &terminal->palette,
	    cause) != 0 || restart_write_screen(rw, &rw->top,
	    &terminal->screen, cause) != 0 || restart_write_parser(rw,
	    &rw->top, &terminal->parser, cause) != 0 ||
	    restart_write_stream(rw, &rw->top, RESTART_TERMINAL_PTY_INPUT,
	    terminal->pty_input.data, terminal->pty_input.size,
	    "pane input", cause) != 0 ||
	    restart_write_stream(rw, &rw->top, RESTART_TERMINAL_PTY_OUTPUT,
	    terminal->pty_output.data, terminal->pty_output.size,
	    "pane output", cause) != 0)
		return (-1);
	rw->features |= terminal->features;
	return (0);
}

/* Write a decoded terminal into a record. */
int
restart_terminal_write_decoded_nested(struct restart_writer *rw,
    const struct restart_terminal *terminal, char **cause)
{
	struct restart_write_scope parent = rw->top;
	uint64_t parent_features = rw->features, nested_features;
	int error = -1;

	rw->features = 0;
	if (restart_write_envelope_begin(rw, RESTART_KIND_TERMINAL,
	    cause) != 0 ||
	    restart_terminal_write_payload(rw, terminal, cause) != 0 ||
	    restart_write_envelope_end(rw, cause) != 0)
		goto out;
	error = 0;
out:
	nested_features = rw->features;
	rw->top = parent;
	rw->features = parent_features | nested_features;
	return (error);
}

/* Write a pane's terminal into a record. */
int
restart_terminal_write_nested(struct restart_writer *rw,
    const struct window_pane *wp, char **cause)
{
	struct restart_terminal	*terminal = NULL;
	int			 error;

	if (restart_terminal_capture(wp, &terminal, &rw->budget, cause) != 0)
		return (-1);
	error = restart_terminal_write_decoded_nested(rw, terminal, cause);
	restart_terminal_free(terminal);
	return (error);
}

/* Check a record is marked required. */
static int
restart_record_required(const struct restart_record *record, char **cause)
{
	if (record->flags == RESTART_RECORD_REQUIRED)
		return (0);
	restart_set_cause(cause, "invalid restart record flags");
	return (-1);
}

/* Read a cell. */
static int
restart_read_cell(struct restart_record *record, struct restart_cell *cell,
    char **cause)
{
	const u_char *data;
	size_t size;
	struct grid_cell gc;

	memset(cell, 0, sizeof *cell);
	size = ibuf_size(&record->payload);
	if (size < 24) {
		restart_set_cause(cause, "short restart cell");
		return (-1);
	}
	data = ibuf_data(&record->payload);
	cell->size = data[0];
	cell->width = data[1];
	cell->flags = data[2];
	cell->attr = restart_get16(data + 4);
	cell->fg = restart_s32_decode(restart_get32(data + 8));
	cell->bg = restart_s32_decode(restart_get32(data + 12));
	cell->us = restart_s32_decode(restart_get32(data + 16));
	cell->link = restart_get32(data + 20);
	if (data[3] != 0 || restart_get16(data + 6) != 0 ||
	    cell->size == 0 || cell->size > UTF8_SIZE ||
	    size != 24U + cell->size) {
		restart_set_cause(cause, "invalid restart cell length");
		return (-1);
	}
	memcpy(cell->data, data + 24, cell->size);
	if (ibuf_skip(&record->payload, size) != 0 ||
	    restart_cell_build(cell, &gc, cause) != 0)
		return (-1);
	return (0);
}

/* Read a string list. */
static int
restart_read_string_list(struct restart_record *record,
    struct restart_string_list *list, struct restart_budget *budget,
    char **cause)
{
	struct restart_reader rr;
	struct restart_record item;
	char **new;
	int found;

	memset(list, 0, sizeof *list);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &item, cause)) == 1) {
		if (item.type != 1) {
			if (item.flags & RESTART_RECORD_REQUIRED) {
				restart_set_cause(cause,
				    "unknown required string-list record");
				return (-1);
			}
			continue;
		}
		if (restart_record_required(&item, cause) != 0)
			return (-1);
		new = restart_grow(budget, list->items, list->count,
		    list->count + 1, sizeof *list->items, cause);
		if (new == NULL)
			return (-1);
		list->items = new;
		list->items[list->count] = NULL;
		if (restart_read_string(&item, &list->items[list->count],
		    budget, cause) != 0)
			return (-1);
		list->count++;
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	return (0);
}

/* Compare palette entries by index. */
static int
restart_palette_compare(const void *a0, const void *b0)
{
	const struct restart_palette_entry *a = a0;
	const struct restart_palette_entry *b = b0;

	if (a->index < b->index)
		return (-1);
	if (a->index > b->index)
		return (1);
	return (0);
}

/* Read a palette. */
static int
restart_read_palette(struct restart_record *record,
    struct restart_palette *palette, struct restart_budget *budget,
    char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	struct restart_palette_entry *new;
	const u_char *data;
	u_char override_seen[256] = { 0 };
	uint32_t seen = 0;
	uint16_t index;
	int32_t value;
	int found;

	memset(palette, 0, sizeof *palette);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_PALETTE_FG ||
		    child.type > RESTART_PALETTE_OVERRIDE) {
			if (child.flags & RESTART_RECORD_REQUIRED)
				goto unknown;
			continue;
		}
		if (restart_record_required(&child, cause) != 0)
			return (-1);
		if (child.type == RESTART_PALETTE_FG ||
		    child.type == RESTART_PALETTE_BG) {
			uint32_t bit = 1U << (child.type - 1);
			if (seen & bit || restart_read_s32(&child, &value,
			    cause) != 0)
				goto invalid;
			if (!restart_colour_valid(value))
				goto invalid;
			if (child.type == RESTART_PALETTE_FG)
				palette->fg = value;
			else
				palette->bg = value;
			seen |= bit;
			continue;
		}
		if (palette->count == 256 || ibuf_size(&child.payload) != 6)
			goto invalid;
		data = ibuf_data(&child.payload);
		index = restart_get16(data);
		value = restart_s32_decode(restart_get32(data + 2));
		if (index > 255 || override_seen[index] ||
		    !restart_colour_valid(value) || value == -1)
			goto invalid;
		new = restart_grow(budget, palette->items, palette->count,
		    palette->count + 1, sizeof *palette->items, cause);
		if (new == NULL)
			return (-1);
		palette->items = new;
		palette->items[palette->count].index = index;
		palette->items[palette->count].value = value;
		override_seen[index] = 1;
		palette->count++;
		ibuf_skip(&child.payload, 6);
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if (seen != 3)
		goto invalid;
	if (palette->count > 1)
		qsort(palette->items, palette->count, sizeof *palette->items,
		    restart_palette_compare);
	return (0);
unknown:
	restart_set_cause(cause, "unknown required palette record");
	return (-1);
invalid:
	restart_set_cause(cause, "invalid restart palette");
	return (-1);
}

/* Read one cell of a line. */
static int
restart_read_line_cell(struct restart_record *record, uint32_t expected,
    struct restart_cell *cell, char **cause)
{
	struct restart_record nested;
	uint32_t x;
	size_t size = ibuf_size(&record->payload);

	if (size < 4) {
		restart_set_cause(cause, "short restart line cell");
		return (-1);
	}
	x = restart_get32(ibuf_data(&record->payload));
	if (x != expected || ibuf_skip(&record->payload, 4) != 0) {
		restart_set_cause(cause, "invalid restart cell index");
		return (-1);
	}
	nested.type = 1;
	nested.flags = record->flags;
	nested.payload = record->payload;
	return (restart_read_cell(&nested, cell, cause));
}

/* Read a grid line. */
static int
restart_read_line(struct restart_record *record, uint32_t expected,
    struct restart_line *line, struct restart_budget *budget, char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	struct restart_cell *new;
	const u_char *data;
	uint32_t seen = 0, index;
	uint16_t declared_cellsize = 0;
	int found;

	memset(line, 0, sizeof *line);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_LINE_INDEX ||
		    child.type > RESTART_LINE_CELL) {
			if (child.flags & RESTART_RECORD_REQUIRED)
				goto unknown;
			continue;
		}
		if (restart_record_required(&child, cause) != 0)
			return (-1);
		if (child.type == RESTART_LINE_CELL) {
			if (line->cellsize == UINT16_MAX)
				goto invalid;
			new = restart_grow(budget, line->cells,
			    line->cellsize, (size_t)line->cellsize + 1,
			    sizeof *line->cells, cause);
			if (new == NULL)
				return (-1);
			line->cells = new;
			if (restart_read_line_cell(&child, line->cellsize,
			    &line->cells[line->cellsize], cause) != 0)
				return (-1);
			line->cellsize++;
			continue;
		}
		if (seen & (1U << (child.type - 1)))
			goto invalid;
		seen |= 1U << (child.type - 1);
		switch (child.type) {
		case RESTART_LINE_INDEX:
			if (restart_read_u32(&child, &index, cause) != 0 ||
			    index != expected)
				goto invalid;
			break;
		case RESTART_LINE_FLAGS:
			if (restart_read_u16(&child, &line->flags,
			    cause) != 0 ||
			    (line->flags & ~RESTART_LINE_FLAG_MASK) != 0)
				goto invalid;
			break;
		case RESTART_LINE_TIME:
			if (restart_read_u32(&child, &line->time, cause) != 0)
				return (-1);
			break;
		case RESTART_LINE_OSC133:
			if (ibuf_size(&child.payload) != 9)
				goto invalid;
			data = ibuf_data(&child.payload);
			line->prompt_col = restart_get16(data);
			line->command_col = restart_get16(data + 2);
			line->output_start_col = restart_get16(data + 4);
			line->output_end_col = restart_get16(data + 6);
			line->exit_status = data[8];
			ibuf_skip(&child.payload, 9);
			break;
		case RESTART_LINE_CELLUSED:
			if (restart_read_u16(&child, &line->cellused,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_LINE_CELLSIZE:
			if (restart_read_u16(&child, &declared_cellsize,
			    cause) != 0)
				return (-1);
			if (declared_cellsize > RESTART_MAX_DIMENSION)
				goto invalid;
			break;
		default:
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if ((seen & 0x3f) != 0x3f || line->cellsize != declared_cellsize ||
	    line->cellused > line->cellsize)
		goto invalid;
	return (0);
unknown:
	restart_set_cause(cause, "unknown required grid-line record");
	return (-1);
invalid:
	restart_set_cause(cause, "invalid restart grid line");
	return (-1);
}

/* Read a grid. */
static int
restart_read_grid(struct restart_record *record, struct restart_grid *grid,
    struct restart_budget *budget, char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	struct restart_line *new;
	const u_char *data;
	uint32_t seen = 0, expected_lines;
	size_t line_count;
	int found;

	memset(grid, 0, sizeof *grid);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_GRID_DIMENSIONS ||
		    child.type > RESTART_GRID_LINE) {
			if (child.flags & RESTART_RECORD_REQUIRED)
				goto unknown;
			continue;
		}
		if (restart_record_required(&child, cause) != 0)
			return (-1);
		if (child.type == RESTART_GRID_LINE) {
			new = restart_grow(budget, grid->lines,
			    grid->line_count, grid->line_count + 1,
			    sizeof *grid->lines, cause);
			if (new == NULL)
				return (-1);
			grid->lines = new;
			memset(&grid->lines[grid->line_count], 0,
			    sizeof *grid->lines);
			grid->line_count++;
			if (restart_read_line(&child, grid->line_count - 1,
			    &grid->lines[grid->line_count - 1], budget,
			    cause) != 0)
				return (-1);
			continue;
		}
		if (seen & (1U << (child.type - 1)))
			goto invalid;
		seen |= 1U << (child.type - 1);
		switch (child.type) {
		case RESTART_GRID_DIMENSIONS:
			if (ibuf_size(&child.payload) != 20)
				goto invalid;
			data = ibuf_data(&child.payload);
			grid->sx = restart_get32(data);
			grid->sy = restart_get32(data + 4);
			grid->hsize = restart_get32(data + 8);
			grid->hlimit = restart_get32(data + 12);
			grid->hscrolled = restart_get32(data + 16);
			ibuf_skip(&child.payload, 20);
			break;
		case RESTART_GRID_FLAGS:
			if (restart_read_u32(&child, &grid->flags,
			    cause) != 0 ||
			    (grid->flags & ~1U) != 0)
				goto invalid;
			break;
		case RESTART_GRID_COUNTERS:
			if (ibuf_size(&child.payload) != 12)
				goto invalid;
			data = ibuf_data(&child.payload);
			grid->scroll_added = restart_get32(data);
			grid->scroll_collected = restart_get32(data + 4);
			grid->scroll_generation = restart_get32(data + 8);
			ibuf_skip(&child.payload, 12);
			break;
		default:
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if (seen != 7 || grid->sx < 1 || grid->sx > RESTART_MAX_DIMENSION ||
	    grid->sy < 1 || grid->sy > RESTART_MAX_DIMENSION ||
	    grid->hscrolled > grid->hsize ||
	    restart_size_add(grid->hsize, grid->sy, &line_count) != 0 ||
	    line_count > RESTART_MAX_RECORDS) {
		goto invalid;
	}
	expected_lines = line_count;
	if (grid->line_count != expected_lines)
		goto invalid;
	for (line_count = 0; line_count < grid->line_count; line_count++) {
		if (grid->lines[line_count].cellsize > grid->sx)
			goto invalid;
	}
	return (0);
unknown:
	restart_set_cause(cause, "unknown required grid record");
	return (-1);
invalid:
	restart_set_cause(cause, "invalid restart grid");
	return (-1);
}

/* Compare hyperlinks by number. */
static int
restart_link_compare(const void *left, const void *right)
{
	const struct restart_link *a = left, *b = right;

	if (a->inner < b->inner)
		return (-1);
	if (a->inner > b->inner)
		return (1);
	return (0);
}

/* Read a hyperlink. */
static int
restart_read_link(struct restart_record *record, struct restart_link *link,
    struct restart_budget *budget, char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	uint32_t seen = 0;
	int found;

	memset(link, 0, sizeof *link);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_LINK_INNER ||
		    child.type > RESTART_LINK_URI) {
			if (child.flags & RESTART_RECORD_REQUIRED)
				goto unknown;
			continue;
		}
		if (restart_record_required(&child, cause) != 0 ||
		    (seen & (1U << (child.type - 1))) != 0)
			goto invalid;
		seen |= 1U << (child.type - 1);
		switch (child.type) {
		case RESTART_LINK_INNER:
			if (restart_read_u32(&child, &link->inner, cause) != 0)
				return (-1);
			break;
		case RESTART_LINK_INTERNAL_ID:
			if (restart_read_string(&child, &link->internal_id,
			    budget, cause) != 0)
				return (-1);
			break;
		case RESTART_LINK_EXTERNAL_ID:
			if (restart_read_string(&child, &link->external_id,
			    budget, cause) != 0)
				return (-1);
			break;
		case RESTART_LINK_URI:
			if (ibuf_size(&child.payload) > 1024)
				goto invalid;
			if (restart_read_string(&child, &link->uri, budget,
			    cause) != 0)
				return (-1);
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if (seen != 15 || link->inner == 0 || link->inner == UINT32_MAX)
		goto invalid;
	return (0);
unknown:
	restart_set_cause(cause, "unknown required hyperlink record");
	return (-1);
invalid:
	restart_set_cause(cause, "invalid restart hyperlink");
	return (-1);
}

/* Read the hyperlinks. */
static int
restart_read_links(struct restart_record *record, struct restart_links *links,
    struct restart_budget *budget, char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	struct restart_link *new;
	size_t i, j;
	int found, seen = 0;

	memset(links, 0, sizeof *links);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type == RESTART_LINKS_NEXT_INNER) {
			if (restart_record_required(&child, cause) != 0 ||
			    seen ||
			    restart_read_u32(&child, &links->next_inner,
			    cause) != 0)
				goto invalid;
			seen = 1;
		} else if (child.type == RESTART_LINKS_ENTRY) {
			if (restart_record_required(&child, cause) != 0 ||
			    links->count == hyperlinks_limit() ||
			    budget->hyperlinks == hyperlinks_limit())
				goto invalid;
			new = restart_grow(budget, links->items, links->count,
			    links->count + 1, sizeof *links->items, cause);
			if (new == NULL)
				return (-1);
			links->items = new;
			memset(&links->items[links->count], 0,
			    sizeof *links->items);
			links->count++;
			if (restart_read_link(&child,
			    &links->items[links->count - 1],
			    budget, cause) != 0)
				return (-1);
			budget->hyperlinks++;
		} else if (child.flags & RESTART_RECORD_REQUIRED)
			goto unknown;
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if (!seen || links->next_inner == 0 ||
	    links->next_inner == UINT32_MAX)
		goto invalid;
	if (links->count > 1)
		qsort(links->items, links->count, sizeof *links->items,
		    restart_link_compare);
	for (i = 0; i < links->count; i++) {
		if (links->items[i].inner >= links->next_inner ||
		    (i != 0 &&
		    links->items[i - 1].inner == links->items[i].inner))
			goto invalid;
		if (*links->items[i].internal_id == '\0')
			continue;
		for (j = i + 1; j < links->count; j++) {
			if (strcmp(links->items[i].internal_id,
			    links->items[j].internal_id) == 0 &&
			    strcmp(links->items[i].uri,
			    links->items[j].uri) == 0)
				goto invalid;
		}
	}
	return (0);
unknown:
	restart_set_cause(cause, "unknown required hyperlink-list record");
	return (-1);
invalid:
	restart_set_cause(cause, "invalid restart hyperlink list");
	return (-1);
}

/* Read an image. */
#ifdef ENABLE_SIXEL
static int
restart_read_image(struct restart_record *record, struct restart_image *image,
    struct restart_budget *budget, char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	struct sixel_image *si = NULL;
	const u_char *data;
	uint32_t seen = 0;
	u_int sx, sy;
	int found;

	memset(image, 0, sizeof *image);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_IMAGE_POSITION ||
		    child.type > RESTART_IMAGE_PIXEL_EXTENT) {
			if (child.flags & RESTART_RECORD_REQUIRED)
				goto unknown;
			continue;
		}
		if (restart_record_required(&child, cause) != 0 ||
		    (seen & (1U << (child.type - 1))) != 0)
			goto invalid;
		seen |= 1U << (child.type - 1);
		if (child.type == RESTART_IMAGE_DCS) {
			if (restart_read_stream(&child, &image->dcs, budget,
			    cause) != 0)
				return (-1);
			continue;
		}
		if (ibuf_size(&child.payload) != 8)
			goto invalid;
		data = ibuf_data(&child.payload);
		if (child.type == RESTART_IMAGE_POSITION) {
			image->px = restart_get32(data);
			image->py = restart_get32(data + 4);
		} else if (child.type == RESTART_IMAGE_CELL_SIZE) {
			image->sx = restart_get32(data);
			image->sy = restart_get32(data + 4);
		} else if (child.type == RESTART_IMAGE_PIXEL_CELL) {
			image->xpixel = restart_get32(data);
			image->ypixel = restart_get32(data + 4);
		} else {
			image->psx = restart_get32(data);
			image->psy = restart_get32(data + 4);
		}
		ibuf_skip(&child.payload, 8);
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if (seen != 31 || image->sx < 1 ||
	    image->sx > RESTART_MAX_DIMENSION || image->sy < 1 ||
	    image->sy > RESTART_MAX_DIMENSION || image->xpixel < 1 ||
	    image->xpixel > RESTART_MAX_DIMENSION || image->ypixel < 1 ||
	    image->ypixel > RESTART_MAX_DIMENSION || image->psx < 1 ||
	    image->psx > RESTART_MAX_DIMENSION || image->psy < 1 ||
	    image->psy > RESTART_MAX_DIMENSION)
		goto invalid;
	si = sixel_restart_parse(image->dcs.data, image->dcs.size,
	    image->xpixel, image->ypixel, image->psx, image->psy);
	if (si == NULL)
		goto invalid;
	sixel_size_in_cells(si, &sx, &sy);
	sixel_free(si);
	if (sx != image->sx || sy != image->sy)
		goto invalid;
	return (0);

unknown:
	restart_set_cause(cause, "unknown required image record");
	return (-1);
invalid:
	restart_set_cause(cause, "invalid restart image");
	return (-1);
}

/* Read the images. */
static int
restart_read_images(struct restart_record *record,
    struct restart_image_list *images, struct restart_budget *budget,
    char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	struct restart_image *new;
	int found;

	memset(images, 0, sizeof *images);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (child.flags & RESTART_RECORD_REQUIRED) {
				restart_set_cause(cause,
				    "unknown required image-list record");
				return (-1);
			}
			continue;
		}
		if (restart_record_required(&child, cause) != 0 ||
		    images->count == image_limit() ||
		    budget->images == image_limit())
			goto invalid;
		new = restart_grow(budget, images->items, images->count,
		    images->count + 1, sizeof *images->items, cause);
		if (new == NULL)
			return (-1);
		images->items = new;
		memset(&images->items[images->count], 0,
		    sizeof *images->items);
		images->count++;
		budget->images++;
		if (restart_read_image(&child,
		    &images->items[images->count - 1], budget, cause) != 0)
			return (-1);
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if (images->count == 0)
		goto invalid;
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart image list");
	return (-1);
}
#endif

/* Read the saved cursor. */
static int
restart_read_saved(struct restart_record *record, struct restart_saved *saved,
    struct restart_budget *budget, char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	uint32_t seen = 0;
	int found;

	memset(saved, 0, sizeof *saved);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < 1 || child.type > 4) {
			if (child.flags & RESTART_RECORD_REQUIRED)
				goto unknown;
			continue;
		}
		if (restart_record_required(&child, cause) != 0 ||
		    (seen & (1U << (child.type - 1))) != 0)
			goto invalid;
		seen |= 1U << (child.type - 1);
		switch (child.type) {
		case 1:
			if (restart_read_u32(&child, &saved->cx, cause) != 0)
				return (-1);
			break;
		case 2:
			if (restart_read_u32(&child, &saved->cy, cause) != 0)
				return (-1);
			break;
		case 3:
			if (restart_read_cell(&child, &saved->cell, cause) != 0)
				return (-1);
			break;
		case 4:
			if (restart_read_u32(&child, &saved->grid_flags,
			    cause) != 0 || (saved->grid_flags & ~1U) != 0)
				goto invalid;
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if (seen != 15)
		goto invalid;
	return (0);
unknown:
	restart_set_cause(cause, "unknown required saved-screen record");
	return (-1);
invalid:
	restart_set_cause(cause, "invalid saved-screen state");
	return (-1);
}

/* Check a hyperlink number was read. */
static int
restart_links_have(const struct restart_links *links, uint32_t inner)
{
	struct restart_link key = { .inner = inner };

	if (links->count == 0)
		return (0);
	return (bsearch(&key, links->items, links->count, sizeof *links->items,
	    restart_link_compare) != NULL);
}

/* Check every cell link has a hyperlink behind it. */
static int
restart_grid_links_valid(const struct restart_grid *grid,
    const struct restart_links *links)
{
	size_t y;
	u_int x;

	for (y = 0; y < grid->line_count; y++) {
		for (x = 0; x < grid->lines[y].cellsize; x++) {
			if (grid->lines[y].cells[x].link != 0 &&
			    !restart_links_have(links,
			    grid->lines[y].cells[x].link))
				return (0);
		}
	}
	return (1);
}

/* Read a screen. */
static int
restart_read_screen(struct restart_record *record,
    struct restart_screen *screen, struct restart_budget *budget, char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	const u_char *data;
	uint32_t seen = 0;
	size_t tabs;
	int found;

	memset(screen, 0, sizeof *screen);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_SCREEN_TITLE ||
		    child.type > RESTART_SCREEN_PROGRESS) {
			if (child.flags & RESTART_RECORD_REQUIRED)
				goto unknown;
			continue;
		}
		if (restart_record_required(&child, cause) != 0 ||
		    (seen & (1U << (child.type - 1))) != 0)
			goto invalid;
		seen |= 1U << (child.type - 1);
		switch (child.type) {
		case RESTART_SCREEN_TITLE:
			if (restart_read_string(&child, &screen->title, budget,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_SCREEN_PATH:
			if (restart_read_string(&child, &screen->path, budget,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_SCREEN_TITLE_STACK:
			if (restart_read_string_list(&child, &screen->titles,
			    budget, cause) != 0 ||
			    screen->titles.count > RESTART_MAX_TITLES)
				goto invalid;
			break;
		case RESTART_SCREEN_GRID:
			if (restart_read_grid(&child, &screen->grid, budget,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_SCREEN_CURSOR:
			if (ibuf_size(&child.payload) != 13)
				goto invalid;
			data = ibuf_data(&child.payload);
			screen->cx = restart_get32(data);
			screen->cy = restart_get32(data + 4);
			screen->cstyle = data[8];
			screen->ccolour = restart_s32_decode(
			    restart_get32(data + 9));
			ibuf_skip(&child.payload, 13);
			break;
		case RESTART_SCREEN_DEFAULT_CURSOR:
			if (ibuf_size(&child.payload) != 5)
				goto invalid;
			data = ibuf_data(&child.payload);
			screen->default_cstyle = data[0];
			screen->default_ccolour = restart_s32_decode(
			    restart_get32(data + 1));
			ibuf_skip(&child.payload, 5);
			break;
		case RESTART_SCREEN_SCROLL_REGION:
			if (ibuf_size(&child.payload) != 8)
				goto invalid;
			data = ibuf_data(&child.payload);
			screen->rupper = restart_get32(data);
			screen->rlower = restart_get32(data + 4);
			ibuf_skip(&child.payload, 8);
			break;
		case RESTART_SCREEN_MODES:
			if (ibuf_size(&child.payload) != 8)
				goto invalid;
			data = ibuf_data(&child.payload);
			screen->mode = restart_get32(data);
			screen->default_mode = restart_get32(data + 4);
			ibuf_skip(&child.payload, 8);
			break;
		case RESTART_SCREEN_SAVED:
			if (restart_read_saved(&child, &screen->saved, budget,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_SCREEN_SAVED_GRID:
			screen->have_saved_grid = 1;
			if (restart_read_grid(&child, &screen->saved_grid,
			    budget, cause) != 0)
				return (-1);
			break;
		case RESTART_SCREEN_TABS:
			if (restart_read_blob(&child, &screen->tabs,
			    RESTART_MAX_FIELD, budget, cause) != 0)
				return (-1);
			break;
		case RESTART_SCREEN_HYPERLINKS:
			if (restart_read_links(&child, &screen->links, budget,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_SCREEN_IMAGES:
#ifdef ENABLE_SIXEL
			if (restart_read_images(&child, &screen->images, budget,
			    cause) != 0)
				return (-1);
			break;
#else
			restart_set_cause(cause, "sixel is not supported");
			return (-1);
#endif
		case RESTART_SCREEN_SAVED_IMAGES:
#ifdef ENABLE_SIXEL
			if (restart_read_images(&child, &screen->saved_images,
			    budget, cause) != 0)
				return (-1);
			break;
#else
			restart_set_cause(cause, "sixel is not supported");
			return (-1);
#endif
		case RESTART_SCREEN_PROGRESS:
			if (ibuf_size(&child.payload) != 5)
				goto invalid;
			data = ibuf_data(&child.payload);
			screen->progress_state = data[0];
			screen->progress = restart_s32_decode(
			    restart_get32(data + 1));
			ibuf_skip(&child.payload, 5);
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if ((seen & 0x4dfdU) != 0x4dfdU) {
		restart_set_cause(cause, "missing restart screen record");
		return (-1);
	}
	if (screen->cstyle > 3 || screen->default_cstyle > 3 ||
	    !restart_colour_valid(screen->ccolour) ||
	    !restart_colour_valid(screen->default_ccolour) ||
	    (screen->mode & ~RESTART_MODE_MASK) != 0 ||
	    (screen->default_mode & ~RESTART_MODE_MASK) != 0 ||
	    screen->cx > screen->grid.sx || screen->cy >= screen->grid.sy ||
	    screen->rupper > screen->rlower ||
	    screen->rlower >= screen->grid.sy || screen->progress_state > 4 ||
		screen->progress < 0 || screen->progress > 100) {
		restart_set_cause(cause, "invalid restart screen scalar");
		return (-1);
	}
	if ((screen->saved.cx == UINT32_MAX) !=
	    (screen->saved.cy == UINT32_MAX) ||
	    (screen->saved.cx != UINT32_MAX &&
	    (screen->saved.cx > screen->grid.sx ||
	    screen->saved.cy >= screen->grid.sy))) {
		restart_set_cause(cause, "invalid restart saved cursor");
		return (-1);
	}
	if (screen->have_saved_grid &&
	    (screen->saved_grid.sx != screen->grid.sx ||
	    screen->saved_grid.sy != screen->grid.sy)) {
		restart_set_cause(cause, "restart saved grid size mismatch");
		return (-1);
	}
#ifdef ENABLE_SIXEL
	if (screen->saved_images.count != 0 && !screen->have_saved_grid)
		goto invalid;
#endif
	tabs = (screen->grid.sx + 7) / 8;
	if (screen->tabs.size != tabs) {
		restart_set_cause(cause, "invalid restart tab size");
		return (-1);
	}
	if (tabs != 0 && screen->grid.sx % 8 != 0 &&
	    (screen->tabs.data[tabs - 1] &
	    ~((1U << (screen->grid.sx % 8)) - 1)) != 0) {
		restart_set_cause(cause, "invalid restart tab padding");
		return (-1);
	}
	if (!restart_grid_links_valid(&screen->grid, &screen->links) ||
	    (screen->have_saved_grid &&
	    !restart_grid_links_valid(&screen->saved_grid, &screen->links)) ||
	    (screen->saved.cell.link != 0 &&
	    !restart_links_have(&screen->links, screen->saved.cell.link))) {
		restart_set_cause(cause, "dangling restart hyperlink");
		return (-1);
	}
	return (0);
unknown:
	restart_set_cause(cause, "unknown required screen record");
	return (-1);
invalid:
	restart_set_cause(cause, "invalid restart screen");
	return (-1);
}

/* Read a parser cell. */
static int
restart_read_parser_cell(struct restart_record *record,
    struct restart_parser_cell *cell, struct restart_budget *budget,
    char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	uint32_t seen = 0;
	uint8_t value;
	int found;

	memset(cell, 0, sizeof *cell);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < 1 || child.type > 4) {
			if (child.flags & RESTART_RECORD_REQUIRED)
				goto unknown;
			continue;
		}
		if (restart_record_required(&child, cause) != 0 ||
		    (seen & (1U << (child.type - 1))) != 0)
			goto invalid;
		seen |= 1U << (child.type - 1);
		if (child.type == 1) {
			if (restart_read_cell(&child, &cell->cell, cause) != 0)
				return (-1);
			continue;
		}
		if (restart_read_u8(&child, &value, cause) != 0 ||
		    !restart_bool_valid(value))
			goto invalid;
		if (child.type == 2)
			cell->set = value;
		else if (child.type == 3)
			cell->g0set = value;
		else
			cell->g1set = value;
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if (seen != 15)
		goto invalid;
	return (0);
unknown:
	restart_set_cause(cause, "unknown required parser-cell record");
	return (-1);
invalid:
	restart_set_cause(cause, "invalid restart parser cell");
	return (-1);
}

/* Read a UTF-8 character. */
static int
restart_read_utf8(struct restart_record *record, struct restart_utf8 *utf8,
    int complete, char **cause)
{
	const u_char *data;
	size_t size = ibuf_size(&record->payload);
	u_int expected, i;

	memset(utf8, 0, sizeof *utf8);
	if (size < 4)
		goto invalid;
	data = ibuf_data(&record->payload);
	utf8->have = data[0];
	utf8->size = data[1];
	utf8->width = data[2];
	if (data[3] != 0 || utf8->have > utf8->size ||
	    utf8->size > UTF8_SIZE || size != 4U + utf8->have ||
	    (complete && utf8->have != utf8->size) ||
	    (utf8->width > 2 && utf8->width != 0xff))
		goto invalid;
	memcpy(utf8->data, data + 4, utf8->have);
	if (utf8->have == 0) {
		if (utf8->size != 0 || (complete && utf8->width != 0))
			goto invalid;
	} else if (complete && utf8->have == 1) {
		if (utf8->size != 1 || utf8->width != 1 ||
		    utf8->data[0] >= 0x80)
			goto invalid;
	} else {
		if (utf8->data[0] >= 0xc2 && utf8->data[0] <= 0xdf)
			expected = 2;
		else if (utf8->data[0] >= 0xe0 && utf8->data[0] <= 0xef)
			expected = 3;
		else if (utf8->data[0] >= 0xf0 && utf8->data[0] <= 0xf4)
			expected = 4;
		else
			goto invalid;
		if (utf8->size != expected)
			goto invalid;
		if (utf8->have > 1 &&
		    ((utf8->data[0] == 0xe0 && utf8->data[1] < 0xa0) ||
		    (utf8->data[0] == 0xed && utf8->data[1] > 0x9f) ||
		    (utf8->data[0] == 0xf0 && utf8->data[1] < 0x90) ||
		    (utf8->data[0] == 0xf4 && utf8->data[1] > 0x8f)))
			goto invalid;
		for (i = 1; i < utf8->have; i++) {
			if ((utf8->data[i] & 0xc0) != 0x80)
				goto invalid;
		}
	}
	ibuf_skip(&record->payload, size);
	return (0);
invalid:
	restart_set_cause(cause, "invalid restart UTF-8 state");
	return (-1);
}

/* Check a parser state name is one tmux has. */
static int
restart_parser_state_valid(const char *state)
{
	static const char *states[] = {
		"ground", "esc_enter", "esc_intermediate", "csi_enter",
		"csi_parameter", "csi_intermediate", "csi_ignore",
		"dcs_enter", "dcs_parameter", "dcs_intermediate",
		"dcs_handler", "dcs_escape", "dcs_ignore", "osc_string",
		"apc_string", "rename_string", "consume_st"
	};
	size_t i;

	for (i = 0; i < nitems(states); i++) {
		if (strcmp(states[i], state) == 0)
			return (1);
	}
	return (0);
}

/* Read a parser. */
static int
restart_read_parser(struct restart_record *record,
    struct restart_parser *parser, struct restart_budget *budget, char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	const u_char *data;
	uint32_t seen = 0;
	int found;

	memset(parser, 0, sizeof *parser);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_PARSER_STATE ||
		    child.type > RESTART_PARSER_FLAGS) {
			if (child.flags & RESTART_RECORD_REQUIRED)
				goto unknown;
			continue;
		}
		if (restart_record_required(&child, cause) != 0 ||
		    (seen & (1U << (child.type - 1))) != 0)
			goto invalid;
		seen |= 1U << (child.type - 1);
		switch (child.type) {
		case RESTART_PARSER_STATE:
			if (restart_read_string(&child, &parser->state, budget,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_PARSER_CELL:
			if (restart_read_parser_cell(&child, &parser->cell,
			    budget, cause) != 0)
				return (-1);
			break;
		case RESTART_PARSER_OLD_CELL:
			if (restart_read_parser_cell(&child, &parser->old_cell,
			    budget, cause) != 0)
				return (-1);
			break;
		case RESTART_PARSER_OLD_CURSOR:
			if (ibuf_size(&child.payload) != 8)
				goto invalid;
			data = ibuf_data(&child.payload);
			parser->old_cx = restart_get32(data);
			parser->old_cy = restart_get32(data + 4);
			ibuf_skip(&child.payload, 8);
			break;
		case RESTART_PARSER_OLD_MODE:
			if (restart_read_u32(&child, &parser->old_mode,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_PARSER_INTERMEDIATE:
			if (restart_read_blob(&child, &parser->intermediate, 3,
			    budget, cause) != 0)
				return (-1);
			break;
		case RESTART_PARSER_PARAMETER:
			if (restart_read_blob(&child, &parser->parameter, 63,
			    budget, cause) != 0)
				return (-1);
			break;
		case RESTART_PARSER_INPUT:
			if (restart_read_stream(&child, &parser->input, budget,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_PARSER_INPUT_END:
			if (restart_read_u8(&child, &parser->input_end,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_PARSER_UTF8:
			if (restart_read_utf8(&child, &parser->utf8, 0,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_PARSER_UTF8_STARTED:
			if (restart_read_u8(&child, &parser->utf8_started,
			    cause) != 0 ||
			    !restart_bool_valid(parser->utf8_started))
				goto invalid;
			break;
		case RESTART_PARSER_LAST_UTF8:
			if (restart_read_utf8(&child, &parser->last_utf8, 1,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_PARSER_FLAGS:
			if (restart_read_u32(&child, &parser->flags,
			    cause) != 0)
				return (-1);
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if (seen != 0x1fffU || !restart_parser_state_valid(parser->state) ||
	    (parser->old_mode & ~RESTART_MODE_MASK) != 0 ||
	    parser->input.size > RESTART_MAX_FIELD ||
	    parser->input_end > 1 || (parser->flags & ~3U) != 0 ||
	    ((parser->flags & 2U) && parser->last_utf8.have == 0) ||
	    (!parser->utf8_started &&
	    (parser->utf8.have != 0 || parser->utf8.size != 0 ||
	    parser->utf8.width != 0)) ||
	    (parser->utf8_started &&
	    (parser->utf8.have == 0 ||
	    parser->utf8.have >= parser->utf8.size ||
	    parser->utf8.width != 0)))
		goto invalid;
	return (0);
unknown:
	restart_set_cause(cause, "unknown required parser record");
	return (-1);
invalid:
	restart_set_cause(cause, "invalid restart parser");
	return (-1);
}

/* Check a decoded terminal is consistent. */
int
restart_terminal_validate_nested(const struct restart_terminal *terminal,
    size_t input_limit, char **cause)
{
	if (terminal->parser.input.size > input_limit) {
		restart_set_cause(cause,
		    "restart parser input exceeds restored limit");
		return (-1);
	}
	return (0);
}

/* Read a terminal from a record. */
int
restart_terminal_read_nested(struct restart_reader *rr,
    struct restart_terminal **out, char **cause)
{
	struct restart_record	 record;
	struct restart_terminal	*terminal;
	uint32_t		 seen = 0;
	int			 found;

	*out = NULL;
	terminal = restart_calloc(rr->budget, 1, sizeof *terminal, cause);
	if (terminal == NULL)
		return (-1);
	while ((found = restart_reader_next(rr, &record, cause)) == 1) {
		if (record.type < RESTART_TERMINAL_PALETTE ||
		    record.type > RESTART_TERMINAL_PTY_OUTPUT) {
			if (record.flags & RESTART_RECORD_REQUIRED) {
				restart_set_cause(cause,
				    "unknown required terminal record");
				goto fail;
			}
			continue;
		}
		if (restart_record_required(&record, cause) != 0)
			goto fail;
		if ((seen & (1U << (record.type - 1))) != 0) {
			restart_set_cause(cause, "duplicate terminal record");
			goto fail;
		}
		seen |= 1U << (record.type - 1);
		switch (record.type) {
		case RESTART_TERMINAL_PALETTE:
			if (restart_read_palette(&record, &terminal->palette,
			    rr->budget, cause) != 0)
				goto fail;
			break;
		case RESTART_TERMINAL_SCREEN:
			if (restart_read_screen(&record, &terminal->screen,
			    rr->budget, cause) != 0)
				goto fail;
			break;
		case RESTART_TERMINAL_PARSER:
			if (restart_read_parser(&record, &terminal->parser,
			    rr->budget, cause) != 0)
				goto fail;
			break;
		case RESTART_TERMINAL_PTY_INPUT:
			if (restart_read_stream(&record, &terminal->pty_input,
			    rr->budget, cause) != 0)
				goto fail;
			break;
		case RESTART_TERMINAL_PTY_OUTPUT:
			if (restart_read_stream(&record, &terminal->pty_output,
			    rr->budget, cause) != 0)
				goto fail;
			break;
		}
	}
	if (found == -1)
		goto fail;
	if (seen != 0x1fU) {
		restart_set_cause(cause, "missing terminal record");
		goto fail;
	}
	if (terminal->parser.old_cx > terminal->screen.grid.sx ||
	    terminal->parser.old_cy >= terminal->screen.grid.sy) {
		restart_set_cause(cause, "invalid parser cursor");
		goto fail;
	}
#ifdef ENABLE_SIXEL
	if (terminal->screen.images.count != 0 ||
	    terminal->screen.saved_images.count != 0)
		terminal->features |= RESTART_FEATURE_SIXEL;
#endif
	rr->observed_features |= terminal->features;
	*out = terminal;
	return (0);
fail:
	restart_terminal_free(terminal);
	return (-1);
}
