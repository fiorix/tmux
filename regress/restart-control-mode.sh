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

$TMUX -f/dev/null new-session -d -s control -x80 -y24 'cat' || exit 1

# A control mode client has no terminal, so it reaches none of the pty paths
# that release an attached client and has to be let go some other way.
FIFO="$TMPDIR/fifo"
mkfifo "$FIFO" || exit 1
$TMUX -C attach -t control <"$FIFO" >"$TMPDIR/control" 2>&1 &
client=$!
exec 9>"$FIFO"

i=0
while [ "$($TMUX list-clients -F '#{client_control_mode}')" != "1" ]; do
	[ "$i" -eq 50 ] && break
	i=$((i + 1))
	sleep 0.1
done
[ "$($TMUX list-clients -F '#{client_control_mode}')" = "1" ] ||
	fail "control mode client did not attach"

before=$($TMUX list-panes -a -F '#{pane_id} #{pane_pid}')

$TMUX restart-server || fail "restart-server failed"

i=0
while ! $TMUX display-message -p '#{pid}' >/dev/null 2>&1; do
	[ "$i" -eq 50 ] && break
	i=$((i + 1))
	sleep 0.1
done
$TMUX display-message -p '#{pid}' >/dev/null 2>&1 ||
	fail "no server after the restart"

# %exit is how control mode says the client is finished, so a client left to
# discover the loss by reading a closed socket would show up as a missing one.
i=0
while ! grep -q '^%exit' "$TMPDIR/control" 2>/dev/null; do
	[ "$i" -eq 50 ] && break
	i=$((i + 1))
	sleep 0.1
done
[ "$($TMUX list-clients | wc -l)" -eq 0 ] ||
	fail "the control mode client was not released"

if grep -q '^%exit' "$TMPDIR/control" 2>/dev/null; then
	wait "$client"
	[ $? -eq 0 ] || fail "control mode client exited with an error"
else
	fail "control mode client was not told the server was going"
	kill "$client" 2>/dev/null
	wait "$client" 2>/dev/null
fi
exec 9>&-

after=$($TMUX list-panes -a -F '#{pane_id} #{pane_pid}')
[ "$before" = "$after" ] ||
	fail "panes changed across the restart, '$before' to '$after'"

# The replacement has to be able to take a control mode client of its own, not
# merely survive losing one.
FIFO2="$TMPDIR/fifo2"
mkfifo "$FIFO2" || exit 1
$TMUX -C attach -t control <"$FIFO2" >"$TMPDIR/control2" 2>&1 &
client2=$!
exec 8>"$FIFO2"

i=0
while [ "$($TMUX list-clients -F '#{client_control_mode}')" != "1" ]; do
	[ "$i" -eq 50 ] && break
	i=$((i + 1))
	sleep 0.1
done
[ "$($TMUX list-clients -F '#{client_control_mode}')" = "1" ] ||
	fail "a control mode client cannot attach after the restart"

kill "$client2" 2>/dev/null
wait "$client2" 2>/dev/null
exec 8>&-

exit "$bad"
