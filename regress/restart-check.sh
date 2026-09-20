#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
[ -x "$TEST_TMUX" ] || exit 1

ROOT=$(mktemp -d) || exit 1
SOCKET="$ROOT/socket"

# The server restarts into the path it was started from, so starting it from
# IMAGE lets the test put a different program there. Each one is moved into
# place rather than copied over, because the running server has the old file
# open for execution and writing to it would fail.
IMAGE="$ROOT/tmux"
TMUX="$TEST_TMUX -S$SOCKET"

cleanup()
{
	$TMUX kill-server >/dev/null 2>&1
	rm -rf "$ROOT"
}

fail()
{
	echo "$1" >&2
	exit 1
}

# Put a script at IMAGE. When it is run as the check it does what it is given
# first. Otherwise it is the replacement, and leaves STARTED behind to say so,
# because nothing else can: the pid and the panes are the same whether the
# server was replaced or carried on. Either way it runs the real tmux under the
# name IMAGE, so the server still restarts into IMAGE.
STARTED="$ROOT/started"
put_script()
{
	cat >"$ROOT/next" <<EOF || exit 1
#!/bin/sh
if [ -n "\$TMUX_RESTART_CHECK" ]; then
	$1
else
	: >"$STARTED"
fi
exec perl -e '\$p = shift; exec {\$p} @ARGV or die "exec: \$!"' \\
    "$TEST_TMUX" "\$0" "\$@"
EOF
	chmod a+x "$ROOT/next" || exit 1
	mv "$ROOT/next" "$IMAGE" || exit 1
}

wait_for_server()
{
	i=0
	while ! $TMUX display-message -p '#{pid}' >/dev/null 2>&1; do
		[ "$i" -eq 50 ] && fail "no server $1"
		i=$((i + 1))
		sleep 0.1
	done
}

# The window running cat proves the panes are read and written again, not only
# that they still exist.
check_panes()
{
	[ "$($TMUX display-message -p '#{pid}')" = "$before_pid" ] ||
		fail "server pid changed $1"
	[ "$($TMUX list-panes -a -F '#{pane_id} #{pane_pid} #{pane_dead}')" = \
	    "$before_panes" ] || fail "panes changed $1"

	$TMUX send-keys -t:cat "$2" Enter || fail "cannot send keys $1"
	i=0
	while [ "$($TMUX capture-pane -pt:cat | grep -c "^$2\$")" -ne 2 ]; do
		[ "$i" -eq 50 ] && fail "pane does not echo input $1"
		i=$((i + 1))
		sleep 0.1
	done
}

unset TMUX_PANE TMUX_TMPDIR
trap cleanup 0 1 15

cp "$TEST_TMUX" "$IMAGE" || exit 1
chmod a+x "$IMAGE" || exit 1

"$IMAGE" -S"$SOCKET" -f/dev/null new-session -d -s check 'sleep 60' ||
	fail "failed to start server"
$TMUX new-window -d -n cat cat || fail "failed to create window"

before_pid=$($TMUX display-message -p '#{pid}') ||
	fail "cannot read server pid"
before_panes=$($TMUX list-panes -a -F '#{pane_id} #{pane_pid} #{pane_dead}') ||
	fail "cannot read panes"

# The new image says it cannot restore the server. Nothing has been given away
# yet, so the restart is refused with its reason and the server is unchanged.
put_script 'echo "checkpoint refused by test" >&2; exit 1'

error=$($TMUX restart-server 2>&1) &&
	fail "restart-server succeeded when the check failed"
case "$error" in
*"checkpoint refused by test"*)
	;;
*)
	fail "restart-server did not give the reason from the check: $error"
	;;
esac
check_panes "after a failed check" one
[ -e "$STARTED" ] && fail "replacement started after a failed check"

# The new image passes the check and then cannot be executed. The clients have
# been released by then, but this server still has the panes and carries on
# with them. Every execute bit has to go: root is refused only when the file has
# none at all.
put_script 'chmod a-x "$0"'

$TMUX restart-server || fail "restart-server failed when the check passed"
wait_for_server "after a failed exec"
check_panes "after a failed exec" two
[ -e "$STARTED" ] && fail "replacement started after a failed exec"

# A server that has carried on can still restart.
put_script ':'

$TMUX restart-server || fail "restart-server failed after a failed exec"
wait_for_server "after a restart"
check_panes "after a restart" three
[ -e "$STARTED" ] || fail "replacement did not start"

exit 0
