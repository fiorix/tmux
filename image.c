/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
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

static struct images	all_images = TAILQ_HEAD_INITIALIZER(all_images);
static u_int		all_images_count;
#define MAX_IMAGE_COUNT 20

/*
 * Get the most images that can be held. image_store evicts on reaching the
 * limit, so one fewer than it can be present at once.
 */
u_int
image_limit(void)
{
	return (MAX_IMAGE_COUNT - 1);
}

/*
 * Check whether count more images can be held. image_store evicts on reaching
 * the limit, so the most that can be present at once is one fewer than it.
 */
int
image_restart_room(size_t count)
{
	if (all_images_count > image_limit() ||
	    count > image_limit() - all_images_count)
		return (-1);
	return (0);
}

/*
 * Free a list of images that is not attached to a screen. Used to discard a
 * candidate that was built but will not be published.
 */
void
image_restart_discard(struct images *images)
{
	struct image	*im;

	while ((im = TAILQ_FIRST(images)) != NULL) {
		TAILQ_REMOVE(images, im, entry);
		sixel_free(im->data);
		free(im->fallback);
		free(im);
	}
}

/*
 * Attach a prepared list to a screen and enter every image in the global
 * list. The head is repaired by hand because a list moved with TAILQ_CONCAT
 * still points back at the head it came from.
 */
void
image_restart_commit(struct screen *s, struct images *images, int saved)
{
	struct images	*target = saved ? &s->saved_images : &s->images;
	struct image	*im;

	if (target != images)
		TAILQ_CONCAT(target, images, entry);
	if (TAILQ_EMPTY(target)) {
		TAILQ_INIT(target);
		return;
	}
	TAILQ_FIRST(target)->entry.tqe_prev = &target->tqh_first;
	TAILQ_FOREACH(im, target, entry) {
		im->s = s;
		im->list = target;
		TAILQ_INSERT_TAIL(&all_images, im, all_entry);
		all_images_count++;
	}
}

static void printflike(3, 4)
image_log(struct image *im, const char* from, const char* fmt, ...)
{
	va_list	ap;
	char	s[128];

	if (log_get_level() == 0)
		return;

	if (fmt == NULL) {
		log_debug("%s: %p (%ux%u %u,%u)", from, im, im->sx, im->sy,
		    im->px, im->py);
		return;
	}

	va_start(ap, fmt);
	vsnprintf(s, sizeof s, fmt, ap);
	va_end(ap);

	log_debug("%s: %p (%ux%u %u,%u): %s", from, im, im->sx, im->sy,
	    im->px, im->py, s);
}

static void
image_free(struct image *im)
{
	image_log(im, __func__, NULL);

	TAILQ_REMOVE(&all_images, im, all_entry);
	all_images_count--;

	TAILQ_REMOVE(im->list, im, entry);
	sixel_free(im->data);
	free(im->fallback);
	free(im);
}

int
image_free_all(struct screen *s)
{
	struct image	*im, *im1;
	int		 redraw = !TAILQ_EMPTY(&s->images);

	if (redraw)
		log_debug ("%s", __func__);
	TAILQ_FOREACH_SAFE(im, &s->images, entry, im1)
		image_free(im);
	return (redraw);
}

/* Create text placeholder for an image. */
static void
image_fallback(char **ret, u_int sx, u_int sy)
{
	char	*buf, *label;
	u_int	 py, size, lsize;

	if (sy == 0) {
		*ret = xstrdup("");
		return;
	}

	/* Allocate first line. */
	lsize = xasprintf(&label, "SIXEL IMAGE (%ux%u)\r\n", sx, sy) + 1;
	if (sx < lsize - 3)
		size = lsize - 1;
	else
		size = sx + 2;

	/* Remaining lines. Every placeholder line has \r\n at the end. */
	size += (sx + 2) * (sy - 1) + 1;
	*ret = buf = xmalloc(size);

	/* Render first line. */
	if (sx < lsize - 3) {
		memcpy(buf, label, lsize);
		buf += lsize - 1;
	} else {
		memcpy(buf, label, lsize - 3);
		buf += lsize - 3;
		memset(buf, '+', sx - lsize + 3);
		buf += sx - lsize + 3;
		snprintf(buf, 3, "\r\n");
		buf += 2;
	}

	/* Remaining lines. */
	for (py = 1; py < sy; py++) {
		memset(buf, '+', sx);
		buf += sx;
		snprintf(buf, 3, "\r\n");
		buf += 2;
	}

	free(label);
}

struct image*
image_store(struct screen *s, struct sixel_image *si)
{
	struct image	*im;

	im = xcalloc(1, sizeof *im);
	im->s = s;
	im->data = si;

	im->px = s->cx;
	im->py = s->cy;
	sixel_size_in_cells(si, &im->sx, &im->sy);

	image_fallback(&im->fallback, im->sx, im->sy);

	image_log(im, __func__, NULL);
	im->list = &s->images;
	TAILQ_INSERT_TAIL(im->list, im, entry);

	TAILQ_INSERT_TAIL(&all_images, im, all_entry);
	if (++all_images_count == MAX_IMAGE_COUNT)
		image_free(TAILQ_FIRST(&all_images));

	return (im);
}

int
image_check_line(struct screen *s, u_int py, u_int ny)
{
	struct image	*im, *im1;
	int		 redraw = 0, in;

	TAILQ_FOREACH_SAFE(im, &s->images, entry, im1) {
		in = (py + ny > im->py && py < im->py + im->sy);
		image_log(im, __func__, "py=%u, ny=%u, in=%d", py, ny, in);
		if (in) {
			image_free(im);
			redraw = 1;
		}
	}
	return (redraw);
}

int
image_check_area(struct screen *s, u_int px, u_int py, u_int nx, u_int ny)
{
	struct image	*im, *im1;
	int		 redraw = 0, in;

	TAILQ_FOREACH_SAFE(im, &s->images, entry, im1) {
		in = (py < im->py + im->sy &&
		    py + ny > im->py &&
		    px < im->px + im->sx &&
		    px + nx > im->px);
		image_log(im, __func__, "py=%u, ny=%u, in=%d", py, ny, in);
		if (in) {
			image_free(im);
			redraw = 1;
		}
	}
	return (redraw);
}

int
image_scroll_up(struct screen *s, u_int lines)
{
	struct image		*im, *im1;
	int			 redraw = 0;
	u_int			 sx, sy;
	struct sixel_image	*new;

	TAILQ_FOREACH_SAFE(im, &s->images, entry, im1) {
		if (im->py >= lines) {
			image_log(im, __func__, "1, lines=%u", lines);
			im->py -= lines;
			redraw = 1;
			continue;
		}
		if (im->py + im->sy <= lines) {
			image_log(im, __func__, "2, lines=%u", lines);
			image_free(im);
			redraw = 1;
			continue;
		}
		sx = im->sx;
		sy = (im->py + im->sy) - lines;
		image_log(im, __func__, "3, lines=%u, sy=%u", lines, sy);

		new = sixel_scale(im->data, 0, 0, 0, im->sy - sy, sx, sy, 1);
		sixel_free(im->data);
		im->data = new;

		im->py = 0;
		sixel_size_in_cells(im->data, &im->sx, &im->sy);

		free(im->fallback);
		image_fallback(&im->fallback, im->sx, im->sy);
		redraw = 1;
	}
	return (redraw);
}
