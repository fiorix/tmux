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

#ifndef TMUX_RESTART_STATE_PRIVATE_H
#define TMUX_RESTART_STATE_PRIVATE_H

#include "restart-codec-private.h"

#define RESTART_KIND_SERVER 2
#define RESTART_MAX_LAYOUT_DEPTH 1000U

enum restart_server_record {
	RESTART_SERVER_META = 1,
	RESTART_SERVER_GLOBAL_ENVIRONMENT,
	RESTART_SERVER_GLOBAL_OPTIONS,
	RESTART_SERVER_GLOBAL_SESSION_OPTIONS,
	RESTART_SERVER_GLOBAL_WINDOW_OPTIONS,
	RESTART_SERVER_GROUPS,
	RESTART_SERVER_SESSIONS,
	RESTART_SERVER_WINDOWS,
	RESTART_SERVER_PANES,
	RESTART_SERVER_DESCRIPTOR_KEYS,
	RESTART_SERVER_BUFFERS
};

enum restart_buffer_record {
	RESTART_BUFFER_NAME = 1,
	RESTART_BUFFER_DATA,
	RESTART_BUFFER_CREATED,
	RESTART_BUFFER_AUTOMATIC,
	RESTART_BUFFER_ORDER
};

enum restart_meta_record {
	RESTART_META_START_TIME = 1,
	RESTART_META_NEXT_SESSION_ID,
	RESTART_META_NEXT_WINDOW_ID,
	RESTART_META_NEXT_PANE_ID,
	RESTART_META_NEXT_ACTIVE_POINT,
	RESTART_META_NEXT_HYPERLINK_EXTERNAL_ID
};

enum restart_env_entry_record {
	RESTART_ENV_NAME = 1,
	RESTART_ENV_STATE,
	RESTART_ENV_FLAGS,
	RESTART_ENV_VALUE
};

enum restart_option_entry_record {
	RESTART_OPTION_NAME = 1,
	RESTART_OPTION_TYPE,
	RESTART_OPTION_ARRAY,
	RESTART_OPTION_STRING_VALUE,
	RESTART_OPTION_NUMBER_VALUE,
	RESTART_OPTION_ITEMS
};

enum restart_option_item_record {
	RESTART_ITEM_KEY = 1,
	RESTART_ITEM_STRING_VALUE,
	RESTART_ITEM_NUMBER_VALUE
};

enum restart_option_type {
	RESTART_OPTION_USER = 1,
	RESTART_OPTION_STRING,
	RESTART_OPTION_NUMBER,
	RESTART_OPTION_KEY,
	RESTART_OPTION_COLOUR,
	RESTART_OPTION_FLAG,
	RESTART_OPTION_CHOICE,
	RESTART_OPTION_COMMAND
};

enum restart_group_record {
	RESTART_GROUP_NAME = 1,
	RESTART_GROUP_MEMBERS
};

enum restart_session_record {
	RESTART_SESSION_ID = 1,
	RESTART_SESSION_NAME,
	RESTART_SESSION_CWD,
	RESTART_SESSION_TIMES,
	RESTART_SESSION_CURRENT_INDEX,
	RESTART_SESSION_LAST_INDICES,
	RESTART_SESSION_WINLINKS,
	RESTART_SESSION_OPTIONS,
	RESTART_SESSION_ENVIRONMENT,
	RESTART_SESSION_TERMIOS_CC
};

enum restart_session_times_record {
	RESTART_SESSION_TIME_CREATION = 1,
	RESTART_SESSION_TIME_LAST_ATTACHED,
	RESTART_SESSION_TIME_ACTIVITY,
	RESTART_SESSION_TIME_LAST_ACTIVITY
};

enum restart_winlink_record {
	RESTART_WINLINK_INDEX = 1,
	RESTART_WINLINK_WINDOW_ID,
	RESTART_WINLINK_ALERT_FLAGS
};

enum restart_window_record {
	RESTART_WINDOW_ID = 1,
	RESTART_WINDOW_NAME,
	RESTART_WINDOW_TIMES,
	RESTART_WINDOW_ACTIVE_PANE,
	RESTART_WINDOW_MODAL_PANE,
	RESTART_WINDOW_MODAL_LAST,
	RESTART_WINDOW_PANE_ORDER,
	RESTART_WINDOW_LAST_PANES,
	RESTART_WINDOW_Z_ORDER,
	RESTART_WINDOW_LAST_LAYOUT,
	RESTART_WINDOW_LAYOUT,
	RESTART_WINDOW_VISIBLE_LAYOUT,
	RESTART_WINDOW_OLD_LAYOUT,
	RESTART_WINDOW_SIZE,
	RESTART_WINDOW_LAST_NEW_POSITION,
	RESTART_WINDOW_FLAGS,
	RESTART_WINDOW_OPTIONS
};

