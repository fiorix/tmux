#!/bin/sh
# shellcheck disable=SC2317

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
[ -x "$TEST_TMUX" ] || exit 1

BUILD=$(dirname "$TEST_TMUX")
SERVICE="$BUILD/tmux@.service"
SOCKET=$(readlink -f ../tmux@.socket 2>/dev/null)
CONFIG_STATUS="$BUILD/config.status"
[ -f "$CONFIG_STATUS" ] || exit 1

# Every build generates the service file, so its presence says nothing about
# whether this build has anything to check. Only a build with systemd enabled
# installs the units.
grep -Fq 'S["HAVE_SYSTEMD_TRUE"]=""' "$CONFIG_STATUS" || {
	echo "SKIP: systemd is not enabled in this build"
	exit 0
}

[ -f "$SERVICE" ] || exit 1
[ -f "$SOCKET" ] || exit 1

fail()
{
	echo "$1" >&2
	exit 1
}

# The substituted path is the part a build can get wrong while still producing
# a syntactically valid unit. Whether that path exists is decided by make
# install and by --prefix rather than by the build, so only the substitution
# is checked here.
exec_line=$(grep '^ExecStart=' "$SERVICE") ||
	fail "generated service has no ExecStart"
exec_path=$(printf '%s\n' "$exec_line" | sed 's/^ExecStart=//; s/ .*//; s/^"//; s/"$//')
case $exec_path in
/*)
	;;
*)
	fail "ExecStart path is not absolute: $exec_path"
	;;
esac

# The server finds its socket path through $TMUX_TMPDIR like any other tmux,
# so the directory the service sets must be the one the socket listens in.
tmpdir=$(sed -n 's/^Environment=TMUX_TMPDIR=//p' "$SERVICE")
[ -n "$tmpdir" ] || fail "generated service does not set TMUX_TMPDIR"
listen=$(sed -n 's/^ListenStream=//p' "$SOCKET")
[ "$listen" = "$tmpdir/tmux-%U/%i" ] ||
	fail "socket listens on $listen, service expects $tmpdir/tmux-%U/%i"

# systemd-analyze is the only thing here that can judge unit syntax, so check
# it works before reading its silence as approval.
command -v systemd-analyze >/dev/null 2>&1 || exit 0
systemd-analyze --version >/dev/null 2>&1 || exit 0

ROOT=$(mktemp -d) || exit 1
trap 'rm -rf "$ROOT"' 0 1 15
cp "$SERVICE" "$SOCKET" "$ROOT/" || fail "cannot stage the units"
cp "$TEST_TMUX" "$ROOT/tmux" || fail "cannot stage the executable"
sed -i "s|^ExecStart=.*|ExecStart=\"$ROOT/tmux\" -D -L %i|" \
	"$ROOT/tmux@.service" || fail "cannot rewrite staged ExecStart"

# systemd-analyze also reports on whatever the host has installed, so scope its
# output to the staged files. Accepting all of it would fail on any machine with
# a single deprecated directive in /etc, which has nothing to do with these
# units and would make the test look broken rather than the unit.
verify_unit()
{
	out=$(systemd-analyze verify "$ROOT/$1" 2>&1)
	status=$?
	mine=$(printf '%s\n' "$out" | grep -F "$ROOT/")
	[ "$status" -eq 0 ] ||
		fail "systemd-analyze rejected $1: ${mine:-$out}"
	[ -z "$mine" ] || fail "systemd-analyze warned about $1: $mine"
}

verify_unit 'tmux@.service'
verify_unit 'tmux@.socket'

exit 0
