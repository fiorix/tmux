#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
[ -x "$TEST_TMUX" ] || exit 1

TMPDIR=$(mktemp -d) || exit 1
HOME="$TMPDIR/home"
mkdir -p "$HOME" || exit 1
printf '%s\n' 'set -g @restart-config-loaded yes' >"$HOME/.tmux.conf" ||
	exit 1
export HOME
TMUX="$TEST_TMUX -S$TMPDIR/tmux.sock"
BEFORE="$TMPDIR/before"
AFTER="$TMPDIR/after"
PANES=21

cleanup()
{
	$TMUX kill-server 2>/dev/null
	rm -rf "$TMPDIR"
}

fail()
{
	echo "$1" >&2
	bad=1
}

unset TMUX_PANE TMUX_TMPDIR
trap cleanup 0 1 15
bad=0

SESS_FMT='#{session_id} #{session_name} #{session_windows}'
WIN_FMT='#{session_id} #{window_id} #{window_index} #{window_name}'
WIN_FMT="$WIN_FMT #{window_layout}"
PANE_FMT='#{session_id} #{window_id} #{pane_id} #{pane_pid} #{pane_index}'
PANE_FMT="$PANE_FMT #{pane_width}x#{pane_height} #{pane_title}"
PANE_FMT="$PANE_FMT #{history_size} #{pane_floating_flag}"

snapshot()
{
	out=$1
	mkdir -p "$out" || exit 1

	$TMUX list-clients -F '#{client_tty}' >"$out/clients" || exit 1
	$TMUX list-sessions -F "$SESS_FMT" >"$out/sessions" || exit 1
	$TMUX list-windows -a -F "$WIN_FMT" >"$out/windows" || exit 1
	$TMUX list-panes -a -F "$PANE_FMT" >"$out/panes" || exit 1
	$TMUX show-options -g >"$out/options" || exit 1
	$TMUX show-options -g -w >"$out/window-options" || exit 1
	$TMUX show-environment -g >"$out/environment" || exit 1
	$TMUX list-buffers >"$out/buffers" || exit 1

	# Content is the dimension the graph cannot stand in for: identifiers
	# and layouts can all match while the grid is empty. Captured with
	# escape sequences and full history, so attributes and scrollback are
	# in scope and not only the visible region.
	: >"$out/content"
	$TMUX list-panes -a -F '#{pane_id}' | while read -r p; do
		printf '=== %s\n' "$p" >>"$out/content"
		$TMUX capture-pane -p -e -S - -t "$p" >>"$out/content"
	done
}

wait_for_server()
{
	i=0
	while ! $TMUX display-message -p '#{pid}' >/dev/null 2>&1; do
		[ "$i" -eq 50 ] && return 1
		i=$((i + 1))
		sleep 0.1
	done
	sleep 1
	return 0
}

$TMUX new-session -d -s one -x80 -y24 'cat'
$TMUX new-window -t one -n second 'cat'
$TMUX split-window -t one:second 'cat'
$TMUX split-window -h -t one:second 'cat'
$TMUX new-session -d -s two -x80 -y24 'cat'

# Every pane hands a descriptor to the replacement and has its layout
# re-encoded, so one window is filled well past the two or three panes a
# restart is usually tried with.
$TMUX new-window -t one -n many 'cat'
i=1
while [ "$i" -le 15 ]; do
	$TMUX split-window -t one:many 'cat'
	$TMUX select-layout -t one:many tiled
	i=$((i + 1))
done

$TMUX set -g history-limit 4321
$TMUX set -g status-left 'restart-fixture'
$TMUX set -g @user-option 'kept across the restart'
$TMUX set -g -w main-pane-width 37
$TMUX setenv -g RESTART_FIXTURE 'one two three'
$TMUX set-buffer -b fixture 'buffer contents'
$TMUX select-layout -t one:second tiled
$TMUX select-pane -t one:second.1 -T 'pane title'
[ "$($TMUX show-options -gv @restart-config-loaded)" = yes ] ||
	fail "configuration fixture was not loaded"

