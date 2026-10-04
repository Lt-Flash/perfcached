#!/bin/sh
# menuconfig.sh - S250: choose what `make` builds, and write config.mk.
#
#   standalone - the cache alone: no cluster code linked, a [cluster]
#                section in the config is refused
#   clustered  - everything (eager, store, shard, proxy, spread); the
#                default when there is no config.mk
#
# Uses whiptail or dialog when present, plain prompts otherwise, so it
# works over ssh and in a minimal build container alike.  Non-interactive:
#   sh tools/menuconfig.sh standalone|clustered
set -u
CFG=config.mk
cur=clustered
[ -f "$CFG" ] && cur=$(sed -n 's/^PC_EDITION *= *//p' "$CFG")
[ -n "$cur" ] || cur=clustered

choice=${1:-}
if [ -z "$choice" ]; then
	if [ -t 0 ] && command -v whiptail >/dev/null 2>&1; then UI=whiptail
	elif [ -t 0 ] && command -v dialog >/dev/null 2>&1; then UI=dialog
	else UI=; fi
	on_s=OFF; on_c=OFF; [ "$cur" = standalone ] && on_s=ON || on_c=ON
	if [ -n "$UI" ]; then
		choice=$($UI --title "perfcached build" --radiolist \
			"Which daemon should make build?  (space selects, enter confirms)" 14 72 2 \
			standalone "the cache alone - no cluster code, smallest, fastest" $on_s \
			clustered  "everything: eager, store, shard, proxy, spread" $on_c \
			3>&1 1>&2 2>&3) || { echo "menuconfig: cancelled, $CFG unchanged"; exit 1; }
	else
		echo "perfcached build - which daemon should make build?"
		echo "  1) standalone  the cache alone - no cluster code, smallest, fastest"
		echo "  2) clustered   everything: eager, store, shard, proxy, spread"
		printf "choice [current: %s]: " "$cur"
		read -r a || a=
		case "$a" in 1|standalone) choice=standalone;; 2|clustered) choice=clustered;;
			"") choice=$cur;; *) echo "menuconfig: '$a' is not 1 or 2"; exit 1;; esac
	fi
fi
case "$choice" in standalone|clustered) ;; *) echo "menuconfig: '$choice' - want standalone or clustered"; exit 1;; esac
{
	echo "# written by make menuconfig ($(date -u +%Y-%m-%dT%H:%MZ)) - edit with make menuconfig"
	echo "PC_EDITION = $choice"
} > "$CFG"
echo "menuconfig: $CFG says PC_EDITION = $choice$( [ "$choice" != "$cur" ] && echo " (was $cur) - the next make rebuilds the daemon's objects")"
