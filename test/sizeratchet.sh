#!/bin/sh
# sizeratchet.sh - the three files that kept growing may not pass their budgets (test/size-budget): new
# behaviour goes into its own module; an extraction lowers the budget in its commit.
set -u
fail=0
while read -r f budget; do
	case "$f" in ''|\#*) continue ;; esac
	n=$(wc -l < "$f")
	if [ "$n" -gt "$budget" ]; then
		echo "  FAIL $f: $n lines, its budget $budget - put the new code in a module (MODULARITY.md)"
		fail=1
	else
		echo "  ok   $f: $n of $budget lines"
	fi
done < test/size-budget
[ $fail -eq 0 ]
