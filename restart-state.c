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
#include <sys/wait.h>

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>

#include "tmux.h"
#include "restart-state-private.h"

static u_int	restart_flags_mask(const struct restart_flag_map *, u_int);
static int	restart_flags_from_wire(const struct restart_flag_map *,
		    u_int, u_int);
static int	restart_flags_to_wire(const struct restart_flag_map *, u_int,
		    int, size_t, u_int *, char **);

struct restart_termios_key {
	uint16_t	 key;
	int		 index;
	int		 required;
};

static const struct restart_termios_key restart_termios_keys[] = {
	{ 1, VINTR, 1 },
	{ 2, VQUIT, 1 },
	{ 3, VERASE, 1 },
	{ 4, VKILL, 1 },
	{ 5, VEOF, 1 },
	{ 6, VEOL, 1 },
	{ 7, VSTART, 1 },
	{ 8, VSTOP, 1 },
	{ 9, VSUSP, 1 },
	{ 10, VMIN, 1 },
	{ 11, VTIME, 1 },
	{ 12, VEOL2, 0 },
#ifdef VDSUSP
	{ 13, VDSUSP, 0 },
#endif
	{ 14, VREPRINT, 0 },
	{ 15, VDISCARD, 0 },
	{ 16, VWERASE, 0 },
	{ 17, VLNEXT, 0 },
#ifdef VSTATUS
	{ 18, VSTATUS, 0 },
#endif
};

const struct restart_flag_map restart_pane_flag_map[] = {
	{ .wire = 0x0001, .live = PANE_INPUTOFF },
	{ .wire = 0x0002, .live = PANE_CHANGED },
	{ .wire = RESTART_PANE_WIRE_CMDRUNNING, .live = PANE_CMDRUNNING },
	{ .wire = 0x0008, .live = PANE_CLOSEONCLICK },
	{ .wire = 0x0010, .live = PANE_CAPTUREALLKEYS },
	{ .wire = 0x0020, .live = PANE_FLOATOVERZOOM },
	{ .wire = 0x0040, .live = PANE_CLOSEONCANCEL }
};

const struct restart_flag_map restart_window_flag_map[] = {
	{ .wire = RESTART_WINDOW_ZOOMED, .live = WINDOW_ZOOMED }
};

const struct restart_flag_map restart_winlink_flag_map[] = {
	{ .wire = 0x01, .live = WINLINK_BELL },
	{ .wire = 0x02, .live = WINLINK_ACTIVITY },
	{ .wire = 0x04, .live = WINLINK_SILENCE }
};

const struct restart_flag_map restart_environ_flag_map[] = {
	{ .wire = RESTART_ENVIRON_HIDDEN, .live = ENVIRON_HIDDEN }
};

const u_int restart_pane_flag_count = nitems(restart_pane_flag_map);
const u_int restart_window_flag_count = nitems(restart_window_flag_map);
const u_int restart_winlink_flag_count = nitems(restart_winlink_flag_map);
const u_int restart_environ_flag_count = nitems(restart_environ_flag_map);

#define RESTART_TERMIOS_REQUIRED 0x000007ffU

static void	restart_options_free(struct restart_options *);
static void	restart_environment_free(struct restart_environment *);
static void	restart_state_string_list_free(struct restart_string_list *);
static int	restart_pane_validate_running(const struct restart_pane *,
		    char **);

static int
restart_u32_compare(uint32_t a, uint32_t b)
{
	if (a < b)
		return (-1);
	if (a > b)
		return (1);
	return (0);
}

static int
restart_s32_compare(int32_t a, int32_t b)
{
	if (a < b)
		return (-1);
	if (a > b)
		return (1);
	return (0);
}

static uint64_t
restart_state_s64_encode(int64_t value)
{
	if (value < 0)
		return (((uint64_t)(-(value + 1)) << 1) | 1);
	return ((uint64_t)value << 1);
}

static void
restart_state_put32(u_char *data, uint32_t value)
{
	data[0] = value >> 24;
	data[1] = value >> 16;
	data[2] = value >> 8;
	data[3] = value;
}

static void
restart_state_put64(u_char *data, uint64_t value)
{
	restart_state_put32(data, value >> 32);
	restart_state_put32(data + 4, value);
}