enum restart_window_times_record {
	RESTART_WINDOW_TIME_NAME = 1,
	RESTART_WINDOW_TIME_ACTIVITY,
	RESTART_WINDOW_TIME_CREATION
};

enum restart_pane_record {
	RESTART_PANE_ID = 1,
	RESTART_PANE_WINDOW_ID,
	RESTART_PANE_ACTIVE_POINT,
	RESTART_PANE_LIFECYCLE,
	RESTART_PANE_FLAGS,
	RESTART_PANE_GEOMETRY,
	RESTART_PANE_COMMAND,
	RESTART_PANE_PROCESS,
	RESTART_PANE_ACTIVITY,
	RESTART_PANE_OPTIONS,
	RESTART_PANE_TERMINAL
};

enum restart_command_record {
	RESTART_COMMAND_ARGV = 1,
	RESTART_COMMAND_SHELL,
	RESTART_COMMAND_CWD
};

enum restart_process_record {
	RESTART_PROCESS_PID = 1,
	RESTART_PROCESS_TTY,
	RESTART_PROCESS_DEAD_RESULT,
	RESTART_PROCESS_DEAD_TIME
};

enum restart_dead_result_record {
	RESTART_DEAD_KIND = 1,
	RESTART_DEAD_VALUE
};

enum restart_activity_record {
	RESTART_ACTIVITY_OUTPUT_GENERATION = 1,
	RESTART_ACTIVITY_LAST_OUTPUT,
	RESTART_ACTIVITY_LAST_PROMPT,
	RESTART_ACTIVITY_COMMAND_START,
	RESTART_ACTIVITY_COMMAND_END,
	RESTART_ACTIVITY_COMMAND_STATUS,
	RESTART_ACTIVITY_LAST_THEME
};

enum restart_lifecycle {
	RESTART_LIFECYCLE_LIVE_FD = 1,
	RESTART_LIFECYCLE_EMPTY,
	RESTART_LIFECYCLE_DEAD_KNOWN,
	RESTART_LIFECYCLE_DEAD_UNKNOWN,
	RESTART_LIFECYCLE_DRAINING_KNOWN,
	RESTART_LIFECYCLE_DRAINING_UNKNOWN,
	RESTART_LIFECYCLE_EXITED_UNKNOWN,
	RESTART_LIFECYCLE_INACTIVE
};

enum restart_dead_kind {
	RESTART_DEAD_EXITED = 1,
	RESTART_DEAD_SIGNALED = 2
};

enum restart_layout_type {
	RESTART_LAYOUT_HORIZONTAL = 1,
	RESTART_LAYOUT_VERTICAL = 2,
	RESTART_LAYOUT_PANE = 3
};

enum restart_layout_record {
	RESTART_LAYOUT_TYPE = 1,
	RESTART_LAYOUT_SX,
	RESTART_LAYOUT_SY,
	RESTART_LAYOUT_XOFF,
	RESTART_LAYOUT_YOFF,
	RESTART_LAYOUT_PANE_ID,
	RESTART_LAYOUT_PANE_INDEX,
	RESTART_LAYOUT_LAST_INDEX,
	RESTART_LAYOUT_Z_INDEX,
	RESTART_LAYOUT_ACTIVE,
	RESTART_LAYOUT_FLOATING,
	RESTART_LAYOUT_FG_SX,
	RESTART_LAYOUT_FG_SY,
	RESTART_LAYOUT_FG_XOFF,
	RESTART_LAYOUT_FG_YOFF,
	RESTART_LAYOUT_CHILDREN
};

#define RESTART_WINLINK_ALERTS 0x07
#define RESTART_WINDOW_ZOOMED 0x01
#define RESTART_PANE_WIRE_FLAGS 0x007f
#define RESTART_PANE_WIRE_CMDRUNNING 0x0004
#define RESTART_ENVIRON_HIDDEN 0x01

#define RESTART_ENV_STATE_SET 1
#define RESTART_ENV_STATE_CLEARED 2

struct restart_timeval {
	int64_t		 sec;
	uint32_t	 usec;
};

struct restart_id_list {
	size_t		 count;
	uint32_t	*items;
};

struct restart_index_list {
	size_t		 count;
	int32_t		*items;
};

struct restart_environment_entry {
	char		*name;
	char		*value;
	uint8_t		 state;
	uint8_t		 flags;
};

struct restart_environment {
	size_t		 count;
	struct restart_environment_entry *entries;
};

struct restart_option_item {
	char		*key;
	char		*string;
	int64_t		 number;
};

struct restart_option {
	char		*name;
	uint8_t		 type;
	uint8_t		 is_array;
	char		*string;
	int64_t		 number;
	size_t		 item_count;
	struct restart_option_item *items;
};

struct restart_options {
	uint8_t		 scope;
	size_t		 count;
	struct restart_option *entries;
};

struct restart_termios_cc {
	uint32_t	 present;
	uint8_t		 value[18];
};

struct restart_winlink {
	int32_t		 index;
	uint32_t	 window_id;
	uint8_t		 flags;
};

