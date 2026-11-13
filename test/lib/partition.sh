#!/bin/sh
# partition.sh - cut, mute and heal the peer plane between named containers: iptables rules inside
# each container's netns via nsenter, dropped by source address at both ends (multicast included).
set -u

PART_RT=""
PART_CUTS=""
PART_MUTES=""

part_init() { # part_init <runtime>
	PART_RT=${1:-nerdctl}
	command -v "$PART_RT" >/dev/null 2>&1 ||
		{ echo "partition: no $PART_RT in PATH" >&2; return 1; }
	command -v nsenter >/dev/null 2>&1 ||
		{ echo "partition: nsenter missing (util-linux)" >&2; return 1; }
	iptables -L -n >/dev/null 2>&1 ||
		{ echo "partition: iptables unusable (need root)" >&2; return 1; }
}

part_pid() { # part_pid <container> -> host pid
	$PART_RT inspect -f '{{.State.Pid}}' "$1" 2>/dev/null
}

part_ip() { # part_ip <container> -> its address on the bridge
	# `ip -o addr` inside the netns: the runtime's metadata is what was asked for, this is what the
	# interface has
	pi_pid=$(part_pid "$1") || return 1
	[ -n "$pi_pid" ] && [ "$pi_pid" != 0 ] || return 1
	nsenter -t "$pi_pid" -n ip -o -4 addr show eth0 2>/dev/null |
		sed -n 's/.*inet \([0-9.]*\)\/.*/\1/p' | head -1
}

# one direction of the cut: make $1 refuse everything from $2's address
part_drop() { # part_drop <container> <peer-ip> <-I|-D>
	pd_pid=$(part_pid "$1") || return 1
	nsenter -t "$pd_pid" -n iptables "$3" INPUT -s "$2" -j DROP 2>/dev/null
}

part_cut() { # part_cut <a> <b>
	pc_a=$1; pc_b=$2
	pc_ai=$(part_ip "$pc_a") || return 1
	pc_bi=$(part_ip "$pc_b") || return 1
	[ -n "$pc_ai" ] && [ -n "$pc_bi" ] ||
		{ echo "partition: could not resolve $pc_a/$pc_b" >&2; return 1; }
	part_drop "$pc_a" "$pc_bi" -I || return 1
	part_drop "$pc_b" "$pc_ai" -I || return 1
	PART_CUTS="$PART_CUTS $pc_a:$pc_b"
	echo "  partition: $pc_a <-X-> $pc_b ($pc_ai / $pc_bi)"
}

part_heal() { # part_heal <a> <b>
	ph_a=$1; ph_b=$2
	ph_ai=$(part_ip "$ph_a") || return 0
	ph_bi=$(part_ip "$ph_b") || return 0
	# delete by spec, never by index: an index shifts and the wrong rule goes
	part_drop "$ph_a" "$ph_bi" -D 2>/dev/null
	part_drop "$ph_b" "$ph_ai" -D 2>/dev/null
	echo "  partition: $ph_a <---> $ph_b healed"
}

# ---- one-directional loss, by name: @to stops hearing @from, which keeps hearing @to - not a
# partition, the two ends disagree about who is alive
part_mute() { # part_mute <from> <to>
	pm_f=$1; pm_t=$2
	pm_fi=$(part_ip "$pm_f") || return 1
	[ -n "$pm_fi" ] ||
		{ echo "partition: could not resolve $pm_f" >&2; return 1; }
	part_drop "$pm_t" "$pm_fi" -I || return 1
	PART_MUTES="$PART_MUTES $pm_f:$pm_t"
	echo "  partition: $pm_f --X-> $pm_t (one-directional, $pm_fi muted)"
}

part_unmute() { # part_unmute <from> <to>
	pu_f=$1; pu_t=$2
	pu_fi=$(part_ip "$pu_f") || return 0
	part_drop "$pu_t" "$pu_fi" -D 2>/dev/null
	echo "  partition: $pu_f -----> $pu_t unmuted"
}

# heal everything cut or muted; call it from an EXIT trap: a DROP rule left behind poisons every
# later run against the same fleet
part_heal_all() {
	for pha_p in $PART_MUTES; do
		part_unmute "${pha_p%%:*}" "${pha_p##*:}"
	done
	PART_MUTES=""
	for pha_p in $PART_CUTS; do
		part_heal "${pha_p%%:*}" "${pha_p##*:}"
	done
	PART_CUTS=""
}

# rules this container is dropping on: proof a cut is in place, and that a heal removed it
part_rules() { # part_rules <container>
	pr_pid=$(part_pid "$1") || { echo 0; return; }
	nsenter -t "$pr_pid" -n iptables -S INPUT 2>/dev/null |
		/bin/grep -c -- "-j DROP"
}
