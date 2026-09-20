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

#ifndef TMUX_RESTART_CODEC_PRIVATE_H
#define TMUX_RESTART_CODEC_PRIVATE_H

#include "restart-codec.h"

#define RESTART_MAGIC "TMUXRST\0"
#define RESTART_MAGIC_SIZE 8
#define RESTART_MAJOR 1
#define RESTART_MINOR 0
#define RESTART_KIND_TERMINAL 1
#define RESTART_HEADER_SIZE 48
#define RESTART_RECORD_REQUIRED 0x0001

#define RESTART_MAX_SIZE ((size_t)1024 * 1024 * 1024)
#define RESTART_MAX_FIELD ((size_t)1024 * 1024)
#define RESTART_MAX_PRODUCER 256U
#define RESTART_MAX_RECORDS 1000000U
#define RESTART_MAX_TITLES 10U
#define RESTART_MAX_DIMENSION 10000U
#define RESTART_FEATURE_SIXEL 0x1ULL

enum restart_terminal_record {
	RESTART_TERMINAL_PALETTE = 1,
	RESTART_TERMINAL_SCREEN,
	RESTART_TERMINAL_PARSER,
	RESTART_TERMINAL_PTY_INPUT,
	RESTART_TERMINAL_PTY_OUTPUT
};

struct restart_budget {
	size_t	 allocation;
	uint32_t records;
	uint32_t hyperlinks;
	uint32_t images;
};

struct restart_write_scope {
	size_t	 length_offset;
	size_t	 payload_offset;
	size_t	 count_offset;
	size_t	 features_offset;
	uint32_t count;
};

struct restart_writer {
	struct ibuf		*buf;
	struct restart_budget	 budget;
	uint64_t		 features;
	struct restart_write_scope top;
};

struct restart_reader {
	struct ibuf		 buf;
	struct restart_budget	*budget;
	struct restart_budget	 own_budget;
	uint64_t		 required_features;
	uint64_t		 observed_features;
	uint16_t		 kind;
	uint16_t		 minor;
	uint32_t		 records_left;
};

struct restart_record {
	uint16_t type;
	uint16_t flags;
	struct ibuf payload;
};

struct restart_bytes {
	size_t	 size;
	u_char	*data;
};

struct restart_string_list {
	size_t	 count;
	char  **items;
};

struct restart_cell {
	u_char	 data[UTF8_SIZE];
	uint8_t size;
	uint8_t width;
	uint8_t flags;
	uint16_t attr;
	int32_t	 fg;
	int32_t	 bg;
	int32_t	 us;
	uint32_t link;
};

struct restart_line {
	uint16_t	 flags;
	uint32_t	 time;
	uint16_t	 prompt_col;
	uint16_t	 command_col;
	uint16_t	 output_start_col;
	uint16_t	 output_end_col;
	uint8_t		 exit_status;
	uint16_t	 cellused;
	uint16_t	 cellsize;
	struct restart_cell *cells;
};

struct restart_grid {
	uint32_t sx, sy, hsize, hlimit, hscrolled;
	uint32_t flags;
	uint32_t scroll_added, scroll_collected, scroll_generation;
	size_t line_count;
	struct restart_line *lines;
};

struct restart_link {
	uint32_t inner;
	char	*internal_id;
	char	*external_id;
	char	*uri;
};

struct restart_links {
	uint32_t next_inner;
	size_t count;
	struct restart_link *items;
};

#ifdef ENABLE_SIXEL
struct restart_image {
	uint32_t px, py, sx, sy, xpixel, ypixel, psx, psy;
	struct restart_bytes dcs;
};

struct restart_image_list {
	size_t count;
	struct restart_image *items;
};
#endif

struct restart_saved {
	uint32_t cx, cy;
	struct restart_cell cell;
	uint32_t grid_flags;
};

struct restart_screen {
	char	*title;
	char	*path;
	struct restart_string_list titles;
	struct restart_grid grid;
	uint32_t cx, cy;
	uint8_t cstyle, default_cstyle;
	int32_t ccolour, default_ccolour;
	uint32_t rupper, rlower;
	uint32_t mode, default_mode;
	struct restart_saved saved;
	int have_saved_grid;
	struct restart_grid saved_grid;
	struct restart_bytes tabs;
	struct restart_links links;
#ifdef ENABLE_SIXEL
	struct restart_image_list images;
	struct restart_image_list saved_images;
#endif
	uint8_t progress_state;
	int32_t progress;
};

struct restart_parser_cell {
	struct restart_cell cell;
	uint8_t set, g0set, g1set;
};

struct restart_utf8 {
	uint8_t have, size, width;
	u_char data[UTF8_SIZE];
};

struct restart_parser {
	char	*state;
	struct restart_parser_cell cell, old_cell;
	uint32_t old_cx, old_cy, old_mode;
	struct restart_bytes intermediate;
	struct restart_bytes parameter;
	struct restart_bytes input;
	uint8_t input_end;
	struct restart_utf8 utf8;
	uint8_t utf8_started;
	struct restart_utf8 last_utf8;
	uint32_t flags;
};

