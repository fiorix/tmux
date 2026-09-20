#!/bin/sh

# codepoint-widths option, measured by the rendered cursor column.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestCPW$$ -f/dev/null"
$TMUX kill-server 2>/dev/null

# The oracle is the cursor column, not the captured text: a double width
# character emits the same bytes and only occupies a different number of
# columns. Both widths are checked, because asserting only the override would
# also pass where every codepoint measured two.

# U+2500 BOX DRAWINGS LIGHT HORIZONTAL is single width by default.
$TMUX new -d -- sh -c 'printf "\342\224\200"; sleep 5'
sleep 1
plain=$($TMUX display -p '#{cursor_x}')
$TMUX kill-server 2>/dev/null

$TMUX set -s codepoint-widths 'U+2500=2' \; \
	new -d -- sh -c 'printf "\342\224\200"; sleep 5'
sleep 1
wide=$($TMUX display -p '#{cursor_x}')
$TMUX kill-server 2>/dev/null

[ "$plain" = 1 ] || exit 1
[ "$wide" = 2 ] || exit 1

exit 0
