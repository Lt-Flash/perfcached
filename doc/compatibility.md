# Compatibility

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

perfcached is pre-1.0.  The native protocols, the cluster wire, the
write-ahead log and the snapshot format can change in any release, and
its notes say so.  A change to the cluster wire is a new wire epoch
(0.7.1).  A build that still speaks the fleet's epoch rolls onto it and
takes the new one with `upgrade commit epoch=<n>`; one that does not -
0.7.5, which speaks 4 alone, against 0.7.4's 2 - is a whole-fleet
restart, named in its notes.  State from 0.4.1 or later has been kept
across every upgrade since; the upgrades that stopped a fleet whole
were 0.4.5, 0.7.4.3 (from 0.7.3) and 0.7.5, and there is no path from 0.3.x.  Mixed versions
are for a roll, not for running: read a release's notes before
upgrading.
