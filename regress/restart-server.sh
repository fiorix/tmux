#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
[ -x "$TEST_TMUX" ] || exit 1

ROOT=$(mktemp -d) || exit 1
SOCKET="$ROOT/socket"

# The server resolves the image to restart into from the argv[0] it was given,
# so starting it from a copy lets the test break that copy and watch the command
# refuse. Breaking the installed binary is not an option. Commands are sent with
# the real binary, which stays executable, because a client is not the image the
# server restarts into.
IMAGE="$ROOT/tmux"
TMUX="$TEST_TMUX -S$SOCKET"

SOCKET2="$ROOT/socket2"
TMUX2="$TEST_TMUX -S$SOCKET2"

cleanup()
{
	$TMUX kill-server >/dev/null 2>&1
	$TMUX2 kill-server >/dev/null 2>&1
	rm -rf "$ROOT"
}

fail()
{
	echo "$1" >&2
	exit 1
}

unset TMUX_PANE TMUX_TMPDIR
trap cleanup 0 1 15

cp "$TEST_TMUX" "$IMAGE" || exit 1
chmod a+x "$IMAGE" || exit 1

"$IMAGE" -S"$SOCKET" -f/dev/null new-session -d -s restart 'sleep 60' ||
	fail "failed to start server"

[ "$($TMUX list-commands 2>/dev/null | grep -c '^restart-server')" -eq 1 ] ||
	fail "restart-server is not listed"

before_pid=$($TMUX display-message -p '#{pid}') ||
	fail "cannot read server pid"
before_pane=$($TMUX display-message -p '#{pane_id} #{pane_pid}') ||
	fail "cannot read pane identity"

# A replacement that cannot even start must be found before anything is given
# away. The refusal is the easy half; the point is that refusing leaves the
# server exactly as it was, because every rollback path runs after the panes
# and clients have already been stopped. Every execute bit has to go: root is
# refused only when the file has none at all.
chmod a-x "$IMAGE" || exit 1

error=$($TMUX restart-server 2>&1) &&
	fail "restart-server succeeded with an image that cannot start"
[ -n "$error" ] || fail "restart-server failed without reporting a reason"

after_pid=$($TMUX display-message -p '#{pid}') ||
	fail "server did not survive a refused restart: $error"
[ "$before_pid" = "$after_pid" ] ||
	fail "server pid changed from $before_pid to $after_pid"

after_pane=$($TMUX display-message -p '#{pane_id} #{pane_pid}') ||
	fail "cannot read pane identity after a refused restart"
[ "$before_pane" = "$after_pane" ] ||
	fail "pane changed from '$before_pane' to '$after_pane'"

# Quiesce stops pane and client reading, so a rollback that forgot to restore
# it would leave a server that answers this far and then never reads again.
$TMUX new-window -d ||
	fail "server does not accept commands after a refused restart"
[ "$($TMUX list-windows | wc -l)" -eq 2 ] ||
	fail "window was not created after a refused restart"

# And with a working image the same command succeeds, which is what makes the
# refusal above a decision rather than the only thing this command can do.
chmod a+x "$IMAGE" || exit 1

$TMUX restart-server || fail "restart-server failed with a working image"

i=0
while ! $TMUX display-message -p '#{pid}' >/dev/null 2>&1; do
	[ "$i" -eq 50 ] && fail "no server after a successful restart"
	i=$((i + 1))
	sleep 0.1
done

restored_pane=$($TMUX display-message -p '#{pane_id} #{pane_pid}') ||
	fail "cannot read pane identity after a restart"
[ "$before_pane" = "$restored_pane" ] ||
	fail "pane changed by the restart, '$before_pane' to '$restored_pane'"

[ "$($TMUX list-windows | wc -l)" -eq 2 ] ||
	fail "windows were not preserved across the restart"

# A server resolves its image once, at startup, from the argv[0] it was handed.
# Started under a bare name that no PATH entry resolves, it has nothing to
# restart into, and that refusal has to be as harmless as the one above. The
# server is given a PATH with nothing in it, because an installed tmux would
# otherwise resolve the name and the refusal would never happen, and its pane
# command is absolute because that PATH cannot find one either.
mkdir "$ROOT/nopath" || exit 1
perl -e '$ENV{PATH} = shift;
    exec {$ARGV[0]} "tmux", @ARGV[1..$#ARGV] or die "exec: $!"' \
    "$ROOT/nopath" "$IMAGE" -S"$SOCKET2" -f/dev/null new-session -d \
    '/bin/sleep 60' ||
	fail "failed to start a server under a bare name"

i=0
while ! $TMUX2 display-message -p '#{pid}' >/dev/null 2>&1; do
	[ "$i" -eq 50 ] && fail "server started under a bare name did not start"
	i=$((i + 1))
	sleep 0.1
done

named_pid=$($TMUX2 display-message -p '#{pid}') ||
	fail "cannot read the pid of the server started under a bare name"
named_pane=$($TMUX2 display-message -p '#{pane_id} #{pane_pid}') ||
	fail "cannot read the pane of the server started under a bare name"

error=$($TMUX2 restart-server 2>&1) &&
	fail "restart-server succeeded with no image to restart into"
# The cause is worth asserting here because the refusal distinguishes an
# unknown image from a server the transport never started. The refusal above
# stays a non-emptiness check because its message carries strerror, which
# differs between systems.
[ "$error" = "cannot find the server image to restart" ] ||
	fail "restart-server gave the wrong reason: $error"

[ "$($TMUX2 display-message -p '#{pid}')" = "$named_pid" ] ||
	fail "server pid changed when the image could not be found"
[ "$($TMUX2 display-message -p '#{pane_id} #{pane_pid}')" = "$named_pane" ] ||
	fail "pane changed when the image could not be found"
$TMUX2 new-window -d ||
	fail "server does not accept commands after a refused restart"

exit 0