static uint32_t
restart_state_get32(const u_char *data)
{
	return (((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
	    ((uint32_t)data[2] << 8) | data[3]);
}

static uint64_t
restart_state_get64(const u_char *data)
{
	return (((uint64_t)restart_state_get32(data) << 32) |
	    restart_state_get32(data + 4));
}

static int64_t
restart_state_s64_decode(uint64_t encoded)
{
	uint64_t magnitude = encoded >> 1;

	if (encoded & 1)
		return (-(int64_t)magnitude - 1);
	return ((int64_t)magnitude);
}

static int
restart_state_write_timeval(struct restart_writer *rw,
    struct restart_write_scope *scope, uint16_t type,
    const struct restart_timeval *value, char **cause)
{
	u_char data[12];

	restart_state_put64(data, restart_state_s64_encode(value->sec));
	restart_state_put32(data + 8, value->usec);
	return (restart_write_record(rw, scope, type, RESTART_RECORD_REQUIRED,
	    data, sizeof data, cause));
}

static int
restart_state_read_timeval(struct restart_record *record,
    struct restart_timeval *value, char **cause)
{
	const u_char	*data;

	if (ibuf_size(&record->payload) != 12) {
		restart_set_cause(cause, "invalid restart timeval");
		return (-1);
	}
	data = ibuf_data(&record->payload);
	value->sec = restart_state_s64_decode(restart_state_get64(data));
	value->usec = restart_state_get32(data + 8);
	if (value->usec > 999999 || ibuf_skip(&record->payload, 12) != 0) {
		restart_set_cause(cause, "invalid restart timeval");
		return (-1);
	}
	if ((int64_t)(time_t)value->sec != value->sec) {
		restart_set_cause(cause, "restart time is out of range");
		return (-1);
	}
	return (0);
}

static void
restart_state_capture_timeval(const struct timeval *tv,
    struct restart_timeval *out)
{
	out->sec = tv->tv_sec;
	out->usec = tv->tv_usec;
}

/*
 * Copy a string into the state, refusing one over the per-field limit. The
 * caller names what it is copying, because a user who is told only that a
 * string was too large has nothing to act on.
 */
static int
restart_state_capture_string(const char *value, char **out,
    struct restart_budget *budget, const char *what, char **cause)
{
	size_t size = strlen(value);

	if (size > RESTART_MAX_FIELD) {
		restart_set_cause(cause, "restart %s is too large: %zu bytes, "
		    "limit %zu", what, size, (size_t)RESTART_MAX_FIELD);
		return (-1);
	}
	*out = restart_alloc(budget, size + 1, cause);
	if (*out == NULL)
		return (-1);
	memcpy(*out, value, size + 1);
	return (0);
}

static int
restart_state_write_string_list(struct restart_writer *rw,
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

static int
restart_state_unknown(struct restart_record *record, const char *what,
    char **cause)
{
	if (record->flags & RESTART_RECORD_REQUIRED) {
		restart_set_cause(cause, "unknown required %s record", what);
		return (-1);
	}
	return (0);
}

static int
restart_state_required(const struct restart_record *record, char **cause)
{
	if (record->flags == RESTART_RECORD_REQUIRED)
		return (0);
	restart_set_cause(cause, "invalid restart record flags");
	return (-1);
}

static int
restart_state_read_string_list(struct restart_record *record,
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
			if (restart_state_unknown(&item, "string-list",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&item, cause) != 0)
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

static int
restart_state_write_id_list(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type,
    const struct restart_id_list *list, char **cause)
{
	struct restart_write_scope scope;
	size_t i;

	if (restart_write_container_begin(rw, parent, type,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0)
		return (-1);
	for (i = 0; i < list->count; i++) {
		if (restart_write_u32(rw, &scope, 1, list->items[i],
		    cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

static int
restart_s32_sort_compare(const void *a0, const void *b0)
{
	const int32_t *a = a0, *b = b0;

	return (restart_s32_compare(*a, *b));
}

static int
restart_u32_sort_compare(const void *a0, const void *b0)
{
	const uint32_t *a = a0, *b = b0;

	return (restart_u32_compare(*a, *b));
}

static int
restart_name_sort_compare(const void *a0, const void *b0)
{
	const char *const *a = a0, *const *b = b0;

	return (strcmp(*a, *b));
}

/*
 * Read an ID list. The list name is threaded in rather than split across
 * callers: there is one reader action here and one remedy, but the remedy is
 * carried out on a particular list, and at this point in the code the list has
 * no name. Naming it in the cause fixes every caller at once, including the
 * ones no matrix class covers.
 */
static int
restart_state_read_id_list(struct restart_record *record,
    struct restart_id_list *list, const char *what,
    struct restart_budget *budget, char **cause)
{
	struct restart_reader rr;
	struct restart_record item;
	uint32_t *new, *sorted;
	size_t i;
	int found;

	memset(list, 0, sizeof *list);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &item, cause)) == 1) {
		if (item.type != 1) {
			if (restart_state_unknown(&item, "ID-list", cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&item, cause) != 0)
			return (-1);
		new = restart_grow(budget, list->items, list->count,
		    list->count + 1, sizeof *list->items, cause);
		if (new == NULL)
			return (-1);
		list->items = new;
		if (restart_read_u32(&item, &list->items[list->count],
		    cause) != 0 || ibuf_size(&item.payload) != 0) {
			restart_set_cause(cause, "invalid restart %s list",
			    what);
			return (-1);
		}
		list->count++;
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if (list->count < 2)
		return (0);
	sorted = restart_grow(budget, NULL, 0, list->count, sizeof *sorted,
	    cause);
	if (sorted == NULL)
		return (-1);
	memcpy(sorted, list->items, list->count * sizeof *sorted);
	qsort(sorted, list->count, sizeof *sorted, restart_u32_sort_compare);
	for (i = 0; i + 1 < list->count; i++) {
		if (sorted[i] == sorted[i + 1]) {
			free(sorted);
			restart_set_cause(cause,
			    "duplicate restart %s entry", what);
			return (-1);
		}
	}
	free(sorted);
	return (0);
}

static int
restart_state_write_index_list(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type,
    const struct restart_index_list *list, char **cause)
{
	struct restart_write_scope scope;
	size_t i;

	if (restart_write_container_begin(rw, parent, type,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0)
		return (-1);
	for (i = 0; i < list->count; i++) {
		if (restart_write_s32(rw, &scope, 1, list->items[i],
		    cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

static int
restart_state_read_index_list(struct restart_record *record,
    struct restart_index_list *list, struct restart_budget *budget,
    char **cause)
{
	struct restart_reader rr;
	struct restart_record item;
	int32_t *new, *sorted;
	size_t i;
	int found;

	memset(list, 0, sizeof *list);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &item, cause)) == 1) {
		if (item.type != 1) {
			if (restart_state_unknown(&item, "index-list",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&item, cause) != 0)
			return (-1);
		new = restart_grow(budget, list->items, list->count,
		    list->count + 1, sizeof *list->items, cause);
		if (new == NULL)
			return (-1);
		list->items = new;
		if (restart_read_s32(&item, &list->items[list->count],
		    cause) != 0 || ibuf_size(&item.payload) != 0) {
			restart_set_cause(cause, "invalid restart index list");
			return (-1);
		}
		if (list->items[list->count] < 0) {
			restart_set_cause(cause, "negative restart index");
			return (-1);
		}
		list->count++;
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if (list->count < 2)
		return (0);
	sorted = restart_grow(budget, NULL, 0, list->count, sizeof *sorted,
	    cause);
	if (sorted == NULL)
		return (-1);
	memcpy(sorted, list->items, list->count * sizeof *sorted);
	qsort(sorted, list->count, sizeof *sorted, restart_s32_sort_compare);
	for (i = 0; i + 1 < list->count; i++) {
		if (sorted[i] == sorted[i + 1]) {
			free(sorted);
			restart_set_cause(cause, "duplicate restart index");
			return (-1);
		}
	}
	free(sorted);
	return (0);
}

/*
 * The theme is an enumeration rather than a flag set, so it converts by the
 * same rule for the same reason: the wire value and the enumerator ordinal
 * are separate numberings that agree today. The valid range comes from the
 * table, because a literal bound accepts or rejects wrongly the moment an
 * enumerator is added, and adding one is an ordinary upstream change.
 */
static const struct {
	uint8_t			 wire;
	enum client_theme	 live;
} restart_theme_map[] = {
	{ .wire = 0, .live = THEME_UNKNOWN },
	{ .wire = 1, .live = THEME_LIGHT },
	{ .wire = 2, .live = THEME_DARK }
};

static int
restart_theme_to_wire(enum client_theme live, uint8_t *wire)
{
	u_int	i;

	for (i = 0; i < nitems(restart_theme_map); i++) {
		if (restart_theme_map[i].live == live) {
			*wire = restart_theme_map[i].wire;
			return (0);
		}
	}
	return (-1);
}

static int
restart_theme_from_wire(uint8_t wire, enum client_theme *live)
{
	u_int	i;

	for (i = 0; i < nitems(restart_theme_map); i++) {
		if (restart_theme_map[i].wire == wire) {
			*live = restart_theme_map[i].live;
			return (0);
		}
	}
	return (-1);
}

static int
restart_environment_capture(struct environ *env,
    struct restart_environment *out, struct restart_budget *budget,
    char **cause)
{
	struct environ_entry *entry;
	struct restart_environment_entry *item;
	size_t count = 0;
	u_int flags;

	memset(out, 0, sizeof *out);
	for (entry = environ_first(env); entry != NULL;
	    entry = environ_next(entry))
		count++;
	if (count == 0)
		return (0);
	if (count > RESTART_MAX_RECORDS) {
		restart_set_cause(cause,
		    "too many restart environment entries");
		return (-1);
	}
	out->entries = restart_calloc(budget, count, sizeof *out->entries,
	    cause);
	if (out->entries == NULL)
		return (-1);
	for (entry = environ_first(env); entry != NULL;
	    entry = environ_next(entry)) {
		item = &out->entries[out->count++];
		if (*entry->name == '\0') {
			restart_set_cause(cause,
			    "empty restart environment name");
			return (-1);
		}
		if ((entry->flags & ~ENVIRON_HIDDEN) != 0) {
			restart_set_cause(cause,
			    "invalid restart environment flags");
			return (-1);
		}
		if (restart_state_capture_string(entry->name, &item->name,
		    budget, "environment name", cause) != 0)
			return (-1);
		if (restart_flags_to_wire(restart_environ_flag_map,
		    nitems(restart_environ_flag_map), entry->flags,
		    sizeof(item->flags), &flags, cause) != 0)
			return (-1);
		item->flags = flags;
		if (entry->value == NULL)
			item->state = RESTART_ENV_STATE_CLEARED;
		else {
			item->state = RESTART_ENV_STATE_SET;
			if (restart_state_capture_string(entry->value,
			    &item->value, budget, "environment value",
			    cause) != 0)
				return (-1);
		}
	}
	return (0);
}

static int
restart_environment_write(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type,
    const struct restart_environment *environment, char **cause)
{
	struct restart_write_scope scope, entry;
	const struct restart_environment_entry *item;
	size_t i;

	if (restart_write_container_begin(rw, parent, type,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0)
		return (-1);
	for (i = 0; i < environment->count; i++) {
		item = &environment->entries[i];
		if (restart_write_container_begin(rw, &scope, 1,
		    RESTART_RECORD_REQUIRED, &entry, cause) != 0 ||
		    restart_write_string(rw, &entry, RESTART_ENV_NAME,
		    item->name, "environment name", cause) != 0 ||
		    restart_write_u8(rw, &entry, RESTART_ENV_STATE,
		    item->state, cause) != 0 ||
		    restart_write_u8(rw, &entry, RESTART_ENV_FLAGS,
		    item->flags, cause) != 0)
			return (-1);
		if (item->state == RESTART_ENV_STATE_SET &&
		    restart_write_string(rw, &entry, RESTART_ENV_VALUE,
		    item->value, "environment value", cause) != 0)
			return (-1);
		if (restart_write_container_end(rw, &entry, cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}


static int
restart_environment_read_entry(struct restart_record *record,
    struct restart_environment_entry *entry, struct restart_budget *budget,
    char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	uint32_t seen = 0;
	int found;

	memset(entry, 0, sizeof *entry);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_ENV_NAME ||
		    child.type > RESTART_ENV_VALUE) {
			if (restart_state_unknown(&child, "environment",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0 ||
		    (seen & (1U << child.type)) != 0)
			goto invalid;
		seen |= 1U << child.type;
		switch (child.type) {
		case RESTART_ENV_NAME:
			if (restart_read_string(&child, &entry->name, budget,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_ENV_STATE:
			if (restart_read_u8(&child, &entry->state, cause) != 0)
				return (-1);
			break;
		case RESTART_ENV_FLAGS:
			if (restart_read_u8(&child, &entry->flags, cause) != 0)
				return (-1);
			break;
		case RESTART_ENV_VALUE:
			if (restart_read_string(&child, &entry->value, budget,
			    cause) != 0)
				return (-1);
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if ((seen & 0x0eU) != 0x0eU)
		goto invalid;
	if (entry->name == NULL || *entry->name == '\0')
		goto invalid;
	if ((entry->flags & ~RESTART_ENVIRON_HIDDEN) != 0)
		goto invalid;
	if (entry->state == RESTART_ENV_STATE_SET) {
		if (entry->value == NULL)
			goto invalid;
	} else if (entry->state == RESTART_ENV_STATE_CLEARED) {
		if (entry->value != NULL)
			goto invalid;
	} else
		goto invalid;
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart environment entry");
	return (-1);
}

static int
restart_environment_read(struct restart_record *record,
    struct restart_environment *environment, struct restart_budget *budget,
    char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	struct restart_environment_entry *new;
	size_t i;
	int found;

	memset(environment, 0, sizeof *environment);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (restart_state_unknown(&child, "environment-list",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			return (-1);
		new = restart_grow(budget, environment->entries,
		    environment->count, environment->count + 1,
		    sizeof *environment->entries, cause);
		if (new == NULL)
			return (-1);
		environment->entries = new;
		memset(&environment->entries[environment->count], 0,
		    sizeof *environment->entries);
		environment->count++;
		if (restart_environment_read_entry(&child,
		    &environment->entries[environment->count - 1], budget,
		    cause) != 0)
			return (-1);
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	for (i = 0; i + 1 < environment->count; i++) {
		if (strcmp(environment->entries[i].name,
		    environment->entries[i + 1].name) == 0) {
			restart_set_cause(cause,
			    "duplicate restart environment name");
			return (-1);
		}
	}
	for (i = 1; i < environment->count; i++) {
		if (strcmp(environment->entries[i].name,
		    environment->entries[i - 1].name) < 0) {
			restart_set_cause(cause,
			    "restart environment names are not ordered");
			return (-1);
		}
	}
	return (0);
}

static int
restart_termios_capture(const struct termios *tio,
    struct restart_termios_cc *out, char **cause)
{
	const struct restart_termios_key *key;
	size_t i;

	memset(out, 0, sizeof *out);
	for (i = 0; i < nitems(restart_termios_keys); i++) {
		key = &restart_termios_keys[i];
		if (key->index >= NCCS) {
			if (!key->required)
				continue;
			restart_set_cause(cause,
			    "unsupported restart terminal control key %u",
			    key->key);
			return (-1);
		}
		out->present |= 1U << (key->key - 1);
		out->value[key->key - 1] = tio->c_cc[key->index];
	}
	if ((out->present & RESTART_TERMIOS_REQUIRED) !=
	    RESTART_TERMIOS_REQUIRED) {
		restart_set_cause(cause,
		    "missing required live terminal control key");
		return (-1);
	}
	return (0);
}

static int
restart_termios_write(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type,
    const struct restart_termios_cc *cc, char **cause)
{
	struct restart_write_scope scope;
	const struct restart_termios_key *key;
	u_char data[3];
	size_t i;

	if (restart_write_container_begin(rw, parent, type,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0)
		return (-1);
	for (i = 0; i < nitems(restart_termios_keys); i++) {
		key = &restart_termios_keys[i];
		if ((cc->present & (1U << (key->key - 1))) == 0)
			continue;
		data[0] = key->key >> 8;
		data[1] = key->key;
		data[2] = cc->value[key->key - 1];
		if (restart_write_record(rw, &scope, 1,
		    key->required ? RESTART_RECORD_REQUIRED : 0, data,
		    sizeof data, cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

static const struct restart_termios_key *
restart_termios_lookup(uint16_t key)
{
	size_t i;

	for (i = 0; i < nitems(restart_termios_keys); i++) {
		if (restart_termios_keys[i].key == key)
			return (&restart_termios_keys[i]);
	}
	return (NULL);
}

static int
restart_termios_read(struct restart_record *record,
    struct restart_termios_cc *cc, struct restart_budget *budget, char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	const struct restart_termios_key *key;
	const u_char *data;
	uint16_t number, last = 0;
	uint8_t value;
	int found;

	memset(cc, 0, sizeof *cc);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (restart_state_unknown(&child, "terminal-control",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (ibuf_size(&child.payload) != 3)
			goto invalid;
		data = ibuf_data(&child.payload);
		number = ((uint16_t)data[0] << 8) | data[1];
		value = data[2];
		if (ibuf_skip(&child.payload, 3) != 0)
			goto invalid;
		if (number < last) {
			restart_set_cause(cause,
			    "restart terminal control keys are not ordered");
			return (-1);
		}
		last = number;
		key = restart_termios_lookup(number);
		if (key == NULL || key->index >= NCCS) {
			if (child.flags & RESTART_RECORD_REQUIRED) {
				restart_set_cause(cause,
				    "unsupported required restart terminal "
				    "control key");
				return (-1);
			}
			continue;
		}
		if (key->required != ((child.flags & RESTART_RECORD_REQUIRED)
		    != 0))
			goto invalid;
		if ((cc->present & (1U << (number - 1))) != 0)
			goto invalid;
		cc->present |= 1U << (number - 1);
		cc->value[number - 1] = value;
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if ((cc->present & RESTART_TERMIOS_REQUIRED) !=
	    RESTART_TERMIOS_REQUIRED) {
		restart_set_cause(cause,
		    "missing required restart terminal control key");
		return (-1);
	}
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart terminal control key");
	return (-1);
}

static int
restart_termios_validate(const struct restart_termios_cc *cc, char **cause)
{
	const struct restart_termios_key *a, *b;
	size_t i, j;

	for (i = 0; i < nitems(restart_termios_keys); i++) {
		a = &restart_termios_keys[i];
		if ((cc->present & (1U << (a->key - 1))) == 0)
			continue;
		for (j = i + 1; j < nitems(restart_termios_keys); j++) {
			b = &restart_termios_keys[j];
			if ((cc->present & (1U << (b->key - 1))) == 0 ||
			    a->index != b->index)
				continue;
			if (cc->value[a->key - 1] != cc->value[b->key - 1]) {
				restart_set_cause(cause,
				    "conflicting restart terminal control "
				    "values");
				return (-1);
			}
		}
	}
	return (0);
}

static void
restart_state_string_list_free(struct restart_string_list *list)
{
	size_t i;

	for (i = 0; i < list->count; i++)
		free(list->items[i]);
	free(list->items);
	list->items = NULL;
	list->count = 0;
}

static void
restart_environment_free(struct restart_environment *environment)
{
	size_t i;

	for (i = 0; i < environment->count; i++) {
		free(environment->entries[i].name);
		free(environment->entries[i].value);
	}
	free(environment->entries);
	environment->entries = NULL;
	environment->count = 0;
}

static void
restart_options_free(struct restart_options *options)
{
	struct restart_option *option;
	size_t i, j;

	for (i = 0; i < options->count; i++) {
		option = &options->entries[i];
		for (j = 0; j < option->item_count; j++) {
			free(option->items[j].key);
			free(option->items[j].string);
		}
		free(option->items);
		free(option->string);
		free(option->name);
	}
	free(options->entries);
	options->entries = NULL;
	options->count = 0;
}

/* Get the wire type for an option. */
static uint8_t
restart_option_wire_type(const struct options_table_entry *oe)
{
	if (oe == NULL)
		return (RESTART_OPTION_USER);
	switch (oe->type) {
	case OPTIONS_TABLE_STRING:
		return (RESTART_OPTION_STRING);
	case OPTIONS_TABLE_NUMBER:
		return (RESTART_OPTION_NUMBER);
	case OPTIONS_TABLE_KEY:
		return (RESTART_OPTION_KEY);
	case OPTIONS_TABLE_COLOUR:
		return (RESTART_OPTION_COLOUR);
	case OPTIONS_TABLE_FLAG:
		return (RESTART_OPTION_FLAG);
	case OPTIONS_TABLE_CHOICE:
		return (RESTART_OPTION_CHOICE);
	case OPTIONS_TABLE_COMMAND:
		return (RESTART_OPTION_COMMAND);
	}
	return (0);
}

/* Capture one option value. */
static int
restart_option_capture_value(uint8_t type, union options_value *ov,
    char **string, int64_t *number, struct restart_budget *budget,
    char **cause)
{
	char	*printed;
	int	 error;

	switch (type) {
	case RESTART_OPTION_USER:
	case RESTART_OPTION_STRING:
		return (restart_state_capture_string(ov->string, string, budget,
		    "option value", cause));
	case RESTART_OPTION_COMMAND:
		printed = cmd_list_print(ov->cmdlist, 0);
		error = restart_state_capture_string(printed, string, budget,
		    "option command", cause);
		free(printed);
		return (error);
	default:
		*number = ov->number;
		return (0);
	}
}

/* Capture a set of options. */
static int
restart_options_capture(struct options *oo, struct restart_options *out,
    struct restart_budget *budget, char **cause)
{
	struct options_entry		*o;
	struct options_array_item	*a;
	struct restart_option		*entry;
	struct restart_option_item	*item;
	const char			*name;
	size_t				 count = 0, items;

	memset(out, 0, sizeof *out);
	for (o = options_first(oo); o != NULL; o = options_next(o))
		count++;
	if (count == 0)
		return (0);
	if (count > RESTART_MAX_RECORDS) {
		restart_set_cause(cause, "too many restart options");
		return (-1);
	}
	out->entries = restart_calloc(budget, count, sizeof *out->entries,
	    cause);
	if (out->entries == NULL)
		return (-1);

	for (o = options_first(oo); o != NULL; o = options_next(o)) {
		name = options_name(o);
		entry = &out->entries[out->count++];
		entry->type = restart_option_wire_type(options_table_entry(o));
		if (entry->type == 0) {
			restart_set_cause(cause, "unknown restart option type");
			return (-1);
		}
		if (entry->type == RESTART_OPTION_USER && *name != '@') {
			restart_set_cause(cause,
			    "restart user option has no @ prefix");
			return (-1);
		}
		if (restart_state_capture_string(name, &entry->name, budget,
		    "option name", cause) != 0)
			return (-1);
		if (!options_is_array(o)) {
			if (restart_option_capture_value(entry->type,
			    options_entry_value(o), &entry->string,
			    &entry->number, budget, cause) != 0)
				return (-1);
			continue;
		}
		entry->is_array = 1;
		items = 0;
		for (a = options_array_first(o); a != NULL;
		    a = options_array_next(a))
			items++;
		if (items > RESTART_MAX_RECORDS) {
			restart_set_cause(cause,
			    "too many restart option items");
			return (-1);
		}
		if (items == 0)
			continue;
		entry->items = restart_calloc(budget, items,
		    sizeof *entry->items, cause);
		if (entry->items == NULL)
			return (-1);
		for (a = options_array_first(o); a != NULL;
		    a = options_array_next(a)) {
			item = &entry->items[entry->item_count++];
			if (restart_state_capture_string(
			    options_array_item_key(a), &item->key, budget,
			    "option array key", cause) != 0 ||
			    restart_option_capture_value(entry->type,
			    options_array_item_value(a), &item->string,
			    &item->number, budget, cause) != 0)
				return (-1);
		}
	}
	return (0);
}

/* Compare option items by key, for a deterministic order. */
static int
restart_option_item_cmp(const void *a0, const void *b0)
{
	const struct restart_option_item	*a = a0, *b = b0;

	return (options_array_key_cmp(a->key, b->key));
}

/* Set the scope a set of options was captured from. */
static void
restart_options_set_scope(struct restart_options *options, int scope)
{
	options->scope = scope;
}

/* Check a number is one the option can hold. */
static int
restart_option_validate_number(const struct options_table_entry *oe,
    const char *name, uint8_t type, int64_t number, char **cause)
{
	int64_t	n = 0;

	switch (type) {
	case RESTART_OPTION_NUMBER:
		if (number < (int64_t)oe->minimum ||
		    number > (int64_t)oe->maximum)
			break;
		return (0);
	case RESTART_OPTION_KEY:
		if (number >= 0 && (key_code)number != KEYC_UNKNOWN)
			return (0);
		break;
	case RESTART_OPTION_COLOUR:
		if (number < INT_MIN || number > INT_MAX)
			break;
		if (number == -1)
			return (0);
		if (colour_fromstring(colour_tostring(number)) == (int)number)
			return (0);
		break;
	case RESTART_OPTION_FLAG:
		if (number == 0 || number == 1)
			return (0);
		break;
	case RESTART_OPTION_CHOICE:
		if (number >= 0 && oe->choices != NULL) {
			while (oe->choices[n] != NULL)
				n++;
			if (number < n)
				return (0);
		}
		break;
	}
	restart_set_cause(cause, "invalid restart option %s value %lld", name,
	    (long long)number);
	return (-1);
}

/* Check a string is one the option can hold. */
static int
restart_option_validate_string(const struct options_table_entry *oe,
    const char *name, uint8_t type, const char *value, char **cause)
{
	struct cmd_parse_result	*pr;
	struct cmd_parse_input	 pi;
	struct style		 sy;

	if (value == NULL) {
		restart_set_cause(cause, "missing restart option %s value",
		    name);
		return (-1);
	}
	if (type == RESTART_OPTION_COMMAND) {
		memset(&pi, 0, sizeof pi);
		pi.flags = CMD_PARSE_PARSEONLY|CMD_PARSE_NOALIAS;
		pr = cmd_parse_from_string(value, &pi);
		if (pr->status != CMD_PARSE_SUCCESS) {
			restart_set_cause(cause,
			    "invalid restart option %s command", name);
			free(pr->error);
			return (-1);
		}
		cmd_list_free(pr->cmdlist);
		return (0);
	}
	if (oe == NULL)
		return (0);
	if (strcmp(oe->name, "default-shell") == 0 && !checkshell(value)) {
		restart_set_cause(cause,
		    "restart option %s is not a suitable shell", name);
		return (-1);
	}
	if (oe->pattern != NULL && fnmatch(oe->pattern, value, 0) != 0) {
		restart_set_cause(cause, "invalid restart option %s value",
		    name);
		return (-1);
	}
	if ((oe->flags & OPTIONS_TABLE_IS_STYLE) &&
	    strstr(value, "#{") == NULL &&
	    style_parse(&sy, &grid_default_cell, value) != 0) {
		restart_set_cause(cause, "invalid restart option %s style",
		    name);
		return (-1);
	}
	if ((oe->flags & OPTIONS_TABLE_IS_COLOUR) &&
	    strstr(value, "#{") == NULL &&
	    style_parse_colour(&sy, &grid_default_cell, value) != 0) {
		restart_set_cause(cause, "invalid restart option %s colour",
		    name);
		return (-1);
	}
	return (0);
}

/* Check one option. */
static int
restart_option_validate(const struct restart_option *entry, int scope,
    char **cause)
{
	const struct options_table_entry	*oe = NULL;
	struct restart_option_item		*item;
	size_t					 i;
	int					 is_array;

	if (entry->name == NULL || *entry->name == '\0') {
		restart_set_cause(cause, "empty restart option name");
		return (-1);
	}
	if (entry->type == RESTART_OPTION_USER) {
		if (*entry->name != '@' || entry->is_array ||
		    options_search(entry->name) != NULL) {
			restart_set_cause(cause, "invalid restart user option");
			return (-1);
		}
	} else {
		oe = options_search(entry->name);
		if (oe == NULL)
			return (0);
		if ((oe->scope & scope) == 0) {
			restart_set_cause(cause,
			    "restart option %s is out of scope", entry->name);
			return (-1);
		}
		is_array = ((oe->flags & OPTIONS_TABLE_IS_ARRAY) != 0);
		if (is_array != (entry->is_array != 0)) {
			restart_set_cause(cause,
			    "restart option %s has the wrong shape",
			    entry->name);
			return (-1);
		}
		if (restart_option_wire_type(oe) != entry->type) {
			restart_set_cause(cause,
			    "restart option %s has the wrong type",
			    entry->name);
			return (-1);
		}
	}

	if (!entry->is_array) {
		if (entry->item_count != 0 || entry->items != NULL) {
			restart_set_cause(cause,
			    "restart option has unexpected items");
			return (-1);
		}
		if (entry->type == RESTART_OPTION_USER ||
		    entry->type == RESTART_OPTION_STRING ||
		    entry->type == RESTART_OPTION_COMMAND)
			return (restart_option_validate_string(oe, entry->name,
			    entry->type, entry->string, cause));
		if (entry->string != NULL) {
			restart_set_cause(cause,
			    "restart option has the wrong value kind");
			return (-1);
		}
		return (restart_option_validate_number(oe, entry->name,
		    entry->type, entry->number, cause));
	}

	if (entry->string != NULL) {
		restart_set_cause(cause, "restart array option has a scalar");
		return (-1);
	}
	for (i = 0; i < entry->item_count; i++) {
		item = &entry->items[i];
		if (item->key == NULL) {
			restart_set_cause(cause, "missing restart option key");
			return (-1);
		}
		if (entry->type == RESTART_OPTION_USER ||
		    entry->type == RESTART_OPTION_STRING ||
		    entry->type == RESTART_OPTION_COMMAND) {
			if (restart_option_validate_string(oe, entry->name,
			    entry->type, item->string, cause) != 0)
				return (-1);
		} else {
			if (item->string != NULL) {
				restart_set_cause(cause,
				    "restart option item has the wrong value "
				    "kind");
				return (-1);
			}
			if (restart_option_validate_number(oe, entry->name,
			    entry->type, item->number, cause) != 0)
				return (-1);
		}
	}
	for (i = 0; i + 1 < entry->item_count; i++) {
		if (options_array_key_cmp(entry->items[i].key,
		    entry->items[i + 1].key) == 0) {
			restart_set_cause(cause,
			    "duplicate restart option key");
			return (-1);
		}
	}
	return (0);
}

/* Check a set of options. */
static int
restart_options_validate(const struct restart_options *options, int scope,
    char **cause)
{
	size_t	i;

	for (i = 0; i < options->count; i++) {
		if (restart_option_validate(&options->entries[i], scope,
		    cause) != 0)
			return (-1);
	}
	for (i = 0; i + 1 < options->count; i++) {
		if (strcmp(options->entries[i].name,
		    options->entries[i + 1].name) == 0) {
			restart_set_cause(cause,
			    "duplicate restart option name");
			return (-1);
		}
	}
	return (0);
}

static void
restart_layout_cell_free(struct restart_layout_cell *cell)
{
	size_t i;

	if (cell == NULL)
		return;
	for (i = 0; i < cell->child_count; i++)
		restart_layout_cell_free(cell->children[i]);
	free(cell->children);
	free(cell);
}

static void
restart_layout_free(struct restart_layout *layout)
{
	if (layout == NULL)
		return;
	restart_layout_cell_free(layout->root);
	free(layout);
}

#define RESTART_LAYOUT_SEEN_BASE 0x1fU
#define RESTART_LAYOUT_SEEN_LEAF 0x7fU
#define RESTART_LAYOUT_SEEN_LEAFONLY 0x7fe0U
#define RESTART_LAYOUT_SEEN_NODEONLY 0x8000U
#define RESTART_LAYOUT_SEEN_FG 0x7800U

static int
restart_layout_zindex(struct window_pane *wp, u_int *i)
{
	struct window		*w = wp->window;
	struct window_pane	*wq;
	struct layout_cell	*lc;

	*i = 0;
	TAILQ_FOREACH(wq, &w->z_index, zentry) {
		if (wq == wp)
			return (0);
		lc = wq->layout_cell;
		if (lc == NULL)
			lc = wq->saved_layout_cell;
		if (lc != NULL && (lc->flags & LAYOUT_CELL_FLOATING))
			(*i)++;
	}
	return (-1);
}

static int
restart_layout_capture_cell(struct layout_cell *lc,
    struct restart_layout_cell **out, struct restart_budget *budget,
    char **cause)
{
	struct restart_layout_cell	*cell;
	struct window_pane		*wp;
	u_int				 index;

	*out = NULL;
	if (lc->g.sx < 1 || lc->g.sx > RESTART_MAX_DIMENSION ||
	    lc->g.sy < 1 || lc->g.sy > RESTART_MAX_DIMENSION) {
		restart_set_cause(cause, "restart layout cell is out of range");
		return (-1);
	}
	if (lc->g.xoff < -(int)RESTART_MAX_DIMENSION ||
	    lc->g.xoff > (int)RESTART_MAX_DIMENSION ||
	    lc->g.yoff < -(int)RESTART_MAX_DIMENSION ||
	    lc->g.yoff > (int)RESTART_MAX_DIMENSION) {
		restart_set_cause(cause,
		    "restart layout offset is out of range");
		return (-1);
	}
	cell = restart_calloc(budget, 1, sizeof *cell, cause);
	if (cell == NULL)
		return (-1);
	cell->sx = lc->g.sx;
	cell->sy = lc->g.sy;
	cell->xoff = lc->g.xoff;
	cell->yoff = lc->g.yoff;

	if (lc->type == LAYOUT_TOPBOTTOM)
		cell->type = RESTART_LAYOUT_VERTICAL;
	else if (lc->type == LAYOUT_LEFTRIGHT)
		cell->type = RESTART_LAYOUT_HORIZONTAL;
	else if (lc->type == LAYOUT_WINDOWPANE)
		cell->type = RESTART_LAYOUT_PANE;
	else {
		restart_set_cause(cause, "unknown restart layout cell type");
		goto fail;
	}
	if (cell->type != RESTART_LAYOUT_PANE) {
		*out = cell;
		return (0);
	}

	wp = lc->wp;
	if (wp == NULL) {
		restart_set_cause(cause, "restart layout pane has no pane");
		goto fail;
	}
	if (wp == wp->window->active)
		cell->active = 1;
	else if (window_pane_last_index(wp, &index) == 0) {
		if (index > INT_MAX) {
			restart_set_cause(cause,
			    "restart layout last index is out of range");
			goto fail;
		}
		cell->last_index = index;
		cell->have_last = 1;
	}
	if (window_pane_index(wp, &index) != 0 || index > INT_MAX) {
		restart_set_cause(cause, "restart layout pane has no index");
		goto fail;
	}
	cell->pane_index = index;
	if (lc->flags & LAYOUT_CELL_FLOATING) {
		cell->floating = 1;
		if (restart_layout_zindex(wp, &index) == 0) {
			if (index > INT_MAX) {
				restart_set_cause(cause,
				    "restart layout z index is out of range");
				goto fail;
			}
			cell->z_index = index;
			cell->have_z = 1;
		}
	}
	if (lc->fg.sx != UINT_MAX) {
		if (lc->fg.sx > RESTART_MAX_DIMENSION ||
		    lc->fg.sy > RESTART_MAX_DIMENSION ||
		    lc->fg.xoff < -(int)RESTART_MAX_DIMENSION ||
		    lc->fg.xoff > (int)RESTART_MAX_DIMENSION ||
		    lc->fg.yoff < -(int)RESTART_MAX_DIMENSION ||
		    lc->fg.yoff > (int)RESTART_MAX_DIMENSION) {
			restart_set_cause(cause,
			    "restart saved layout geometry is out of range");
			goto fail;
		}
		cell->fg_sx = lc->fg.sx;
		cell->fg_sy = lc->fg.sy;
		cell->fg_xoff = lc->fg.xoff;
		cell->fg_yoff = lc->fg.yoff;
		cell->have_fg = 1;
	}
	cell->pane_id = wp->id;
	*out = cell;
	return (0);

fail:
	free(cell);
	return (-1);
}

static int
restart_layout_capture_child(struct restart_layout_cell *parent,
    struct restart_layout_cell *child, struct restart_budget *budget,
    char **cause)
{
	struct restart_layout_cell	**new;

	new = restart_grow(budget, parent->children, parent->child_count,
	    parent->child_count + 1, sizeof *parent->children, cause);
	if (new == NULL)
		return (-1);
	parent->children = new;
	parent->children[parent->child_count++] = child;
	return (0);
}

static void
restart_layout_count(const struct restart_layout_cell *cell, size_t *cells,
    size_t *leaves)
{
	size_t	i;

	(*cells)++;
	if (cell->type == RESTART_LAYOUT_PANE)
		(*leaves)++;
	for (i = 0; i < cell->child_count; i++)
		restart_layout_count(cell->children[i], cells, leaves);
}

struct restart_layout_frame {
	struct layout_cell		*next;
	struct restart_layout_cell	*cell;
};

static int
restart_layout_capture(struct layout_cell *root, struct restart_layout **out,
    struct restart_budget *budget, char **cause)
{
	struct restart_layout_frame	*stack, *frame;
	struct restart_layout		*layout;
	struct restart_layout_cell	*cell;
	struct layout_cell		*lc;
	size_t				 size = 16, used = 0, cells = 0;

	*out = NULL;
	if (root == NULL) {
		restart_set_cause(cause, "missing restart layout");
		return (-1);
	}
	layout = restart_calloc(budget, 1, sizeof *layout, cause);
	if (layout == NULL)
		return (-1);
	stack = restart_calloc(budget, size, sizeof *stack, cause);
	if (stack == NULL) {
		restart_layout_free(layout);
		return (-1);
	}
	if (restart_layout_capture_cell(root, &layout->root, budget,
	    cause) != 0)
		goto fail;
	cells++;
	if (layout->root->type != RESTART_LAYOUT_PANE) {
		stack[used].cell = layout->root;
		stack[used].next = TAILQ_FIRST(&root->cells);
		used++;
	}

	while (used != 0) {
		frame = &stack[used - 1];
		if (frame->next == NULL) {
			if (frame->cell->child_count < 2) {
				restart_set_cause(cause,
				    "restart layout node has too few children");
				goto fail;
			}
			used--;
			continue;
		}
		lc = frame->next;
		frame->next = TAILQ_NEXT(lc, entry);
		if (cells == RESTART_MAX_RECORDS) {
			restart_set_cause(cause, "too many live layout cells");
			goto fail;
		}
		if (restart_layout_capture_cell(lc, &cell, budget, cause) != 0)
			goto fail;
		cells++;
		if (restart_layout_capture_child(frame->cell, cell, budget,
		    cause) != 0) {
			free(cell);
			goto fail;
		}
		if (cell->type == RESTART_LAYOUT_PANE)
			continue;
		if (used == RESTART_MAX_LAYOUT_DEPTH) {
			restart_set_cause(cause, "restart layout is too deep");
			goto fail;
		}
		if (used == size) {
			struct restart_layout_frame	*new;

			new = restart_grow(budget, stack, size, size * 2,
			    sizeof *stack, cause);
			if (new == NULL)
				goto fail;
			memset(new + size, 0, size * sizeof *stack);
			stack = new;
			size *= 2;
			frame = &stack[used - 1];
		}
		stack[used].cell = cell;
		stack[used].next = TAILQ_FIRST(&lc->cells);
		used++;
	}

	free(stack);
	restart_layout_count(layout->root, &layout->cell_count,
	    &layout->leaf_count);
	*out = layout;
	return (0);

fail:
	free(stack);
	restart_layout_free(layout);
	return (-1);
}

static int
restart_layout_write_cell(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type,
    const struct restart_layout_cell *cell, u_int depth, char **cause)
{
	struct restart_write_scope	 scope, children;
	size_t				 i;

	if (depth == RESTART_MAX_LAYOUT_DEPTH) {
		restart_set_cause(cause, "restart layout is too deep");
		return (-1);
	}
	if (restart_write_container_begin(rw, parent, type,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0 ||
	    restart_write_u8(rw, &scope, RESTART_LAYOUT_TYPE, cell->type,
	    cause) != 0 ||
	    restart_write_u32(rw, &scope, RESTART_LAYOUT_SX, cell->sx,
	    cause) != 0 ||
	    restart_write_u32(rw, &scope, RESTART_LAYOUT_SY, cell->sy,
	    cause) != 0 ||
	    restart_write_s32(rw, &scope, RESTART_LAYOUT_XOFF, cell->xoff,
	    cause) != 0 ||
	    restart_write_s32(rw, &scope, RESTART_LAYOUT_YOFF, cell->yoff,
	    cause) != 0)
		return (-1);

	if (cell->type == RESTART_LAYOUT_PANE) {
		if (restart_write_u32(rw, &scope, RESTART_LAYOUT_PANE_ID,
		    cell->pane_id, cause) != 0 ||
		    restart_write_u32(rw, &scope, RESTART_LAYOUT_PANE_INDEX,
		    cell->pane_index, cause) != 0)
			return (-1);
		if (cell->have_last &&
		    restart_write_u32(rw, &scope, RESTART_LAYOUT_LAST_INDEX,
		    cell->last_index, cause) != 0)
			return (-1);
		if (cell->have_z &&
		    restart_write_u32(rw, &scope, RESTART_LAYOUT_Z_INDEX,
		    cell->z_index, cause) != 0)
			return (-1);
		if (cell->active &&
		    restart_write_u8(rw, &scope, RESTART_LAYOUT_ACTIVE, 1,
		    cause) != 0)
			return (-1);
		if (cell->floating &&
		    restart_write_u8(rw, &scope, RESTART_LAYOUT_FLOATING, 1,
		    cause) != 0)
			return (-1);
		if (cell->have_fg &&
		    (restart_write_u32(rw, &scope, RESTART_LAYOUT_FG_SX,
		    cell->fg_sx, cause) != 0 ||
		    restart_write_u32(rw, &scope, RESTART_LAYOUT_FG_SY,
		    cell->fg_sy, cause) != 0 ||
		    restart_write_s32(rw, &scope, RESTART_LAYOUT_FG_XOFF,
		    cell->fg_xoff, cause) != 0 ||
		    restart_write_s32(rw, &scope, RESTART_LAYOUT_FG_YOFF,
		    cell->fg_yoff, cause) != 0))
			return (-1);
		return (restart_write_container_end(rw, &scope, cause));
	}

	if (restart_write_container_begin(rw, &scope,
	    RESTART_LAYOUT_CHILDREN, RESTART_RECORD_REQUIRED, &children,
	    cause) != 0)
		return (-1);
	for (i = 0; i < cell->child_count; i++) {
		if (restart_layout_write_cell(rw, &children, 1,
		    cell->children[i], depth + 1, cause) != 0)
			return (-1);
	}
	if (restart_write_container_end(rw, &children, cause) != 0)
		return (-1);
	return (restart_write_container_end(rw, &scope, cause));
}

static int
restart_layout_write(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type,
    const struct restart_layout *layout, char **cause)
{
	return (restart_layout_write_cell(rw, parent, type, layout->root, 0,
	    cause));
}

static int
restart_layout_read_cell(struct restart_record *, struct restart_layout_cell **,
    struct restart_budget *, u_int, size_t *, char **);

static int
restart_layout_read_children(struct restart_record *record,
    struct restart_layout_cell *cell, struct restart_budget *budget,
    u_int depth, size_t *cells, char **cause)
{
	struct restart_reader		 rr;
	struct restart_record		 child;
	struct restart_layout_cell	*kid, **new;
	int				 found;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (restart_state_unknown(&child, "layout child",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			return (-1);
		if (restart_layout_read_cell(&child, &kid, budget, depth + 1,
		    cells, cause) != 0)
			return (-1);
		new = restart_grow(budget, cell->children, cell->child_count,
		    cell->child_count + 1, sizeof *cell->children, cause);
		if (new == NULL) {
			restart_layout_cell_free(kid);
			return (-1);
		}
		cell->children = new;
		cell->children[cell->child_count++] = kid;
	}
	if (found == -1)
		return (-1);
	return (restart_reader_finish_container(&rr, cause));
}

static int
restart_layout_read_cell(struct restart_record *record,
    struct restart_layout_cell **out, struct restart_budget *budget,
    u_int depth, size_t *cells, char **cause)
{
	struct restart_reader		 rr;
	struct restart_record		 child;
	struct restart_layout_cell	*cell;
	uint32_t			 seen = 0;
	uint8_t				 active;
	int				 found;

	*out = NULL;
	if (depth == RESTART_MAX_LAYOUT_DEPTH) {
		restart_set_cause(cause, "restart layout is too deep");
		return (-1);
	}
	if (*cells == RESTART_MAX_RECORDS) {
		restart_set_cause(cause, "too many restart layout cells");
		return (-1);
	}
	(*cells)++;
	cell = restart_calloc(budget, 1, sizeof *cell, cause);
	if (cell == NULL)
		return (-1);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		goto fail;
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_LAYOUT_TYPE ||
		    child.type > RESTART_LAYOUT_CHILDREN) {
			if (restart_state_unknown(&child, "layout", cause) != 0)
				goto fail;
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			goto fail;
		if (seen & (1U << (child.type - 1)))
			goto invalid;
		seen |= 1U << (child.type - 1);
		switch (child.type) {
		case RESTART_LAYOUT_TYPE:
			if (restart_read_u8(&child, &cell->type, cause) != 0)
				goto fail;
			if (cell->type < RESTART_LAYOUT_HORIZONTAL ||
			    cell->type > RESTART_LAYOUT_PANE)
				goto invalid;
			break;
		case RESTART_LAYOUT_SX:
			if (restart_read_u32(&child, &cell->sx, cause) != 0)
				goto fail;
			if (cell->sx < 1 || cell->sx > RESTART_MAX_DIMENSION)
				goto invalid;
			break;
		case RESTART_LAYOUT_SY:
			if (restart_read_u32(&child, &cell->sy, cause) != 0)
				goto fail;
			if (cell->sy < 1 || cell->sy > RESTART_MAX_DIMENSION)
				goto invalid;
			break;
		case RESTART_LAYOUT_XOFF:
			if (restart_read_s32(&child, &cell->xoff, cause) != 0)
				goto fail;
			if (cell->xoff < -(int32_t)RESTART_MAX_DIMENSION ||
			    cell->xoff > (int32_t)RESTART_MAX_DIMENSION)
				goto invalid;
			break;
		case RESTART_LAYOUT_YOFF:
			if (restart_read_s32(&child, &cell->yoff, cause) != 0)
				goto fail;
			if (cell->yoff < -(int32_t)RESTART_MAX_DIMENSION ||
			    cell->yoff > (int32_t)RESTART_MAX_DIMENSION)
				goto invalid;
			break;
		case RESTART_LAYOUT_PANE_ID:
			if (restart_read_u32(&child, &cell->pane_id,
			    cause) != 0)
				goto fail;
			break;
		case RESTART_LAYOUT_PANE_INDEX:
			if (restart_read_u32(&child, &cell->pane_index,
			    cause) != 0)
				goto fail;
			if (cell->pane_index > INT_MAX)
				goto invalid;
			break;
		case RESTART_LAYOUT_LAST_INDEX:
			if (restart_read_u32(&child, &cell->last_index,
			    cause) != 0)
				goto fail;
			if (cell->last_index > INT_MAX)
				goto invalid;
			cell->have_last = 1;
			break;
		case RESTART_LAYOUT_Z_INDEX:
			if (restart_read_u32(&child, &cell->z_index,
			    cause) != 0)
				goto fail;
			if (cell->z_index > INT_MAX)
				goto invalid;
			cell->have_z = 1;
			break;
		case RESTART_LAYOUT_ACTIVE:
			if (restart_read_u8(&child, &active, cause) != 0)
				goto fail;
			if (active != 1)
				goto invalid;
			cell->active = 1;
			break;
		case RESTART_LAYOUT_FLOATING:
			if (restart_read_u8(&child, &active, cause) != 0)
				goto fail;
			if (active != 1)
				goto invalid;
			cell->floating = 1;
			break;
		case RESTART_LAYOUT_FG_SX:
			if (restart_read_u32(&child, &cell->fg_sx, cause) != 0)
				goto fail;
			if (cell->fg_sx > RESTART_MAX_DIMENSION)
				goto invalid;
			break;
		case RESTART_LAYOUT_FG_SY:
			if (restart_read_u32(&child, &cell->fg_sy, cause) != 0)
				goto fail;
			if (cell->fg_sy > RESTART_MAX_DIMENSION)
				goto invalid;
			break;
		case RESTART_LAYOUT_FG_XOFF:
			if (restart_read_s32(&child, &cell->fg_xoff,
			    cause) != 0)
				goto fail;
			if (cell->fg_xoff < -(int32_t)RESTART_MAX_DIMENSION ||
			    cell->fg_xoff > (int32_t)RESTART_MAX_DIMENSION)
				goto invalid;
			break;
		case RESTART_LAYOUT_FG_YOFF:
			if (restart_read_s32(&child, &cell->fg_yoff,
			    cause) != 0)
				goto fail;
			if (cell->fg_yoff < -(int32_t)RESTART_MAX_DIMENSION ||
			    cell->fg_yoff > (int32_t)RESTART_MAX_DIMENSION)
				goto invalid;
			break;
		case RESTART_LAYOUT_CHILDREN:
			if (restart_layout_read_children(&child, cell, budget,
			    depth, cells, cause) != 0)
				goto fail;
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		goto fail;
	if ((seen & RESTART_LAYOUT_SEEN_BASE) != RESTART_LAYOUT_SEEN_BASE)
		goto invalid;
	if (cell->type == RESTART_LAYOUT_PANE) {
		if ((seen & RESTART_LAYOUT_SEEN_LEAF) !=
		    RESTART_LAYOUT_SEEN_LEAF ||
		    (seen & RESTART_LAYOUT_SEEN_NODEONLY) != 0)
			goto invalid;
		if (cell->active && cell->have_last)
			goto invalid;
		seen &= RESTART_LAYOUT_SEEN_FG;
		if (seen != 0 && seen != RESTART_LAYOUT_SEEN_FG)
			goto invalid;
		cell->have_fg = (seen == RESTART_LAYOUT_SEEN_FG);
	} else {
		if ((seen & RESTART_LAYOUT_SEEN_NODEONLY) == 0 ||
		    (seen & RESTART_LAYOUT_SEEN_LEAFONLY) != 0 ||
		    cell->child_count < 2)
			goto invalid;
	}
	*out = cell;
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart layout cell");
fail:
	restart_layout_cell_free(cell);
	return (-1);
}

static int
restart_layout_read(struct restart_record *record,
    struct restart_layout **out, struct restart_budget *budget, char **cause)
{
	struct restart_layout	*layout;
	size_t			 cells = 0;

	*out = NULL;
	layout = restart_calloc(budget, 1, sizeof *layout, cause);
	if (layout == NULL)
		return (-1);
	if (restart_layout_read_cell(record, &layout->root, budget, 0, &cells,
	    cause) != 0) {
		free(layout);
		return (-1);
	}
	restart_layout_count(layout->root, &layout->cell_count,
	    &layout->leaf_count);
	*out = layout;
	return (0);
}

static void
restart_session_free_contents(struct restart_session *session)
{
	restart_environment_free(&session->environment);
	restart_options_free(&session->options);
	free(session->winlinks);
	free(session->last_indices.items);
	free(session->cwd);
	free(session->name);
}

static void
restart_window_free_contents(struct restart_window *window)
{
	restart_layout_free(window->layout);
	restart_layout_free(window->visible_layout);
	free(window->old_layout);
	free(window->z_order.items);
	free(window->last_panes.items);
	free(window->pane_order.items);
	restart_options_free(&window->options);
	free(window->name);
}

static void
restart_pane_free_contents(struct restart_pane *pane)
{
	restart_terminal_free(pane->terminal);
	restart_options_free(&pane->options);
	restart_state_string_list_free(&pane->argv);
	free(pane->shell);
	free(pane->cwd);
	free(pane->tty);
}

struct restart_live_pane {
	uint32_t		 id;
	struct window_pane	*wp;
};

struct restart_state_write_ctx {
	const struct restart_state	 *state;
	struct restart_live_pane	 *live;
	size_t				  live_count;
};

static int
restart_options_write(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type,
    const struct restart_options *options, char **cause)
{
	struct restart_write_scope	 scope, entry, items, item;
	const struct restart_option	*option;
	size_t				 i, j;

	if (restart_write_container_begin(rw, parent, type,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0)
		return (-1);
	for (i = 0; i < options->count; i++) {
		option = &options->entries[i];
		if (restart_write_container_begin(rw, &scope, 1,
		    RESTART_RECORD_REQUIRED, &entry, cause) != 0 ||
		    restart_write_string(rw, &entry, RESTART_OPTION_NAME,
		    option->name, "option name", cause) != 0 ||
		    restart_write_u8(rw, &entry, RESTART_OPTION_TYPE,
		    option->type, cause) != 0 ||
		    restart_write_u8(rw, &entry, RESTART_OPTION_ARRAY,
		    option->is_array, cause) != 0)
			return (-1);
		if (!option->is_array) {
			if (option->string != NULL) {
				if (restart_write_string(rw, &entry,
				    RESTART_OPTION_STRING_VALUE,
				    option->string, "option value",
				    cause) != 0)
					return (-1);
			} else if (restart_write_s64(rw, &entry,
			    RESTART_OPTION_NUMBER_VALUE, option->number,
			    cause) != 0)
				return (-1);
		} else {
			if (restart_write_container_begin(rw, &entry,
			    RESTART_OPTION_ITEMS, RESTART_RECORD_REQUIRED,
			    &items, cause) != 0)
				return (-1);
			for (j = 0; j < option->item_count; j++) {
				if (restart_write_container_begin(rw, &items, 1,
				    RESTART_RECORD_REQUIRED, &item,
				    cause) != 0 ||
				    restart_write_string(rw, &item,
				    RESTART_ITEM_KEY, option->items[j].key,
				    "option array key",
				    cause) != 0)
					return (-1);
				if (option->items[j].string != NULL) {
					if (restart_write_string(rw, &item,
					    RESTART_ITEM_STRING_VALUE,
					    option->items[j].string,
					    "option array value",
					    cause) != 0)
						return (-1);
				} else if (restart_write_s64(rw, &item,
				    RESTART_ITEM_NUMBER_VALUE,
				    option->items[j].number, cause) != 0)
					return (-1);
				if (restart_write_container_end(rw, &item,
				    cause) != 0)
					return (-1);
			}
			if (restart_write_container_end(rw, &items, cause) != 0)
				return (-1);
		}
		if (restart_write_container_end(rw, &entry, cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

static int
restart_meta_write(struct restart_writer *rw,
    struct restart_write_scope *parent, const struct restart_state *state,
    char **cause)
{
	struct restart_write_scope	scope;

	if (restart_write_container_begin(rw, parent, RESTART_SERVER_META,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0 ||
	    restart_state_write_timeval(rw, &scope, RESTART_META_START_TIME,
	    &state->start_time, cause) != 0 ||
	    restart_write_u32(rw, &scope, RESTART_META_NEXT_SESSION_ID,
	    state->next_session_id, cause) != 0 ||
	    restart_write_u32(rw, &scope, RESTART_META_NEXT_WINDOW_ID,
	    state->next_window_id, cause) != 0 ||
	    restart_write_u32(rw, &scope, RESTART_META_NEXT_PANE_ID,
	    state->next_pane_id, cause) != 0 ||
	    restart_write_u32(rw, &scope, RESTART_META_NEXT_ACTIVE_POINT,
	    state->next_active_point, cause) != 0 ||
	    restart_write_u64(rw, &scope,
	    RESTART_META_NEXT_HYPERLINK_EXTERNAL_ID,
	    state->next_hyperlink_external_id, cause) != 0)
		return (-1);
	return (restart_write_container_end(rw, &scope, cause));
}

static int
restart_groups_write(struct restart_writer *rw,
    struct restart_write_scope *parent, const struct restart_state *state,
    char **cause)
{
	struct restart_write_scope	 scope, entry;
	const struct restart_group	*group;
	size_t				 i;

	if (restart_write_container_begin(rw, parent, RESTART_SERVER_GROUPS,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0)
		return (-1);
	for (i = 0; i < state->group_count; i++) {
		group = &state->groups[i];
		if (restart_write_container_begin(rw, &scope, 1,
		    RESTART_RECORD_REQUIRED, &entry, cause) != 0 ||
		    restart_write_string(rw, &entry, RESTART_GROUP_NAME,
		    group->name, "session group name", cause) != 0 ||
		    restart_state_write_id_list(rw, &entry,
		    RESTART_GROUP_MEMBERS, &group->members, cause) != 0 ||
		    restart_write_container_end(rw, &entry, cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

static int
restart_session_write(struct restart_writer *rw,
    struct restart_write_scope *parent,
    const struct restart_session *session, char **cause)
{
	struct restart_write_scope	 entry, times, links, link;
	size_t				 i;

	if (restart_write_container_begin(rw, parent, 1,
	    RESTART_RECORD_REQUIRED, &entry, cause) != 0 ||
	    restart_write_u32(rw, &entry, RESTART_SESSION_ID, session->id,
	    cause) != 0 ||
	    restart_write_string(rw, &entry, RESTART_SESSION_NAME,
	    session->name, "session name", cause) != 0 ||
	    restart_write_string(rw, &entry, RESTART_SESSION_CWD,
	    session->cwd, "session working directory", cause) != 0)
		return (-1);
	if (restart_write_container_begin(rw, &entry, RESTART_SESSION_TIMES,
	    RESTART_RECORD_REQUIRED, &times, cause) != 0 ||
	    restart_state_write_timeval(rw, &times,
	    RESTART_SESSION_TIME_CREATION, &session->creation_time,
	    cause) != 0 ||
	    restart_state_write_timeval(rw, &times,
	    RESTART_SESSION_TIME_LAST_ATTACHED, &session->last_attached_time,
	    cause) != 0 ||
	    restart_state_write_timeval(rw, &times,
	    RESTART_SESSION_TIME_ACTIVITY, &session->activity_time,
	    cause) != 0 ||
	    restart_state_write_timeval(rw, &times,
	    RESTART_SESSION_TIME_LAST_ACTIVITY, &session->last_activity_time,
	    cause) != 0 ||
	    restart_write_container_end(rw, &times, cause) != 0)
		return (-1);
	if (restart_write_s32(rw, &entry, RESTART_SESSION_CURRENT_INDEX,
	    session->current_index, cause) != 0 ||
	    restart_state_write_index_list(rw, &entry,
	    RESTART_SESSION_LAST_INDICES, &session->last_indices, cause) != 0)
		return (-1);
	if (restart_write_container_begin(rw, &entry, RESTART_SESSION_WINLINKS,
	    RESTART_RECORD_REQUIRED, &links, cause) != 0)
		return (-1);
	for (i = 0; i < session->winlink_count; i++) {
		if (restart_write_container_begin(rw, &links, 1,
		    RESTART_RECORD_REQUIRED, &link, cause) != 0 ||
		    restart_write_s32(rw, &link, RESTART_WINLINK_INDEX,
		    session->winlinks[i].index, cause) != 0 ||
		    restart_write_u32(rw, &link, RESTART_WINLINK_WINDOW_ID,
		    session->winlinks[i].window_id, cause) != 0 ||
		    restart_write_u8(rw, &link, RESTART_WINLINK_ALERT_FLAGS,
		    session->winlinks[i].flags, cause) != 0 ||
		    restart_write_container_end(rw, &link, cause) != 0)
			return (-1);
	}
	if (restart_write_container_end(rw, &links, cause) != 0)
		return (-1);
	if (restart_options_write(rw, &entry, RESTART_SESSION_OPTIONS,
	    &session->options, cause) != 0 ||
	    restart_environment_write(rw, &entry, RESTART_SESSION_ENVIRONMENT,
	    &session->environment, cause) != 0)
		return (-1);
	if (session->have_termios &&
	    restart_termios_write(rw, &entry, RESTART_SESSION_TERMIOS_CC,
	    &session->termios_cc, cause) != 0)
		return (-1);
	return (restart_write_container_end(rw, &entry, cause));
}

static int
restart_sessions_write(struct restart_writer *rw,
    struct restart_write_scope *parent, const struct restart_state *state,
    char **cause)
{
	struct restart_write_scope	scope;
	size_t				i;

	if (restart_write_container_begin(rw, parent, RESTART_SERVER_SESSIONS,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0)
		return (-1);
	for (i = 0; i < state->session_count; i++) {
		if (restart_session_write(rw, &scope, &state->sessions[i],
		    cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

static int
restart_window_write(struct restart_writer *rw,
    struct restart_write_scope *parent, const struct restart_window *window,
    char **cause)
{
	struct restart_write_scope	 entry, times;
	u_char				 data[24];
	int				 error = -1;

	if (restart_write_container_begin(rw, parent, 1,
	    RESTART_RECORD_REQUIRED, &entry, cause) != 0 ||
	    restart_write_u32(rw, &entry, RESTART_WINDOW_ID, window->id,
	    cause) != 0 ||
	    restart_write_string(rw, &entry, RESTART_WINDOW_NAME, window->name,
	    "window name", cause) != 0)
		return (-1);
	if (restart_write_container_begin(rw, &entry, RESTART_WINDOW_TIMES,
	    RESTART_RECORD_REQUIRED, &times, cause) != 0 ||
	    restart_state_write_timeval(rw, &times, RESTART_WINDOW_TIME_NAME,
	    &window->name_time, cause) != 0 ||
	    restart_state_write_timeval(rw, &times,
	    RESTART_WINDOW_TIME_ACTIVITY, &window->activity_time, cause) != 0 ||
	    restart_state_write_timeval(rw, &times,
	    RESTART_WINDOW_TIME_CREATION, &window->creation_time, cause) != 0 ||
	    restart_write_container_end(rw, &times, cause) != 0)
		return (-1);
	if (restart_write_u32(rw, &entry, RESTART_WINDOW_ACTIVE_PANE,
	    window->active_pane_id, cause) != 0)
		return (-1);
	if (window->have_modal &&
	    restart_write_u32(rw, &entry, RESTART_WINDOW_MODAL_PANE,
	    window->modal_pane_id, cause) != 0)
		return (-1);
	if (window->have_modal_last &&
	    restart_write_u32(rw, &entry, RESTART_WINDOW_MODAL_LAST,
	    window->modal_last_id, cause) != 0)
		return (-1);
	if (restart_state_write_id_list(rw, &entry, RESTART_WINDOW_PANE_ORDER,
	    &window->pane_order, cause) != 0 ||
	    restart_state_write_id_list(rw, &entry, RESTART_WINDOW_LAST_PANES,
	    &window->last_panes, cause) != 0 ||
	    restart_state_write_id_list(rw, &entry, RESTART_WINDOW_Z_ORDER,
	    &window->z_order, cause) != 0 ||
	    restart_write_s32(rw, &entry, RESTART_WINDOW_LAST_LAYOUT,
	    window->last_layout, cause) != 0)
		return (-1);
	if (restart_layout_write(rw, &entry, RESTART_WINDOW_LAYOUT,
	    window->layout, cause) != 0)
		goto out;
	if (window->visible_layout != NULL &&
	    restart_layout_write(rw, &entry, RESTART_WINDOW_VISIBLE_LAYOUT,
	    window->visible_layout, cause) != 0)
		goto out;
	if (window->have_old_layout &&
	    restart_write_string(rw, &entry, RESTART_WINDOW_OLD_LAYOUT,
	    window->old_layout, "window layout", cause) != 0)
		goto out;
	restart_state_put32(data, window->sx);
	restart_state_put32(data + 4, window->sy);
	restart_state_put32(data + 8, window->manual_sx);
	restart_state_put32(data + 12, window->manual_sy);
	restart_state_put32(data + 16, window->xpixel);
	restart_state_put32(data + 20, window->ypixel);
	if (restart_write_record(rw, &entry, RESTART_WINDOW_SIZE,
	    RESTART_RECORD_REQUIRED, data, 24, cause) != 0)
		goto out;
	restart_state_put32(data, window->last_new_x);
	restart_state_put32(data + 4, window->last_new_y);
	if (restart_write_record(rw, &entry, RESTART_WINDOW_LAST_NEW_POSITION,
	    RESTART_RECORD_REQUIRED, data, 8, cause) != 0 ||
	    restart_write_u8(rw, &entry, RESTART_WINDOW_FLAGS, window->flags,
	    cause) != 0 ||
	    restart_options_write(rw, &entry, RESTART_WINDOW_OPTIONS,
	    &window->options, cause) != 0 ||
	    restart_write_container_end(rw, &entry, cause) != 0)
		goto out;
	error = 0;
out:
	return (error);
}

static int
restart_windows_write(struct restart_writer *rw,
    struct restart_write_scope *parent, const struct restart_state *state,
    char **cause)
{
	struct restart_write_scope	scope;
	size_t				i;

	if (restart_write_container_begin(rw, parent, RESTART_SERVER_WINDOWS,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0)
		return (-1);
	for (i = 0; i < state->window_count; i++) {
		if (restart_window_write(rw, &scope, &state->windows[i],
		    cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

static int
restart_pane_write(struct restart_writer *rw,
    struct restart_write_scope *parent, const struct restart_pane *pane,
    struct window_pane *live, char **cause)
{
	struct restart_write_scope	 entry, command, process, result;
	struct restart_write_scope	 activity, terminal;
	u_char				 data[16];

	if (restart_write_container_begin(rw, parent, 1,
	    RESTART_RECORD_REQUIRED, &entry, cause) != 0 ||
	    restart_write_u32(rw, &entry, RESTART_PANE_ID, pane->id,
	    cause) != 0 ||
	    restart_write_u32(rw, &entry, RESTART_PANE_WINDOW_ID,
	    pane->window_id, cause) != 0 ||
	    restart_write_u32(rw, &entry, RESTART_PANE_ACTIVE_POINT,
	    pane->active_point, cause) != 0 ||
	    restart_write_u8(rw, &entry, RESTART_PANE_LIFECYCLE,
	    pane->lifecycle, cause) != 0 ||
	    restart_write_u16(rw, &entry, RESTART_PANE_FLAGS, pane->flags,
	    cause) != 0)
		return (-1);
	restart_state_put32(data, pane->sx);
	restart_state_put32(data + 4, pane->sy);
	restart_state_put32(data + 8,
	    (uint32_t)restart_state_s64_encode(pane->xoff));
	restart_state_put32(data + 12,
	    (uint32_t)restart_state_s64_encode(pane->yoff));
	if (restart_write_record(rw, &entry, RESTART_PANE_GEOMETRY,
	    RESTART_RECORD_REQUIRED, data, 16, cause) != 0)
		return (-1);
	if (restart_write_container_begin(rw, &entry, RESTART_PANE_COMMAND,
	    RESTART_RECORD_REQUIRED, &command, cause) != 0 ||
	    restart_state_write_string_list(rw, &command,
	    RESTART_COMMAND_ARGV, &pane->argv, "pane command argument",
	    cause) != 0)
		return (-1);
	if (pane->have_shell &&
	    restart_write_string(rw, &command, RESTART_COMMAND_SHELL,
	    pane->shell, "pane shell", cause) != 0)
		return (-1);
	if (pane->have_cwd &&
	    restart_write_string(rw, &command, RESTART_COMMAND_CWD, pane->cwd,
	    "pane working directory", cause) != 0)
		return (-1);
	if (restart_write_container_end(rw, &command, cause) != 0)
		return (-1);
	if (restart_write_container_begin(rw, &entry, RESTART_PANE_PROCESS,
	    RESTART_RECORD_REQUIRED, &process, cause) != 0 ||
	    restart_write_s64(rw, &process, RESTART_PROCESS_PID, pane->pid,
	    cause) != 0 ||
	    restart_write_string(rw, &process, RESTART_PROCESS_TTY, pane->tty,
	    "pane tty", cause) != 0)
		return (-1);
	if (pane->have_result) {
		if (restart_write_container_begin(rw, &process,
		    RESTART_PROCESS_DEAD_RESULT, RESTART_RECORD_REQUIRED,
		    &result, cause) != 0 ||
		    restart_write_u8(rw, &result, RESTART_DEAD_KIND,
		    pane->dead_kind, cause) != 0 ||
		    restart_write_u32(rw, &result, RESTART_DEAD_VALUE,
		    pane->dead_value, cause) != 0 ||
		    restart_write_container_end(rw, &result, cause) != 0)
			return (-1);
	}
	if (pane->have_dead_time &&
	    restart_state_write_timeval(rw, &process,
	    RESTART_PROCESS_DEAD_TIME, &pane->dead_time, cause) != 0)
		return (-1);
	if (restart_write_container_end(rw, &process, cause) != 0)
		return (-1);
	if (restart_write_container_begin(rw, &entry, RESTART_PANE_ACTIVITY,
	    RESTART_RECORD_REQUIRED, &activity, cause) != 0 ||
	    restart_write_u64(rw, &activity,
	    RESTART_ACTIVITY_OUTPUT_GENERATION, pane->output_generation,
	    cause) != 0 ||
	    restart_write_s64(rw, &activity, RESTART_ACTIVITY_LAST_OUTPUT,
	    pane->last_output, cause) != 0 ||
	    restart_write_s64(rw, &activity, RESTART_ACTIVITY_LAST_PROMPT,
	    pane->last_prompt, cause) != 0 ||
	    restart_write_s64(rw, &activity, RESTART_ACTIVITY_COMMAND_START,
	    pane->command_start, cause) != 0 ||
	    restart_write_s64(rw, &activity, RESTART_ACTIVITY_COMMAND_END,
	    pane->command_end, cause) != 0 ||
	    restart_write_s32(rw, &activity, RESTART_ACTIVITY_COMMAND_STATUS,
	    pane->command_status, cause) != 0 ||
	    restart_write_u8(rw, &activity, RESTART_ACTIVITY_LAST_THEME,
	    pane->last_theme, cause) != 0 ||
	    restart_write_container_end(rw, &activity, cause) != 0)
		return (-1);
	if (restart_options_write(rw, &entry, RESTART_PANE_OPTIONS,
	    &pane->options, cause) != 0)
		return (-1);
	if (restart_write_record_begin(rw, &entry, RESTART_PANE_TERMINAL,
	    RESTART_RECORD_REQUIRED, &terminal, cause) != 0)
		return (-1);
	if (live != NULL) {
		if (restart_terminal_write_nested(rw, live, cause) != 0)
			return (-1);
	} else if (restart_terminal_write_decoded_nested(rw, pane->terminal,
	    cause) != 0)
		return (-1);
	if (restart_write_record_end(rw, &terminal, cause) != 0)
		return (-1);
	return (restart_write_container_end(rw, &entry, cause));
}

static int
restart_panes_write(struct restart_writer *rw,
    struct restart_write_scope *parent,
    const struct restart_state_write_ctx *ctx, char **cause)
{
	struct restart_write_scope	 scope;
	const struct restart_state	*state = ctx->state;
	struct window_pane		*live;
	size_t				 i, low, high, middle;

	if (restart_write_container_begin(rw, parent, RESTART_SERVER_PANES,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0)
		return (-1);
	for (i = 0; i < state->pane_count; i++) {
		live = NULL;
		if (ctx->live != NULL) {
			low = 0;
			high = ctx->live_count;
			while (low < high) {
				middle = low + (high - low) / 2;
				if (ctx->live[middle].id ==
				    state->panes[i].id) {
					live = ctx->live[middle].wp;
					if (live == NULL ||
					    live->id != ctx->live[middle].id ||
					    ctx->live[middle].id !=
					    state->panes[i].id) {
						restart_set_cause(cause,
						    "restart pane %u does not "
						    "match its live pane",
						    state->panes[i].id);
						return (-1);
					}
					break;
				}
				if (ctx->live[middle].id < state->panes[i].id)
					low = middle + 1;
				else
					high = middle;
			}
			if (live == NULL) {
				restart_set_cause(cause,
				    "restart pane %u has no live pane",
				    state->panes[i].id);
				return (-1);
			}
		}
		if (restart_pane_write(rw, &scope, &state->panes[i], live,
		    cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}


/* Capture the paste buffers. */
static int
restart_buffers_capture(struct restart_state *state,
    struct restart_budget *budget, char **cause)
{
	struct paste_buffer	*pb = NULL;
	struct restart_buffer	*out;
	const char		*data, *name;
	size_t			 size;
	void			*new;

	while ((pb = paste_walk(pb)) != NULL) {
		if (state->buffer_count == RESTART_MAX_RECORDS) {
			restart_set_cause(cause, "too many restart buffers");
			return (-1);
		}
		new = restart_grow(budget, state->buffers,
		    state->buffer_count, state->buffer_count + 1,
		    sizeof *state->buffers, cause);
		if (new == NULL)
			return (-1);
		state->buffers = new;
		out = &state->buffers[state->buffer_count++];
		memset(out, 0, sizeof *out);

		name = paste_buffer_name(pb);
		data = paste_buffer_data(pb, &size);
		if (restart_state_capture_string(name, &out->name, budget,
		    "buffer name", cause) != 0)
			return (-1);
		if (size > RESTART_MAX_FIELD) {
			restart_set_cause(cause, "restart buffer is too large");
			return (-1);
		}
		out->data.data = restart_alloc(budget, size, cause);
		if (out->data.data == NULL)
			return (-1);
		memcpy(out->data.data, data, size);
		out->data.size = size;
		out->created = paste_buffer_created(pb);
		out->order = paste_buffer_order(pb);
		out->automatic = (paste_buffer_automatic(pb) != 0);
	}
	return (0);
}

/*
 * Install the saved paste buffers. Each carries the order it was saved with,
 * so the stack is rebuilt by those values rather than by the sequence of
 * these calls, and a reordering of the records cannot change what
 * paste_get_top returns.
 */
static void
restart_buffers_publish(const struct restart_state *state)
{
	const struct restart_buffer	*buffer;
	char				*data, *cause;
	size_t				 i;

	for (i = 0; i < state->buffer_count; i++) {
		buffer = &state->buffers[i];
		data = xmalloc(buffer->data.size);
		memcpy(data, buffer->data.data, buffer->data.size);
		if (paste_restart_set(data, buffer->data.size, buffer->name,
		    buffer->order, buffer->automatic, buffer->created,
		    &cause) != 0) {
			log_debug("%s: %s", __func__, cause);
			free(cause);
		}
	}
}

/* Write the paste buffers. */
static int
restart_buffers_write(struct restart_writer *rw,
    struct restart_write_scope *parent, const struct restart_state *state,
    char **cause)
{
	struct restart_write_scope	 scope, entry;
	const struct restart_buffer	*buffer;
	size_t				 i;

	if (restart_write_container_begin(rw, parent, RESTART_SERVER_BUFFERS,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0)
		return (-1);
	for (i = 0; i < state->buffer_count; i++) {
		buffer = &state->buffers[i];
		if (restart_write_container_begin(rw, &scope, 1,
		    RESTART_RECORD_REQUIRED, &entry, cause) != 0 ||
		    restart_write_string(rw, &entry, RESTART_BUFFER_NAME,
		    buffer->name, "buffer name", cause) != 0 ||
		    restart_write_stream(rw, &entry, RESTART_BUFFER_DATA,
		    buffer->data.data, buffer->data.size, "buffer",
		    cause) != 0 ||
		    restart_write_s64(rw, &entry, RESTART_BUFFER_CREATED,
		    buffer->created, cause) != 0 ||
		    restart_write_u8(rw, &entry, RESTART_BUFFER_AUTOMATIC,
		    buffer->automatic, cause) != 0 ||
		    restart_write_u32(rw, &entry, RESTART_BUFFER_ORDER,
		    buffer->order, cause) != 0 ||
		    restart_write_container_end(rw, &entry, cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

/* Read one paste buffer. */
static int
restart_buffer_read(struct restart_record *record,
    struct restart_buffer *buffer, struct restart_budget *budget, char **cause)
{
	struct restart_reader	 rr;
	struct restart_record	 child;
	uint32_t		 seen = 0;
	uint8_t			 automatic;
	int			 found;

	memset(buffer, 0, sizeof *buffer);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_BUFFER_NAME ||
		    child.type > RESTART_BUFFER_ORDER) {
			if (restart_state_unknown(&child, "buffer", cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			return (-1);
		if (seen & (1U << (child.type - 1)))
			goto invalid;
		seen |= 1U << (child.type - 1);
		switch (child.type) {
		case RESTART_BUFFER_NAME:
			if (restart_read_string(&child, &buffer->name, budget,
			    cause) != 0)
				return (-1);
			if (*buffer->name == '\0')
				goto invalid;
			break;
		case RESTART_BUFFER_DATA:
			if (restart_read_stream(&child, &buffer->data, budget,
			    cause) != 0)
				return (-1);
			if (buffer->data.size == 0)
				goto invalid;
			break;
		case RESTART_BUFFER_CREATED:
			if (restart_read_s64(&child, &buffer->created,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_BUFFER_AUTOMATIC:
			if (restart_read_u8(&child, &automatic, cause) != 0)
				return (-1);
			if (automatic > 1)
				goto invalid;
			buffer->automatic = automatic;
			break;
		case RESTART_BUFFER_ORDER:
			if (restart_read_u32(&child, &buffer->order,
			    cause) != 0)
				return (-1);
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if (seen != 0x1fU)
		goto invalid;
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart buffer");
	return (-1);
}

/* Read the paste buffers. */
static int
restart_buffers_read(struct restart_record *record,
    struct restart_state *state, struct restart_budget *budget, char **cause)
{
	struct restart_reader	 rr;
	struct restart_record	 child;
	void			*new;
	size_t			 i;
	int			 found;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (restart_state_unknown(&child, "buffer list",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			return (-1);
		if (state->buffer_count == RESTART_MAX_RECORDS)
			goto invalid;
		new = restart_grow(budget, state->buffers,
		    state->buffer_count, state->buffer_count + 1,
		    sizeof *state->buffers, cause);
		if (new == NULL)
			return (-1);
		state->buffers = new;
		if (restart_buffer_read(&child,
		    &state->buffers[state->buffer_count++], budget,
		    cause) != 0)
			return (-1);
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	for (i = 1; i < state->buffer_count; i++) {
		if (strcmp(state->buffers[i].name,
		    state->buffers[i - 1].name) == 0)
			goto invalid;
	}
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart buffer list");
	return (-1);
}

static int
restart_descriptors_write(struct restart_writer *rw,
    struct restart_write_scope *parent, const struct restart_state *state,
    char **cause)
{
	struct restart_write_scope	scope;
	u_char				data[12];
	size_t				i;

	if (restart_write_container_begin(rw, parent,
	    RESTART_SERVER_DESCRIPTOR_KEYS, RESTART_RECORD_REQUIRED, &scope,
	    cause) != 0)
		return (-1);
	for (i = 0; i < state->descriptor_count; i++) {
		restart_state_put32(data, state->descriptors[i].pane_id);
		restart_state_put64(data + 4, restart_state_s64_encode(
		    state->descriptors[i].pid));
		if (restart_write_record(rw, &scope, 1,
		    RESTART_RECORD_REQUIRED, data, sizeof data, cause) != 0)
			return (-1);
	}
	return (restart_write_container_end(rw, &scope, cause));
}

static int
restart_state_read_bool(struct restart_record *record, uint8_t *value,
    char **cause)
{
	if (restart_read_u8(record, value, cause) != 0)
		return (-1);
	if (*value > 1) {
		restart_set_cause(cause, "invalid restart boolean");
		return (-1);
	}
	return (0);
}

static int
restart_option_read_items(struct restart_record *record,
    struct restart_option *option, struct restart_budget *budget, char **cause)
{
	struct restart_reader		 rr, ir;
	struct restart_record		 child, field;
	struct restart_option_item	*new, *item;
	uint32_t			 seen;
	int				 found, inner;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (restart_state_unknown(&child, "option-item-list",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			return (-1);
		new = restart_grow(budget, option->items, option->item_count,
		    option->item_count + 1, sizeof *option->items, cause);
		if (new == NULL)
			return (-1);
		option->items = new;
		item = &option->items[option->item_count++];
		memset(item, 0, sizeof *item);
		if (restart_reader_open_container(&child, &ir, budget,
		    cause) != 0)
			return (-1);
		seen = 0;
		while ((inner = restart_reader_next(&ir, &field, cause)) == 1) {
			if (field.type < RESTART_ITEM_KEY ||
			    field.type > RESTART_ITEM_NUMBER_VALUE) {
				if (restart_state_unknown(&field, "option-item",
				    cause) != 0)
					return (-1);
				continue;
			}
			if (restart_state_required(&field, cause) != 0 ||
			    (seen & (1U << field.type)) != 0)
				goto invalid;
			seen |= 1U << field.type;
			switch (field.type) {
			case RESTART_ITEM_KEY:
				if (restart_read_string(&field, &item->key,
				    budget, cause) != 0)
					return (-1);
				break;
			case RESTART_ITEM_STRING_VALUE:
				if (restart_read_string(&field, &item->string,
				    budget, cause) != 0)
					return (-1);
				break;
			case RESTART_ITEM_NUMBER_VALUE:
				if (restart_read_s64(&field, &item->number,
				    cause) != 0 ||
				    ibuf_size(&field.payload) != 0)
					goto invalid;
				break;
			}
		}
		if (inner == -1 ||
		    restart_reader_finish_container(&ir, cause) != 0)
			return (-1);
		if ((seen & (1U << RESTART_ITEM_KEY)) == 0)
			goto invalid;
		if (((seen & (1U << RESTART_ITEM_STRING_VALUE)) != 0) ==
		    ((seen & (1U << RESTART_ITEM_NUMBER_VALUE)) != 0))
			goto invalid;
		if (option->item_count > 1 &&
		    restart_option_item_cmp(
		    &option->items[option->item_count - 1],
		    &option->items[option->item_count - 2]) < 0) {
			restart_set_cause(cause,
			    "restart option items are not ordered");
			return (-1);
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart option item");
	return (-1);
}

static int
restart_options_read(struct restart_record *record,
    struct restart_options *options, struct restart_budget *budget,
    char **cause)
{
	struct restart_reader	 rr, er;
	struct restart_record	 child, field;
	struct restart_option	*new, *option;
	uint32_t		 seen;
	int			 found, inner;
	size_t			 i;

	memset(options, 0, sizeof *options);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (restart_state_unknown(&child, "option-set",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			return (-1);
		new = restart_grow(budget, options->entries, options->count,
		    options->count + 1, sizeof *options->entries, cause);
		if (new == NULL)
			return (-1);
		options->entries = new;
		option = &options->entries[options->count++];
		memset(option, 0, sizeof *option);
		if (restart_reader_open_container(&child, &er, budget,
		    cause) != 0)
			return (-1);
		seen = 0;
		while ((inner = restart_reader_next(&er, &field, cause)) == 1) {
			if (field.type < RESTART_OPTION_NAME ||
			    field.type > RESTART_OPTION_ITEMS) {
				if (restart_state_unknown(&field, "option",
				    cause) != 0)
					return (-1);
				continue;
			}
			if (restart_state_required(&field, cause) != 0 ||
			    (seen & (1U << field.type)) != 0)
				goto invalid;
			seen |= 1U << field.type;
			switch (field.type) {
			case RESTART_OPTION_NAME:
				if (restart_read_string(&field, &option->name,
				    budget, cause) != 0)
					return (-1);
				break;
			case RESTART_OPTION_TYPE:
				if (restart_read_u8(&field, &option->type,
				    cause) != 0)
					return (-1);
				if (option->type < RESTART_OPTION_USER ||
				    option->type > RESTART_OPTION_COMMAND)
					goto invalid;
				break;
			case RESTART_OPTION_ARRAY:
				if (restart_state_read_bool(&field,
				    &option->is_array, cause) != 0)
					return (-1);
				break;
			case RESTART_OPTION_STRING_VALUE:
				if (restart_read_string(&field,
				    &option->string, budget, cause) != 0)
					return (-1);
				break;
			case RESTART_OPTION_NUMBER_VALUE:
				if (restart_read_s64(&field, &option->number,
				    cause) != 0 ||
				    ibuf_size(&field.payload) != 0)
					goto invalid;
				break;
			case RESTART_OPTION_ITEMS:
				if (restart_option_read_items(&field, option,
				    budget, cause) != 0)
					return (-1);
				break;
			}
		}
		if (inner == -1 ||
		    restart_reader_finish_container(&er, cause) != 0)
			return (-1);
		if ((seen & 0x0eU) != 0x0eU)
			goto invalid;
		if (option->is_array) {
			if ((seen & (1U << RESTART_OPTION_ITEMS)) == 0 ||
			    (seen & ((1U << RESTART_OPTION_STRING_VALUE) |
			    (1U << RESTART_OPTION_NUMBER_VALUE))) != 0)
				goto invalid;
		} else {
			if ((seen & (1U << RESTART_OPTION_ITEMS)) != 0)
				goto invalid;
			if (((seen & (1U << RESTART_OPTION_STRING_VALUE)) !=
			    0) == ((seen &
			    (1U << RESTART_OPTION_NUMBER_VALUE)) != 0))
				goto invalid;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	for (i = 1; i < options->count; i++) {
		if (strcmp(options->entries[i].name,
		    options->entries[i - 1].name) < 0) {
			restart_set_cause(cause,
			    "restart option names are not ordered");
			return (-1);
		}
	}
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart option");
	return (-1);
}

static int
restart_meta_read(struct restart_record *record, struct restart_state *state,
    struct restart_budget *budget, char **cause)
{
	struct restart_reader	rr;
	struct restart_record	child;
	uint32_t		seen = 0;
	int			found;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_META_START_TIME ||
		    child.type > RESTART_META_NEXT_HYPERLINK_EXTERNAL_ID) {
			if (restart_state_unknown(&child, "metadata",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0 ||
		    (seen & (1U << child.type)) != 0)
			goto invalid;
		seen |= 1U << child.type;
		switch (child.type) {
		case RESTART_META_START_TIME:
			if (restart_state_read_timeval(&child,
			    &state->start_time, cause) != 0)
				return (-1);
			break;
		case RESTART_META_NEXT_SESSION_ID:
			if (restart_read_u32(&child, &state->next_session_id,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_META_NEXT_WINDOW_ID:
			if (restart_read_u32(&child, &state->next_window_id,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_META_NEXT_PANE_ID:
			if (restart_read_u32(&child, &state->next_pane_id,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_META_NEXT_ACTIVE_POINT:
			if (restart_read_u32(&child, &state->next_active_point,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_META_NEXT_HYPERLINK_EXTERNAL_ID:
			if (restart_read_u64(&child,
			    &state->next_hyperlink_external_id, cause) != 0)
				return (-1);
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if ((seen & 0x7eU) != 0x7eU)
		goto invalid;
	if (state->next_session_id == UINT32_MAX ||
	    state->next_window_id == UINT32_MAX ||
	    state->next_pane_id == UINT32_MAX ||
	    state->next_active_point == UINT32_MAX)
		goto invalid;
	if (state->next_hyperlink_external_id < 1 ||
	    state->next_hyperlink_external_id >= (uint64_t)LLONG_MAX)
		goto invalid;
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart metadata");
	return (-1);
}

static int
restart_groups_read(struct restart_record *record, struct restart_state *state,
    struct restart_budget *budget, char **cause)
{
	struct restart_reader	 rr, gr;
	struct restart_record	 child, field;
	struct restart_group	*new, *group;
	uint32_t		 seen;
	int			 found, inner;
	size_t			 i;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (restart_state_unknown(&child, "group-list",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			return (-1);
		new = restart_grow(budget, state->groups, state->group_count,
		    state->group_count + 1, sizeof *state->groups, cause);
		if (new == NULL)
			return (-1);
		state->groups = new;
		group = &state->groups[state->group_count++];
		memset(group, 0, sizeof *group);
		if (restart_reader_open_container(&child, &gr, budget,
		    cause) != 0)
			return (-1);
		seen = 0;
		while ((inner = restart_reader_next(&gr, &field, cause)) == 1) {
			if (field.type < RESTART_GROUP_NAME ||
			    field.type > RESTART_GROUP_MEMBERS) {
				if (restart_state_unknown(&field, "group",
				    cause) != 0)
					return (-1);
				continue;
			}
			if (restart_state_required(&field, cause) != 0 ||
			    (seen & (1U << field.type)) != 0)
				goto invalid;
			seen |= 1U << field.type;
			if (field.type == RESTART_GROUP_NAME) {
				if (restart_read_string(&field, &group->name,
				    budget, cause) != 0)
					return (-1);
			} else if (restart_state_read_id_list(&field,
			    &group->members, "group member", budget,
			    cause) != 0)
				return (-1);
		}
		if (inner == -1 ||
		    restart_reader_finish_container(&gr, cause) != 0)
			return (-1);
		if ((seen & 0x06U) != 0x06U || group->members.count == 0)
			goto invalid;
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	for (i = 1; i < state->group_count; i++) {
		if (strcmp(state->groups[i].name,
		    state->groups[i - 1].name) < 0) {
			restart_set_cause(cause,
			    "restart group names are not ordered");
			return (-1);
		}
	}
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart group");
	return (-1);
}

static int
restart_session_read_times(struct restart_record *record,
    struct restart_session *session, struct restart_budget *budget,
    char **cause)
{
	struct restart_reader	rr;
	struct restart_record	child;
	uint32_t		seen = 0;
	int			found;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_SESSION_TIME_CREATION ||
		    child.type > RESTART_SESSION_TIME_LAST_ACTIVITY) {
			if (restart_state_unknown(&child, "session-times",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0 ||
		    (seen & (1U << child.type)) != 0)
			goto invalid;
		seen |= 1U << child.type;
		switch (child.type) {
		case RESTART_SESSION_TIME_CREATION:
			if (restart_state_read_timeval(&child,
			    &session->creation_time, cause) != 0)
				return (-1);
			break;
		case RESTART_SESSION_TIME_LAST_ATTACHED:
			if (restart_state_read_timeval(&child,
			    &session->last_attached_time, cause) != 0)
				return (-1);
			break;
		case RESTART_SESSION_TIME_ACTIVITY:
			if (restart_state_read_timeval(&child,
			    &session->activity_time, cause) != 0)
				return (-1);
			break;
		case RESTART_SESSION_TIME_LAST_ACTIVITY:
			if (restart_state_read_timeval(&child,
			    &session->last_activity_time, cause) != 0)
				return (-1);
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if ((seen & 0x1eU) != 0x1eU)
		goto invalid;
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart session times");
	return (-1);
}


static int
restart_winlinks_read(struct restart_record *record,
    struct restart_session *session, struct restart_budget *budget,
    char **cause)
{
	struct restart_reader	 rr, wr;
	struct restart_record	 child, field;
	struct restart_winlink	*new, *link;
	uint32_t		 seen;
	int			 found, inner;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (restart_state_unknown(&child, "winlink-list",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			return (-1);
		new = restart_grow(budget, session->winlinks,
		    session->winlink_count, session->winlink_count + 1,
		    sizeof *session->winlinks, cause);
		if (new == NULL)
			return (-1);
		session->winlinks = new;
		link = &session->winlinks[session->winlink_count++];
		memset(link, 0, sizeof *link);
		if (restart_reader_open_container(&child, &wr, budget,
		    cause) != 0)
			return (-1);
		seen = 0;
		while ((inner = restart_reader_next(&wr, &field, cause)) == 1) {
			if (field.type < RESTART_WINLINK_INDEX ||
			    field.type > RESTART_WINLINK_ALERT_FLAGS) {
				if (restart_state_unknown(&field, "winlink",
				    cause) != 0)
					return (-1);
				continue;
			}
			if (restart_state_required(&field, cause) != 0 ||
			    (seen & (1U << field.type)) != 0)
				goto invalid;
			seen |= 1U << field.type;
			switch (field.type) {
			case RESTART_WINLINK_INDEX:
				if (restart_read_s32(&field, &link->index,
				    cause) != 0 ||
				    ibuf_size(&field.payload) != 0)
					goto invalid;
				if (link->index < 0)
					goto invalid;
				break;
			case RESTART_WINLINK_WINDOW_ID:
				if (restart_read_u32(&field, &link->window_id,
				    cause) != 0)
					return (-1);
				break;
			case RESTART_WINLINK_ALERT_FLAGS:
				if (restart_read_u8(&field, &link->flags,
				    cause) != 0)
					return (-1);
				if ((link->flags &
				    ~RESTART_WINLINK_ALERTS) != 0)
					goto invalid;
				break;
			}
		}
		if (inner == -1 ||
		    restart_reader_finish_container(&wr, cause) != 0)
			return (-1);
		if ((seen & 0x0eU) != 0x0eU)
			goto invalid;
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart winlink");
	return (-1);
}

static int
restart_sessions_read(struct restart_record *record,
    struct restart_state *state, struct restart_budget *budget, char **cause)
{
	struct restart_reader	 rr, sr;
	struct restart_record	 child, field;
	struct restart_session	*new, *session;
	uint32_t		 seen;
	size_t			 i;
	int			 found, inner;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (restart_state_unknown(&child, "session-list",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			return (-1);
		new = restart_grow(budget, state->sessions,
		    state->session_count, state->session_count + 1,
		    sizeof *state->sessions, cause);
		if (new == NULL)
			return (-1);
		state->sessions = new;
		session = &state->sessions[state->session_count++];
		memset(session, 0, sizeof *session);
		if (restart_reader_open_container(&child, &sr, budget,
		    cause) != 0)
			return (-1);
		seen = 0;
		while ((inner = restart_reader_next(&sr, &field, cause)) == 1) {
			if (field.type < RESTART_SESSION_ID ||
			    field.type > RESTART_SESSION_TERMIOS_CC) {
				if (restart_state_unknown(&field, "session",
				    cause) != 0)
					return (-1);
				continue;
			}
			if (restart_state_required(&field, cause) != 0 ||
			    (seen & (1U << field.type)) != 0)
				goto invalid;
			seen |= 1U << field.type;
			switch (field.type) {
			case RESTART_SESSION_ID:
				if (restart_read_u32(&field, &session->id,
				    cause) != 0)
					return (-1);
				break;
			case RESTART_SESSION_NAME:
				if (restart_read_string(&field, &session->name,
				    budget, cause) != 0)
					return (-1);
				break;
			case RESTART_SESSION_CWD:
				if (restart_read_string(&field, &session->cwd,
				    budget, cause) != 0)
					return (-1);
				break;
			case RESTART_SESSION_TIMES:
				if (restart_session_read_times(&field, session,
				    budget, cause) != 0)
					return (-1);
				break;
			case RESTART_SESSION_CURRENT_INDEX:
				if (restart_read_s32(&field,
				    &session->current_index, cause) != 0 ||
				    ibuf_size(&field.payload) != 0)
					goto invalid;
				if (session->current_index < 0)
					goto invalid;
				break;
			case RESTART_SESSION_LAST_INDICES:
				if (restart_state_read_index_list(&field,
				    &session->last_indices, budget, cause) != 0)
					return (-1);
				break;
			case RESTART_SESSION_WINLINKS:
				if (restart_winlinks_read(&field, session,
				    budget, cause) != 0)
					return (-1);
				break;
			case RESTART_SESSION_OPTIONS:
				if (restart_options_read(&field,
				    &session->options, budget, cause) != 0)
					return (-1);
				break;
			case RESTART_SESSION_ENVIRONMENT:
				if (restart_environment_read(&field,
				    &session->environment, budget, cause) != 0)
					return (-1);
				break;
			case RESTART_SESSION_TERMIOS_CC:
				if (restart_termios_read(&field,
				    &session->termios_cc, budget, cause) != 0)
					return (-1);
				session->have_termios = 1;
				break;
			}
		}
		if (inner == -1 ||
		    restart_reader_finish_container(&sr, cause) != 0)
			return (-1);
		if ((seen & 0x03feU) != 0x03feU)
			goto invalid;
		if (session->winlink_count == 0)
			goto invalid;
		if (session->have_termios &&
		    restart_termios_validate(&session->termios_cc, cause) != 0)
			return (-1);
		for (i = 1; i < session->winlink_count; i++) {
			if (session->winlinks[i].index <
			    session->winlinks[i - 1].index) {
				restart_set_cause(cause,
				    "restart winlinks are not ordered");
				return (-1);
			}
			if (session->winlinks[i].index ==
			    session->winlinks[i - 1].index) {
				restart_set_cause(cause,
				    "duplicate restart winlink index");
				return (-1);
			}
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	for (i = 1; i < state->session_count; i++) {
		if (state->sessions[i].id < state->sessions[i - 1].id) {
			restart_set_cause(cause,
			    "restart sessions are not ordered");
			return (-1);
		}
	}
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart session");
	return (-1);
}

static int
restart_window_read_times(struct restart_record *record,
    struct restart_window *window, struct restart_budget *budget, char **cause)
{
	struct restart_reader	rr;
	struct restart_record	child;
	uint32_t		seen = 0;
	int			found;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_WINDOW_TIME_NAME ||
		    child.type > RESTART_WINDOW_TIME_CREATION) {
			if (restart_state_unknown(&child, "window-times",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0 ||
		    (seen & (1U << child.type)) != 0)
			goto invalid;
		seen |= 1U << child.type;
		switch (child.type) {
		case RESTART_WINDOW_TIME_NAME:
			if (restart_state_read_timeval(&child,
			    &window->name_time, cause) != 0)
				return (-1);
			break;
		case RESTART_WINDOW_TIME_ACTIVITY:
			if (restart_state_read_timeval(&child,
			    &window->activity_time, cause) != 0)
				return (-1);
			break;
		case RESTART_WINDOW_TIME_CREATION:
			if (restart_state_read_timeval(&child,
			    &window->creation_time, cause) != 0)
				return (-1);
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if ((seen & 0x0eU) != 0x0eU)
		goto invalid;
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart window times");
	return (-1);
}

static int
restart_window_read_size(struct restart_record *record,
    struct restart_window *window, char **cause)
{
	const u_char	*data;

	if (ibuf_size(&record->payload) != 24)
		goto invalid;
	data = ibuf_data(&record->payload);
	window->sx = restart_state_get32(data);
	window->sy = restart_state_get32(data + 4);
	window->manual_sx = restart_state_get32(data + 8);
	window->manual_sy = restart_state_get32(data + 12);
	window->xpixel = restart_state_get32(data + 16);
	window->ypixel = restart_state_get32(data + 20);
	if (ibuf_skip(&record->payload, 24) != 0)
		goto invalid;
	if (window->sx < 1 || window->sx > RESTART_MAX_DIMENSION ||
	    window->sy < 1 || window->sy > RESTART_MAX_DIMENSION)
		goto invalid;
	if (window->xpixel < 1 || window->ypixel < 1)
		goto invalid;
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart window size");
	return (-1);
}

static int
restart_windows_read(struct restart_record *record,
    struct restart_state *state, struct restart_budget *budget, char **cause)
{
	struct restart_reader	 rr, wr;
	struct restart_record	 child, field;
	struct restart_window	*new, *window;
	const u_char		*data;
	char			*layout = NULL;
	uint32_t		 seen;
	int			 found, inner, error = -1;
	size_t			 i;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (restart_state_unknown(&child, "window-list",
			    cause) != 0)
				goto out;
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			goto out;
		new = restart_grow(budget, state->windows, state->window_count,
		    state->window_count + 1, sizeof *state->windows, cause);
		if (new == NULL)
			goto out;
		state->windows = new;
		window = &state->windows[state->window_count++];
		memset(window, 0, sizeof *window);
		if (restart_reader_open_container(&child, &wr, budget,
		    cause) != 0)
			goto out;
		seen = 0;
		while ((inner = restart_reader_next(&wr, &field, cause)) == 1) {
			if (field.type < RESTART_WINDOW_ID ||
			    field.type > RESTART_WINDOW_OPTIONS) {
				if (restart_state_unknown(&field, "window",
				    cause) != 0)
					goto out;
				continue;
			}
			if (restart_state_required(&field, cause) != 0 ||
			    (seen & (1U << field.type)) != 0)
				goto invalid;
			seen |= 1U << field.type;
			switch (field.type) {
			case RESTART_WINDOW_ID:
				if (restart_read_u32(&field, &window->id,
				    cause) != 0)
					goto out;
				break;
			case RESTART_WINDOW_NAME:
				if (restart_read_string(&field, &window->name,
				    budget, cause) != 0)
					goto out;
				break;
			case RESTART_WINDOW_TIMES:
				if (restart_window_read_times(&field, window,
				    budget, cause) != 0)
					goto out;
				break;
			case RESTART_WINDOW_ACTIVE_PANE:
				if (restart_read_u32(&field,
				    &window->active_pane_id, cause) != 0)
					goto out;
				break;
			case RESTART_WINDOW_MODAL_PANE:
				if (restart_read_u32(&field,
				    &window->modal_pane_id, cause) != 0)
					goto out;
				window->have_modal = 1;
				break;
			case RESTART_WINDOW_MODAL_LAST:
				if (restart_read_u32(&field,
				    &window->modal_last_id, cause) != 0)
					goto out;
				window->have_modal_last = 1;
				break;
			case RESTART_WINDOW_PANE_ORDER:
				if (restart_state_read_id_list(&field,
				    &window->pane_order, "pane order", budget,
				    cause) != 0)
					goto out;
				break;
			case RESTART_WINDOW_LAST_PANES:
				if (restart_state_read_id_list(&field,
				    &window->last_panes, "last pane", budget,
				    cause) != 0)
					goto out;
				break;
			case RESTART_WINDOW_Z_ORDER:
				if (restart_state_read_id_list(&field,
				    &window->z_order, "z order", budget,
				    cause) != 0)
					goto out;
				break;
			case RESTART_WINDOW_LAST_LAYOUT:
				if (restart_read_s32(&field,
				    &window->last_layout, cause) != 0 ||
				    ibuf_size(&field.payload) != 0)
					goto invalid;
				if (window->last_layout < -1 ||
				window->last_layout > 6) {
					restart_set_cause(cause,
					    "invalid restart window "
					        "last layout");
					return (-1);
				}
				break;
			case RESTART_WINDOW_LAYOUT:
				if (restart_layout_read(&field, &window->layout,
				    budget, cause) != 0)
					goto out;
				break;
			case RESTART_WINDOW_VISIBLE_LAYOUT:
				if (restart_layout_read(&field,
				    &window->visible_layout, budget,
				    cause) != 0)
					goto out;
				break;
			case RESTART_WINDOW_OLD_LAYOUT:
				if (restart_read_string(&field,
				    &window->old_layout, budget, cause) != 0)
					goto out;
				window->have_old_layout = 1;
				break;
			case RESTART_WINDOW_SIZE:
				if (restart_window_read_size(&field, window,
				    cause) != 0)
					goto out;
				break;
			case RESTART_WINDOW_LAST_NEW_POSITION:
				if (ibuf_size(&field.payload) != 8)
					goto invalid;
				data = ibuf_data(&field.payload);
				window->last_new_x = restart_state_get32(data);
				window->last_new_y =
				    restart_state_get32(data + 4);
				if (ibuf_skip(&field.payload, 8) != 0)
					goto invalid;
				break;
			case RESTART_WINDOW_FLAGS:
				if (restart_read_u8(&field, &window->flags,
				    cause) != 0)
					goto out;
				if ((window->flags &
				    ~RESTART_WINDOW_ZOOMED) != 0)
					goto invalid;
				break;
			case RESTART_WINDOW_OPTIONS:
				if (restart_options_read(&field,
				    &window->options, budget, cause) != 0)
					goto out;
				break;
			}
		}
		if (inner == -1 ||
		    restart_reader_finish_container(&wr, cause) != 0)
			goto out;
		if ((seen & 0x3cf9eU) != 0x3cf9eU) {
			restart_set_cause(cause,
			    "restart window is missing a required "
			        "record");
			return (-1);
		}
		if (window->have_modal_last && !window->have_modal) {
			restart_set_cause(cause,
			    "restart modal last pane without a modal "
			        "pane");
			return (-1);
		}
		if (window->flags & RESTART_WINDOW_ZOOMED) {
			if (window->visible_layout == NULL) {
				restart_set_cause(cause,
				    "restart zoomed window has no "
				        "visible layout");
				return (-1);
			}
		} else if (window->visible_layout != NULL) {
			restart_set_cause(cause,
			    "restart unzoomed window has a visible "
			        "layout");
			return (-1);
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		goto out;
	for (i = 1; i < state->window_count; i++) {
		if (state->windows[i].id < state->windows[i - 1].id) {
			restart_set_cause(cause,
			    "restart windows are not ordered");
			goto out;
		}
	}
	error = 0;
	goto out;

invalid:
	restart_set_cause(cause, "invalid restart window record");
out:
	free(layout);
	return (error);
}

static int
restart_pane_read_command(struct restart_record *record,
    struct restart_pane *pane, struct restart_budget *budget, char **cause)
{
	struct restart_reader	rr;
	struct restart_record	child;
	uint32_t		seen = 0;
	int			found;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_COMMAND_ARGV ||
		    child.type > RESTART_COMMAND_CWD) {
			if (restart_state_unknown(&child, "command",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0 ||
		    (seen & (1U << child.type)) != 0)
			goto invalid;
		seen |= 1U << child.type;
		switch (child.type) {
		case RESTART_COMMAND_ARGV:
			if (restart_state_read_string_list(&child, &pane->argv,
			    budget, cause) != 0)
				return (-1);
			break;
		case RESTART_COMMAND_SHELL:
			if (restart_read_string(&child, &pane->shell, budget,
			    cause) != 0)
				return (-1);
			pane->have_shell = 1;
			break;
		case RESTART_COMMAND_CWD:
			if (restart_read_string(&child, &pane->cwd, budget,
			    cause) != 0)
				return (-1);
			pane->have_cwd = 1;
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if ((seen & 0x02U) == 0)
		goto invalid;
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart pane command");
	return (-1);
}

static int
restart_pane_read_result(struct restart_record *record,
    struct restart_pane *pane, struct restart_budget *budget, char **cause)
{
	struct restart_reader	rr;
	struct restart_record	child;
	uint32_t		seen = 0;
	int			found;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_DEAD_KIND ||
		    child.type > RESTART_DEAD_VALUE) {
			if (restart_state_unknown(&child, "dead-result",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0 ||
		    (seen & (1U << child.type)) != 0)
			goto invalid;
		seen |= 1U << child.type;
		if (child.type == RESTART_DEAD_KIND) {
			if (restart_read_u8(&child, &pane->dead_kind,
			    cause) != 0)
				return (-1);
		} else if (restart_read_u32(&child, &pane->dead_value,
		    cause) != 0)
			return (-1);
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if ((seen & 0x06U) != 0x06U)
		goto invalid;
	if (pane->dead_kind == RESTART_DEAD_EXITED) {
		if (pane->dead_value > 255)
			goto invalid;
	} else if (pane->dead_kind == RESTART_DEAD_SIGNALED) {
		if (pane->dead_value < 1 || pane->dead_value >= NSIG)
			goto invalid;
	} else
		goto invalid;
	pane->have_result = 1;
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart dead result");
	return (-1);
}

static int
restart_pane_read_process(struct restart_record *record,
    struct restart_pane *pane, struct restart_budget *budget, char **cause)
{
	struct restart_reader	rr;
	struct restart_record	child;
	uint32_t		seen = 0;
	int			found;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_PROCESS_PID ||
		    child.type > RESTART_PROCESS_DEAD_TIME) {
			if (restart_state_unknown(&child, "process",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0 ||
		    (seen & (1U << child.type)) != 0)
			goto invalid;
		seen |= 1U << child.type;
		switch (child.type) {
		case RESTART_PROCESS_PID:
			if (restart_read_s64(&child, &pane->pid, cause) != 0 ||
			    ibuf_size(&child.payload) != 0)
				goto invalid;
			if ((int64_t)(pid_t)pane->pid != pane->pid)
				goto invalid;
			break;
		case RESTART_PROCESS_TTY:
			if (restart_read_string(&child, &pane->tty, budget,
			    cause) != 0)
				return (-1);
			if (strlen(pane->tty) >= TTY_NAME_MAX)
				goto invalid;
			break;
		case RESTART_PROCESS_DEAD_RESULT:
			if (restart_pane_read_result(&child, pane, budget,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_PROCESS_DEAD_TIME:
			if (restart_state_read_timeval(&child, &pane->dead_time,
			    cause) != 0)
				return (-1);
			pane->have_dead_time = 1;
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if ((seen & 0x06U) != 0x06U)
		goto invalid;
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart pane process");
	return (-1);
}

static int
restart_pane_read_activity(struct restart_record *record,
    struct restart_pane *pane, struct restart_budget *budget, char **cause)
{
	struct restart_reader	rr;
	struct restart_record	child;
	enum client_theme	theme;
	uint32_t		seen = 0;
	int			found;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type < RESTART_ACTIVITY_OUTPUT_GENERATION ||
		    child.type > RESTART_ACTIVITY_LAST_THEME) {
			if (restart_state_unknown(&child, "activity",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0 ||
		    (seen & (1U << child.type)) != 0)
			goto invalid;
		seen |= 1U << child.type;
		switch (child.type) {
		case RESTART_ACTIVITY_OUTPUT_GENERATION:
			if (restart_read_u64(&child, &pane->output_generation,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_ACTIVITY_LAST_OUTPUT:
			if (restart_read_s64(&child, &pane->last_output,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_ACTIVITY_LAST_PROMPT:
			if (restart_read_s64(&child, &pane->last_prompt,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_ACTIVITY_COMMAND_START:
			if (restart_read_s64(&child, &pane->command_start,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_ACTIVITY_COMMAND_END:
			if (restart_read_s64(&child, &pane->command_end,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_ACTIVITY_COMMAND_STATUS:
			if (restart_read_s32(&child, &pane->command_status,
			    cause) != 0 || ibuf_size(&child.payload) != 0)
				goto invalid;
			if (pane->command_status < -1 ||
			    pane->command_status > 255)
				goto invalid;
			break;
		case RESTART_ACTIVITY_LAST_THEME:
			if (restart_read_u8(&child, &pane->last_theme,
			    cause) != 0)
				return (-1);
			if (restart_theme_from_wire(pane->last_theme,
			    &theme) != 0)
				goto invalid;
			break;
		}
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	if ((seen & 0xfeU) != 0xfeU)
		goto invalid;
	if ((int64_t)(time_t)pane->last_output != pane->last_output ||
	    (int64_t)(time_t)pane->last_prompt != pane->last_prompt ||
	    (int64_t)(time_t)pane->command_start != pane->command_start ||
	    (int64_t)(time_t)pane->command_end != pane->command_end)
		goto invalid;
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart pane activity");
	return (-1);
}

static int
restart_pane_read_terminal(struct restart_record *record,
    struct restart_pane *pane, struct restart_budget *budget, char **cause)
{
	struct restart_reader	nested;
	size_t			size;

	size = ibuf_size(&record->payload);
	if (restart_reader_open_envelope(&nested, ibuf_data(&record->payload),
	    size, RESTART_KIND_TERMINAL, budget, cause) != 0)
		return (-1);
	if (restart_terminal_read_nested(&nested, &pane->terminal, cause) != 0)
		return (-1);
	if (restart_reader_finish_envelope(&nested, cause) != 0)
		return (-1);
	if (pane->terminal->features != nested.required_features) {
		restart_set_cause(cause, "restart terminal feature mismatch");
		return (-1);
	}
	if (ibuf_skip(&record->payload, size) != 0) {
		restart_set_cause(cause, "invalid restart pane terminal");
		return (-1);
	}
	return (0);
}

static int
restart_panes_read(struct restart_record *record, struct restart_state *state,
    struct restart_budget *budget, char **cause)
{
	struct restart_reader	 rr, pr;
	struct restart_record	 child, field;
	struct restart_pane	*new, *pane;
	const u_char		*data;
	uint32_t		 seen;
	int			 found, inner;
	size_t			 i;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (restart_state_unknown(&child, "pane-list",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			return (-1);
		new = restart_grow(budget, state->panes, state->pane_count,
		    state->pane_count + 1, sizeof *state->panes, cause);
		if (new == NULL)
			return (-1);
		state->panes = new;
		pane = &state->panes[state->pane_count++];
		memset(pane, 0, sizeof *pane);
		if (restart_reader_open_container(&child, &pr, budget,
		    cause) != 0)
			return (-1);
		seen = 0;
		while ((inner = restart_reader_next(&pr, &field, cause)) == 1) {
			if (field.type < RESTART_PANE_ID ||
			    field.type > RESTART_PANE_TERMINAL) {
				if (restart_state_unknown(&field, "pane",
				    cause) != 0)
					return (-1);
				continue;
			}
			if (restart_state_required(&field, cause) != 0 ||
			    (seen & (1U << field.type)) != 0)
				goto invalid;
			seen |= 1U << field.type;
			switch (field.type) {
			case RESTART_PANE_ID:
				if (restart_read_u32(&field, &pane->id,
				    cause) != 0)
					return (-1);
				break;
			case RESTART_PANE_WINDOW_ID:
				if (restart_read_u32(&field, &pane->window_id,
				    cause) != 0)
					return (-1);
				break;
			case RESTART_PANE_ACTIVE_POINT:
				if (restart_read_u32(&field,
				    &pane->active_point,
				    cause) != 0)
					return (-1);
				break;
			case RESTART_PANE_LIFECYCLE:
				if (restart_read_u8(&field, &pane->lifecycle,
				    cause) != 0)
					return (-1);
				if (pane->lifecycle <
				    RESTART_LIFECYCLE_LIVE_FD ||
				    pane->lifecycle >
				RESTART_LIFECYCLE_INACTIVE) {
					restart_set_cause(cause,
					    "invalid restart pane "
					        "lifecycle");
					return (-1);
				}
				break;
			case RESTART_PANE_FLAGS:
				if (restart_read_u16(&field, &pane->flags,
				    cause) != 0)
					return (-1);
				if ((pane->flags &
				    ~restart_flags_mask(restart_pane_flag_map,
				nitems(restart_pane_flag_map))) != 0) {
					restart_set_cause(cause,
					    "invalid restart pane "
					        "flags");
					return (-1);
				}
				break;
			case RESTART_PANE_GEOMETRY:
				if (ibuf_size(&field.payload) != 16)
					goto invalid;
				data = ibuf_data(&field.payload);
				pane->sx = restart_state_get32(data);
				pane->sy = restart_state_get32(data + 4);
				pane->xoff = (int32_t)restart_state_s64_decode(
				    restart_state_get32(data + 8));
				pane->yoff = (int32_t)restart_state_s64_decode(
				    restart_state_get32(data + 12));
				if (ibuf_skip(&field.payload, 16) != 0)
					goto invalid;
				if (pane->sx < 1 ||
				    pane->sx > RESTART_MAX_DIMENSION ||
				    pane->sy < 1 ||
				    pane->sy > RESTART_MAX_DIMENSION ||
				    pane->xoff <
				    -(int32_t)RESTART_MAX_DIMENSION ||
				    pane->xoff >
				    (int32_t)RESTART_MAX_DIMENSION ||
				    pane->yoff <
				    -(int32_t)RESTART_MAX_DIMENSION ||
				pane->yoff > (int32_t)RESTART_MAX_DIMENSION) {
					restart_set_cause(cause,
					    "invalid restart pane "
					        "geometry");
					return (-1);
				}
				break;
			case RESTART_PANE_COMMAND:
				if (restart_pane_read_command(&field, pane,
				    budget, cause) != 0)
					return (-1);
				break;
			case RESTART_PANE_PROCESS:
				if (restart_pane_read_process(&field, pane,
				    budget, cause) != 0)
					return (-1);
				break;
			case RESTART_PANE_ACTIVITY:
				if (restart_pane_read_activity(&field, pane,
				    budget, cause) != 0)
					return (-1);
				break;
			case RESTART_PANE_OPTIONS:
				if (restart_options_read(&field, &pane->options,
				    budget, cause) != 0)
					return (-1);
				break;
			case RESTART_PANE_TERMINAL:
				if (restart_pane_read_terminal(&field, pane,
				    budget, cause) != 0)
					return (-1);
				break;
			}
		}
		if (inner == -1 ||
		    restart_reader_finish_container(&pr, cause) != 0)
			return (-1);
		if ((seen & 0x0ffeU) != 0x0ffeU) {
			restart_set_cause(cause,
			    "restart pane is missing a required record");
			return (-1);
		}
		if (restart_pane_validate_running(pane, cause) != 0)
			return (-1);
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	for (i = 1; i < state->pane_count; i++) {
		if (state->panes[i].id < state->panes[i - 1].id) {
			restart_set_cause(cause,
			    "restart panes are not ordered");
			return (-1);
		}
	}
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart pane record");
	return (-1);
}

static int
restart_descriptors_read(struct restart_record *record,
    struct restart_state *state, struct restart_budget *budget, char **cause)
{
	struct restart_reader		 rr;
	struct restart_record		 child;
	struct restart_descriptor_key	*new, *key;
	const u_char			*data;
	int				 found;
	size_t			 i;

	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1) {
			if (restart_state_unknown(&child, "descriptor-key",
			    cause) != 0)
				return (-1);
			continue;
		}
		if (restart_state_required(&child, cause) != 0)
			return (-1);
		new = restart_grow(budget, state->descriptors,
		    state->descriptor_count, state->descriptor_count + 1,
		    sizeof *state->descriptors, cause);
		if (new == NULL)
			return (-1);
		state->descriptors = new;
		key = &state->descriptors[state->descriptor_count++];
		memset(key, 0, sizeof *key);
		if (ibuf_size(&child.payload) != 12)
			goto invalid;
		data = ibuf_data(&child.payload);
		key->pane_id = restart_state_get32(data);
		key->pid = restart_state_s64_decode(
		    restart_state_get64(data + 4));
		if (ibuf_skip(&child.payload, 12) != 0)
			goto invalid;
		if (key->pid <= 0 || (int64_t)(pid_t)key->pid != key->pid)
			goto invalid;
	}
	if (found == -1 || restart_reader_finish_container(&rr, cause) != 0)
		return (-1);
	for (i = 1; i < state->descriptor_count; i++) {
		if (state->descriptors[i].pane_id <
		    state->descriptors[i - 1].pane_id) {
			restart_set_cause(cause,
			    "restart descriptor keys are not ordered");
			return (-1);
		}
	}
	return (0);

invalid:
	restart_set_cause(cause, "invalid restart descriptor key");
	return (-1);
}

static int
restart_session_compare(const void *a0, const void *b0)
{
	const struct restart_session *a = a0, *b = b0;

	return (restart_u32_compare(a->id, b->id));
}

static int
restart_window_compare(const void *a0, const void *b0)
{
	const struct restart_window *a = a0, *b = b0;

	return (restart_u32_compare(a->id, b->id));
}

static int
restart_live_pane_compare(const void *a0, const void *b0)
{
	const struct restart_live_pane *a = a0, *b = b0;

	return (restart_u32_compare(a->id, b->id));
}

static int
restart_pane_compare(const void *a0, const void *b0)
{
	const struct restart_pane *a = a0, *b = b0;

	return (restart_u32_compare(a->id, b->id));
}

static int
restart_group_compare(const void *a0, const void *b0)
{
	const struct restart_group *a = a0, *b = b0;

	return (strcmp(a->name, b->name));
}

static int
restart_descriptor_compare(const void *a0, const void *b0)
{
	const struct restart_descriptor_key *a = a0, *b = b0;

	return (restart_u32_compare(a->pane_id, b->pane_id));
}

static struct restart_session *
restart_find_session(const struct restart_state *state, uint32_t id)
{
	size_t	low = 0, high = state->session_count, mid;

	while (low < high) {
		mid = low + (high - low) / 2;
		if (state->sessions[mid].id == id)
			return (&state->sessions[mid]);
		if (state->sessions[mid].id < id)
			low = mid + 1;
		else
			high = mid;
	}
	return (NULL);
}

static struct restart_window *
restart_find_window(const struct restart_state *state, uint32_t id)
{
	size_t	low = 0, high = state->window_count, mid;

	while (low < high) {
		mid = low + (high - low) / 2;
		if (state->windows[mid].id == id)
			return (&state->windows[mid]);
		if (state->windows[mid].id < id)
			low = mid + 1;
		else
			high = mid;
	}
	return (NULL);
}

static struct restart_pane *
restart_find_pane(const struct restart_state *state, uint32_t id)
{
	size_t	low = 0, high = state->pane_count, mid;

	while (low < high) {
		mid = low + (high - low) / 2;
		if (state->panes[mid].id == id)
			return (&state->panes[mid]);
		if (state->panes[mid].id < id)
			low = mid + 1;
		else
			high = mid;
	}
	return (NULL);
}

static void
restart_state_set_option_scopes(struct restart_state *state)
{
	size_t	i;

	restart_options_set_scope(&state->global_options,
	    OPTIONS_TABLE_SERVER);
	restart_options_set_scope(&state->global_session_options,
	    OPTIONS_TABLE_SESSION);
	restart_options_set_scope(&state->global_window_options,
	    OPTIONS_TABLE_WINDOW);
	for (i = 0; i < state->session_count; i++) {
		restart_options_set_scope(&state->sessions[i].options,
		    OPTIONS_TABLE_SESSION);
	}
	for (i = 0; i < state->window_count; i++) {
		restart_options_set_scope(&state->windows[i].options,
		    OPTIONS_TABLE_WINDOW);
	}
	for (i = 0; i < state->pane_count; i++) {
		restart_options_set_scope(&state->panes[i].options,
		    OPTIONS_TABLE_PANE);
	}
}

static int
restart_state_check_unique(const struct restart_state *state,
    struct restart_budget *budget, char **cause)
{
	const char	**names;
	size_t		  i;

	for (i = 0; i + 1 < state->session_count; i++) {
		if (state->sessions[i].id == state->sessions[i + 1].id) {
			restart_set_cause(cause,
			    "duplicate restart session ID");
			return (-1);
		}
	}
	for (i = 0; i + 1 < state->window_count; i++) {
		if (state->windows[i].id == state->windows[i + 1].id) {
			restart_set_cause(cause,
			    "duplicate restart window ID");
			return (-1);
		}
	}
	for (i = 0; i + 1 < state->pane_count; i++) {
		if (state->panes[i].id == state->panes[i + 1].id) {
			restart_set_cause(cause, "duplicate restart pane ID");
			return (-1);
		}
	}
	for (i = 0; i + 1 < state->group_count; i++) {
		if (strcmp(state->groups[i].name,
		    state->groups[i + 1].name) == 0) {
			restart_set_cause(cause,
			    "duplicate restart group name");
			return (-1);
		}
	}
	for (i = 0; i + 1 < state->descriptor_count; i++) {
		if (state->descriptors[i].pane_id ==
		    state->descriptors[i + 1].pane_id) {
			restart_set_cause(cause,
			    "duplicate restart descriptor key");
			return (-1);
		}
	}
	if (state->session_count > 1) {
		names = restart_grow(budget, NULL, 0, state->session_count,
		    sizeof *names, cause);
		if (names == NULL)
			return (-1);
		for (i = 0; i < state->session_count; i++)
			names[i] = state->sessions[i].name;
		qsort(names, state->session_count, sizeof *names,
		    restart_name_sort_compare);
		for (i = 0; i + 1 < state->session_count; i++) {
			if (strcmp(names[i], names[i + 1]) == 0) {
				free(names);
				restart_set_cause(cause,
				    "duplicate restart session name");
				return (-1);
			}
		}
		free(names);
	}
	return (0);
}

/*
 * Buffers are keyed by order rather than by an id, and a duplicate order is
 * not merely a duplicate: paste_cmp_times returns 0 for it, so RB_INSERT
 * would return the sitting element without inserting and the buffer would
 * enter the name tree while staying invisible to paste_walk. A capture this
 * code wrote cannot contain one, because the live counter only increases,
 * which is exactly why it has to be checked here rather than assumed.
 */
static int
restart_state_validate_buffers(const struct restart_state *state, char **cause)
{
	size_t	i, j;

	for (i = 0; i < state->buffer_count; i++) {
		for (j = i + 1; j < state->buffer_count; j++) {
			if (state->buffers[i].order ==
			    state->buffers[j].order) {
				restart_set_cause(cause,
				    "duplicate restart buffer order");
				return (-1);
			}
			if (strcmp(state->buffers[i].name,
			    state->buffers[j].name) == 0) {
				restart_set_cause(cause,
				    "duplicate restart buffer name");
				return (-1);
			}
		}
	}
	return (0);
}

static int
restart_state_sort(struct restart_state *state, struct restart_budget *budget,
    char **cause)
{
	if (state->session_count > 1) {
		qsort(state->sessions, state->session_count,
		    sizeof *state->sessions, restart_session_compare);
	}
	if (state->window_count > 1) {
		qsort(state->windows, state->window_count,
		    sizeof *state->windows, restart_window_compare);
	}
	if (state->pane_count > 1) {
		qsort(state->panes, state->pane_count, sizeof *state->panes,
		    restart_pane_compare);
	}
	if (state->group_count > 1) {
		qsort(state->groups, state->group_count, sizeof *state->groups,
		    restart_group_compare);
	}
	if (state->descriptor_count > 1) {
		qsort(state->descriptors, state->descriptor_count,
		    sizeof *state->descriptors, restart_descriptor_compare);
	}
	return (restart_state_check_unique(state, budget, cause));
}

struct restart_validate_ctx {
	struct restart_budget		*budget;
	uint32_t			*window_refs;
	size_t				 window_ref_count;
	uint32_t			*group_members;
	size_t				 group_member_count;
	size_t				*window_panes;
};

static int
restart_winlink_contains(const struct restart_winlink *winlinks, size_t count,
    int32_t index)
{
	size_t	low = 0, high = count, mid;

	while (low < high) {
		mid = low + (high - low) / 2;
		if (winlinks[mid].index == index)
			return (1);
		if (winlinks[mid].index < index)
			low = mid + 1;
		else
			high = mid;
	}
	return (0);
}

static int
restart_u32_index_contains(const uint32_t *items, size_t count, uint32_t value)
{
	size_t	low = 0, high = count, mid;

	while (low < high) {
		mid = low + (high - low) / 2;
		if (items[mid] == value)
			return (1);
		if (items[mid] < value)
			low = mid + 1;
		else
			high = mid;
	}
	return (0);
}

static void
restart_validate_ctx_free(struct restart_validate_ctx *ctx)
{
	free(ctx->window_refs);
	free(ctx->group_members);
	free(ctx->window_panes);
	memset(ctx, 0, sizeof *ctx);
}

static int
restart_validate_ctx_build(const struct restart_state *state,
    struct restart_validate_ctx *ctx, struct restart_budget *budget,
    char **cause)
{
	struct restart_session	*session;
	struct restart_window	*window;
	size_t			 i, j, refs = 0, members = 0;

	memset(ctx, 0, sizeof *ctx);
	ctx->budget = budget;
	for (i = 0; i < state->session_count; i++) {
		session = &state->sessions[i];
		if (restart_size_add(refs, session->winlink_count,
		    &refs) != 0) {
			restart_set_cause(cause, "too many restart winlinks");
			return (-1);
		}
	}
	for (i = 0; i < state->group_count; i++) {
		if (restart_size_add(members, state->groups[i].members.count,
		    &members) != 0) {
			restart_set_cause(cause,
			    "too many restart group members");
			return (-1);
		}
	}

	if (refs != 0) {
		ctx->window_refs = restart_grow(budget, NULL, 0, refs,
		    sizeof *ctx->window_refs, cause);
		if (ctx->window_refs == NULL)
			goto fail;
		for (i = 0; i < state->session_count; i++) {
			session = &state->sessions[i];
			for (j = 0; j < session->winlink_count; j++) {
				ctx->window_refs[ctx->window_ref_count++] =
				    session->winlinks[j].window_id;
			}
		}
		qsort(ctx->window_refs, ctx->window_ref_count,
		    sizeof *ctx->window_refs, restart_u32_sort_compare);
	}

	if (members != 0) {
		ctx->group_members = restart_grow(budget, NULL, 0, members,
		    sizeof *ctx->group_members, cause);
		if (ctx->group_members == NULL)
			goto fail;
		for (i = 0; i < state->group_count; i++) {
			for (j = 0; j < state->groups[i].members.count; j++) {
				ctx->group_members[ctx->group_member_count++] =
				    state->groups[i].members.items[j];
			}
		}
		qsort(ctx->group_members, ctx->group_member_count,
		    sizeof *ctx->group_members, restart_u32_sort_compare);
	}

	if (state->window_count != 0) {
		ctx->window_panes = restart_grow(budget, NULL, 0,
		    state->window_count, sizeof *ctx->window_panes, cause);
		if (ctx->window_panes == NULL)
			goto fail;
		memset(ctx->window_panes, 0,
		    state->window_count * sizeof *ctx->window_panes);
		for (i = 0; i < state->pane_count; i++) {
			window = restart_find_window(state,
			    state->panes[i].window_id);
			if (window == NULL) {
				restart_set_cause(cause,
				    "restart pane has no window");
				goto fail;
			}
			ctx->window_panes[window - state->windows]++;
		}
	}

	return (0);

fail:
	restart_validate_ctx_free(ctx);
	return (-1);
}

static int
restart_state_validate_counters(const struct restart_state *state, char **cause)
{
	size_t	i;

	for (i = 0; i < state->session_count; i++) {
		if (state->sessions[i].id >= state->next_session_id) {
			restart_set_cause(cause,
			    "restart session ID is not below its counter");
			return (-1);
		}
	}
	for (i = 0; i < state->window_count; i++) {
		if (state->windows[i].id >= state->next_window_id) {
			restart_set_cause(cause,
			    "restart window ID is not below its counter");
			return (-1);
		}
	}
	for (i = 0; i < state->pane_count; i++) {
		if (state->panes[i].id >= state->next_pane_id) {
			restart_set_cause(cause,
			    "restart pane ID is not below its counter");
			return (-1);
		}
		if (state->panes[i].active_point >= state->next_active_point) {
			restart_set_cause(cause,
			    "restart active point is not below its counter");
			return (-1);
		}
	}
	return (0);
}

static int
restart_state_validate_sessions(const struct restart_state *state,
    struct restart_validate_ctx *ctx, char **cause)
{
	struct restart_session	*session;
	size_t			 i, j, k;
	int			 found;

	for (i = 0; i < state->session_count; i++) {
		session = &state->sessions[i];
		found = 0;
		for (j = 0; j < session->winlink_count; j++) {
			if (restart_find_window(state,
			    session->winlinks[j].window_id) == NULL) {
				restart_set_cause(cause,
				    "restart winlink has no window");
				return (-1);
			}
			if (session->winlinks[j].index ==
			    session->current_index)
				found = 1;
		}
		if (!found) {
			restart_set_cause(cause,
			    "restart current winlink does not resolve");
			return (-1);
		}
		for (k = 0; k < session->last_indices.count; k++) {
			if (session->last_indices.items[k] ==
			    session->current_index) {
				restart_set_cause(cause,
				    "restart last index equals current");
				return (-1);
			}
			if (!restart_winlink_contains(session->winlinks,
			    session->winlink_count,
			    session->last_indices.items[k])) {
				restart_set_cause(cause,
				    "restart last index does not resolve");
				return (-1);
			}
		}
	}
	for (i = 0; i < state->window_count; i++) {
		if (!restart_u32_index_contains(ctx->window_refs,
		    ctx->window_ref_count, state->windows[i].id)) {
			restart_set_cause(cause, "restart window is an orphan");
			return (-1);
		}
	}
	return (0);
}

static int
restart_state_validate_groups(const struct restart_state *state,
    struct restart_validate_ctx *ctx, char **cause)
{
	struct restart_group	*group;
	struct restart_session	*first, *other;
	size_t			 i, j, k;

	for (i = 0; i + 1 < ctx->group_member_count; i++) {
		if (ctx->group_members[i] == ctx->group_members[i + 1]) {
			restart_set_cause(cause,
			    "restart session is in two groups");
			return (-1);
		}
	}
	for (i = 0; i < state->group_count; i++) {
		group = &state->groups[i];
		for (j = 0; j < group->members.count; j++) {
			if (restart_find_session(state,
			    group->members.items[j]) == NULL) {
				restart_set_cause(cause,
				    "restart group member has no session");
				return (-1);
			}
		}
		first = restart_find_session(state, group->members.items[0]);
		for (j = 1; j < group->members.count; j++) {
			other = restart_find_session(state,
			    group->members.items[j]);
			if (other->winlink_count != first->winlink_count) {
				restart_set_cause(cause,
				    "restart grouped sessions differ");
				return (-1);
			}
			for (k = 0; k < first->winlink_count; k++) {
				if (other->winlinks[k].index !=
				    first->winlinks[k].index ||
				    other->winlinks[k].window_id !=
				    first->winlinks[k].window_id) {
					restart_set_cause(cause,
					    "restart grouped sessions differ");
					return (-1);
				}
			}
		}
	}
	return (0);
}

static int
restart_state_validate_windows(const struct restart_state *state,
    struct restart_validate_ctx *ctx, char **cause)
{
	struct restart_window	*window;
	struct restart_pane	*pane;
	size_t			 i, j, owned;

	for (i = 0; i < state->pane_count; i++) {
		if (restart_find_window(state,
		    state->panes[i].window_id) == NULL) {
			restart_set_cause(cause, "restart pane has no window");
			return (-1);
		}
	}
	for (i = 0; i < state->window_count; i++) {
		window = &state->windows[i];
		owned = ctx->window_panes[i];
		if (owned == 0) {
			restart_set_cause(cause, "restart window has no pane");
			return (-1);
		}
		if (window->pane_order.count != owned ||
		    window->z_order.count != owned) {
			restart_set_cause(cause,
			    "restart window pane list is incomplete");
			return (-1);
		}
		for (j = 0; j < window->pane_order.count; j++) {
			pane = restart_find_pane(state,
			    window->pane_order.items[j]);
			if (pane == NULL || pane->window_id != window->id) {
				restart_set_cause(cause,
				    "restart pane order is foreign");
				return (-1);
			}
		}
		for (j = 0; j < window->z_order.count; j++) {
			pane = restart_find_pane(state,
			    window->z_order.items[j]);
			if (pane == NULL || pane->window_id != window->id) {
				restart_set_cause(cause,
				    "restart z order is foreign");
				return (-1);
			}
		}
		for (j = 0; j < window->last_panes.count; j++) {
			pane = restart_find_pane(state,
			    window->last_panes.items[j]);
			if (pane == NULL || pane->window_id != window->id) {
				restart_set_cause(cause,
				    "restart last pane is foreign");
				return (-1);
			}
			if (window->last_panes.items[j] ==
			    window->active_pane_id) {
				restart_set_cause(cause,
				    "restart last pane list contains active");
				return (-1);
			}
		}
		pane = restart_find_pane(state, window->active_pane_id);
		if (pane == NULL || pane->window_id != window->id) {
			restart_set_cause(cause,
			    "restart active pane is foreign");
			return (-1);
		}
		if (window->have_modal) {
			pane = restart_find_pane(state, window->modal_pane_id);
			if (pane == NULL || pane->window_id != window->id ||
			    window->modal_pane_id != window->active_pane_id) {
				restart_set_cause(cause,
				    "restart modal pane is invalid");
				return (-1);
			}
		}
		if (window->have_modal_last) {
			pane = restart_find_pane(state, window->modal_last_id);
			if (pane == NULL || pane->window_id != window->id ||
			    window->modal_last_id == window->modal_pane_id) {
				restart_set_cause(cause,
				    "restart modal last pane is invalid");
				return (-1);
			}
		}
	}
	return (0);
}

static int
restart_layout_tiled(const struct restart_layout_cell *cell)
{
	size_t	i;

	if (cell->type == RESTART_LAYOUT_PANE)
		return (!cell->have_z);
	for (i = 0; i < cell->child_count; i++) {
		if (restart_layout_tiled(cell->children[i]))
			return (1);
	}
	return (0);
}

static int
restart_layout_geometry(const struct restart_layout_cell *cell, char **cause)
{
	const struct restart_layout_cell	*child;
	uint64_t				 total = 0;
	size_t					 i;

	if (cell->type == RESTART_LAYOUT_PANE)
		return (0);
	for (i = 0; i < cell->child_count; i++) {
		child = cell->children[i];
		if (!restart_layout_tiled(child))
			continue;
		if (cell->type == RESTART_LAYOUT_HORIZONTAL) {
			if (child->sy != cell->sy)
				goto invalid;
			total += (uint64_t)child->sx + 1;
		} else {
			if (child->sx != cell->sx)
				goto invalid;
			total += (uint64_t)child->sy + 1;
		}
		if (restart_layout_geometry(child, cause) != 0)
			return (-1);
	}
	if (total != 0) {
		if (cell->type == RESTART_LAYOUT_HORIZONTAL) {
			if (total - 1 != cell->sx)
				goto invalid;
		} else if (total - 1 != cell->sy)
			goto invalid;
	}
	return (0);

invalid:
	restart_set_cause(cause, "restart layout geometry does not fit");
	return (-1);
}

static void
restart_layout_collect(struct restart_layout_cell *cell,
    struct restart_layout_cell **out, size_t *count)
{
	size_t	i;

	if (cell->type == RESTART_LAYOUT_PANE) {
		out[(*count)++] = cell;
		return;
	}
	for (i = 0; i < cell->child_count; i++)
		restart_layout_collect(cell->children[i], out, count);
}

static int
restart_options_get_number(const struct restart_options *options,
    const char *name, int64_t *out)
{
	size_t	i;

	for (i = 0; i < options->count; i++) {
		if (strcmp(options->entries[i].name, name) != 0)
			continue;
		if (options->entries[i].is_array)
			return (-1);
		*out = options->entries[i].number;
		return (0);
	}
	return (-1);
}

struct restart_id_position {
	uint32_t	id;
	size_t		position;
};

static int
restart_id_position_compare(const void *a0, const void *b0)
{
	const struct restart_id_position *a = a0, *b = b0;

	return (restart_u32_compare(a->id, b->id));
}

static struct restart_id_position *
restart_id_position_build(const struct restart_id_list *list,
    struct restart_budget *budget, char **cause)
{
	struct restart_id_position	*index;
	size_t				 i;

	index = restart_grow(budget, NULL, 0, list->count + 1, sizeof *index,
	    cause);
	if (index == NULL)
		return (NULL);
	for (i = 0; i < list->count; i++) {
		index[i].id = list->items[i];
		index[i].position = i;
	}
	if (list->count > 1) {
		qsort(index, list->count, sizeof *index,
		    restart_id_position_compare);
	}
	return (index);
}

static int
restart_id_position_find(const struct restart_id_position *index, size_t count,
    uint32_t id, size_t *position)
{
	size_t	low = 0, high = count, mid;

	while (low < high) {
		mid = low + (high - low) / 2;
		if (index[mid].id == id) {
			*position = index[mid].position;
			return (1);
		}
		if (index[mid].id < id)
			low = mid + 1;
		else
			high = mid;
	}
	return (0);
}

static int
restart_layout_validate(const struct restart_state *state,
    struct restart_window *window, struct restart_layout *layout, int visible,
    int geometry, int64_t base, struct restart_budget *budget, char **cause)
{
	struct restart_layout_cell	**leaves, *leaf;
	struct restart_pane		 *pane;
	struct restart_id_position	 *order = NULL, *last = NULL;
	struct restart_id_position	 *zorder = NULL, *leafindex = NULL;
	uint32_t			 *keys = NULL;
	size_t				 *prefix = NULL;
	size_t				  count = 0, i, j, seen, running;
	uint32_t			  expect, gap, common = 0;
	int64_t				  scrollbars;
	int				  tiled = 0, error = -1, reserve;

	if (restart_layout_geometry(layout->root, cause) != 0)
		return (-1);
	/*
	 * Only the always setting reserves space; modal and autohide draw
	 * over the pane and leave its size alone.
	 */
	if (restart_options_get_number(&window->options, "pane-scrollbars",
	    &scrollbars) != 0 &&
	    restart_options_get_number(&state->global_window_options,
	    "pane-scrollbars", &scrollbars) != 0)
		scrollbars = PANE_SCROLLBARS_OFF;
	reserve = (scrollbars == PANE_SCROLLBARS_ALWAYS);
	if (layout->leaf_count == 0) {
		restart_set_cause(cause, "restart layout has no pane");
		return (-1);
	}
	leaves = restart_calloc(budget, layout->leaf_count, sizeof *leaves,
	    cause);
	if (leaves == NULL)
		return (-1);
	restart_layout_collect(layout->root, leaves, &count);
	if (count != layout->leaf_count)
		goto invalid;

	keys = restart_grow(budget, NULL, 0, count, sizeof *keys, cause);
	leafindex = restart_grow(budget, NULL, 0, count + 1, sizeof *leafindex,
	    cause);
	order = restart_id_position_build(&window->pane_order, budget, cause);
	last = restart_id_position_build(&window->last_panes, budget, cause);
	zorder = restart_id_position_build(&window->z_order, budget, cause);
	prefix = restart_grow(budget, NULL, 0, window->z_order.count + 1,
	    sizeof *prefix, cause);
	if (keys == NULL || leafindex == NULL || order == NULL ||
	    last == NULL || zorder == NULL || prefix == NULL)
		goto out;

	for (i = 0; i < count; i++) {
		keys[i] = leaves[i]->pane_id;
		leafindex[i].id = leaves[i]->pane_id;
		leafindex[i].position = i;
	}
	if (count > 1) {
		qsort(keys, count, sizeof *keys, restart_u32_sort_compare);
		for (i = 0; i + 1 < count; i++) {
			if (keys[i] == keys[i + 1])
				goto invalid;
		}
		qsort(leafindex, count, sizeof *leafindex,
		    restart_id_position_compare);
	}
	for (i = 0; i < count; i++)
		keys[i] = leaves[i]->pane_index;
	if (count > 1) {
		qsort(keys, count, sizeof *keys, restart_u32_sort_compare);
		for (i = 0; i + 1 < count; i++) {
			if (keys[i] == keys[i + 1])
				goto invalid;
		}
	}

	for (i = 0; i < count; i++) {
		leaf = leaves[i];
		pane = restart_find_pane(state, leaf->pane_id);
		if (pane == NULL || pane->window_id != window->id)
			goto invalid;
		/*
		 * A reserved scrollbar makes the pane narrower than the cell
		 * that holds it, so requiring equality rejects every server
		 * with pane-scrollbars set to always. The contract states the
		 * relationship as the pane plus any reserved width equalling
		 * the cell; the width itself comes from a style and is not on
		 * the wire, so what is checked here is that the height is
		 * exact, the cell is never narrower than its pane, and any
		 * reserve is both permitted by the option and the same for
		 * every pane in the window.
		 */
		if (geometry) {
			if (leaf->sy != pane->sy || leaf->sx < pane->sx)
				goto invalid;
			gap = leaf->sx - pane->sx;
			if (gap != 0 && !reserve)
				goto invalid;
			if (gap != 0 && common != 0 && gap != common)
				goto invalid;
			if (gap != 0)
				common = gap;
		}
		if (!restart_id_position_find(order, window->pane_order.count,
		    leaf->pane_id, &seen))
			goto invalid;
		if (base + (int64_t)seen > INT_MAX)
			goto invalid;
		if (leaf->pane_index != (uint32_t)(base + (int64_t)seen))
			goto invalid;
		if (leaf->active != (leaf->pane_id == window->active_pane_id))
			goto invalid;
		if (!restart_id_position_find(last, window->last_panes.count,
		    leaf->pane_id, &seen)) {
			if (leaf->have_last)
				goto invalid;
		} else if (!leaf->have_last || leaf->last_index != seen)
			goto invalid;
		if (!leaf->have_z)
			tiled++;
	}

	running = 0;
	for (j = 0; j < window->z_order.count; j++) {
		prefix[j] = running;
		if (restart_id_position_find(leafindex, count,
		    window->z_order.items[j], &seen) &&
		    leaves[seen]->have_z)
			running++;
	}
	for (i = 0; i < count; i++) {
		leaf = leaves[i];
		if (!leaf->have_z)
			continue;
		if (!restart_id_position_find(zorder, window->z_order.count,
		    leaf->pane_id, &j))
			goto invalid;
		expect = prefix[j];
		if (leaf->z_index != expect)
			goto invalid;
	}

	if (!visible) {
		if (count != window->pane_order.count)
			goto invalid;
	} else if (tiled != 1)
		goto invalid;
	error = 0;
	goto out;
invalid:
	restart_set_cause(cause, "restart layout does not match its window");
out:
	free(leaves);
	free(keys);
	free(leafindex);
	free(order);
	free(last);
	free(zorder);
	free(prefix);
	return (error);
}

static int
restart_state_validate_layouts(const struct restart_state *state,
    struct restart_budget *budget, char **cause)
{
	struct restart_window	*window;
	int64_t			 base;
	size_t			 i;

	for (i = 0; i < state->window_count; i++) {
		window = &state->windows[i];
		if (restart_options_get_number(&window->options,
		    "pane-base-index", &base) != 0 &&
		    restart_options_get_number(&state->global_window_options,
		    "pane-base-index", &base) != 0) {
			restart_set_cause(cause,
			    "restart pane-base-index is missing");
			return (-1);
		}
		if (base < 0 || base > INT_MAX) {
			restart_set_cause(cause,
			    "restart pane-base-index is out of range");
			return (-1);
		}
		if (restart_layout_validate(state, window, window->layout, 0,
		    window->visible_layout == NULL, base, budget, cause) != 0)
			return (-1);
		if (window->visible_layout != NULL &&
		    restart_layout_validate(state, window,
		    window->visible_layout, 1, 1, base, budget, cause) != 0)
			return (-1);
	}
	return (0);
}

/* Check a captured cell is the default cell. */
static int
restart_cell_is_default(const struct restart_cell *cell)
{
	const struct grid_cell	*gc = &grid_default_cell;

	return (cell->size == gc->data.size && cell->width == gc->data.width &&
	    cell->flags == 0 && cell->attr == gc->attr && cell->fg == gc->fg &&
	    cell->bg == gc->bg && cell->us == gc->us && cell->link == 0 &&
	    memcmp(cell->data, gc->data.data, cell->size) == 0);
}

/* Check a captured UTF-8 character is empty. */
static int
restart_utf8_is_zero(const struct restart_utf8 *utf8)
{
	static const u_char	zero[UTF8_SIZE];

	return (utf8->have == 0 && utf8->size == 0 && utf8->width == 0 &&
	    memcmp(utf8->data, zero, sizeof zero) == 0);
}

/* Check a decoded parser is in the ground state with nothing accumulated. */
static int
restart_parser_is_ground(const struct restart_parser *parser)
{
	if (parser->state == NULL || strcmp(parser->state, "ground") != 0)
		return (0);
	if (!restart_cell_is_default(&parser->cell.cell) ||
	    parser->cell.set != 0 || parser->cell.g0set != 0 ||
	    parser->cell.g1set != 0)
		return (0);
	if (!restart_cell_is_default(&parser->old_cell.cell) ||
	    parser->old_cell.set != 0 || parser->old_cell.g0set != 0 ||
	    parser->old_cell.g1set != 0)
		return (0);
	if (parser->old_cx != 0 || parser->old_cy != 0 || parser->old_mode != 0)
		return (0);
	if (parser->intermediate.size != 0 || parser->parameter.size != 0 ||
	    parser->input.size != 0)
		return (0);
	if (parser->input_end != 0 || parser->utf8_started != 0 ||
	    parser->flags != 0)
		return (0);
	return (restart_utf8_is_zero(&parser->utf8) &&
	    restart_utf8_is_zero(&parser->last_utf8));
}

/* Check a live parser is in the ground state with nothing accumulated. */
static int
restart_live_parser_is_ground(const struct input_parser_state *ips)
{
	static const struct utf8_data	 zero;

	if (ips->state == NULL || strcmp(ips->state, "ground") != 0)
		return (0);
	if (!grid_cells_equal(&ips->cell.cell, &grid_default_cell) ||
	    ips->cell.set != 0 || ips->cell.g0set != 0 || ips->cell.g1set != 0)
		return (0);
	if (!grid_cells_equal(&ips->old_cell.cell, &grid_default_cell) ||
	    ips->old_cell.set != 0 || ips->old_cell.g0set != 0 ||
	    ips->old_cell.g1set != 0)
		return (0);
	if (ips->old_cx != 0 || ips->old_cy != 0 || ips->old_mode != 0)
		return (0);
	if (ips->interm_len != 0 || ips->param_len != 0 || ips->input_len != 0)
		return (0);
	if (ips->input_end != INPUT_END_ST || ips->utf8started != 0 ||
	    ips->flags != 0)
		return (0);
	return (memcmp(&ips->utf8data, &zero, sizeof zero) == 0 &&
	    memcmp(&ips->last, &zero, sizeof zero) == 0);
}

static struct restart_descriptor_key *
restart_find_descriptor(const struct restart_state *state, uint32_t pane_id)
{
	size_t	low = 0, high = state->descriptor_count, mid;

	while (low < high) {
		mid = low + (high - low) / 2;
		if (state->descriptors[mid].pane_id == pane_id)
			return (&state->descriptors[mid]);
		if (state->descriptors[mid].pane_id < pane_id)
			low = mid + 1;
		else
			high = mid;
	}
	return (NULL);
}

static int
restart_state_validate_lifecycles(const struct restart_state *state,
    char **cause)
{
	struct restart_pane		*pane;
	struct restart_descriptor_key	*key;
	size_t				 i, keys = 0;
	int				 wants_key, wants_result, wants_time;

	for (i = 0; i < state->pane_count; i++) {
		pane = &state->panes[i];
		switch (pane->lifecycle) {
		case RESTART_LIFECYCLE_LIVE_FD:
		case RESTART_LIFECYCLE_DRAINING_UNKNOWN:
			wants_key = 1;
			wants_result = 0;
			wants_time = 0;
			break;
		case RESTART_LIFECYCLE_DRAINING_KNOWN:
			wants_key = 1;
			wants_result = 1;
			wants_time = 0;
			break;
		case RESTART_LIFECYCLE_DEAD_KNOWN:
			wants_key = 0;
			wants_result = 1;
			wants_time = 1;
			break;
		case RESTART_LIFECYCLE_DEAD_UNKNOWN:
			wants_key = 0;
			wants_result = 0;
			wants_time = 1;
			break;
		default:
			wants_key = 0;
			wants_result = 0;
			wants_time = 0;
			break;
		}
		if (pane->have_result != wants_result) {
			restart_set_cause(cause, "restart pane result does "
			    "not match its lifecycle");
			return (-1);
		}
		if (pane->have_dead_time != wants_time) {
			restart_set_cause(cause, "restart pane dead time "
			    "does not match its lifecycle");
			return (-1);
		}
		if (pane->lifecycle == RESTART_LIFECYCLE_INACTIVE) {
			if (pane->pid != -1) {
				restart_set_cause(cause, "restart inactive "
				    "pane pid is not -1");
				return (-1);
			}
		} else if (pane->lifecycle != RESTART_LIFECYCLE_EMPTY) {
			if (pane->pid <= 0) {
				restart_set_cause(cause,
				    "restart pane pid is not positive");
				return (-1);
			}
			if (*pane->tty == '\0') {
				restart_set_cause(cause,
				    "restart pane tty is empty");
				return (-1);
			}
		}
		key = restart_find_descriptor(state, pane->id);
		if (wants_key) {
			if (key == NULL) {
				restart_set_cause(cause, "restart pane "
				    "descriptor key is missing");
				return (-1);
			}
			if (key->pid != pane->pid) {
				restart_set_cause(cause, "restart descriptor "
				    "key pid does not match its pane");
				return (-1);
			}
			keys++;
		} else if (key != NULL) {
			restart_set_cause(cause, "restart pane descriptor "
			    "key is not allowed");
			return (-1);
		}
	}
	if (keys != state->descriptor_count) {
		restart_set_cause(cause, "restart descriptor key is extra");
		return (-1);
	}
	return (0);
}

static int
restart_state_validate_terminals(const struct restart_state *state,
    char **cause)
{
	const struct restart_pane	*pane;
	size_t				 i;

	for (i = 0; i < state->pane_count; i++) {
		pane = &state->panes[i];
		if (pane->terminal->screen.grid.sx != pane->sx ||
		    pane->terminal->screen.grid.sy != pane->sy) {
			restart_set_cause(cause,
			    "restart terminal size does not match its pane");
			return (-1);
		}
		switch (pane->lifecycle) {
		case RESTART_LIFECYCLE_DEAD_KNOWN:
		case RESTART_LIFECYCLE_DEAD_UNKNOWN:
		case RESTART_LIFECYCLE_EXITED_UNKNOWN:
		case RESTART_LIFECYCLE_INACTIVE:
			if (pane->terminal->pty_input.size != 0 ||
			    pane->terminal->pty_output.size != 0) {
				restart_set_cause(cause,
				    "restart closed pane retains PTY data");
				return (-1);
			}
			break;
		}
		if (pane->lifecycle == RESTART_LIFECYCLE_INACTIVE &&
		    !restart_parser_is_ground(&pane->terminal->parser)) {
			restart_set_cause(cause,
			    "restart inactive pane parser is not ground");
			return (-1);
		}
	}
	return (0);
}

static int
restart_external_id_parse(const char *id, long long next, long long *out)
{
	unsigned long long	 value = 0;
	const char		*cp;
	int			 digit;

	if (strncmp(id, "tmux", 4) != 0)
		return (-1);
	cp = id + 4;
	if (*cp == '\0' || *cp == '0')
		return (-1);
	for (; *cp != '\0'; cp++) {
		if (*cp >= '0' && *cp <= '9')
			digit = *cp - '0';
		else if (*cp >= 'A' && *cp <= 'F')
			digit = 10 + (*cp - 'A');
		else
			return (-1);
		if (value > (unsigned long long)LLONG_MAX / 16)
			return (-1);
		value *= 16;
		if ((unsigned long long)LLONG_MAX - value <
		    (unsigned long long)digit)
			return (-1);
		value += digit;
	}
	if (value == 0 || (long long)value >= next)
		return (-1);
	*out = (long long)value;
	return (0);
}

static int
restart_ll_sort_compare(const void *a0, const void *b0)
{
	const long long *a = a0, *b = b0;

	if (*a < *b)
		return (-1);
	if (*a > *b)
		return (1);
	return (0);
}

static int
restart_state_validate_external_ids(const struct restart_state *state,
    struct restart_budget *budget, char **cause)
{
	const struct restart_links	*links;
	long long			*values, next;
	size_t				 i, j, total = 0, count = 0;

	if (state->next_hyperlink_external_id > (uint64_t)LLONG_MAX) {
		restart_set_cause(cause,
		    "restart external ID counter is out of range");
		return (-1);
	}
	next = (long long)state->next_hyperlink_external_id;
	for (i = 0; i < state->pane_count; i++) {
		links = &state->panes[i].terminal->screen.links;
		if (restart_size_add(total, links->count, &total) != 0) {
			restart_set_cause(cause,
			    "too many restart hyperlinks");
			return (-1);
		}
	}
	if (total == 0)
		return (0);
	values = restart_grow(budget, NULL, 0, total, sizeof *values, cause);
	if (values == NULL)
		return (-1);
	for (i = 0; i < state->pane_count; i++) {
		links = &state->panes[i].terminal->screen.links;
		for (j = 0; j < links->count; j++) {
			if (links->items[j].external_id == NULL ||
			    restart_external_id_parse(
			    links->items[j].external_id, next,
			    &values[count]) != 0) {
				free(values);
				restart_set_cause(cause,
				    "invalid restart external ID");
				return (-1);
			}
			count++;
		}
	}
	if (count > 1) {
		qsort(values, count, sizeof *values, restart_ll_sort_compare);
		for (i = 0; i + 1 < count; i++) {
			if (values[i] == values[i + 1]) {
				free(values);
				restart_set_cause(cause,
				    "duplicate restart external ID");
				return (-1);
			}
		}
	}
	free(values);
	return (0);
}

static int
restart_state_validate_features(const struct restart_state *state, char **cause)
{
	uint64_t	features = 0;
	size_t		i;

	for (i = 0; i < state->pane_count; i++)
		features |= state->panes[i].terminal->features;
	if (features != state->features) {
		restart_set_cause(cause, "restart feature mask mismatch");
		return (-1);
	}
	return (0);
}

/*
 * Which wire bits a map defines, derived from the map. Written as a literal
 * it is a second statement of the same relationship, and the moment a row is
 * added the two disagree with nothing to notice it.
 */
/* Convert wire flags to live flags. */
static int
restart_flags_from_wire(const struct restart_flag_map *map, u_int count,
    u_int wire)
{
	u_int	i;
	int	live = 0;

	for (i = 0; i < count; i++) {
		if (wire & map[i].wire)
			live |= map[i].live;
	}
	return (live);
}

static u_int
restart_flags_mask(const struct restart_flag_map *map, u_int count)
{
	u_int	mask = 0, i;

	for (i = 0; i < count; i++)
		mask |= map[i].wire;
	return (mask);
}

/*
 * Convert live flags to wire flags, refusing any value the destination
 * cannot hold. One converter serves four maps across two field widths, so
 * the fit is not a property of the table and cannot be checked there.
 *
 * The caller passes the size of its own destination field rather than a
 * capacity constant, so widening the field carries the check with it. A
 * constant written here would be a second statement of the field's width,
 * and would keep passing the day the two stopped agreeing.
 */
static int
restart_flags_to_wire(const struct restart_flag_map *map, u_int count,
    int live, size_t width, u_int *wire, char **cause)
{
	u_int	value = 0, i, capacity;

	for (i = 0; i < count; i++) {
		if (live & map[i].live)
			value |= map[i].wire;
	}
	if (width >= sizeof(u_int))
		capacity = ~0U;
	else
		capacity = (1U << (width * 8)) - 1;
	if ((value & ~capacity) != 0) {
		restart_set_cause(cause, "restart flag does not fit its field");
		return (-1);
	}
	*wire = value;
	return (0);
}

static int
restart_pane_validate_running(const struct restart_pane *pane, char **cause)
{
	if ((pane->flags & RESTART_PANE_WIRE_CMDRUNNING) == 0)
		return (0);
	if (pane->command_start == 0 || pane->command_end != 0 ||
	    pane->command_status != -1) {
		restart_set_cause(cause,
		    "invalid restart running command state");
		return (-1);
	}
	return (0);
}

static int
restart_pane_classify(struct window_pane *wp, struct restart_pane *pane,
    char **cause)
{
	int	bits;

	bits = wp->flags & (PANE_EMPTY|PANE_EXITED|PANE_STATUSREADY|
	    PANE_STATUSDRAWN);
	if (wp->fd == -1 && wp->pid == -1) {
		pane->lifecycle = RESTART_LIFECYCLE_INACTIVE;
		return (0);
	}
	if (wp->fd != -1) {
		if (wp->event == NULL) {
			restart_set_cause(cause,
			    "restart pane has an open fd with no bufferevent");
			return (-1);
		}
		if (bufferevent_get_enabled(wp->event) & (EV_READ|EV_WRITE)) {
			restart_set_cause(cause,
			    "restart pane bufferevent is still enabled");
			return (-1);
		}
		if (bits == 0)
			pane->lifecycle = RESTART_LIFECYCLE_LIVE_FD;
		else if (bits == (PANE_EXITED|PANE_STATUSREADY))
			pane->lifecycle = RESTART_LIFECYCLE_DRAINING_KNOWN;
		else if (bits == PANE_EXITED)
			pane->lifecycle = RESTART_LIFECYCLE_DRAINING_UNKNOWN;
		else
			goto invalid;
		return (0);
	}
	if (bits == PANE_EMPTY)
		pane->lifecycle = RESTART_LIFECYCLE_EMPTY;
	else if (bits == (PANE_EXITED|PANE_STATUSREADY|PANE_STATUSDRAWN))
		pane->lifecycle = RESTART_LIFECYCLE_DEAD_KNOWN;
	else if (bits == (PANE_EXITED|PANE_STATUSDRAWN))
		pane->lifecycle = RESTART_LIFECYCLE_DEAD_UNKNOWN;
	else if (bits == PANE_EXITED && wp->pid > 0)
		pane->lifecycle = RESTART_LIFECYCLE_EXITED_UNKNOWN;
	else
		goto invalid;
	return (0);

invalid:
	restart_set_cause(cause, "unsupported restart pane state");
	return (-1);
}

static int
restart_pane_capture_result(struct window_pane *wp, struct restart_pane *pane,
    char **cause)
{
	if (WIFEXITED(wp->status)) {
		pane->dead_kind = RESTART_DEAD_EXITED;
		pane->dead_value = WEXITSTATUS(wp->status);
		if (pane->dead_value > 255)
			goto invalid;
	} else if (WIFSIGNALED(wp->status)) {
		pane->dead_kind = RESTART_DEAD_SIGNALED;
		pane->dead_value = WTERMSIG(wp->status);
		if (pane->dead_value < 1 || pane->dead_value >= NSIG)
			goto invalid;
	} else
		goto invalid;
	pane->have_result = 1;
	return (0);

invalid:
	restart_set_cause(cause, "unsupported restart pane exit status");
	return (-1);
}

static int
restart_pane_validate_live(const struct restart_pane *pane,
    struct window_pane *wp, __unused struct restart_budget *budget,
    char **cause)
{
	struct input_parser_state	 ips;
	size_t				 input = 0, output = 0, used;
	int				 ground;

	if (wp->base.grid->sx != pane->sx || wp->base.grid->sy != pane->sy) {
		restart_set_cause(cause,
		    "restart live pane size does not match its terminal");
		return (-1);
	}
	if (wp->event != NULL) {
		input = EVBUFFER_LENGTH(wp->event->input);
		output = EVBUFFER_LENGTH(wp->event->output);
		if (wp->offset.used < wp->base_offset) {
			restart_set_cause(cause,
			    "invalid restart pane input offset");
			return (-1);
		}
		used = wp->offset.used - wp->base_offset;
		if (used > input) {
			restart_set_cause(cause,
			    "invalid restart pane input offset");
			return (-1);
		}
		input -= used;
	}
	switch (pane->lifecycle) {
	case RESTART_LIFECYCLE_DEAD_KNOWN:
	case RESTART_LIFECYCLE_DEAD_UNKNOWN:
	case RESTART_LIFECYCLE_EXITED_UNKNOWN:
	case RESTART_LIFECYCLE_INACTIVE:
		if (input != 0 || output != 0) {
			restart_set_cause(cause,
			    "restart live closed pane retains PTY data");
			return (-1);
		}
		break;
	}
	if (pane->lifecycle != RESTART_LIFECYCLE_INACTIVE || wp->ictx == NULL)
		return (0);

	input_save_parser(wp->ictx, &ips);
	ground = restart_live_parser_is_ground(&ips);
	input_free_parser_state(&ips);
	if (!ground) {
		restart_set_cause(cause,
		    "restart live inactive pane parser is not ground");
		return (-1);
	}
	return (0);
}

static int
restart_pane_capture(struct window_pane *wp, struct restart_pane *pane,
    struct restart_budget *budget, char **cause)
{
	int	i;
	u_int	flags;

	pane->id = wp->id;
	pane->window_id = wp->window->id;
	pane->active_point = wp->active_point;
	if (restart_pane_classify(wp, pane, cause) != 0)
		return (-1);
	if (restart_flags_to_wire(restart_pane_flag_map,
	    nitems(restart_pane_flag_map), wp->flags, sizeof(pane->flags),
	    &flags, cause) != 0)
		return (-1);
	pane->flags |= flags;
	pane->sx = wp->sx;
	pane->sy = wp->sy;
	pane->xoff = wp->xoff;
	pane->yoff = wp->yoff;
	if (pane->sx < 1 || pane->sx > RESTART_MAX_DIMENSION ||
	    pane->sy < 1 || pane->sy > RESTART_MAX_DIMENSION) {
		restart_set_cause(cause, "restart pane size is out of range");
		return (-1);
	}
	if (wp->argc > 0) {
		if ((size_t)wp->argc > RESTART_MAX_RECORDS) {
			restart_set_cause(cause, "too many restart arguments");
			return (-1);
		}
		pane->argv.items = restart_calloc(budget, wp->argc,
		    sizeof *pane->argv.items, cause);
		if (pane->argv.items == NULL)
			return (-1);
		for (i = 0; i < wp->argc; i++) {
			if (restart_state_capture_string(wp->argv[i],
			    &pane->argv.items[pane->argv.count], budget,
			    "pane command argument", cause) != 0)
				return (-1);
			pane->argv.count++;
		}
	}
	if (wp->shell != NULL) {
		if (restart_state_capture_string(wp->shell, &pane->shell,
		    budget, "pane shell", cause) != 0)
			return (-1);
		pane->have_shell = 1;
	}
	if (wp->cwd != NULL) {
		if (restart_state_capture_string(wp->cwd, &pane->cwd, budget,
		    "pane working directory", cause) != 0)
			return (-1);
		pane->have_cwd = 1;
	}
	pane->pid = wp->pid;
	if (restart_state_capture_string(wp->tty, &pane->tty, budget,
	    "pane tty", cause) != 0)
		return (-1);
	if (strlen(pane->tty) >= TTY_NAME_MAX) {
		restart_set_cause(cause, "restart pane tty is too long");
		return (-1);
	}
	if (pane->lifecycle == RESTART_LIFECYCLE_DEAD_KNOWN ||
	    pane->lifecycle == RESTART_LIFECYCLE_DRAINING_KNOWN) {
		if (restart_pane_capture_result(wp, pane, cause) != 0)
			return (-1);
	}
	if (pane->lifecycle == RESTART_LIFECYCLE_DEAD_KNOWN ||
	    pane->lifecycle == RESTART_LIFECYCLE_DEAD_UNKNOWN) {
		restart_state_capture_timeval(&wp->dead_time, &pane->dead_time);
		pane->have_dead_time = 1;
	}
	pane->output_generation = wp->output_generation;
	pane->last_output = wp->last_output_time;
	pane->last_prompt = wp->last_prompt_time;
	pane->command_start = wp->cmd_start_time;
	pane->command_end = wp->cmd_end_time;
	pane->command_status = wp->cmd_status;
	if (pane->command_status < -1 || pane->command_status > 255) {
		restart_set_cause(cause,
		    "restart command status is out of range");
		return (-1);
	}
	if (restart_theme_to_wire(wp->last_theme, &pane->last_theme) != 0) {
		restart_set_cause(cause, "restart pane theme is out of range");
		return (-1);
	}
	if (restart_pane_validate_running(pane, cause) != 0)
		return (-1);
	if (restart_options_capture(wp->options, &pane->options, budget,
	    cause) != 0)
		return (-1);
	return (restart_pane_validate_live(pane, wp, budget, cause));
}

static int
restart_window_capture_layout(__unused struct window *w,
    struct layout_cell *root, struct restart_layout **out,
    struct restart_budget *budget, char **cause)
{
	if (root == NULL) {
		restart_set_cause(cause, "restart window has no layout");
		return (-1);
	}
	return (restart_layout_capture(root, out, budget, cause));
}

static int
restart_window_capture_layouts(struct window *w,
    struct restart_window *window, struct restart_budget *budget, char **cause)
{
	if (w->flags & WINDOW_ZOOMED) {
		if (restart_window_capture_layout(w, w->saved_layout_root,
		    &window->layout, budget, cause) != 0 ||
		    restart_window_capture_layout(w, w->layout_root,
		    &window->visible_layout, budget, cause) != 0)
			return (-1);
		return (0);
	}
	return (restart_window_capture_layout(w, w->layout_root,
	    &window->layout, budget, cause));
}

static int
restart_window_capture(struct window *w, struct restart_window *window,
    struct restart_budget *budget, char **cause)
{
	struct window_pane	*wp;
	size_t			 count = 0;
	u_int			 flags;

	window->id = w->id;
	if (restart_state_capture_string(w->name, &window->name, budget,
	    "window name", cause) != 0)
		return (-1);
	restart_state_capture_timeval(&w->name_time, &window->name_time);
	restart_state_capture_timeval(&w->activity_time,
	    &window->activity_time);
	restart_state_capture_timeval(&w->creation_time,
	    &window->creation_time);
	if (w->active == NULL) {
		restart_set_cause(cause, "restart window has no active pane");
		return (-1);
	}
	window->active_pane_id = w->active->id;
	if (w->modal != NULL) {
		window->modal_pane_id = w->modal->id;
		window->have_modal = 1;
	}
	if (w->modal_last != NULL) {
		window->modal_last_id = w->modal_last->id;
		window->have_modal_last = 1;
	}
	TAILQ_FOREACH(wp, &w->panes, entry)
		count++;
	if (count == 0 || count > RESTART_MAX_RECORDS) {
		restart_set_cause(cause, "invalid restart window pane count");
		return (-1);
	}
	window->pane_order.items = restart_calloc(budget, count,
	    sizeof *window->pane_order.items, cause);
	if (window->pane_order.items == NULL)
		return (-1);
	TAILQ_FOREACH(wp, &w->panes, entry)
		window->pane_order.items[window->pane_order.count++] = wp->id;
	window->z_order.items = restart_calloc(budget, count,
	    sizeof *window->z_order.items, cause);
	if (window->z_order.items == NULL)
		return (-1);
	TAILQ_FOREACH(wp, &w->z_index, zentry)
		window->z_order.items[window->z_order.count++] = wp->id;
	count = 0;
	TAILQ_FOREACH(wp, &w->last_panes, sentry)
		count++;
	if (count != 0) {
		window->last_panes.items = restart_calloc(budget, count,
		    sizeof *window->last_panes.items, cause);
		if (window->last_panes.items == NULL)
			return (-1);
		TAILQ_FOREACH(wp, &w->last_panes, sentry)
			window->last_panes.items[window->last_panes.count++] =
			    wp->id;
	}
	window->last_layout = w->lastlayout;
	if (window->last_layout < -1 || window->last_layout > 6) {
		restart_set_cause(cause, "restart layout index is unsupported");
		return (-1);
	}
	window->sx = w->sx;
	window->sy = w->sy;
	window->manual_sx = w->manual_sx;
	window->manual_sy = w->manual_sy;
	window->xpixel = w->xpixel;
	window->ypixel = w->ypixel;
	if (window->sx < 1 || window->sx > RESTART_MAX_DIMENSION ||
	    window->sy < 1 || window->sy > RESTART_MAX_DIMENSION ||
	    window->xpixel < 1 || window->ypixel < 1) {
		restart_set_cause(cause, "restart window size is out of range");
		return (-1);
	}
	window->last_new_x = w->last_new_pane_x;
	window->last_new_y = w->last_new_pane_y;
	if (restart_flags_to_wire(restart_window_flag_map,
	    nitems(restart_window_flag_map), w->flags, sizeof(window->flags),
	    &flags, cause) != 0)
		return (-1);
	window->flags |= flags;
	if (w->old_layout != NULL) {
		if (restart_state_capture_string(w->old_layout,
		    &window->old_layout, budget, "window layout", cause) != 0)
			return (-1);
		window->have_old_layout = 1;
	}
	if (restart_window_capture_layouts(w, window, budget, cause) != 0)
		return (-1);
	return (restart_options_capture(w->options, &window->options, budget,
	    cause));
}

static int
restart_session_capture(struct session *ss, struct restart_session *session,
    struct restart_budget *budget, char **cause)
{
	struct winlink	*wl;
	size_t		 count = 0;
	u_int		 flags;

	session->id = ss->id;
	if (restart_state_capture_string(ss->name, &session->name, budget,
	    "session name", cause) != 0 ||
	    restart_state_capture_string(ss->cwd, &session->cwd, budget,
	    "session working directory", cause) != 0)
		return (-1);
	restart_state_capture_timeval(&ss->creation_time,
	    &session->creation_time);
	restart_state_capture_timeval(&ss->last_attached_time,
	    &session->last_attached_time);
	restart_state_capture_timeval(&ss->activity_time,
	    &session->activity_time);
	restart_state_capture_timeval(&ss->last_activity_time,
	    &session->last_activity_time);
	if (ss->curw == NULL || ss->curw->idx < 0) {
		restart_set_cause(cause,
		    "restart session has no current window");
		return (-1);
	}
	session->current_index = ss->curw->idx;
	RB_FOREACH(wl, winlinks, &ss->windows) {
		if (wl->idx < 0) {
			restart_set_cause(cause,
			    "restart winlink index is negative");
			return (-1);
		}
		count++;
	}
	if (count == 0 || count > RESTART_MAX_RECORDS) {
		restart_set_cause(cause, "invalid restart winlink count");
		return (-1);
	}
	session->winlinks = restart_calloc(budget, count,
	    sizeof *session->winlinks,
	    cause);
	if (session->winlinks == NULL)
		return (-1);
	RB_FOREACH(wl, winlinks, &ss->windows) {
		session->winlinks[session->winlink_count].index = wl->idx;
		session->winlinks[session->winlink_count].window_id =
		    wl->window->id;
		if (restart_flags_to_wire(restart_winlink_flag_map,
		    nitems(restart_winlink_flag_map), wl->flags,
		    sizeof(session->winlinks[0].flags), &flags, cause) != 0)
			return (-1);
		session->winlinks[session->winlink_count].flags = flags;
		session->winlink_count++;
	}
	count = 0;
	TAILQ_FOREACH(wl, &ss->lastw, sentry)
		count++;
	if (count != 0) {
		session->last_indices.items = restart_calloc(budget, count,
		    sizeof *session->last_indices.items, cause);
		if (session->last_indices.items == NULL)
			return (-1);
		TAILQ_FOREACH(wl, &ss->lastw, sentry) {
			if (wl->idx < 0) {
				restart_set_cause(cause,
				    "restart last index is negative");
				return (-1);
			}
			session->last_indices.items[
			    session->last_indices.count++] = wl->idx;
		}
	}
	if (ss->tio != NULL) {
		if (restart_termios_capture(ss->tio, &session->termios_cc,
		    cause) != 0)
			return (-1);
		session->have_termios = 1;
	}
	if (restart_options_capture(ss->options, &session->options, budget,
	    cause) != 0)
		return (-1);
	return (restart_environment_capture(ss->environ,
	    &session->environment, budget, cause));
}

static int
restart_state_capture(struct restart_state **out,
    struct restart_live_pane **live,
    struct restart_budget *budget, char **cause)
{
	struct restart_state	*state;
	struct session		*ss;
	struct session_group	*sg;
	struct winlink		*wl;
	struct window		*w;
	struct window_pane	*wp;
	struct restart_group	*group;
	size_t			 count, i, j;
	u_int			 window_id, pane_id, active_point;

	*out = NULL;
	*live = NULL;
	state = restart_calloc(budget, 1, sizeof *state, cause);
	if (state == NULL)
		return (-1);
	restart_state_capture_timeval(&start_time, &state->start_time);
	state->next_session_id = next_session_id;
	window_get_counters(&window_id, &pane_id, &active_point);
	state->next_window_id = window_id;
	state->next_pane_id = pane_id;
	state->next_active_point = active_point;
	state->next_hyperlink_external_id = hyperlinks_get_next_external_id();
	if (state->next_session_id == UINT32_MAX ||
	    state->next_window_id == UINT32_MAX ||
	    state->next_pane_id == UINT32_MAX ||
	    state->next_active_point == UINT32_MAX ||
	    state->next_hyperlink_external_id < 1 ||
	    state->next_hyperlink_external_id >= (uint64_t)LLONG_MAX) {
		restart_set_cause(cause, "restart counter cannot be preserved");
		goto fail;
	}
	if (restart_environment_capture(global_environ,
	    &state->global_environment, budget, cause) != 0 ||
	    restart_options_capture(global_options, &state->global_options,
	    budget, cause) != 0 ||
	    restart_options_capture(global_s_options,
	    &state->global_session_options, budget, cause) != 0 ||
	    restart_options_capture(global_w_options,
	    &state->global_window_options, budget, cause) != 0)
		goto fail;

	count = 0;
	RB_FOREACH(sg, session_groups, &session_groups)
		count++;
	if (count != 0) {
		state->groups = restart_calloc(budget, count,
		    sizeof *state->groups,
		    cause);
		if (state->groups == NULL)
			goto fail;
		RB_FOREACH(sg, session_groups, &session_groups) {
			group = &state->groups[state->group_count++];
			if (restart_state_capture_string(sg->name,
			    &group->name, budget, "session group name",
			    cause) != 0)
				goto fail;
			count = 0;
			TAILQ_FOREACH(ss, &sg->sessions, gentry)
				count++;
			if (count == 0) {
				restart_set_cause(cause,
				    "restart session group is empty");
				goto fail;
			}
			group->members.items = restart_calloc(budget, count,
			    sizeof *group->members.items, cause);
			if (group->members.items == NULL)
				goto fail;
			TAILQ_FOREACH(ss, &sg->sessions, gentry)
				group->members.items[
				    group->members.count++] = ss->id;
		}
	}

	count = 0;
	RB_FOREACH(ss, sessions, &sessions)
		count++;
	if (count != 0) {
		state->sessions = restart_calloc(budget, count,
		    sizeof *state->sessions, cause);
		if (state->sessions == NULL)
			goto fail;
		RB_FOREACH(ss, sessions, &sessions) {
			if (restart_session_capture(ss,
			    &state->sessions[state->session_count++], budget,
			    cause) != 0)
				goto fail;
		}
	}

	count = 0;
	RB_FOREACH(ss, sessions, &sessions) {
		RB_FOREACH(wl, winlinks, &ss->windows)
			count++;
	}
	if (count != 0) {
		state->windows = restart_calloc(budget, count,
		    sizeof *state->windows, cause);
		if (state->windows == NULL)
			goto fail;
		RB_FOREACH(ss, sessions, &sessions) {
			RB_FOREACH(wl, winlinks, &ss->windows) {
				w = wl->window;
				for (i = 0; i < state->window_count; i++) {
					if (state->windows[i].id == w->id)
						break;
				}
				if (i != state->window_count)
					continue;
				if (restart_window_capture(w,
				    &state->windows[state->window_count++],
				    budget, cause) != 0)
					goto fail;
			}
		}
	}

	count = 0;
	for (i = 0; i < state->window_count; i++)
		count += state->windows[i].pane_order.count;
	if (count != 0) {
		state->panes = restart_calloc(budget, count,
		    sizeof *state->panes,
		    cause);
		if (state->panes == NULL)
			goto fail;
		*live = restart_calloc(budget, count, sizeof **live, cause);
		if (*live == NULL)
			goto fail;
		RB_FOREACH(ss, sessions, &sessions) {
			RB_FOREACH(wl, winlinks, &ss->windows) {
				w = wl->window;
				TAILQ_FOREACH(wp, &w->panes, entry) {
					for (i = 0; i < state->pane_count;
					    i++) {
						if (state->panes[i].id ==
						    wp->id)
							break;
					}
					if (i != state->pane_count)
						continue;
					(*live)[state->pane_count].id = wp->id;
					(*live)[state->pane_count].wp = wp;
					if (restart_pane_capture(wp,
					    &state->panes[state->pane_count++],
					    budget, cause) != 0)
						goto fail;
				}
			}
		}
	}

	count = 0;
	for (i = 0; i < state->pane_count; i++) {
		if (state->panes[i].lifecycle == RESTART_LIFECYCLE_LIVE_FD ||
		    state->panes[i].lifecycle ==
		    RESTART_LIFECYCLE_DRAINING_KNOWN ||
		    state->panes[i].lifecycle ==
		    RESTART_LIFECYCLE_DRAINING_UNKNOWN)
			count++;
	}
	if (count != 0) {
		state->descriptors = restart_calloc(budget, count,
		    sizeof *state->descriptors, cause);
		if (state->descriptors == NULL)
			goto fail;
		for (i = 0; i < state->pane_count; i++) {
			if (state->panes[i].lifecycle !=
			    RESTART_LIFECYCLE_LIVE_FD &&
			    state->panes[i].lifecycle !=
			    RESTART_LIFECYCLE_DRAINING_KNOWN &&
			    state->panes[i].lifecycle !=
			    RESTART_LIFECYCLE_DRAINING_UNKNOWN)
				continue;
			if (state->panes[i].pid <= 0) {
				restart_set_cause(cause,
				    "restart descriptor pane has no process");
				goto fail;
			}
			j = state->descriptor_count++;
			state->descriptors[j].pane_id = state->panes[i].id;
			state->descriptors[j].pid = state->panes[i].pid;
		}
	}
	if (restart_buffers_capture(state, budget, cause) != 0)
		goto fail;
	*out = state;
	return (0);

fail:
	free(*live);
	*live = NULL;
	restart_state_free(state);
	return (-1);
}

static int
restart_state_write(struct restart_writer *rw,
    const struct restart_state_write_ctx *ctx, char **cause)
{
	const struct restart_state	*state = ctx->state;

	if (restart_write_envelope_begin(rw, RESTART_KIND_SERVER, cause) != 0 ||
	    restart_meta_write(rw, &rw->top, state, cause) != 0 ||
	    restart_environment_write(rw, &rw->top,
	    RESTART_SERVER_GLOBAL_ENVIRONMENT, &state->global_environment,
	    cause) != 0 ||
	    restart_options_write(rw, &rw->top, RESTART_SERVER_GLOBAL_OPTIONS,
	    &state->global_options, cause) != 0 ||
	    restart_options_write(rw, &rw->top,
	    RESTART_SERVER_GLOBAL_SESSION_OPTIONS,
	    &state->global_session_options, cause) != 0 ||
	    restart_options_write(rw, &rw->top,
	    RESTART_SERVER_GLOBAL_WINDOW_OPTIONS,
	    &state->global_window_options, cause) != 0 ||
	    restart_groups_write(rw, &rw->top, state, cause) != 0 ||
	    restart_sessions_write(rw, &rw->top, state, cause) != 0 ||
	    restart_windows_write(rw, &rw->top, state, cause) != 0 ||
	    restart_panes_write(rw, &rw->top, ctx, cause) != 0 ||
	    restart_descriptors_write(rw, &rw->top, state, cause) != 0 ||
	    restart_buffers_write(rw, &rw->top, state, cause) != 0 ||
	    restart_write_envelope_end(rw, cause) != 0)
		return (-1);
	return (0);
}

static int
restart_state_validate(const struct restart_state *state,
    struct restart_budget *budget, char **cause)
{
	struct restart_validate_ctx	 vctx;
	int64_t				 limit;
	size_t				 i;

	if (restart_state_check_unique(state, budget, cause) != 0 ||
	    restart_state_validate_counters(state, cause) != 0)
		return (-1);
	if (restart_options_validate(&state->global_options,
	    OPTIONS_TABLE_SERVER, cause) != 0 ||
	    restart_options_validate(&state->global_session_options,
	    OPTIONS_TABLE_SESSION, cause) != 0 ||
	    restart_options_validate(&state->global_window_options,
	    OPTIONS_TABLE_WINDOW, cause) != 0)
		return (-1);
	for (i = 0; i < state->session_count; i++) {
		if (restart_options_validate(&state->sessions[i].options,
		    OPTIONS_TABLE_SESSION, cause) != 0)
			return (-1);
	}
	for (i = 0; i < state->window_count; i++) {
		if (restart_options_validate(&state->windows[i].options,
		    OPTIONS_TABLE_WINDOW, cause) != 0)
			return (-1);
	}
	for (i = 0; i < state->pane_count; i++) {
		if (restart_options_validate(&state->panes[i].options,
		    OPTIONS_TABLE_PANE, cause) != 0)
			return (-1);
	}
	if (restart_options_get_number(&state->global_options,
	    "input-buffer-size", &limit) != 0 || limit < 0) {
		restart_set_cause(cause,
		    "restart input-buffer-size is missing");
		return (-1);
	}
	for (i = 0; i < state->pane_count; i++) {
		if (restart_terminal_validate_nested(state->panes[i].terminal,
		    (size_t)limit, cause) != 0)
			return (-1);
	}
	if (restart_validate_ctx_build(state, &vctx, budget, cause) != 0)
		return (-1);
	if (restart_state_validate_sessions(state, &vctx, cause) != 0 ||
	    restart_state_validate_groups(state, &vctx, cause) != 0 ||
	    restart_state_validate_windows(state, &vctx, cause) != 0 ||
	    restart_state_validate_layouts(state, budget, cause) != 0 ||
	    restart_state_validate_lifecycles(state, cause) != 0 ||
	    restart_state_validate_terminals(state, cause) != 0 ||
	    restart_state_validate_external_ids(state, budget, cause) != 0 ||
	    restart_state_validate_features(state, cause) != 0 ||
	    restart_state_validate_buffers(state, cause) != 0) {
		restart_validate_ctx_free(&vctx);
		return (-1);
	}
	restart_validate_ctx_free(&vctx);
	return (0);
}

static int
restart_state_encode1(const struct restart_state_write_ctx *ctx,
    struct ibuf **out, char **cause)
{
	struct restart_writer	*rw;

	rw = restart_writer_create(cause);
	if (rw == NULL)
		return (-1);
	if (restart_state_write(rw, ctx, cause) != 0 ||
	    restart_writer_detach(rw, out, cause) != 0) {
		restart_writer_discard(rw);
		return (-1);
	}
	return (0);
}

int
restart_state_encode(struct ibuf **out, char **cause)
{
	struct restart_budget		 budget = { 0 };
	struct restart_state_write_ctx	 ctx;
	struct restart_state		*state = NULL;
	struct restart_validate_ctx	 vctx;
	struct restart_live_pane	*live = NULL;
	size_t				 i;

	*out = NULL;
	if (cause != NULL)
		*cause = NULL;
	if (restart_state_capture(&state, &live, &budget, cause) != 0)
		return (-1);
	if (restart_state_sort(state, &budget, cause) != 0 ||
	    restart_state_validate_counters(state, cause) != 0)
		goto fail;
	if (restart_validate_ctx_build(state, &vctx, &budget, cause) != 0)
		goto fail;
	if (restart_state_validate_sessions(state, &vctx, cause) != 0 ||
	    restart_state_validate_groups(state, &vctx, cause) != 0 ||
	    restart_state_validate_windows(state, &vctx, cause) != 0 ||
	    restart_state_validate_layouts(state, &budget, cause) != 0 ||
	    restart_state_validate_lifecycles(state, cause) != 0) {
		restart_validate_ctx_free(&vctx);
		goto fail;
	}
	restart_validate_ctx_free(&vctx);
	if (state->pane_count > 1) {
		qsort(live, state->pane_count, sizeof *live,
		    restart_live_pane_compare);
		for (i = 1; i < state->pane_count; i++) {
			if (live[i].id == live[i - 1].id) {
				restart_set_cause(cause,
				    "duplicate restart live pane %u",
				    live[i].id);
				goto fail;
			}
		}
	}
	ctx.state = state;
	ctx.live = live;
	ctx.live_count = state->pane_count;
	if (restart_state_encode1(&ctx, out, cause) != 0)
		goto fail;
	free(live);
	restart_state_free(state);
	return (0);

fail:
	free(live);
	restart_state_free(state);
	return (-1);
}

int
restart_state_decode(const void *data, size_t size,
    struct restart_state **out, char **cause)
{
	struct restart_reader		 rr;
	struct restart_record		 record;
	struct restart_state		*state;
	uint32_t			 seen = 0;
	int				 found;

	*out = NULL;
	if (cause != NULL)
		*cause = NULL;
	if (restart_reader_open_envelope(&rr, data, size, RESTART_KIND_SERVER,
	    NULL, cause) != 0)
		return (-1);
	state = restart_calloc(rr.budget, 1, sizeof *state, cause);
	if (state == NULL)
		return (-1);
	state->features = rr.required_features;
	while ((found = restart_reader_next(&rr, &record, cause)) == 1) {
		if (record.type < RESTART_SERVER_META ||
		    record.type > RESTART_SERVER_BUFFERS) {
			if (restart_state_unknown(&record, "server",
			    cause) != 0)
				goto fail;
			continue;
		}
		if (restart_state_required(&record, cause) != 0 ||
		    (seen & (1U << record.type)) != 0) {
			restart_set_cause(cause,
			    "invalid restart server record");
			goto fail;
		}
		seen |= 1U << record.type;
		switch (record.type) {
		case RESTART_SERVER_META:
			if (restart_meta_read(&record, state, rr.budget,
			    cause) != 0)
				goto fail;
			break;
		case RESTART_SERVER_GLOBAL_ENVIRONMENT:
			if (restart_environment_read(&record,
			    &state->global_environment, rr.budget, cause) != 0)
				goto fail;
			break;
		case RESTART_SERVER_GLOBAL_OPTIONS:
			if (restart_options_read(&record,
			    &state->global_options, rr.budget, cause) != 0)
				goto fail;
			break;
		case RESTART_SERVER_GLOBAL_SESSION_OPTIONS:
			if (restart_options_read(&record,
			    &state->global_session_options, rr.budget,
			    cause) != 0)
				goto fail;
			break;
		case RESTART_SERVER_GLOBAL_WINDOW_OPTIONS:
			if (restart_options_read(&record,
			    &state->global_window_options, rr.budget,
			    cause) != 0)
				goto fail;
			break;
		case RESTART_SERVER_GROUPS:
			if (restart_groups_read(&record, state, rr.budget,
			    cause) != 0)
				goto fail;
			break;
		case RESTART_SERVER_SESSIONS:
			if (restart_sessions_read(&record, state, rr.budget,
			    cause) != 0)
				goto fail;
			break;
		case RESTART_SERVER_WINDOWS:
			if (restart_windows_read(&record, state, rr.budget,
			    cause) != 0)
				goto fail;
			break;
		case RESTART_SERVER_PANES:
			if (restart_panes_read(&record, state, rr.budget,
			    cause) != 0)
				goto fail;
			break;
		case RESTART_SERVER_DESCRIPTOR_KEYS:
			if (restart_descriptors_read(&record, state, rr.budget,
			    cause) != 0)
				goto fail;
			break;
		case RESTART_SERVER_BUFFERS:
			if (restart_buffers_read(&record, state, rr.budget,
			    cause) != 0)
				goto fail;
			break;
		}
	}
	if (found == -1 || restart_reader_finish_envelope(&rr, cause) != 0)
		goto fail;
	if ((seen & 0x0ffeU) != 0x0ffeU) {
		restart_set_cause(cause, "missing restart server record");
		goto fail;
	}
	restart_state_set_option_scopes(state);
	if (restart_state_validate(state, rr.budget, cause) != 0)
		goto fail;
	*out = state;
	return (0);

fail:
	restart_state_free(state);
	return (-1);
}

void
restart_state_free(struct restart_state *state)
{
	size_t i;

	if (state == NULL)
		return;
	free(state->descriptors);
	for (i = 0; i < state->pane_count; i++)
		restart_pane_free_contents(&state->panes[i]);
	free(state->panes);
	for (i = 0; i < state->window_count; i++)
		restart_window_free_contents(&state->windows[i]);
	free(state->windows);
	for (i = 0; i < state->session_count; i++)
		restart_session_free_contents(&state->sessions[i]);
	free(state->sessions);
	for (i = 0; i < state->group_count; i++) {
		free(state->groups[i].members.items);
		free(state->groups[i].name);
	}
	free(state->groups);
	for (i = 0; i < state->buffer_count; i++) {
		free(state->buffers[i].data.data);
		free(state->buffers[i].name);
	}
	free(state->buffers);
	restart_options_free(&state->global_window_options);
	restart_options_free(&state->global_session_options);
	restart_options_free(&state->global_options);
	restart_environment_free(&state->global_environment);
	free(state);
}

uint64_t
restart_state_features(const struct restart_state *state)
{
	return (state->features);
}

size_t
restart_state_descriptor_count(const struct restart_state *state)
{
	return (state->descriptor_count);
}

int
restart_state_descriptor_at(const struct restart_state *state, size_t index,
    u_int *pane_id, pid_t *pid)
{
	const struct restart_descriptor_key *key;

	if (index >= state->descriptor_count)
		return (-1);
	key = &state->descriptors[index];
	if (key->pid <= 0 || (int64_t)(pid_t)key->pid != key->pid)
		return (-1);
	*pane_id = key->pane_id;
	*pid = (pid_t)key->pid;
	return (0);
}
/*
 * One entry of the lifecycle-7 cascade queue.
 *
 * The queue is sized before finalization and an object is marked removed when
 * its work is accepted, so a pointer reached twice is queued once. That is
 * what keeps every reverse link, reference, event, duplicate and object
 * released exactly once, and it is why the cascade never recurses through
 * deferred destruction.
 */
enum restart_work_kind {
	RESTART_WORK_PANE,
	RESTART_WORK_WINDOW,
	RESTART_WORK_SESSION,
	RESTART_WORK_GROUP
};

struct restart_work {
	enum restart_work_kind	 kind;
	size_t			 index;
};


/* Build one option value into a set. */
static int
restart_option_build(struct options *oo, const struct restart_option *entry,
    const char *key, const char *string, int64_t number, char **cause)
{
	struct options_entry			*o;
	const struct options_table_entry	*oe;
	char					*value = NULL;
	int					 error;

	if (entry->type == RESTART_OPTION_USER) {
		options_set_string(oo, entry->name, 0, "%s", string);
		return (0);
	}
	oe = options_search(entry->name);
	if (oe == NULL)
		return (0);

	if (!entry->is_array) {
		/*
		 * Some of these sets have no parent, and without one the
		 * setters fatal rather than create an option that is missing.
		 */
		if (options_get_only(oo, entry->name) == NULL)
			options_default(oo, oe);
		switch (entry->type) {
		case RESTART_OPTION_STRING:
			options_set_string(oo, entry->name, 0, "%s", string);
			return (0);
		case RESTART_OPTION_COMMAND:
			return (options_from_string(oo, oe, entry->name,
			    string, 0, cause));
		default:
			options_set_number(oo, entry->name, number);
			return (0);
		}
	}

	o = options_get_only(oo, entry->name);
	if (o == NULL)
		o = options_empty(oo, oe);
	switch (entry->type) {
	case RESTART_OPTION_STRING:
	case RESTART_OPTION_COMMAND:
		return (options_array_set(o, key, string, 0, cause));
	case RESTART_OPTION_COLOUR:
		xasprintf(&value, "%s", colour_tostring(number));
		error = options_array_set(o, key, value, 0, cause);
		free(value);
		return (error);
	}
	restart_set_cause(cause, "restart option %s cannot be an array",
	    entry->name);
	return (-1);
}



struct restart_layout_build_frame {
	const struct restart_layout_cell	*in;
	struct layout_cell			*lc;
	size_t					 next;
};

/* Find a pane of this window by id. */
static struct window_pane *
restart_layout_find_pane(struct window *w, uint32_t id)
{
	struct window_pane	*wp;

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (wp->id == id)
			return (wp);
	}
	return (NULL);
}

/* Build a live layout tree from a decoded one. */
static int
restart_layout_build(const struct restart_layout *in, struct window *w,
    int saved, struct restart_budget *budget, struct layout_cell **out,
    char **cause)
{
	struct restart_layout_build_frame	*stack = NULL;
	struct layout_cell			*root = NULL, *lc, *child;
	const struct restart_layout_cell	*cell;
	struct window_pane			*wp;
	size_t					 depth = 0, i;
	int					 retval = -1;

	*out = NULL;
	if (in == NULL || in->root == NULL) {
		restart_set_cause(cause, "restart layout has no root");
		return (-1);
	}

	stack = restart_calloc(budget, in->cell_count, sizeof *stack, cause);
	if (stack == NULL)
		return (-1);

	root = layout_create_cell(NULL);
	stack[depth].in = in->root;
	stack[depth].lc = root;
	stack[depth].next = 0;
	depth++;

	while (depth != 0) {
		cell = stack[depth - 1].in;
		lc = stack[depth - 1].lc;

		if (stack[depth - 1].next == 0) {
			if (cell->type == RESTART_LAYOUT_HORIZONTAL)
				layout_make_node(lc, LAYOUT_LEFTRIGHT);
			else if (cell->type == RESTART_LAYOUT_VERTICAL)
				layout_make_node(lc, LAYOUT_TOPBOTTOM);
			layout_set_size(lc, cell->sx, cell->sy, cell->xoff,
			    cell->yoff);
			if (cell->have_fg) {
				lc->fg.sx = cell->fg_sx;
				lc->fg.sy = cell->fg_sy;
				lc->fg.xoff = cell->fg_xoff;
				lc->fg.yoff = cell->fg_yoff;
			}

			if (cell->type == RESTART_LAYOUT_PANE) {
				wp = restart_layout_find_pane(w, cell->pane_id);
				if (wp == NULL) {
					restart_set_cause(cause, "restart "
					    "layout names a foreign pane");
					goto out;
				}
				lc->wp = wp;
				if (cell->floating)
					lc->flags |= LAYOUT_CELL_FLOATING;
				if (saved)
					wp->saved_layout_cell = lc;
				else
					wp->layout_cell = lc;
			}
		}

		if (stack[depth - 1].next == cell->child_count) {
			depth--;
			continue;
		}
		i = stack[depth - 1].next++;
		if (depth == in->cell_count) {
			restart_set_cause(cause, "restart layout is too deep");
			goto out;
		}
		child = layout_create_cell(lc);
		TAILQ_INSERT_TAIL(&lc->cells, child, entry);
		stack[depth].in = cell->children[i];
		stack[depth].lc = child;
		stack[depth].next = 0;
		depth++;
	}

	*out = root;
	root = NULL;
	retval = 0;

out:
	if (root != NULL)
		layout_restart_free_cell(root);
	free(stack);
	return (retval);
}

/* Build an environment from a captured one. */
static struct environ *
restart_environment_build(const struct restart_environment *in)
{
	const struct restart_environment_entry	*entry;
	struct environ				*env;
	size_t					 i;
	int					 flags;

	env = environ_create();
	for (i = 0; i < in->count; i++) {
		entry = &in->entries[i];
		flags = 0;
		if (entry->flags & RESTART_ENVIRON_HIDDEN)
			flags |= ENVIRON_HIDDEN;
		if (entry->state == RESTART_ENV_STATE_SET)
			environ_set(env, entry->name, flags, "%s",
			    entry->value);
		else
			environ_clear(env, entry->name);
	}
	return (env);
}

/*
 * Build a set of options. An option the table has and the checkpoint does not
 * takes its default, which is what lets an older checkpoint be read.
 */
static int
restart_options_build(const struct restart_options *in, struct options *parent,
    struct options **out, char **cause)
{
	const struct options_table_entry	*oe;
	const struct restart_option		*entry;
	struct options				*oo;
	size_t					 i, j;

	*out = NULL;
	oo = options_create(parent);

	for (i = 0; i < in->count; i++) {
		entry = &in->entries[i];
		if (!entry->is_array) {
			if (restart_option_build(oo, entry, NULL,
			    entry->string, entry->number, cause) != 0)
				goto fail;
			continue;
		}
		for (j = 0; j < entry->item_count; j++) {
			if (restart_option_build(oo, entry,
			    entry->items[j].key, entry->items[j].string,
			    entry->items[j].number, cause) != 0)
				goto fail;
		}
	}

	if (in->scope != 0) {
		for (oe = options_table; oe->name != NULL; oe++) {
			if ((oe->scope & in->scope) == 0)
				continue;
			if (options_get_only(oo, oe->name) == NULL)
				options_default(oo, oe);
		}
	}

	*out = oo;
	return (0);

fail:
	options_free(oo);
	return (-1);
}

struct restart_apply_context {
	struct restart_budget		 budget;

	struct sessions			 sessions;
	struct session_groups		 groups;
	struct windows			 windows;
	struct window_pane_tree		 panes;

	struct options			*global_options;
	struct options			*global_s_options;
	struct options			*global_w_options;
	struct environ			*global_environ;
	struct utf8_restart_cache	*utf8_cache;

	struct session			**sessions_by_id;
	struct session_group		**groups_by_record;
	struct window			**windows_by_id;
	struct window_pane		**panes_by_id;
	struct restart_terminal_prepared **terminals;
	struct restart_terminal_prepared **survivors;
	int				*borrowed_fds;
	int				*duplicate_fds;

	uint8_t				*removed_sessions;
	uint8_t				*removed_groups;
	uint8_t				*removed_windows;
	uint8_t				*removed_panes;

	struct restart_work		*work;
	size_t				 work_count;
	size_t				 work_head;
	size_t				 work_capacity;

	size_t				 pane_count;
	size_t				 descriptor_count;
	size_t				 input_buffer_size;

	struct timeval			 start_time;
	uint32_t			 next_session_id;
	uint32_t			 next_window_id;
	uint32_t			 next_pane_id;
	uint32_t			 next_active_point;
	long long			 next_hyperlink_external_id;

	int				 published;
};

static int
restart_apply_roots_empty(char **cause)
{
	if (!RB_EMPTY(&sessions) || !RB_EMPTY(&session_groups) ||
	    !RB_EMPTY(&windows) || !RB_EMPTY(&all_window_panes)) {
		restart_set_cause(cause, "restart apply needs an empty server");
		return (-1);
	}
	if (!TAILQ_EMPTY(&clients)) {
		restart_set_cause(cause, "restart apply needs no clients");
		return (-1);
	}
	return (0);
}

static void
restart_apply_init(struct restart_apply_context *ctx)
{
	restart_terminal_preflight_rearm();
	memset(ctx, 0, sizeof *ctx);
	RB_INIT(&ctx->sessions);
	RB_INIT(&ctx->groups);
	RB_INIT(&ctx->windows);
	RB_INIT(&ctx->panes);
}

/*
 * Releases every candidate allocation and leaves the live roots, globals and
 * counters exactly as they were. Safe to call at any point before the first
 * terminal commit.
 */
/*
 * The index arrays, the removal bitmaps and the work queue. None of these is
 * part of the published graph, so both endings release them: rollback because
 * the candidate is being discarded, and publication because ownership of the
 * graph moved and the scratch that built it did not move with it.
 *
 * Shared rather than duplicated, because a scratch array added to one path
 * and not the other leaks only on whichever ending is rarer, and until
 * publication existed the rare one was never taken.
 */
static void
restart_apply_release_scratch(struct restart_apply_context *ctx)
{
	free(ctx->sessions_by_id);
	free(ctx->groups_by_record);
	free(ctx->windows_by_id);
	free(ctx->panes_by_id);
	free(ctx->terminals);
	free(ctx->survivors);
	free(ctx->borrowed_fds);
	free(ctx->duplicate_fds);
	free(ctx->removed_sessions);
	free(ctx->removed_groups);
	free(ctx->removed_windows);
	free(ctx->removed_panes);
	free(ctx->work);
}

static void
restart_apply_rollback(struct restart_apply_context *ctx)
{
	struct session		*s, *s1;
	struct window		*w, *w1;
	struct window_pane	*wp, *wp1;
	size_t			 i;

	if (ctx->terminals != NULL) {
		i = ctx->pane_count;
		while (i-- > 0)
			restart_terminal_discard(ctx->terminals[i]);
	}

	RB_FOREACH_SAFE(s, sessions, &ctx->sessions, s1)
		session_restart_destroy(&ctx->sessions, &ctx->groups, s);

	/*
	 * The candidate pane root is the authoritative owner. Walking the
	 * windows alone reaches only panes already linked into one, so a
	 * rollback between pane creation and list linking would release none
	 * of them.
	 */
	RB_FOREACH_SAFE(wp, window_pane_tree, &ctx->panes, wp1)
		window_pane_restart_release(&ctx->panes, wp);
	RB_FOREACH_SAFE(w, windows, &ctx->windows, w1)
		window_restart_destroy(&ctx->windows, &ctx->panes, w);

	if (ctx->duplicate_fds != NULL) {
		for (i = 0; i < ctx->descriptor_count; i++) {
			if (ctx->duplicate_fds[i] != -1)
				close(ctx->duplicate_fds[i]);
		}
	}

	utf8_restart_discard_width_cache(ctx->utf8_cache);
	environ_free(ctx->global_environ);
	options_free(ctx->global_w_options);
	options_free(ctx->global_s_options);
	options_free(ctx->global_options);

	restart_apply_release_scratch(ctx);
	restart_apply_init(ctx);
}

static int
restart_apply_meta(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	if (state->start_time.sec < 0 ||
	    (int64_t)(time_t)state->start_time.sec != state->start_time.sec) {
		restart_set_cause(cause, "restart start time is out of range");
		return (-1);
	}
	ctx->start_time.tv_sec = (time_t)state->start_time.sec;
	ctx->start_time.tv_usec = (suseconds_t)state->start_time.usec;

	ctx->next_session_id = state->next_session_id;
	ctx->next_window_id = state->next_window_id;
	ctx->next_pane_id = state->next_pane_id;
	ctx->next_active_point = state->next_active_point;

	if (state->next_hyperlink_external_id < 1 ||
	    state->next_hyperlink_external_id >= (uint64_t)LLONG_MAX) {
		restart_set_cause(cause,
		    "restart hyperlink counter is out of range");
		return (-1);
	}
	ctx->next_hyperlink_external_id =
	    (long long)state->next_hyperlink_external_id;
	return (0);
}

static int
restart_apply_globals(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	if (restart_options_build(&state->global_options, NULL,
	    &ctx->global_options, cause) != 0)
		return (-1);
	if (restart_options_build(&state->global_session_options, NULL,
	    &ctx->global_s_options, cause) != 0)
		return (-1);
	if (restart_options_build(&state->global_window_options, NULL,
	    &ctx->global_w_options, cause) != 0)
		return (-1);
	ctx->global_environ = restart_environment_build(
	    &state->global_environment);
	ctx->utf8_cache = utf8_restart_prepare_width_cache(ctx->global_options);
	return (0);
}

/*
 * Rollback closes every descriptor entry that is not -1, so an array of fds
 * is filled as part of being produced rather than by a later pass. Zeroed
 * entries would close stdin once per descriptor.
 */
static int *
restart_apply_fds(struct restart_apply_context *ctx, size_t count,
    char **cause)
{
	int	*fds;
	size_t	 i;

	fds = restart_calloc(&ctx->budget, count, sizeof *fds, cause);
	if (fds == NULL)
		return (NULL);
	for (i = 0; i < count; i++)
		fds[i] = -1;
	return (fds);
}

static int
restart_apply_indexes(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	if (state->session_count != 0) {
		ctx->sessions_by_id = restart_calloc(&ctx->budget,
		    state->session_count,
		    sizeof *ctx->sessions_by_id, cause);
		if (ctx->sessions_by_id == NULL)
			return (-1);
	}

	if (state->group_count != 0) {
		ctx->groups_by_record = restart_calloc(&ctx->budget,
		    state->group_count,
		    sizeof *ctx->groups_by_record, cause);
		if (ctx->groups_by_record == NULL)
			return (-1);
	}

	if (state->window_count != 0) {
		ctx->windows_by_id = restart_calloc(&ctx->budget,
		    state->window_count,
		    sizeof *ctx->windows_by_id, cause);
		if (ctx->windows_by_id == NULL)
			return (-1);
	}

	if (state->pane_count != 0) {
		ctx->panes_by_id = restart_calloc(&ctx->budget,
		    state->pane_count,
		    sizeof *ctx->panes_by_id, cause);
		if (ctx->panes_by_id == NULL)
			return (-1);
	}

	if (state->pane_count != 0) {
		ctx->terminals = restart_calloc(&ctx->budget,
		    state->pane_count,
		    sizeof *ctx->terminals, cause);
		if (ctx->terminals == NULL)
			return (-1);
		ctx->survivors = restart_calloc(&ctx->budget,
		    state->pane_count,
		    sizeof *ctx->survivors, cause);
		if (ctx->survivors == NULL)
			return (-1);
	}

	if (state->session_count != 0) {
		ctx->removed_sessions = restart_calloc(&ctx->budget,
		    state->session_count,
		    sizeof *ctx->removed_sessions, cause);
		if (ctx->removed_sessions == NULL)
			return (-1);
	}

	if (state->group_count != 0) {
		ctx->removed_groups = restart_calloc(&ctx->budget,
		    state->group_count,
		    sizeof *ctx->removed_groups, cause);
		if (ctx->removed_groups == NULL)
			return (-1);
	}

	if (state->window_count != 0) {
		ctx->removed_windows = restart_calloc(&ctx->budget,
		    state->window_count,
		    sizeof *ctx->removed_windows, cause);
		if (ctx->removed_windows == NULL)
			return (-1);
	}

	if (state->pane_count != 0) {
		ctx->removed_panes = restart_calloc(&ctx->budget,
		    state->pane_count,
		    sizeof *ctx->removed_panes, cause);
		if (ctx->removed_panes == NULL)
			return (-1);
	}

	if (state->descriptor_count != 0) {
		ctx->borrowed_fds = restart_apply_fds(ctx,
		    state->descriptor_count, cause);
		if (ctx->borrowed_fds == NULL)
			return (-1);
		ctx->duplicate_fds = restart_apply_fds(ctx,
		    state->descriptor_count, cause);
		if (ctx->duplicate_fds == NULL)
			return (-1);
	}

	/*
	 * One slot per object that can be removed. Every index is accepted at
	 * most once, so this cannot overflow and the cascade needs no growth
	 * path in the middle of an unwindable transaction.
	 */
	ctx->work_capacity = state->session_count + state->group_count +
	    state->window_count + state->pane_count;
	if (ctx->work_capacity != 0) {
		ctx->work = restart_calloc(&ctx->budget, ctx->work_capacity,
		    sizeof *ctx->work, cause);
		if (ctx->work == NULL)
			return (-1);
	}

	ctx->pane_count = state->pane_count;
	ctx->descriptor_count = state->descriptor_count;
	return (0);
}

static void
restart_apply_timeval(struct timeval *tv, const struct restart_timeval *in)
{
	tv->tv_sec = (time_t)in->sec;
	tv->tv_usec = (suseconds_t)in->usec;
}

static int
restart_apply_time(int64_t value, time_t *out, char **cause)
{
	if ((int64_t)(time_t)value != value) {
		restart_set_cause(cause, "restart time is out of range");
		return (-1);
	}
	*out = (time_t)value;
	return (0);
}

static int
restart_apply_windows(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	const struct restart_window	*in;
	struct window			*w;
	struct options			*oo;
	size_t				 i;

	for (i = 0; i < state->window_count; i++) {
		in = &state->windows[i];
		if (i != 0 && in->id <= state->windows[i - 1].id) {
			/*
			 * Not reachable from a validated state, and not only
			 * for the shapes tried. Validate rejects a window
			 * with no pane and a pane list whose count disagrees
			 * with the panes counted, so every element of this
			 * array is searched for every state rather than for
			 * some of them.
			 *
			 * Binary search finds every key only when the array
			 * is sorted: the first midpoint must compare greater
			 * than every key reached to its left and less than
			 * every key reached to its right, so it partitions
			 * correctly, and the same holds in each half. An
			 * unordered array therefore always misses a search
			 * first.
			 *
			 * Kept because that argument is about validate and
			 * not about this function's own input.
			 */
			restart_set_cause(cause,
			    "restart windows are not in ascending id order");
			return (-1);
		}
		if (restart_options_build(&in->options, ctx->global_w_options,
		    &oo, cause) != 0)
			return (-1);
		if (window_restart_create(&ctx->windows, in->id, in->name,
		    in->sx, in->sy, in->xpixel, in->ypixel, oo,
		    &w, cause) != 0) {
			options_free(oo);
			return (-1);
		}
		ctx->windows_by_id[i] = w;

		w->manual_sx = in->manual_sx;
		w->manual_sy = in->manual_sy;
		w->new_sx = 0;
		w->new_sy = 0;
		w->new_xpixel = 0;
		w->new_ypixel = 0;
		w->lastlayout = in->last_layout;
		w->last_new_pane_x = in->last_new_x;
		w->last_new_pane_y = in->last_new_y;
		restart_apply_timeval(&w->name_time, &in->name_time);
		restart_apply_timeval(&w->activity_time, &in->activity_time);
		restart_apply_timeval(&w->creation_time, &in->creation_time);
	}
	return (0);
}

static struct window *
restart_apply_find_window(struct restart_apply_context *ctx,
    const struct restart_state *state, uint32_t id)
{
	size_t	low = 0, high = state->window_count, mid;

	while (low < high) {
		mid = low + (high - low) / 2;
		if (state->windows[mid].id == id)
			return (ctx->windows_by_id[mid]);
		if (state->windows[mid].id < id)
			low = mid + 1;
		else
			high = mid;
	}
	return (NULL);
}

static size_t
restart_apply_find_session_index(const struct restart_state *state,
    uint32_t id)
{
	size_t	low = 0, high = state->session_count, mid;

	while (low < high) {
		mid = low + (high - low) / 2;
		if (state->sessions[mid].id == id)
			return (mid);
		if (state->sessions[mid].id < id)
			low = mid + 1;
		else
			high = mid;
	}
	return (SIZE_MAX);
}

static int
restart_apply_pane_command(__unused struct restart_apply_context *ctx,
    const struct restart_pane *in, struct window_pane *wp,
    __unused char **cause)
{
	size_t	i;

	if (in->argv.count != 0) {
		wp->argv = xcalloc(in->argv.count + 1, sizeof *wp->argv);
		for (i = 0; i < in->argv.count; i++) {
			wp->argv[i] = xstrdup(in->argv.items[i]);
			wp->argc = (int)(i + 1);
		}
	}
	if (in->have_shell)
		wp->shell = xstrdup(in->shell);
	if (in->have_cwd)
		wp->cwd = xstrdup(in->cwd);
	return (0);
}

static int
restart_apply_panes(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	const struct restart_pane	*in;
	struct window_pane		*wp;
	struct window			*w;
	struct options			*oo;
	size_t				 i;

	for (i = 0; i < state->pane_count; i++) {
		in = &state->panes[i];
		if (i != 0 && in->id <= state->panes[i - 1].id) {
			/*
			 * Not reachable from a validated state, and not only
			 * for the shapes tried. Validate rejects a window
			 * with no pane and a pane list whose count disagrees
			 * with the panes counted, so every element of this
			 * array is searched for every state rather than for
			 * some of them.
			 *
			 * Binary search finds every key only when the array
			 * is sorted: the first midpoint must compare greater
			 * than every key reached to its left and less than
			 * every key reached to its right, so it partitions
			 * correctly, and the same holds in each half. An
			 * unordered array therefore always misses a search
			 * first.
			 *
			 * Kept because that argument is about validate and
			 * not about this function's own input.
			 */
			restart_set_cause(cause,
			    "restart panes are not in ascending id order");
			return (-1);
		}
		w = restart_apply_find_window(ctx, state, in->window_id);
		if (w == NULL) {
			restart_set_cause(cause,
			    "restart pane names an unknown window");
			return (-1);
		}
		if (restart_options_build(&in->options, w->options,
		    &oo, cause) != 0)
			return (-1);
		if (window_pane_restart_create(&ctx->panes, w, in->id,
		    in->active_point, in->sx, in->sy, oo,
		    &wp, cause) != 0) {
			options_free(oo);
			return (-1);
		}
		ctx->panes_by_id[i] = wp;

		wp->xoff = in->xoff;
		wp->yoff = in->yoff;
		wp->flags |= restart_flags_from_wire(restart_pane_flag_map,
		    nitems(restart_pane_flag_map), in->flags);
		wp->cmd_status = in->command_status;
		if (restart_theme_from_wire(in->last_theme,
		    &wp->last_theme) != 0) {
			restart_set_cause(cause,
			    "restart pane theme is out of range");
			return (-1);
		}
		wp->output_generation = in->output_generation;
		if (restart_apply_time(in->last_output, &wp->last_output_time,
		    cause) != 0 ||
		    restart_apply_time(in->last_prompt, &wp->last_prompt_time,
		    cause) != 0 ||
		    restart_apply_time(in->command_start, &wp->cmd_start_time,
		    cause) != 0 ||
		    restart_apply_time(in->command_end, &wp->cmd_end_time,
		    cause) != 0)
			return (-1);
		if (restart_apply_pane_command(ctx, in, wp, cause) != 0)
			return (-1);
	}
	return (0);
}

static size_t
restart_apply_find_pane_index(const struct restart_state *state, uint32_t id)
{
	size_t	low = 0, high = state->pane_count, mid;

	while (low < high) {
		mid = low + (high - low) / 2;
		if (state->panes[mid].id == id)
			return (mid);
		if (state->panes[mid].id < id)
			low = mid + 1;
		else
			high = mid;
	}
	return (SIZE_MAX);
}

/*
 * Links one serialized id list into a window collection. The lists are built
 * by direct insertion because they must keep their serialized order, which
 * the stack helpers would reverse by inserting at the head. Uniqueness is
 * enforced here: apply rebuilds these lists rather than parsing them, so the
 * reader that rejects adjacent equal ids is not in this path.
 */
static int
restart_apply_link_panes(struct restart_apply_context *ctx,
    const struct restart_state *state, size_t window_index,
    uint8_t *linked, uint8_t *zlinked, char **cause)
{
	const struct restart_window	*in = &state->windows[window_index];
	struct window			*w = ctx->windows_by_id[window_index];
	struct window_pane		*wp;
	size_t				 i, index;

	if (in->pane_order.count != in->z_order.count) {
		restart_set_cause(cause,
		    "restart pane and z orders differ in length");
		return (-1);
	}
	for (i = 0; i < in->pane_order.count; i++) {
		index = restart_apply_find_pane_index(state,
		    in->pane_order.items[i]);
		if (index == SIZE_MAX) {
			restart_set_cause(cause,
			    "restart pane order names an unknown pane");
			return (-1);
		}
		wp = ctx->panes_by_id[index];
		if (wp->window != w) {
			restart_set_cause(cause,
			    "restart pane order names a foreign pane");
			return (-1);
		}
		if (linked[index]) {
			restart_set_cause(cause,
			    "restart pane order repeats a pane");
			return (-1);
		}
		linked[index] = 1;
		TAILQ_INSERT_TAIL(&w->panes, wp, entry);
	}
	for (i = 0; i < in->z_order.count; i++) {
		index = restart_apply_find_pane_index(state,
		    in->z_order.items[i]);
		if (index == SIZE_MAX) {
			restart_set_cause(cause,
			    "restart z order names an unknown pane");
			return (-1);
		}
		wp = ctx->panes_by_id[index];
		if (wp->window != w) {
			restart_set_cause(cause,
			    "restart z order names a foreign pane");
			return (-1);
		}
		if (zlinked[index]) {
			restart_set_cause(cause,
			    "restart z order repeats a pane");
			return (-1);
		}
		zlinked[index] = 1;
		TAILQ_INSERT_TAIL(&w->z_index, wp, zentry);
	}
	for (i = 0; i < in->last_panes.count; i++) {
		index = restart_apply_find_pane_index(state,
		    in->last_panes.items[i]);
		if (index == SIZE_MAX) {
			restart_set_cause(cause,
			    "restart last panes names an unknown pane");
			return (-1);
		}
		wp = ctx->panes_by_id[index];
		if (wp->window != w) {
			restart_set_cause(cause,
			    "restart last panes names a foreign pane");
			return (-1);
		}
		if (wp->flags & PANE_VISITED) {
			restart_set_cause(cause,
			    "restart last panes repeats a pane");
			return (-1);
		}
		wp->flags |= PANE_VISITED;
		TAILQ_INSERT_TAIL(&w->last_panes, wp, sentry);
	}
	return (0);
}

static int
restart_apply_window_selection(struct restart_apply_context *ctx,
    const struct restart_state *state, size_t window_index, char **cause)
{
	const struct restart_window	*in = &state->windows[window_index];
	struct window			*w = ctx->windows_by_id[window_index];
	size_t				 index;

	index = restart_apply_find_pane_index(state, in->active_pane_id);
	if (index == SIZE_MAX || ctx->panes_by_id[index]->window != w) {
		restart_set_cause(cause, "restart window has no active pane");
		return (-1);
	}
	w->active = ctx->panes_by_id[index];

	if (in->have_modal) {
		index = restart_apply_find_pane_index(state,
		    in->modal_pane_id);
		if (index == SIZE_MAX || ctx->panes_by_id[index]->window != w) {
			restart_set_cause(cause,
			    "restart window modal pane is unknown");
			return (-1);
		}
		w->modal = ctx->panes_by_id[index];
		if (w->modal != w->active) {
			restart_set_cause(cause,
			    "restart window modal pane is not active");
			return (-1);
		}
	}
	if (in->have_modal_last) {
		if (!in->have_modal) {
			restart_set_cause(cause,
			    "restart window has modal last without modal");
			return (-1);
		}
		index = restart_apply_find_pane_index(state,
		    in->modal_last_id);
		if (index == SIZE_MAX || ctx->panes_by_id[index]->window != w) {
			restart_set_cause(cause,
			    "restart window modal last pane is unknown");
			return (-1);
		}
		w->modal_last = ctx->panes_by_id[index];
		if (w->modal_last == w->modal) {
			restart_set_cause(cause,
			    "restart window modal last equals modal");
			return (-1);
		}
	}
	return (0);
}

static int
restart_apply_link(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	uint8_t	*linked = NULL, *zlinked = NULL;
	size_t	 i;
	int	 retval = -1;

	if (state->pane_count != 0) {
		linked = restart_calloc(&ctx->budget, state->pane_count,
		    sizeof *linked, cause);
		if (linked == NULL)
			return (-1);
		zlinked = restart_calloc(&ctx->budget, state->pane_count,
		    sizeof *zlinked, cause);
		if (zlinked == NULL)
			goto out;
	}

	for (i = 0; i < state->window_count; i++) {
		if (restart_apply_link_panes(ctx, state, i, linked, zlinked,
		    cause) != 0)
			goto out;
		if (restart_apply_window_selection(ctx, state, i, cause) != 0)
			goto out;
	}
	for (i = 0; i < state->pane_count; i++) {
		if (!linked[i] || !zlinked[i]) {
			restart_set_cause(cause,
			    "restart pane is absent from its window order");
			goto out;
		}
	}
	retval = 0;

out:
	free(zlinked);
	free(linked);
	return (retval);
}


/*
 * Builds both layout trees for one window. An unzoomed window has only the
 * normal tree, which becomes the live root. A zoomed window keeps the normal
 * tree as the saved root and the visible tree as the live one, so hidden panes
 * retain a saved cell and carry no live cell at all.
 */
static int
restart_apply_window_layout(struct restart_apply_context *ctx,
    size_t window_index, const struct restart_state *state, char **cause)
{
	const struct restart_window	*in = &state->windows[window_index];
	struct window			*w = ctx->windows_by_id[window_index];
	struct window_pane		*wp;
	struct layout_cell		*root = NULL, *visible = NULL;
	u_int				 zoomed = 0;

	if (!(in->flags & RESTART_WINDOW_ZOOMED)) {
		if (in->visible_layout != NULL) {
			restart_set_cause(cause,
			    "restart window is unzoomed with a visible layout");
			return (-1);
		}
		if (restart_layout_build(in->layout, w, 0, &ctx->budget,
		    &root, cause) != 0)
			return (-1);
		w->layout_root = root;
		return (0);
	}

	if (in->visible_layout == NULL) {
		restart_set_cause(cause,
		    "restart window is zoomed without a visible layout");
		return (-1);
	}
	if (restart_layout_build(in->layout, w, 1, &ctx->budget,
	    &root, cause) != 0)
		return (-1);
	w->saved_layout_root = root;
	if (restart_layout_build(in->visible_layout, w, 0, &ctx->budget,
	    &visible, cause) != 0)
		return (-1);
	w->layout_root = visible;
	w->flags |= restart_flags_from_wire(restart_window_flag_map,
	    nitems(restart_window_flag_map), in->flags);

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (wp->layout_cell == NULL)
			continue;
		if (wp->layout_cell->flags & LAYOUT_CELL_FLOATING)
			continue;
		wp->flags |= PANE_ZOOMED;
		zoomed++;
	}
	if (zoomed != 1) {
		restart_set_cause(cause,
		    "restart zoomed window has %u tiled visible panes", zoomed);
		return (-1);
	}
	return (0);
}

static int
restart_apply_layouts(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	size_t	i;

	for (i = 0; i < state->window_count; i++) {
		if (restart_apply_window_layout(ctx, i, state, cause) != 0)
			return (-1);
	}
	return (0);
}

/*
 * Materializes a termios from the symbolic control keys. Every host slot is
 * disabled first, so a key this platform does not name is left disabled rather
 * than inheriting whatever the zeroed struct implies.
 */
static int
restart_apply_termios(const struct restart_termios_cc *in, struct termios *tio,
    char **cause)
{
	const struct restart_termios_key	*key;
	size_t					 i;
	uint8_t					 value;

	memset(tio, 0, sizeof *tio);
	for (i = 0; i < NCCS; i++)
		tio->c_cc[i] = _POSIX_VDISABLE;

	for (i = 0; i < nitems(restart_termios_keys); i++) {
		key = &restart_termios_keys[i];
		if (!(in->present & (1U << (key->key - 1))))
			continue;
		if (key->index >= NCCS) {
			if (!key->required)
				continue;
			restart_set_cause(cause,
			    "unsupported restart terminal control key %u",
			    key->key);
			return (-1);
		}
		value = in->value[key->key - 1];
		if ((uint8_t)(cc_t)value != value) {
			restart_set_cause(cause,
			    "restart terminal control value is out of range");
			return (-1);
		}
		tio->c_cc[key->index] = (cc_t)value;
	}
	if ((in->present & RESTART_TERMIOS_REQUIRED) !=
	    RESTART_TERMIOS_REQUIRED) {
		restart_set_cause(cause,
		    "missing required restart terminal control key");
		return (-1);
	}
	return (0);
}

static int
restart_apply_sessions(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	const struct restart_session	*in;
	struct session			*sn;
	struct options			*oo;
	struct environ			*env;
	struct termios			 tio;
	size_t				 i;

	for (i = 0; i < state->session_count; i++) {
		in = &state->sessions[i];
		if (i != 0 && in->id <= state->sessions[i - 1].id) {
			restart_set_cause(cause,
			    "restart sessions are not in ascending id order");
			return (-1);
		}
		if (in->have_termios &&
		    restart_apply_termios(&in->termios_cc, &tio, cause) != 0)
			return (-1);
		if (restart_options_build(&in->options, ctx->global_s_options,
		    &oo, cause) != 0)
			return (-1);
		env = restart_environment_build(&in->environment);
		if (session_restart_create(&ctx->sessions, in->id, in->name,
		    in->cwd, env, oo, in->have_termios ? &tio : NULL,
		    &sn, cause) != 0) {
			environ_free(env);
			options_free(oo);
			return (-1);
		}
		ctx->sessions_by_id[i] = sn;

		restart_apply_timeval(&sn->creation_time, &in->creation_time);
		restart_apply_timeval(&sn->last_attached_time,
		    &in->last_attached_time);
		restart_apply_timeval(&sn->activity_time, &in->activity_time);
		restart_apply_timeval(&sn->last_activity_time,
		    &in->last_activity_time);
	}
	return (0);
}

/*
 * Links one session's winlinks. Order is serialized: winlinks ascend by index,
 * and the last stack is most recent first, so both are built by direct
 * insertion with WINLINK_VISITED set only for stack members.
 */
static int
restart_apply_session_winlinks(struct restart_apply_context *ctx,
    const struct restart_state *state, size_t session_index, char **cause)
{
	const struct restart_session	*in = &state->sessions[session_index];
	struct session			*sn;
	struct window			*w;
	struct winlink			*wl;
	size_t				 i;
	int				 found = 0;

	sn = ctx->sessions_by_id[session_index];

	for (i = 0; i < in->winlink_count; i++) {
		/*
		 * A strict-increase check guarded by i != 0 validates the
		 * ordering and never the first element's domain. Winlink index
		 * is the only signed key apply consumes, and tmux encodes an
		 * automatic-allocation request as a negative index, so a
		 * negative stored index would publish a session whose next
		 * checkpoint the following restart's reader rejects.
		 */
		if (in->winlinks[i].index < 0) {
			restart_set_cause(cause,
			    "restart winlink index is negative");
			return (-1);
		}
		if (i != 0 &&
		    in->winlinks[i].index <= in->winlinks[i - 1].index) {
			restart_set_cause(cause,
			    "restart winlinks are not in ascending index "
			    "order");
			return (-1);
		}
		w = restart_apply_find_window(ctx, state,
		    in->winlinks[i].window_id);
		if (w == NULL) {
			restart_set_cause(cause,
			    "restart winlink names an unknown window");
			return (-1);
		}
		if (winlink_restart_create(sn, w, in->winlinks[i].index,
		    in->winlinks[i].flags,
		    &wl, cause) != 0)
			return (-1);
		if (in->winlinks[i].index == in->current_index) {
			sn->curw = wl;
			found = 1;
		}
	}
	if (in->winlink_count != 0 && !found) {
		restart_set_cause(cause,
		    "restart session current index has no winlink");
		return (-1);
	}

	for (i = 0; i < in->last_indices.count; i++) {
		wl = winlink_find_by_index(&sn->windows,
		    in->last_indices.items[i]);
		if (wl == NULL) {
			restart_set_cause(cause,
			    "restart last index has no winlink");
			return (-1);
		}
		if (wl == sn->curw) {
			restart_set_cause(cause,
			    "restart last stack contains the current winlink");
			return (-1);
		}
		if (wl->flags & WINLINK_VISITED) {
			restart_set_cause(cause,
			    "restart last stack repeats a winlink");
			return (-1);
		}
		wl->flags |= WINLINK_VISITED;
		TAILQ_INSERT_TAIL(&sn->lastw, wl, sentry);
	}
	return (0);
}

static int
restart_apply_groups(struct restart_apply_context *ctx,
    const struct restart_state *state, uint8_t *grouped, char **cause)
{
	const struct restart_group	*in;
	struct session_group		*sg;
	struct session			*sn;
	size_t				 i, j, index;

	for (i = 0; i < state->group_count; i++) {
		in = &state->groups[i];
		if (i != 0 &&
		    strcmp(in->name, state->groups[i - 1].name) <= 0) {
			restart_set_cause(cause,
			    "restart groups are not in bytewise name order");
			return (-1);
		}
		if (session_group_restart_create(&ctx->groups, in->name,
		    &sg, cause) != 0)
			return (-1);
		ctx->groups_by_record[i] = sg;
		if (in->members.count == 0) {
			restart_set_cause(cause, "restart group is empty");
			return (-1);
		}
		for (j = 0; j < in->members.count; j++) {
			index = restart_apply_find_session_index(state,
			    in->members.items[j]);
			if (index == SIZE_MAX) {
				restart_set_cause(cause,
				    "restart group names an unknown session");
				return (-1);
			}
			if (grouped[index]) {
				restart_set_cause(cause,
				    "restart session is in two groups");
				return (-1);
			}
			grouped[index] = 1;
			sn = ctx->sessions_by_id[index];
			session_group_restart_add(sg, sn);
		}
	}
	return (0);
}

static int
restart_apply_link_sessions(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	uint8_t	*grouped = NULL;
	size_t	 i;
	int	 retval = -1;

	for (i = 0; i < state->session_count; i++) {
		if (restart_apply_session_winlinks(ctx, state, i, cause) != 0)
			return (-1);
	}
	if (state->session_count != 0) {
		grouped = restart_calloc(&ctx->budget, state->session_count,
		    sizeof *grouped, cause);
		if (grouped == NULL)
			return (-1);
	}
	retval = restart_apply_groups(ctx, state, grouped, cause);
	free(grouped);
	return (retval);
}

/*
 * Builds a candidate wait status without preserving wire-foreign bits, then
 * requires the host macros to round-trip the requested kind and value. A
 * platform whose encoding does not agree fails the apply rather than
 * publishing a status the runtime would misread.
 */
static int
restart_apply_wait_status(uint8_t kind, uint32_t value, int *out, char **cause)
{
	int	status;

	switch (kind) {
	case RESTART_DEAD_EXITED:
		if (value > 0xff) {
			restart_set_cause(cause,
			    "restart exit status is out of range");
			return (-1);
		}
		status = (int)((value & 0xff) << 8);
		if (!WIFEXITED(status) ||
		    (uint32_t)WEXITSTATUS(status) != value) {
			restart_set_cause(cause,
			    "host wait macros do not round-trip an exit "
			    "status");
			return (-1);
		}
		break;
	case RESTART_DEAD_SIGNALED:
		if (value == 0 || value > 0xff) {
			restart_set_cause(cause,
			    "restart exit signal is out of range");
			return (-1);
		}
		status = (int)value;
		if (!WIFSIGNALED(status) ||
		    (uint32_t)WTERMSIG(status) != value) {
			restart_set_cause(cause,
			    "host wait macros do not round-trip a signal");
			return (-1);
		}
		break;
	default:
		restart_set_cause(cause, "unknown restart exit kind");
		return (-1);
	}
	*out = status;
	return (0);
}

/*
 * Derives the runtime state for one pane from its serialized lifecycle. The
 * descriptor-backed lifecycles get their fd and event later; this sets only
 * the flags, status and times, and leaves lifecycle 7 in its pre-finalization
 * form.
 */
static int
restart_apply_pane_lifecycle(const struct restart_pane *in,
    struct window_pane *wp, char **cause)
{
	int	known = 0, exited = 0, drawn = 0, empty = 0;

	wp->status = 0;
	timerclear(&wp->dead_time);
	wp->pid = -1;

	switch (in->lifecycle) {
	case RESTART_LIFECYCLE_LIVE_FD:
		break;
	case RESTART_LIFECYCLE_EMPTY:
		empty = 1;
		break;
	case RESTART_LIFECYCLE_DEAD_KNOWN:
		exited = known = drawn = 1;
		break;
	case RESTART_LIFECYCLE_DEAD_UNKNOWN:
		exited = drawn = 1;
		break;
	case RESTART_LIFECYCLE_DRAINING_KNOWN:
		exited = known = 1;
		break;
	case RESTART_LIFECYCLE_DRAINING_UNKNOWN:
		exited = 1;
		break;
	case RESTART_LIFECYCLE_EXITED_UNKNOWN:
		exited = 1;
		break;
	case RESTART_LIFECYCLE_INACTIVE:
		break;
	default:
		restart_set_cause(cause, "unknown restart pane lifecycle");
		return (-1);
	}

	if (in->lifecycle != RESTART_LIFECYCLE_INACTIVE &&
	    in->lifecycle != RESTART_LIFECYCLE_EMPTY) {
		if (in->pid <= 0 || (int64_t)(pid_t)in->pid != in->pid) {
			restart_set_cause(cause,
			    "restart pane pid is out of range");
			return (-1);
		}
		wp->pid = (pid_t)in->pid;
	}

	if (known) {
		if (!in->have_result) {
			restart_set_cause(cause,
			    "restart known pane has no exit result");
			return (-1);
		}
		if (restart_apply_wait_status(in->dead_kind, in->dead_value,
		    &wp->status, cause) != 0)
			return (-1);
	} else if (in->have_result) {
		restart_set_cause(cause,
		    "restart unknown pane carries an exit result");
		return (-1);
	}

	if (in->have_dead_time) {
		if (in->lifecycle != RESTART_LIFECYCLE_DEAD_KNOWN &&
		    in->lifecycle != RESTART_LIFECYCLE_DEAD_UNKNOWN) {
			restart_set_cause(cause,
			    "restart pane carries an unexpected dead time");
			return (-1);
		}
		restart_apply_timeval(&wp->dead_time, &in->dead_time);
	} else if (in->lifecycle == RESTART_LIFECYCLE_DEAD_KNOWN ||
	    in->lifecycle == RESTART_LIFECYCLE_DEAD_UNKNOWN) {
		restart_set_cause(cause, "restart dead pane has no dead time");
		return (-1);
	}

	if (empty)
		wp->flags |= PANE_EMPTY;
	if (exited)
		wp->flags |= PANE_EXITED;
	if (known)
		wp->flags |= PANE_STATUSREADY;
	if (drawn)
		wp->flags |= PANE_STATUSDRAWN;
	wp->flags |= PANE_REDRAW|PANE_STYLECHANGED|PANE_THEMECHANGED;
	return (0);
}

static int
restart_apply_lifecycles(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	size_t	i;

	for (i = 0; i < state->pane_count; i++) {
		if (restart_apply_pane_lifecycle(&state->panes[i],
		    ctx->panes_by_id[i], cause) != 0)
			return (-1);
	}
	return (0);
}

/*
 * Resolves each descriptor exactly once, in canonical pane id order, and
 * transfers one close-on-exec duplicate to the pane only after its bufferevent
 * exists. The callback descriptor is borrowed throughout and is never closed
 * here.
 *
 * Ordering is asserted rather than assumed: the reader enforces it, but a DTO
 * handed to apply directly does not pass through the reader, and this loop
 * relies on the order to reject a repeated key.
 */
/*
 * Name an adopted pane's terminal from the inherited master rather than from
 * the checkpoint: an empty name matches no client, and a stale one would match
 * the wrong one. spawn.c fills this field for panes it creates; a pane adopted
 * from a descriptor has never been through it.
 */
static int
restart_pane_adopt_tty(struct window_pane *wp, int fd, const char *recorded,
    char **cause)
{
	const char	*name;

	name = ptsname(fd);
	if (name == NULL) {
		restart_set_cause(cause,
		    "restart descriptor has no terminal name");
		return (-1);
	}
	if (recorded != NULL && strcmp(name, recorded) != 0) {
		log_debug("%s: pane %%%u terminal name changed across restart, "
		    "recorded %s, actual %s", __func__, wp->id, recorded, name);
	}
	strlcpy(wp->tty, name, sizeof wp->tty);
	return (0);
}

static int
restart_apply_descriptors(struct restart_apply_context *ctx,
    const struct restart_state *state, restart_fd_lookup_cb lookup, void *arg,
    char **cause)
{
	const struct restart_descriptor_key	*key;
	struct window_pane			*wp;
	size_t					 i, j, index;
	uint8_t					 lifecycle;
	int					 fd, dup;

	for (i = 0; i < state->descriptor_count; i++) {
		key = &state->descriptors[i];
		if (i != 0 &&
		    key->pane_id <= state->descriptors[i - 1].pane_id) {
			restart_set_cause(cause,
			    "restart descriptor keys are not in ascending "
			    "pane order");
			return (-1);
		}
		index = restart_apply_find_pane_index(state, key->pane_id);
		if (index == SIZE_MAX) {
			restart_set_cause(cause,
			    "restart descriptor names an unknown pane");
			return (-1);
		}
		wp = ctx->panes_by_id[index];
		lifecycle = state->panes[index].lifecycle;
		if ((lifecycle != RESTART_LIFECYCLE_LIVE_FD &&
		    lifecycle != RESTART_LIFECYCLE_DRAINING_KNOWN &&
		    lifecycle != RESTART_LIFECYCLE_DRAINING_UNKNOWN &&
		    lifecycle != RESTART_LIFECYCLE_EXITED_UNKNOWN) ||
		    (wp->flags & PANE_STATUSDRAWN)) {
			restart_set_cause(cause,
			    "restart descriptor names a pane without a live "
			    "stream");
			return (-1);
		}
		if (key->pid <= 0 || (int64_t)(pid_t)key->pid != key->pid ||
		    (pid_t)key->pid != wp->pid) {
			restart_set_cause(cause,
			    "restart descriptor pid does not match its pane");
			return (-1);
		}

		fd = -1;
		if (lookup(arg, key->pane_id, (pid_t)key->pid, &fd) != 0 ||
		    fd < 0) {
			restart_set_cause(cause,
			    "restart descriptor lookup failed");
			return (-1);
		}
		if (fcntl(fd, F_GETFD) == -1) {
			restart_set_cause(cause,
			    "restart descriptor is not open");
			return (-1);
		}
		for (j = 0; j < i; j++) {
			if (ctx->borrowed_fds[j] == fd) {
				restart_set_cause(cause,
				    "restart descriptor callback reused a "
				    "descriptor");
				return (-1);
			}
		}
		ctx->borrowed_fds[i] = fd;

		if ((fcntl(fd, F_GETFL) & O_NONBLOCK) == 0) {
			restart_set_cause(cause,
			    "restart descriptor is not nonblocking");
			return (-1);
		}

		/*
		 * Duplicate above the standard descriptors, so that a pane
		 * never lands on 0, 1 or 2 when one of those is closed.
		 */
		dup = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
		if (dup == -1) {
			restart_set_cause(cause,
			    "restart descriptor duplication failed");
			return (-1);
		}
		ctx->duplicate_fds[i] = dup;

		if (window_pane_restart_set_event(wp, dup, cause) != 0)
			return (-1);
		wp->fd = dup;
		ctx->duplicate_fds[i] = -1;

		if (restart_pane_adopt_tty(wp, dup, state->panes[index].tty,
		    cause) != 0)
			return (-1);
	}
	return (0);
}

/*
 * Nonallocating reference algebra over the built graph. Runs before any
 * lifecycle-7 cascade and again after it, so a cascade that drops a reference
 * or strands a reverse link is caught before anything is published. Counts are
 * checked against the DTO counts rather than accumulated freely, so an
 * overflow cannot pass as a match.
 */
/*
 * Accept work for an object, once. Returns zero if it was already accepted,
 * which is how a pointer reached from two directions is acted on exactly
 * once.
 */
static int
restart_apply_queue(struct restart_apply_context *ctx,
    enum restart_work_kind kind, size_t index)
{
	uint8_t	*removed;

	switch (kind) {
	case RESTART_WORK_PANE:
		removed = ctx->removed_panes;
		break;
	case RESTART_WORK_WINDOW:
		removed = ctx->removed_windows;
		break;
	case RESTART_WORK_SESSION:
		removed = ctx->removed_sessions;
		break;
	default:
		removed = ctx->removed_groups;
		break;
	}
	if (removed == NULL || removed[index])
		return (0);
	removed[index] = 1;

	ctx->work[ctx->work_count].kind = kind;
	ctx->work[ctx->work_count].index = index;
	ctx->work_count++;
	return (1);
}

static size_t
restart_apply_find_window_index(const struct restart_state *state, uint32_t id)
{
	size_t	low = 0, high = state->window_count, mid;

	while (low < high) {
		mid = low + (high - low) / 2;
		if (state->windows[mid].id == id)
			return (mid);
		if (state->windows[mid].id < id)
			low = mid + 1;
		else
			high = mid;
	}
	return (SIZE_MAX);
}

/*
 * Retain an exited-unknown pane as the observable lifecycle-4 state.
 *
 * One current time is taken before anything changes, so every pane retained
 * in one apply agrees. No hook, no format expansion and no screen mutation:
 * the checkpointed terminal has to stay byte exact, and remain-on-exit-format
 * is drawn by the runtime path rather than by startup finalization.
 */
static void
restart_apply_retain_exited(struct window_pane *wp)
{
	struct timeval	tv;

	gettimeofday(&tv, NULL);
	wp->dead_time = tv;

	wp->flags |= PANE_STATUSDRAWN;
	wp->flags &= ~PANE_STATUSREADY;
}

static int restart_apply_remove_pane(struct restart_apply_context *,
    const struct restart_state *, size_t, char **);
static int restart_apply_remove_window(struct restart_apply_context *,
    const struct restart_state *, size_t, char **);
static int restart_apply_remove_session(struct restart_apply_context *,
    const struct restart_state *, size_t, char **);
static int restart_apply_remove_group(struct restart_apply_context *,
    const struct restart_state *, size_t, char **);

/*
 * Which candidate group holds this session, by record index.
 *
 * session_group_contains walks the live root, which is empty during apply, so
 * membership has to be resolved against the candidate groups instead.
 */
static size_t
restart_apply_find_group_record(struct restart_apply_context *ctx,
    const struct restart_state *state, struct session *s)
{
	struct session_group	*sg;
	struct session		*member;
	size_t			 i;

	for (i = 0; i < state->group_count; i++) {
		sg = ctx->groups_by_record[i];
		if (sg == NULL)
			continue;
		TAILQ_FOREACH(member, &sg->sessions, gentry) {
			if (member == s)
				return (i);
		}
	}
	return (SIZE_MAX);
}

static int
restart_apply_remove_pane(struct restart_apply_context *ctx,
    const struct restart_state *state, size_t index, char **cause)
{
	struct window_pane	*wp = ctx->panes_by_id[index];
	struct window		*w;
	size_t			 windex;

	if (wp == NULL)
		return (0);
	w = wp->window;

	if (w != NULL)
		layout_restart_remove_pane(w, wp);

	window_pane_restart_remove(&ctx->panes, wp);
	ctx->panes_by_id[index] = NULL;

	if (w != NULL && TAILQ_EMPTY(&w->panes)) {
		windex = restart_apply_find_window_index(state, w->id);
		if (windex == SIZE_MAX) {
			restart_set_cause(cause,
			    "restart cascade reached an unknown window");
			return (-1);
		}
		restart_apply_queue(ctx, RESTART_WORK_WINDOW, windex);
	}
	return (0);
}

static int
restart_apply_remove_window(struct restart_apply_context *ctx,
    const struct restart_state *state, size_t index, char **cause)
{
	struct window		*w = ctx->windows_by_id[index];
	struct window_pane	*wp, *wp1;
	struct session		*s;
	struct winlink		*wl, *wl1;
	size_t			 i, pindex;

	if (w == NULL)
		return (0);

	RB_FOREACH_SAFE(wp, window_pane_tree, &ctx->panes, wp1) {
		if (wp->window != w)
			continue;
		pindex = restart_apply_find_pane_index(state, wp->id);
		if (pindex == SIZE_MAX) {
			restart_set_cause(cause,
			    "restart cascade reached an unknown pane");
			return (-1);
		}
		if (restart_apply_queue(ctx, RESTART_WORK_PANE, pindex) &&
		    restart_apply_remove_pane(ctx, state, pindex, cause) != 0)
			return (-1);
	}

	for (i = 0; i < state->session_count; i++) {
		s = ctx->sessions_by_id[i];
		if (s == NULL)
			continue;
		RB_FOREACH_SAFE(wl, winlinks, &s->windows, wl1) {
			if (wl->window != w)
				continue;
			session_restart_unlink(s, wl);
			if (RB_EMPTY(&s->windows))
				restart_apply_queue(ctx, RESTART_WORK_SESSION,
				    i);
		}
	}

	window_restart_destroy(&ctx->windows, &ctx->panes, w);
	ctx->windows_by_id[index] = NULL;
	return (0);
}

static int
restart_apply_remove_session(struct restart_apply_context *ctx,
    const struct restart_state *state, size_t index, char **cause)
{
	struct session	*s = ctx->sessions_by_id[index];
	size_t		 i;

	(void)cause;

	if (s == NULL)
		return (0);

	/*
	 * A group loses its whole membership with any member, because a
	 * surviving group with one absent member is a state the live server
	 * never produces.
	 *
	 * This does not fire for a valid graph and is kept for one that is
	 * not. Members hold identical window sets, which capture enforces,
	 * and a session is queued only on losing its last winlink, so every
	 * member loses it in the same window-removal loop and is queued
	 * there. By the time this runs, each is already marked removed and
	 * the queue call is a no-op. Removing it changes no observable
	 * outcome, which was measured rather than assumed.
	 */
	i = restart_apply_find_group_record(ctx, state, s);
	if (i != SIZE_MAX)
		restart_apply_queue(ctx, RESTART_WORK_GROUP, i);

	session_restart_destroy(&ctx->sessions, &ctx->groups, s);
	ctx->sessions_by_id[index] = NULL;
	return (0);
}

static int
restart_apply_remove_group(struct restart_apply_context *ctx,
    const struct restart_state *state, size_t index, char **cause)
{
	struct session	*s;
	size_t		 i;

	for (i = 0; i < state->session_count; i++) {
		s = ctx->sessions_by_id[i];
		if (s == NULL ||
		    restart_apply_find_group_record(ctx, state, s) != index)
			continue;
		if (restart_apply_queue(ctx, RESTART_WORK_SESSION, i) &&
		    restart_apply_remove_session(ctx, state, i, cause) != 0)
			return (-1);
	}
	return (0);
}

static int
restart_apply_drain(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	struct restart_work	*item;

	while (ctx->work_head < ctx->work_count) {
		item = &ctx->work[ctx->work_head++];
		switch (item->kind) {
		case RESTART_WORK_PANE:
			if (restart_apply_remove_pane(ctx, state, item->index,
			    cause) != 0)
				return (-1);
			break;
		case RESTART_WORK_WINDOW:
			if (restart_apply_remove_window(ctx, state,
			    item->index, cause) != 0)
				return (-1);
			break;
		case RESTART_WORK_SESSION:
			if (restart_apply_remove_session(ctx, state,
			    item->index, cause) != 0)
				return (-1);
			break;
		case RESTART_WORK_GROUP:
			if (restart_apply_remove_group(ctx, state,
			    item->index, cause) != 0)
				return (-1);
			break;
		}
	}
	return (0);
}

/*
 * Step 14: finalize every lifecycle-7 pane before caches or terminals exist,
 * so no removed pane ever allocates a derived cache or a prepared terminal.
 */
static int
restart_apply_lifecycles_final(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	const struct restart_pane	*in;
	struct window_pane		*wp;
	size_t				 i;
	int				 remain;

	for (i = 0; i < state->pane_count; i++) {
		in = &state->panes[i];
		if (in->lifecycle != RESTART_LIFECYCLE_EXITED_UNKNOWN)
			continue;
		wp = ctx->panes_by_id[i];
		if (wp == NULL)
			continue;

		/*
		 * Only "off" removes. The other four retain because the exit
		 * status is unknown, so success cannot be ruled out.
		 */
		remain = options_get_number(wp->options, "remain-on-exit");
		if (remain != 0) {
			restart_apply_retain_exited(wp);
			continue;
		}
		if (restart_apply_queue(ctx, RESTART_WORK_PANE, i) &&
		    restart_apply_remove_pane(ctx, state, i, cause) != 0)
			return (-1);
	}

	if (restart_apply_drain(ctx, state, cause) != 0)
		return (-1);

	/*
	 * No lifecycle 7 may be observable after this point: every one was
	 * either retained as the lifecycle-4 state or removed. A survivor
	 * here would reach terminal preparation as a state the live server
	 * cannot represent.
	 */
	for (i = 0; i < state->pane_count; i++) {
		if (state->panes[i].lifecycle !=
		    RESTART_LIFECYCLE_EXITED_UNKNOWN)
			continue;
		wp = ctx->panes_by_id[i];
		if (wp != NULL && !(wp->flags & PANE_STATUSDRAWN)) {
			restart_set_cause(cause,
			    "restart lifecycle 7 survived finalization");
			return (-1);
		}
	}
	return (0);
}

static size_t
restart_apply_removed_count(const uint8_t *removed, size_t count)
{
	size_t	i, n = 0;

	if (removed == NULL)
		return (0);
	for (i = 0; i < count; i++) {
		if (removed[i])
			n++;
	}
	return (n);
}

/*
 * Step 15, first stage: window scrollbar flags and position.
 *
 * Read from the candidate window's own option tree. Nothing here consults a
 * live root, which is the property the probe checks by poisoning the live
 * tables rather than by emptying them: an empty table and a correct candidate
 * table agree on any defaulted value, so emptiness cannot tell reading from
 * not reading.
 */
static void
restart_derive_window_scrollbars(struct restart_apply_context *ctx)
{
	struct window	*w;

	RB_FOREACH(w, windows, &ctx->windows) {
		w->sb = options_get_number(w->options, "pane-scrollbars");
		w->sb_pos = options_get_number(w->options,
		    "pane-scrollbars-position");
	}
}

/*
 * Step 15, second stage: every surviving pane's scrollbar style.
 *
 * After the window stage, because the width this produces is what the final
 * geometry pass in step 16 subtracts from each pane.
 */
static int
restart_derive_pane_scrollbars(struct restart_apply_context *ctx, char **cause)
{
	struct window_pane	*wp;

	RB_FOREACH(wp, window_pane_tree, &ctx->panes) {
		if (style_restart_scrollbar(&wp->scrollbar_style, wp->options,
		    wp, cause) != 0)
			return (-1);
	}
	return (0);
}

/*
 * Step 15, third stage: both fill cells, from the final active pane.
 *
 * The live derivation is reused rather than reimplemented. It already sets
 * FORMAT_NOJOBS, reads the window's own option tree and takes its active
 * pane, and the cells it produces are inline members, so it retains no
 * allocation for the transaction to own. Whether it reaches a live root
 * anyway is not settled by reading it, so the probe poisons fill-character
 * and requires the candidate value to be unchanged.
 */
static void
restart_derive_fill_cells(struct restart_apply_context *ctx)
{
	struct window	*w;

	RB_FOREACH(w, windows, &ctx->windows)
		window_set_fill_cells(w);
}

/*
 * Step 15, fourth stage: every surviving pane's default palette.
 *
 * Last, and complete before terminal preparation, because preparation reads
 * the palette when it moves colour state and a pane whose defaults are not
 * yet derived would be prepared against an empty one.
 */
static int
restart_derive_palettes(struct restart_apply_context *ctx,
    __unused char **cause)
{
	struct window_pane	*wp;

	RB_FOREACH(wp, window_pane_tree, &ctx->panes) {
		colour_palette_from_option(&wp->palette, wp->options);
	}
	return (0);
}

static int
restart_apply_derive(struct restart_apply_context *ctx, char **cause)
{
	restart_derive_window_scrollbars(ctx);
	if (restart_derive_pane_scrollbars(ctx, cause) != 0)
		return (-1);
	restart_derive_fill_cells(ctx);
	if (restart_derive_palettes(ctx, cause) != 0)
		return (-1);
	return (0);
}

/*
 * Step 16, first part: final offsets and dimensions.
 *
 * After derivation rather than before, because the widths this subtracts are
 * what step 15 produced. Running it earlier yields a pane sized against the
 * default scrollbar width, which still renders and is therefore wrong in a
 * way nothing downstream reports.
 */
static void
restart_apply_geometry(struct restart_apply_context *ctx)
{
	struct window	*w;

	RB_FOREACH(w, windows, &ctx->windows) {
		if (w->layout_root == NULL)
			continue;
		layout_fix_offsets(w);
		layout_restart_fix_panes(w);
	}
}

/*
 * Step 16, second part: prepare each surviving pane's terminal.
 *
 * Nothing is committed here. The prepared state stays detached, so the
 * process-global hyperlink and image lists are untouched until the no-fail
 * commit boundary, and rollback discards each prepared terminal through the
 * array this fills.
 *
 * Indexed by pane position rather than appended, so a removed pane leaves a
 * NULL rather than shifting its neighbours, which is what lets rollback walk
 * the array without knowing which panes survived.
 */
static int
restart_apply_prepare_terminals(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	struct window_pane	*wp;
	size_t			 i;
	u_int			 final_sx, final_sy;

	for (i = 0; i < state->pane_count; i++) {
		wp = ctx->panes_by_id[i];
		if (wp == NULL)
			continue;
		if (state->panes[i].terminal == NULL) {
			restart_set_cause(cause,
			    "restart pane has no terminal state");
			return (-1);
		}
		if (restart_terminal_prepare(state->panes[i].terminal, wp,
		    &ctx->budget, &ctx->terminals[i], cause) != 0)
			return (-1);

		/*
		 * Final geometry comes from the layout pass and the serialized
		 * geometry is what the child still believes, so a pane whose
		 * two disagree owes its child one resize.
		 */
		if (wp->sx == state->panes[i].sx &&
		    wp->sy == state->panes[i].sy)
			continue;
		if (restart_terminal_resize_prepared(ctx->terminals[i],
		    wp->sx, wp->sy, cause) != 0)
			return (-1);

		/*
		 * Stage against the serialized dimensions rather than the
		 * current ones. The pane already holds its final size, so the
		 * old size the child needs told about is the serialized one,
		 * and the helper reads the pane to fill it. Restoring it for
		 * the call is what makes the record describe the child's
		 * transition rather than an internal one.
		 */
		final_sx = wp->sx;
		final_sy = wp->sy;
		wp->sx = state->panes[i].sx;
		wp->sy = state->panes[i].sy;
		if (window_pane_restart_stage_resize(wp, final_sx, final_sy,
		    cause) != 0) {
			wp->sx = final_sx;
			wp->sy = final_sy;
			return (-1);
		}
	}
	return (0);
}

/*
 * Step 17: one aggregate capacity preflight over the complete surviving
 * batch, then the second reference pass.
 *
 * One call, never one per pane. The preflight accumulates across the batch
 * before a single check, so a loop would pass every element individually and
 * still exceed the total, and during apply the global counts start at zero so
 * each element clears the limit trivially. The count passed is asserted equal
 * to the survivor count held independently, which is a thing a loop cannot
 * satisfy.
 */
static int
restart_apply_preflight(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	size_t	i, n = 0, survivors = 0;

	for (i = 0; i < state->pane_count; i++) {
		if (ctx->panes_by_id[i] != NULL)
			survivors++;
		if (ctx->terminals[i] != NULL)
			ctx->survivors[n++] = ctx->terminals[i];
	}
	if (n != survivors) {
		restart_set_cause(cause,
		    "restart prepared terminals do not cover the survivors");
		return (-1);
	}
	return (restart_terminal_preflight_batch(ctx->survivors, n, cause));
}

/*
 * Step 18: cross the commit boundary and publish.
 *
 * Nothing here may fail. Every fallible action has already run, every
 * allocation the graph needs is charged and held, and each call below either
 * returns void or cannot report an error. A fallible call added here would
 * leave a graph half installed with no way back, because the candidate roots
 * are emptied as ownership moves and there is nothing left to unwind to.
 *
 * Panes commit in ascending id order, which is the order their terminals were
 * prepared and the order the eviction lists will hold them in.
 */
/*
 * Hand each restored pane's staged resize to its child and arm its event.
 * Runs over the live tree rather than the context, because publication has
 * already moved the panes onto it and released the context's scratch index.
 */
static int
restart_apply_deliver(char **cause)
{
	struct window_pane	*wp;
	char			*one;
	int			 failed = 0;

	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		one = NULL;
		if (window_pane_restart_enable_event(wp, &one) == 0)
			continue;
		if (!failed) {
			failed = 1;
			restart_set_cause(cause, "%s", one);
		}
		free(one);
	}
	return (failed ? -1 : 0);
}

static void
restart_apply_publish(struct restart_apply_context *ctx,
    const struct restart_state *state)
{
	struct window_pane	*wp;
	size_t			 i;

	for (i = 0; i < state->pane_count; i++) {
		wp = ctx->panes_by_id[i];
		if (wp == NULL || ctx->terminals[i] == NULL)
			continue;
		restart_terminal_commit(ctx->terminals[i], wp);
		ctx->terminals[i] = NULL;
	}

	/*
	 * Ownership moves rather than copies: the candidate roots are emptied
	 * so nothing holds a second reference to an object the live roots now
	 * own, and rollback can no longer reach any of it.
	 */
	sessions = ctx->sessions;
	session_groups = ctx->groups;
	windows = ctx->windows;
	all_window_panes = ctx->panes;
	RB_INIT(&ctx->sessions);
	RB_INIT(&ctx->groups);
	RB_INIT(&ctx->windows);
	RB_INIT(&ctx->panes);

	options_free(global_options);
	options_free(global_s_options);
	options_free(global_w_options);
	environ_free(global_environ);
	global_options = ctx->global_options;
	global_s_options = ctx->global_s_options;
	global_w_options = ctx->global_w_options;
	global_environ = ctx->global_environ;
	ctx->global_options = NULL;
	ctx->global_s_options = NULL;
	ctx->global_w_options = NULL;
	ctx->global_environ = NULL;

	utf8_restart_commit_width_cache(ctx->utf8_cache);
	ctx->utf8_cache = NULL;

	restart_buffers_publish(state);

	next_session_id = ctx->next_session_id;
	window_set_counters(ctx->next_window_id, ctx->next_pane_id,
	    ctx->next_active_point);
	hyperlinks_restart_set_next_external_id(
	    ctx->next_hyperlink_external_id);
	start_time = ctx->start_time;

	restart_apply_release_scratch(ctx);
	ctx->published = 1;
}

static int
restart_apply_check_references(struct restart_apply_context *ctx,
    const struct restart_state *state, char **cause)
{
	struct session		*sn;
	struct window		*w;
	struct window_pane	*wp;
	struct winlink		*wl;
	size_t			 nsessions = 0, nwindows = 0, npanes = 0;
	u_int			 forward, reverse;

	RB_FOREACH(sn, sessions, &ctx->sessions) {
		nsessions++;
		if (nsessions > state->session_count) {
			restart_set_cause(cause,
			    "restart candidate has more sessions than the "
			    "decoded state");
			return (-1);
		}
		if (sn->references != 1) {
			restart_set_cause(cause,
			    "restart session reference count is %u, not 1",
			    sn->references);
			return (-1);
		}
		if (sn->attached != 0) {
			restart_set_cause(cause,
			    "restart session has %u attached clients",
			    sn->attached);
			return (-1);
		}
		if (!RB_EMPTY(&sn->windows) && sn->curw == NULL) {
			restart_set_cause(cause,
			    "restart session has winlinks but no current");
			return (-1);
		}
	}

	RB_FOREACH(w, windows, &ctx->windows) {
		nwindows++;
		if (nwindows > state->window_count) {
			restart_set_cause(cause,
			    "restart candidate has more windows than the "
			    "decoded state");
			return (-1);
		}

		forward = 0;
		RB_FOREACH(sn, sessions, &ctx->sessions) {
			RB_FOREACH(wl, winlinks, &sn->windows) {
				if (wl->window != w)
					continue;
				if (forward == UINT_MAX) {
					restart_set_cause(cause,
					    "restart window winlink count "
					    "overflows");
					return (-1);
				}
				forward++;
			}
		}
		reverse = 0;
		TAILQ_FOREACH(wl, &w->winlinks, wentry) {
			if (wl->window != w) {
				restart_set_cause(cause,
				    "restart window reverse list holds a "
				    "foreign winlink");
				return (-1);
			}
			if (reverse == UINT_MAX) {
				restart_set_cause(cause,
				    "restart window reverse list overflows");
				return (-1);
			}
			reverse++;
		}
		if (forward == 0) {
			restart_set_cause(cause,
			    "restart window has no winlink");
			return (-1);
		}
		if (forward != reverse || w->references != forward) {
			restart_set_cause(cause,
			    "restart window references %u, forward %u, "
			    "reverse %u", w->references, forward, reverse);
			return (-1);
		}
		if (w->active == NULL || w->active->window != w) {
			restart_set_cause(cause,
			    "restart window has no owned active pane");
			return (-1);
		}
		if (TAILQ_EMPTY(&w->panes)) {
			restart_set_cause(cause, "restart window has no pane");
			return (-1);
		}
	}

	RB_FOREACH(wp, window_pane_tree, &ctx->panes) {
		npanes++;
		if (npanes > state->pane_count) {
			restart_set_cause(cause,
			    "restart candidate has more panes than the "
			    "decoded state");
			return (-1);
		}
		if (wp->references != 1) {
			restart_set_cause(cause,
			    "restart pane reference count is %d, not 1",
			    wp->references);
			return (-1);
		}
		if (wp->window == NULL ||
		    RB_FIND(windows, &ctx->windows, wp->window) != wp->window) {
			restart_set_cause(cause,
			    "restart pane is not owned by a candidate window");
			return (-1);
		}
		if (!TAILQ_EMPTY(&wp->modes) || wp->wait_item != NULL ||
		    wp->editor != NULL) {
			restart_set_cause(cause,
			    "restart pane carries a mode, wait or editor "
			    "owner");
			return (-1);
		}
	}

	/*
	 * Lifecycle-7 finalization removes objects, so the candidate is the
	 * decoded set minus what the cascade took. Comparing against the
	 * decoded counts alone would reject every graph containing an exited
	 * pane with remain-on-exit off.
	 */
	if (nsessions != state->session_count -
	    restart_apply_removed_count(ctx->removed_sessions,
	    state->session_count) ||
	    nwindows != state->window_count -
	    restart_apply_removed_count(ctx->removed_windows,
	    state->window_count) ||
	    npanes != state->pane_count -
	    restart_apply_removed_count(ctx->removed_panes,
	    state->pane_count)) {
		restart_set_cause(cause,
		    "restart candidate root counts do not match the decoded "
		    "state");
		return (-1);
	}
	return (0);
}

int
restart_state_apply(const struct restart_state *state,
    restart_fd_lookup_cb lookup, void *arg, char **cause)
{
	struct restart_apply_context	 ctx;

	if (cause != NULL)
		*cause = NULL;
	if (state == NULL) {
		restart_set_cause(cause, "restart apply needs decoded state");
		return (-1);
	}
	if (lookup == NULL && state->descriptor_count != 0) {
		restart_set_cause(cause,
		    "restart apply needs a descriptor callback");
		return (-1);
	}
	if (restart_apply_roots_empty(cause) != 0)
		return (-1);

	restart_apply_init(&ctx);

	/*
	 * Revalidates the decoded state, so a caller inside the tree cannot
	 * apply a partially constructed or mutated private DTO.
	 */
	if (restart_state_validate(state, &ctx.budget, cause) != 0)
		goto fail;
	if (restart_apply_meta(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_globals(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_indexes(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_windows(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_panes(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_link(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_layouts(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_sessions(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_link_sessions(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_lifecycles(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_descriptors(&ctx, state, lookup, arg, cause) != 0)
		goto fail;
	/*
	 * First reference pass, over the fully built graph before any cascade
	 * removes from it. Running it only after finalization would let a
	 * defect in construction be reported as a defect in the cascade, and
	 * would let a cascade that happened to restore the equations hide one
	 * that construction had already broken.
	 */
	if (restart_apply_check_references(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_lifecycles_final(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_derive(&ctx, cause) != 0)
		goto fail;
	restart_apply_geometry(&ctx);
	if (restart_apply_prepare_terminals(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_preflight(&ctx, state, cause) != 0)
		goto fail;
	if (restart_apply_check_references(&ctx, state, cause) != 0)
		goto fail;

	restart_apply_publish(&ctx, state);

	/*
	 * Delivery runs after publication because it touches the children,
	 * which is the one thing that cannot be undone. There is no rollback
	 * past this point, so a pane that will not take its resize is
	 * reported rather than retried, and the loop continues: aborting on
	 * the first would leave every later pane holding a staged resize its
	 * child never hears about, which is worse than the failure being
	 * reported.
	 */
	return (restart_apply_deliver(cause));

fail:
	restart_apply_rollback(&ctx);
	return (-1);
}
