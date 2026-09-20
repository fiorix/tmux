#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
[ -x "$TEST_TMUX" ] || exit 1

TMPDIR=$(mktemp -d) || exit 1
TMUX="$TEST_TMUX -S$TMPDIR/tmux.sock"

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

$TMUX -f/dev/null new-session -d -s copy -x80 -y24 'cat' || exit 1

i=1
while [ "$i" -le 30 ]; do
	$TMUX send-keys -t copy:0.0 "line-$i" Enter
	i=$((i + 1))
done
sleep 1

before_pane=$($TMUX display-message -p -t copy:0.0 '#{pane_id} #{pane_pid}')
before_content=$($TMUX capture-pane -p -S - -t copy:0.0)

$TMUX copy-mode -t copy:0.0 || fail "cannot enter copy mode"
$TMUX send-keys -t copy:0.0 -X cursor-up
$TMUX send-keys -t copy:0.0 -X begin-selection
$TMUX send-keys -t copy:0.0 -X cursor-up
[ "$($TMUX display-message -p -t copy:0.0 '#{pane_in_mode}')" = "1" ] ||
	fail "the fixture pane is not in copy mode"

$TMUX restart-server || fail "restart-server failed"

i=0
while ! $TMUX display-message -p '#{pid}' >/dev/null 2>&1; do
	[ "$i" -eq 50 ] && break
	i=$((i + 1))
	sleep 0.1
done
$TMUX display-message -p '#{pid}' >/dev/null 2>&1 ||
	fail "no server after the restart"

# A mode is a view built over the pane rather than part of it, and it is not
# carried across. The pane comes back on its own screen, holding the history
# the mode was looking at.
[ "$($TMUX display-message -p -t copy:0.0 '#{pane_in_mode}')" = "0" ] ||
	fail "the pane is still in a mode after the restart"
[ -z "$($TMUX display-message -p -t copy:0.0 '#{pane_mode}')" ] ||
	fail "the pane reports a mode after the restart"

after_pane=$($TMUX display-message -p -t copy:0.0 '#{pane_id} #{pane_pid}')
[ "$before_pane" = "$after_pane" ] ||
	fail "pane changed across the restart, '$before_pane' to '$after_pane'"

after_content=$($TMUX capture-pane -p -S - -t copy:0.0)
[ "$before_content" = "$after_content" ] ||
	fail "history the mode was reading differs across the restart"

# Losing the mode is only acceptable if the pane can be put back into one.
$TMUX copy-mode -t copy:0.0 || fail "cannot enter copy mode after the restart"
[ "$($TMUX display-message -p -t copy:0.0 '#{pane_in_mode}')" = "1" ] ||
	fail "copy mode did not take after the restart"

exit "$bad"