struct restart_session {
	uint32_t	 id;
	char		*name;
	char		*cwd;
	struct restart_timeval creation_time;
	struct restart_timeval last_attached_time;
	struct restart_timeval activity_time;
	struct restart_timeval last_activity_time;
	int32_t		 current_index;
	struct restart_index_list last_indices;
	size_t		 winlink_count;
	struct restart_winlink *winlinks;
	struct restart_options options;
	struct restart_environment environment;
	uint8_t		 have_termios;
	struct restart_termios_cc termios_cc;
};

struct restart_group {
	char		*name;
	struct restart_id_list members;
};

struct restart_layout_cell {
	uint8_t		 type;
	uint32_t	 sx;
	uint32_t	 sy;
	int32_t		 xoff;
	int32_t		 yoff;
	uint32_t	 pane_id;
	uint32_t	 pane_index;
	uint32_t	 last_index;
	uint32_t	 z_index;
	uint8_t		 active;
	uint8_t		 have_last;
	uint8_t		 have_z;
	uint8_t		 floating;
	uint8_t		 have_fg;
	uint32_t	 fg_sx;
	uint32_t	 fg_sy;
	int32_t		 fg_xoff;
	int32_t		 fg_yoff;
	size_t		 child_count;
	struct restart_layout_cell **children;
};

struct restart_layout {
	struct restart_layout_cell *root;
	size_t		 cell_count;
	size_t		 leaf_count;
};

struct restart_window {
	uint32_t	 id;
	char		*name;
	struct restart_timeval name_time;
	struct restart_timeval activity_time;
	struct restart_timeval creation_time;
	uint32_t	 active_pane_id;
	uint32_t	 modal_pane_id;
	uint32_t	 modal_last_id;
	uint8_t		 have_modal;
	uint8_t		 have_modal_last;
	struct restart_id_list pane_order;
	struct restart_id_list last_panes;
	struct restart_id_list z_order;
	int32_t		 last_layout;
	struct restart_layout *layout;
	struct restart_layout *visible_layout;
	char		*old_layout;
	uint8_t		 have_old_layout;
	uint32_t	 sx, sy, manual_sx, manual_sy, xpixel, ypixel;
	uint32_t	 last_new_x, last_new_y;
	uint8_t		 flags;
	struct restart_options options;
};

struct restart_pane {
	uint32_t	 id;
	uint32_t	 window_id;
	uint32_t	 active_point;
	uint8_t		 lifecycle;
	uint16_t	 flags;
	uint32_t	 sx, sy;
	int32_t		 xoff, yoff;
	struct restart_string_list argv;
	char		*shell;
	char		*cwd;
	uint8_t		 have_shell;
	uint8_t		 have_cwd;
	int64_t		 pid;
	char		*tty;
	uint8_t		 have_result;
	uint8_t		 dead_kind;
	uint32_t	 dead_value;
	uint8_t		 have_dead_time;
	struct restart_timeval dead_time;
	uint64_t	 output_generation;
	int64_t		 last_output;
	int64_t		 last_prompt;
	int64_t		 command_start;
	int64_t		 command_end;
	int32_t		 command_status;
	uint8_t		 last_theme;
	struct restart_options options;
	struct restart_terminal *terminal;
};

struct restart_buffer {
	char		*name;
	struct restart_bytes data;
	int64_t		 created;
	uint32_t	 order;
	uint8_t		 automatic;
};

struct restart_descriptor_key {
	uint32_t	 pane_id;
	int64_t		 pid;
};

struct restart_state {
	uint64_t	 features;
	struct restart_timeval start_time;
	uint32_t	 next_session_id;
	uint32_t	 next_window_id;
	uint32_t	 next_pane_id;
	uint32_t	 next_active_point;
	uint64_t	 next_hyperlink_external_id;
	struct restart_environment global_environment;
	struct restart_options global_options;
	struct restart_options global_session_options;
	struct restart_options global_window_options;
	size_t		 group_count;
	struct restart_group *groups;
	size_t		 session_count;
	struct restart_session *sessions;
	size_t		 window_count;
	struct restart_window *windows;
	size_t		 pane_count;
	struct restart_pane *panes;
	size_t		 descriptor_count;
	struct restart_descriptor_key *descriptors;
	size_t		 buffer_count;
	struct restart_buffer *buffers;
};

/*
 * Live and wire flag spaces are related by a table rather than a range,
 * because the two constants only happen to agree for some bits. The wire
 * field is wider than any destination it feeds, so the destination's capacity
 * is checked at conversion, where it is known.
 */
struct restart_flag_map {
	u_int	wire;
	int	live;
};

extern const struct restart_flag_map	restart_pane_flag_map[];
extern const struct restart_flag_map	restart_window_flag_map[];
extern const struct restart_flag_map	restart_winlink_flag_map[];
extern const struct restart_flag_map	restart_environ_flag_map[];

#endif
