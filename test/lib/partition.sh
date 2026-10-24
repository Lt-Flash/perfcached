#!/bin/sh
# partition.sh — cut and heal the peer plane between named cluster
# nodes, so a partition is a TEST INPUT rather than an accident (S42a).
#
# HOW, and why this way.  The rules go inside each container's own
# network namespace, applied with the HOST's iptables via nsenter, so:
#
#  - nothing is added to the host's own tables, and a leaked rule
#    cannot affect anything but the container it was meant for;
#  - it does not depend on br_netfilter (bridge traffic only traverses
#    the host FORWARD chain when net.bridge.bridge-nf-call-iptables is
#    on, which is not guaranteed and is a host-wide setting to boot);
#  - the container image needs no iptables of its own.
#
# A cut is applied at BOTH ends.  One-directional loss is a different
# and far nastier fault than a partition, and conflating them would
# make a failure impossible to interpret - so it is asked for BY NAME:
# part_mute <from> <to> drops, at <to>, everything sourced from <from>,
# and nothing else.  <from> keeps hearing <to>'s heartbeats, so it goes
# on believing <to> is alive while every byte it sends is swallowed -
# the one fault that makes a node hand keys to a peer that will never
# receive them (test/reshardtest.sh).
#
# The cluster plane is multicast discovery plus unicast; dropping by
# SOURCE address covers both, because a multicast datagram still
# carries the sender's unicast source.
#
# Usage:
#   . test/lib/partition.sh
#   part_init nerdctl          # runtime to talk to
#   part_cut  pcnode1 pcnode2
#   part_heal pcnode1 pcnode2
#   part_mute pcnode1 pcnode3   # one direction: node3 stops hearing node1
#   part_unmute pcnode1 pcnode3
#   part_heal_all             # ALWAYS from an EXIT trap (cuts AND mutes)
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
	# `ip -o addr` inside the netns, not the runtime's metadata: the
	# metadata is what was ASKED for, this is what the interface has.
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
	# delete BY SPEC, never by index - an index shifts under you and
	# the wrong rule goes (feedback-no-iptables-rule-by-index)
	part_drop "$ph_a" "$ph_bi" -D 2>/dev/null
	part_drop "$ph_b" "$ph_ai" -D 2>/dev/null
	echo "  partition: $ph_a <---> $ph_b healed"
}

# ---- one-directional loss, by name --------------------------------------
# @to stops receiving from @from; @from is untouched and keeps hearing
# @to.  This is NOT a partition: the two ends disagree about who is
# alive, which is precisely the state a hand-over has to survive.
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

# Heal everything this script cut OR muted.  Call it from an EXIT trap:
# a DROP rule left behind outlives the test and silently poisons every
# later run against the same fleet.
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

# how many rules this container is currently dropping on - the proof
# that a cut is really in place, and that a heal really removed it
part_rules() { # part_rules <container>
	pr_pid=$(part_pid "$1") || { echo 0; return; }
	nsenter -t "$pr_pid" -n iptables -S INPUT 2>/dev/null |
		/bin/grep -c -- "-j DROP"
}