struct restart_palette_entry {
	uint16_t index;
	int32_t value;
};

struct restart_palette {
	int32_t fg, bg;
	size_t count;
	struct restart_palette_entry *items;
};

struct restart_terminal {
	uint64_t features;
	struct restart_palette palette;
	struct restart_screen screen;
	struct restart_parser parser;
	struct restart_bytes pty_input;
	struct restart_bytes pty_output;
};

void printflike(2, 3) restart_set_cause(char **, const char *, ...);
int restart_size_add(size_t, size_t, size_t *);
int restart_size_mul(size_t, size_t, size_t *);
int restart_budget_charge(struct restart_budget *, size_t, char **);
void *restart_alloc(struct restart_budget *, size_t, char **);
void *restart_calloc(struct restart_budget *, size_t, size_t, char **);
void *restart_grow(struct restart_budget *, void *, size_t, size_t, size_t,
    char **);

struct restart_writer *restart_writer_create(char **);
void restart_writer_discard(struct restart_writer *);
int restart_writer_detach(struct restart_writer *, struct ibuf **, char **);
int restart_write_envelope_begin(struct restart_writer *, uint16_t, char **);
int restart_write_envelope_end(struct restart_writer *, char **);
int restart_write_record(struct restart_writer *, struct restart_write_scope *,
    uint16_t, uint16_t, const void *, size_t, char **);
int restart_write_record_begin(struct restart_writer *,
    struct restart_write_scope *, uint16_t, uint16_t,
    struct restart_write_scope *, char **);
int restart_write_record_end(struct restart_writer *,
    struct restart_write_scope *, char **);
int restart_write_container_begin(struct restart_writer *,
    struct restart_write_scope *, uint16_t, uint16_t,
    struct restart_write_scope *, char **);
int restart_write_container_end(struct restart_writer *,
    struct restart_write_scope *, char **);
int restart_write_u8(struct restart_writer *, struct restart_write_scope *,
    uint16_t, uint8_t, char **);
int restart_write_u16(struct restart_writer *, struct restart_write_scope *,
    uint16_t, uint16_t, char **);
int restart_write_u32(struct restart_writer *, struct restart_write_scope *,
    uint16_t, uint32_t, char **);
int restart_write_u64(struct restart_writer *, struct restart_write_scope *,
    uint16_t, uint64_t, char **);
int restart_write_s32(struct restart_writer *, struct restart_write_scope *,
    uint16_t, int32_t, char **);
int restart_write_s64(struct restart_writer *, struct restart_write_scope *,
    uint16_t, int64_t, char **);
int restart_write_string(struct restart_writer *, struct restart_write_scope *,
    uint16_t, const char *, const char *, char **);
int restart_write_blob(struct restart_writer *, struct restart_write_scope *,
    uint16_t, const void *, size_t, const char *, char **);
int restart_write_stream(struct restart_writer *, struct restart_write_scope *,
    uint16_t, const void *, size_t, const char *, char **);

int restart_reader_open_envelope(struct restart_reader *, const void *, size_t,
    uint16_t, struct restart_budget *, char **);
int restart_reader_next(struct restart_reader *, struct restart_record *,
    char **);
int restart_reader_finish_envelope(struct restart_reader *, char **);
int restart_reader_open_container(struct restart_record *,
    struct restart_reader *, struct restart_budget *, char **);
int restart_reader_finish_container(struct restart_reader *, char **);
int restart_read_u8(struct restart_record *, uint8_t *, char **);
int restart_read_u16(struct restart_record *, uint16_t *, char **);
int restart_read_u32(struct restart_record *, uint32_t *, char **);
int restart_read_u64(struct restart_record *, uint64_t *, char **);
int restart_read_s32(struct restart_record *, int32_t *, char **);
int restart_read_s64(struct restart_record *, int64_t *, char **);
int restart_read_string(struct restart_record *, char **,
    struct restart_budget *, char **);
int restart_read_blob(struct restart_record *, struct restart_bytes *,
    size_t, struct restart_budget *, char **);
int restart_read_stream(struct restart_record *, struct restart_bytes *,
    struct restart_budget *, char **);

int restart_terminal_write_nested(struct restart_writer *,
    const struct window_pane *, char **);
int restart_terminal_write_decoded_nested(struct restart_writer *,
    const struct restart_terminal *, char **);
int restart_terminal_read_nested(struct restart_reader *,
    struct restart_terminal **, char **);
int restart_terminal_validate_nested(const struct restart_terminal *, size_t,
    char **);

int restart_cell_capture(const struct grid_cell *, struct restart_cell *,
    char **);
int restart_cell_build(const struct restart_cell *, struct grid_cell *,
    char **);

#endif