FIFO="$TMPDIR/fifo"
mkfifo "$FIFO" || exit 1
TERM=screen setsid sh -c \
    "exec script -qfc '$TMUX attach -t one' $TMPDIR/attach >/dev/null 2>&1 \
    <$FIFO" &
exec 9>"$FIFO"

# Scrollback with attributes, so the comparison covers the grid and not only
# its dimensions. Each pane runs cat, so what is sent is what is echoed.
i=1
while [ "$i" -le 40 ]; do
	$TMUX send-keys -t one:0.0 \
	    "line-$i $(printf '\033[1;31mbold red\033[0m')" Enter
	i=$((i + 1))
done
$TMUX send-keys -t one:second.0 'second window content' Enter
sleep 2

# An assertion about releasing clients is vacuous if none ever attached.
[ "$($TMUX list-clients -F '#{client_tty}' | wc -l)" -eq 1 ] ||
	fail "fixture did not attach a client"

snapshot "$BEFORE"

# The fixture has to be the shape the comparison assumes, or every check below
# passes by comparing nothing against nothing. Two empty files compare equal, so
# each dimension is required to have something in it before it is trusted to
# report agreement.
[ "$(wc -l <"$BEFORE/panes")" -eq "$PANES" ] ||
	fail "fixture built $(wc -l <"$BEFORE/panes") panes, expected $PANES"
for d in sessions windows panes options window-options environment buffers \
    content; do
	[ -s "$BEFORE/$d" ] || fail "fixture captured no $d to compare"
done

$TMUX restart-server || fail "restart-server failed"
wait_for_server || fail "no server after the restart"

snapshot "$AFTER"

# This is the only observable that shows a restart happened. The process id is
# unchanged because the replacement keeps the process, and start_time is
# restored from the encoded state, so neither can signal anything. Releasing
# the clients is reachable only through the commit path, which runs after the
# state has been encoded and verified.
[ -s "$BEFORE/clients" ] || fail "no client was attached before the restart"
[ -s "$AFTER/clients" ] && fail "clients were not released by the restart"

# A replacement starts with an empty graph, so whatever matches below was
# rebuilt from the encoded state rather than merely left alone.
for d in sessions windows panes options window-options environment buffers \
    content; do
	cmp -s "$BEFORE/$d" "$AFTER/$d" || {
		echo "$d differs across the restart" >&2
		diff -u "$BEFORE/$d" "$AFTER/$d" | head -20 >&2
		bad=1
	}
done

# The pane processes are the same ones, which is what separates a preserved
# server from one rebuilt to look the same. The panes comparison above covers
# it through pane_pid; asserted again alone so that a change to PANE_FMT
# cannot quietly drop the only check that matters most.
awk '{print $4}' "$BEFORE/panes" >"$TMPDIR/pids-before"
awk '{print $4}' "$AFTER/panes" >"$TMPDIR/pids-after"
cmp -s "$TMPDIR/pids-before" "$TMPDIR/pids-after" ||
	fail "pane processes changed across the restart"

while read -r p; do
	kill -0 "$p" 2>/dev/null || fail "pane process $p is gone"
done <"$TMPDIR/pids-before"

# A bare program name in argv[0] survives the first restart and fails the
# second, because the first replacement is executed by path and the second by
# whatever name the first was handed.
$TMUX restart-server || fail "the second restart-server failed"
wait_for_server || fail "no server after the second restart"

snapshot "$TMPDIR/after2"
for d in sessions windows panes content; do
	cmp -s "$BEFORE/$d" "$TMPDIR/after2/$d" ||
		fail "$d differs across the second restart"
done

# The restored server is usable, not just readable.
$TMUX new-window -d -t one || fail "cannot create a window after the restart"
$TMUX has-session -t two || fail "session two is gone after the restart"

exit "$bad"
