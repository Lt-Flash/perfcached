/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* statscluster.c - the stats tree's "cluster" block: the node, its membership and mastership,
 * the wire, the planes and their counters.  test/statlint.sh reads it with the rest of the writer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "compat/dprint.h"
#include "compat/compat.h"
#include "compat/timer.h"
#include "reload.h"
#include "quiesce.h"
#include "json.h"
#include "ptree.h"
#include "winctr_tw.h"
#include "store.h"
#include "config.h"
#include "verbs.h"
#include "upstream.h"
#include "pcaddr.h"
#include "upgrade.h"
#include "mail.h"
#include "alerts.h"
#include "iostat.h"
#include "uring.h"
#include "core/pcache_htable.h"
#include "core/pcache_arena.h"
#include "core/pcache_mem.h"
#include "wal.h"
#include "rdb.h"
#include "cluster.h"
#include "clane.h"
#include "clretain.h"
#include "ackstate.h"
#include "clterm.h"
#include "clunk.h"
#include "obs.h"
#include "metrics.h"
#include "proto.h"
#include "memguard.h"
#include "pubsub.h"
#include "daemon.h"
#include "version.h"
#include "storage.h"
#include "walprobe.h"
#include "respmap.h"
#include "verbsint.h"
#include "statsreset.h"
#include "statscluster.h"
#include "mgetfan.h"

/* a bounded list into the tree */
struct ack_list { struct pc_tw *out; int n; };

/* one member's announced mark, /stats cluster.acks.fleet.marks */
#define FLEET_LIST 48
static int fleet_list_cb(uint64_t member, uint32_t incarn, uint64_t oid,
		uint32_t sincarn, uint64_t mark, void *ctx)
{
	struct ack_list *al = ctx;
	char h[17], o[17];

	if (al->n++ >= FLEET_LIST)
		return 1;
	snprintf(h, sizeof h, "%016llx", (unsigned long long)member);
	snprintf(o, sizeof o, "%016llx", (unsigned long long)oid);
	pc_tw_map(al->out);
	pc_tw_key(al->out, "member");
	pc_tw_str(al->out, h);
	pc_tw_key(al->out, "member_incarn");
	pc_tw_i64(al->out, (long long)incarn);
	pc_tw_key(al->out, "origin");
	pc_tw_str(al->out, o);
	pc_tw_key(al->out, "incarn");
	pc_tw_i64(al->out, (long long)sincarn);
	pc_tw_key(al->out, "confirmed");
	pc_tw_i64(al->out, (long long)mark);
	pc_tw_end(al->out);
	return 0;
}

/* one space of /stats cluster.acks.marks */
#define ACK_LIST 16
static int ack_list_cb(uint64_t oid, uint32_t incarn, uint64_t mark,
		uint64_t applied, uint64_t hi, void *ctx)
{
	struct ack_list *al = ctx;
	char h[17];

	if (al->n++ >= ACK_LIST)
		return 1;
	snprintf(h, sizeof h, "%016llx", (unsigned long long)oid);
	pc_tw_map(al->out);
	pc_tw_key(al->out, "origin");
	pc_tw_str(al->out, h);
	pc_tw_key(al->out, "incarn");
	pc_tw_i64(al->out, (long long)incarn);
	pc_tw_key(al->out, "confirmed");
	pc_tw_i64(al->out, (long long)mark);
	pc_tw_key(al->out, "applied");
	pc_tw_i64(al->out, (long long)applied);
	pc_tw_key(al->out, "hi");
	pc_tw_i64(al->out, (long long)hi);
	pc_tw_end(al->out);
	return 0;
}

/* "cluster": null on a node without one */
void stats_tree_cluster(struct pc_tw *out)
{
	struct pc_cl_stats cs;

	pc_cluster_get_stats(&cs);
	since_cl(&cs);
	if (!cs.enabled) {
		pc_tw_nil(out);
	} else {
		pc_tw_map(out);
		pc_tw_key(out, "node");
		pc_tw_i64(out, cs.node_id);
		/* identity survives restarts, incarnation does not; durable says whether the
		 * identity was persisted (it cannot be without a [wal] dir) */
		pc_tw_key(out, "identity");
		pc_tw_bulk(out, pc_cluster_identity(), strlen(pc_cluster_identity()));
		pc_tw_key(out, "incarnation");
		pc_tw_i64(out, (long long)
			pc_cluster_incarnation());
		pc_tw_key(out, "identity_durable");
		pc_tw_bool(out, (pc_cluster_identity_durable()));
		/* where that identity lives, and whether that place survives a reboot */
		pc_tw_key(out, "state_dir");
		if (pc_cluster_state_dir())
			pc_tw_bulk(out, pc_cluster_state_dir(), strlen(pc_cluster_state_dir()));
		else
			pc_tw_nil(out);
		pc_tw_key(out, "state_on_tmpfs");
		pc_tw_bool(out, (pc_cluster_state_tmpfs()));
		/* the Lamport clock: comparable across the
		 * fleet without any clock being in sync */
		pc_tw_key(out, "lamport");
		pc_tw_i64(out, (long long)pc_lamport_now());
		pc_tw_ctr(out, "cluster.lamport_rejected", "lamport_rejected", (unsigned long long)(pc_lamport_rejected));
		{
			char cid[37];
			const char *cst;
			unsigned long long fs;

			pc_cluster_cid_info(cid, &cst, &fs);
			pc_tw_key(out, "cluster_id");
			pc_tw_bulk(out, cid, strlen(cid));
			pc_tw_key(out, "cluster_id_state");
			pc_tw_str(out, cst);
			pc_tw_key(out, "foreign_seen");
			pc_tw_i64(out, (long long)fs);
			{
				char fid[16 * 37], fad[16][24];
				long long fago[16];
				unsigned long long ffr[16];
				int fn = pc_cluster_foreign_list(fid, 16, fad,
					fago, ffr, 16), fi;

				pc_tw_key(out, "self_late");
			pc_tw_i64(out, (long long)pc_cluster_self_late());
			pc_tw_key(out, "election_holds");
			pc_tw_i64(out, (long long)pc_cluster_mprobe_holds());
			pc_tw_key(out, "foreign_clusters");
			pc_tw_arr(out);
				for (fi = 0; fi < fn; fi++) {
					pc_tw_map(out);
					pc_tw_key(out, "id");
					pc_tw_bulk(out, fid + fi * 37, strlen(fid + fi * 37));
					pc_tw_key(out, "from");
					pc_tw_bulk(out, fad[fi], strlen(fad[fi]));
					pc_tw_key(out, "last_seen_s");
					pc_tw_i64(out, fago[fi] / 1000);
					pc_tw_key(out, "frames");
					pc_tw_i64(out, (long long)ffr[fi]);
					pc_tw_end(out);
				}
				pc_tw_end(out);
			}
		}
		/* whether an eager miss here is final, or asks the fleet (not READY, or a
		 * copy was refused for memory since start) */
		pc_tw_key(out, "eager_miss_final");
		pc_tw_bool(out, (pc_cluster_eager_miss_final()));
		{
			const char *dm;
			int sn, sr, kn;
			unsigned long long rf, hs, hr;

			pc_cluster_discovery_info(&dm, &sn, &sr, &kn, &rf,
				&hs, &hr);
			pc_tw_key(out, "discovery");
			pc_tw_map(out);
			pc_tw_key(out, "mode");
			pc_tw_str(out, dm);
			pc_tw_key(out, "seeds");
			pc_tw_i64(out, sn);
			pc_tw_key(out, "seeds_resolved");
			pc_tw_i64(out, sr);
			pc_tw_key(out, "known");
			pc_tw_i64(out, kn);
			pc_tw_key(out, "resolve_fail");
			pc_tw_i64(out, (long long)rf);
			pc_tw_key(out, "hints_sent");
			pc_tw_i64(out, (long long)hs);
			pc_tw_key(out, "hints_recv");
			pc_tw_i64(out, (long long)hr);
			pc_tw_end(out);
		}
		{
			struct clane_figs lf;
			int md, lp;

			memset(&lf, 0, sizeof lf);
			pc_cluster_lane_info(&lf, &md, &lp);
			pc_tw_key(out, "lane");
			pc_tw_map(out);
			pc_tw_key(out, "max_datagram");
			pc_tw_i64(out, md);
			pc_tw_key(out, "port");
			pc_tw_i64(out, lp);
			pc_tw_key(out, "peers_up");
			pc_tw_i64(out, lf.peers_up);
			pc_tw_key(out, "inbound");
			pc_tw_i64(out, lf.inbound);
			pc_tw_key(out, "sent_frames");
			pc_tw_i64(out, (long long)lf.sent_frames);
			pc_tw_key(out, "sent_bytes");
			pc_tw_i64(out, (long long)lf.sent_bytes);
			pc_tw_key(out, "recv_frames");
			pc_tw_i64(out, (long long)lf.recv_frames);
			pc_tw_key(out, "recv_bytes");
			pc_tw_i64(out, (long long)lf.recv_bytes);
			pc_tw_key(out, "dropped_full");
			pc_tw_i64(out, (long long)lf.dropped_full);
			pc_tw_key(out, "dropped_stale");
			pc_tw_i64(out, (long long)lf.dropped_stale);
			pc_tw_key(out, "dropped_rx");
			pc_tw_i64(out, (long long)lf.dropped_rx);
			pc_tw_key(out, "connects");
			pc_tw_i64(out, (long long)lf.connects);
			pc_tw_key(out, "connect_fail");
			pc_tw_i64(out, (long long)lf.connect_fail);
			pc_tw_key(out, "refused");
			pc_tw_i64(out, (long long)lf.refused);
			pc_tw_key(out, "inbound_timeouts");
			pc_tw_i64(out, (long long)lf.inbound_timeouts);
			pc_tw_key(out, "forced");
			pc_tw_i64(out, lf.forced);
			pc_tw_key(out, "udp_blocked_saves");
			pc_tw_i64(out, (long long)pc_cluster_lane_saves());
			/* forced lanes given back to UDP */
			pc_tw_key(out, "udp_restored");
			pc_tw_i64(out, (long long)pc_cluster_lane_restored());
			pc_tw_end(out);
		}
		pc_tw_key(out, "multicast_ttl");
		pc_tw_i64(out, (long long)pc_cluster_multicast_ttl());
		pc_tw_key(out, "map");
		pc_tw_map(out);
		pc_tw_key(out, "valid");
		pc_tw_bool(out, (cs.map_valid));
		pc_tw_key(out, "term");
		pc_tw_i64(out, (long long)cs.map_term);
		pc_tw_key(out, "seq");
		pc_tw_i64(out, (long long)cs.map_seq);
		pc_tw_key(out, "nodes");
		pc_tw_i64(out, (long long)cs.map_nodes);
		pc_tw_key(out, "master");
		pc_tw_i64(out, (long long)cs.map_master);
		pc_tw_key(out, "published");
		pc_tw_i64(out, (long long)cs.map_pub);
		pc_tw_key(out, "received");
		pc_tw_i64(out, (long long)cs.map_rx);
		pc_tw_key(out, "stale");
		pc_tw_i64(out, (long long)cs.map_stale);
		pc_tw_ctr(out, "cluster.map.refused", "refused", (unsigned long long)(cs.map_bad));
		pc_tw_key(out, "unadmitted");
		pc_tw_i64(out, (long long)cs.map_unadmitted);
		/* usable = the map's placeable set matches what this node sees as live;
		 * place_hrw still climbing on a settled fleet means it never caught up */
		pc_tw_key(out, "usable");
		pc_tw_bool(out, (cs.map_usable));
		pc_tw_key(out, "place_map");
		pc_tw_i64(out, (long long)cs.place_map);
		pc_tw_key(out, "place_hrw");
		pc_tw_i64(out, (long long)cs.place_hrw);
		/* the designated standby: none means the cluster is one failure from
		 * losing its control plane */
		pc_tw_key(out, "backup");
		pc_tw_i64(out, (long long)cs.backup_id);
		pc_tw_end(out);
		pc_tw_key(out, "sync");
		pc_tw_map(out);
		pc_tw_key(out, "held");
		pc_tw_bool(out, (cs.held_valid));
		pc_tw_key(out, "held_term");
		pc_tw_i64(out, (long long)cs.held_term);
		pc_tw_key(out, "held_seq");
		pc_tw_i64(out, (long long)cs.held_seq);
		pc_tw_key(out, "held_identities");
		pc_tw_i64(out, (long long)cs.held_hist_n);
		pc_tw_key(out, "sent");
		pc_tw_i64(out, (long long)cs.sync_sent);
		pc_tw_key(out, "received");
		pc_tw_i64(out, (long long)cs.sync_rx);
		pc_tw_key(out, "acked");
		pc_tw_i64(out, (long long)cs.sync_ack);
		pc_tw_key(out, "refused");
		pc_tw_i64(out, (long long)cs.sync_bad);
		pc_tw_key(out, "timeouts");
		pc_tw_i64(out, (long long)cs.sync_timeouts);
		pc_tw_end(out);
		pc_tw_key(out, "identities_seen");
		pc_tw_i64(out, (long long)cs.hist_n);
		pc_tw_key(out, "reconciled");
		pc_tw_i64(out, (long long)cs.reconciled);
		pc_tw_key(out, "reconcile_probed");
		pc_tw_i64(out, (long long)cs.reconcile_probed);
		pc_tw_key(out, "reconcile_holdchecks");
		pc_tw_i64(out, (long long)cs.reconcile_holdq);
		pc_tw_key(out, "holdchecks_served");
		pc_tw_i64(out, (long long)cs.holdq_served);
		pc_tw_key(out, "repl_held_recovered");
		pc_tw_i64(out, (long long)cs.repl_held_recovered);
		pc_tw_key(out, "pull_held_recovered");
		pc_tw_i64(out, (long long)cs.pull_held_recovered);
		pc_tw_key(out, "reconcile_digests");
		pc_tw_map(out);
		pc_tw_key(out, "asked");
		pc_tw_i64(out, (long long)cs.reconcile_dig_asked);
		pc_tw_key(out, "slots_differ");
		pc_tw_i64(out, (long long)cs.reconcile_dig_diff);
		pc_tw_key(out, "keys_skipped");
		pc_tw_i64(out, (long long)cs.reconcile_dig_skipped);
		pc_tw_key(out, "unanswered");
		pc_tw_i64(out, (long long)cs.reconcile_dig_fallback);
		pc_tw_end(out);
		/* durations of the last completed pass, not rates */
		pc_tw_key(out, "reconcile_ms");
		pc_tw_i64(out, (long long)cs.reconcile_ms);
		pc_tw_key(out, "reconcile_pending");
		pc_tw_i64(out, (long long)cs.reconcile_pending);
		/* the fast path - the last pass's, and as a witness */
		pc_tw_key(out, "reconcile_fast");
		pc_tw_bool(out, cs.reconcile_fast);
		pc_tw_key(out, "reconcile_fast_deletes");
		pc_tw_i64(out, (long long)cs.reconcile_fast_deletes);
		pc_tw_key(out, "reconcile_fast_why");
		pc_tw_str(out, cs.reconcile_fast_why);
		pc_tw_key(out, "reconcile_fast_served");
		pc_tw_i64(out, (long long)cs.reconcile_fast_served);
		pc_tw_key(out, "reconcile_fast_refused");
		pc_tw_i64(out, (long long)cs.reconcile_fast_refused);
		/* the walk's budget and how far it has got */
		pc_tw_key(out, "reconcile_budget");
		pc_tw_i64(out, cs.reconcile_budget);
		pc_tw_key(out, "reconcile_open");
		pc_tw_bool(out, cs.reconcile_open);
		pc_tw_key(out, "reconcile_walked");
		pc_tw_i64(out, (long long)cs.reconcile_walked);
		pc_tw_key(out, "reconcile_walk_total");
		pc_tw_i64(out, (long long)cs.reconcile_walk_total);
		pc_tw_key(out, "reconcile_rate");
		pc_tw_i64(out, (long long)cs.reconcile_rate);
		pc_tw_key(out, "reconcile_eta_s");
		pc_tw_i64(out, cs.reconcile_eta_s);
		pc_tw_key(out, "repl_sweep_ms");
		pc_tw_i64(out, (long long)cs.repl_sweep_ms);
		pc_tw_key(out, "repl_sweep_scanned");
		pc_tw_i64(out, (long long)cs.repl_sweep_scanned);
		pc_tw_key(out, "repl_sweep_sent");
		pc_tw_i64(out, (long long)cs.repl_sweep_sent);
		pc_tw_key(out, "repl_sweep_cycles");
		pc_tw_i64(out, (long long)cs.repl_sweep_cycles);
		pc_tw_key(out, "reconcile_deferred");
		pc_tw_i64(out, (long long)cs.reconcile_deferred);
		pc_tw_key(out, "reconcile_rounds");
		pc_tw_i64(out, (long long)cs.reconcile_rounds);
		pc_tw_key(out, "reconcile_unruled");
		pc_tw_i64(out, (long long)cs.reconcile_unruled);
		pc_tw_key(out, "reconcile_witness");
		pc_tw_i64(out, cs.reconcile_witness);
		pc_tw_key(out, "reconcile_skipped");
		pc_tw_bool(out, (cs.reconcile_skipped));
		/* resyncs: what the fleet could not rule on record by record, dropped and pulled again */
		pc_tw_key(out, "resyncs");
		pc_tw_i64(out, (long long)cs.resyncs);
		pc_tw_key(out, "resync_dropped");
		pc_tw_i64(out, (long long)cs.resync_dropped);
		pc_tw_key(out, "resyncing");
		pc_tw_bool(out, cs.resyncing);
		/* would a restart of this node NOW be witnessed */
		pc_tw_key(out, "restart");
		pc_tw_map(out);
		pc_tw_key(out, "safe");
		pc_tw_bool(out, (cs.restart_safe));
		pc_tw_key(out, "witnesses");
		pc_tw_i64(out, cs.restart_witnesses);
		pc_tw_key(out, "safe_in_ms");
		pc_tw_i64(out, cs.restart_safe_in_ms);
		/* the peers whose open reconcile makes it unsafe */
		pc_tw_key(out, "waits_for");
		pc_tw_arr(out);
		{
			int wi;

			for (wi = 0; wi < cs.restart_waits_n && wi < 8; wi++)
				pc_tw_i64(out, cs.restart_waits_for[wi]);
		}
		pc_tw_end(out);
		pc_tw_end(out);
		/* this node's pass has had no live witness this long */
		pc_tw_key(out, "reconcile_no_witness_s");
		pc_tw_i64(out, cs.reconcile_no_witness_s);
		/* live witnesses of this node's open reconcile */
		pc_tw_key(out, "reconcile_witnesses_live");
		pc_tw_i64(out, cs.reconcile_witnesses_live);
		/* the term orders maps across mastership changes; term_rejected counts
		 * terms peers advertised that were refused as implausible */
		pc_tw_key(out, "term");
		pc_tw_i64(out, (long long)pc_term_current());
		pc_tw_ctr(out, "cluster.term_rejected", "term_rejected", (unsigned long long)(pc_term_rejected));
		/* a followed term the state dir refused: the node takes no mastership until it lands */
		pc_tw_key(out, "term_unpersisted");
		pc_tw_i64(out, (long long)cs.term_unpersisted);
		pc_tw_key(out, "eager");
		pc_tw_bool(out, (pc_cluster_eager()));
		pc_tw_key(out, "mode");
		pc_tw_str(out, pc_cluster_mode_name());
		/* role = authority in the membership; the top-level state = whether the
		 * data can be trusted yet - a master can still be reconciling */
		pc_tw_key(out, "role");
		pc_tw_str(out, cs.role == 2 ? "master" :
			cs.role == 1 ? "member" : "joining");
		pc_tw_key(out, "master");
		pc_tw_i64(out, cs.master_id);
		pc_tw_key(out, "peers_up");
		pc_tw_i64(out, cs.peers_up);
		pc_tw_key(out, "replicas_short");
		pc_tw_i64(out, cs.replicas_short);
		/* a master change is the event most worth dating */
		pc_tw_key(out, "elections");
		pc_tw_i64(out, (long long)cs.elections);
		pc_tw_key(out, "demotions");
		pc_tw_i64(out, (long long)cs.demotions);
		pc_tw_key(out, "members_asked");       /* asked the master for the list */
		pc_tw_i64(out, (long long)cs.members_asked);
		pc_tw_key(out, "members_sent");        /* lists sent, as the master */
		pc_tw_i64(out, (long long)cs.members_sent);
		pc_tw_key(out, "admission_stuck");     /* joiners held CONNECTING */
		pc_tw_i64(out, (long long)cs.admission_stuck);
		pc_tw_key(out, "hb_watchdog");         /* beats the watchdog re-sent */
		pc_tw_i64(out, (long long)cs.hb_watchdog);
		pc_tw_key(out, "hb_sent");             /* ALIVEs the cluster thread sent */
		pc_tw_i64(out, (long long)cs.hb_sent);
		pc_tw_key(out, "wire_epoch");          /* the one spoken */
		pc_tw_i64(out, pc_cluster_epoch());
		/* the committed epoch, this build's range, the commits */
		pc_tw_key(out, "epoch");
		pc_tw_map(out);
		pc_tw_key(out, "committed");
		pc_tw_i64(out, pc_cluster_epoch());
		pc_tw_key(out, "min");
		pc_tw_i64(out, pc_cluster_epoch_min());
		pc_tw_key(out, "max");
		pc_tw_i64(out, pc_cluster_epoch_max());
		pc_tw_key(out, "commits");
		pc_tw_i64(out, (long long)cs.epoch_commits);
		pc_tw_end(out);
		pc_tw_key(out, "bad_col_names");
		pc_tw_i64(out, (long long)pc_cluster_bad_col_names());
		pc_tw_key(out, "epoch_refused");
		pc_tw_i64(out, (long long)cs.epoch_refused);
		pc_tw_key(out, "goodbye_refused");
		pc_tw_i64(out, (long long)cs.goodbye_refused);
		pc_tw_key(out, "sender_refused");
		pc_tw_i64(out, (long long)cs.sender_refused);
		pc_tw_key(out, "id_collisions");
		pc_tw_i64(out, (long long)cs.id_collisions);
		pc_tw_key(out, "mcast_stray");
		pc_tw_i64(out, (long long)cs.mcast_stray);
		pc_tw_key(out, "apply_threads");
		pc_tw_i64(out, cs.apply_threads ? cs.apply_threads : 1);
		pc_tw_key(out, "apply_dispatched");
		pc_tw_i64(out, (long long)cs.apply_dispatched);
		pc_tw_key(out, "apply_blocks");
		pc_tw_i64(out, (long long)cs.apply_blocks);
		pc_tw_key(out, "apply_backlog");
		pc_tw_i64(out, (long long)cs.apply_backlog);
		if (cs.apply_threads > 1) {    /* per ring: the stall's post-mortem */
			int ri;

			pc_tw_key(out, "apply_rings");
			pc_tw_arr(out);
			for (ri = 0; ri < cs.apply_threads && ri < 16; ri++) {
				pc_tw_map(out);
				pc_tw_key(out, "backlog");
				pc_tw_i64(out, (long long)cs.apply_ring[ri].backlog);
				pc_tw_key(out, "busy_ms");
				pc_tw_i64(out, cs.apply_ring[ri].busy_ms);
				pc_tw_key(out, "wait_ms");
				pc_tw_i64(out, cs.apply_ring[ri].wait_ms);
				pc_tw_end(out);
			}
			pc_tw_end(out);
		}
		pc_tw_key(out, "stalled");
		pc_tw_bool(out, (cs.stalled));
		pc_tw_key(out, "stalled_for_ms");
		pc_tw_i64(out, cs.stalled_for_ms);
		pc_tw_key(out, "apply_stalls");
		pc_tw_i64(out, (long long)cs.apply_stalls);
		pc_tw_key(out, "apply_stall_ms_max");
		pc_tw_i64(out, (long long)cs.apply_stall_ms_max);
		/* which machine this is: the node id is a reused per-fleet handle; the UUID
		 * version says platform-derived (8) or randomly minted (7) */
		{
			char uu[37];
			int uv = 0;

			pc_cluster_identity_uuid(uu, &uv);
			/* not "identity": that key is published above as 32 hex, and a duplicate
			 * key would change what every parser reads (it takes the last) */
			pc_tw_key(out, "identity_uuid");
			pc_tw_str(out, uu);
			pc_tw_key(out, "identity_ver");
			pc_tw_i64(out, uv);
		}
		/* every peer's identity, so a dashboard can show which
		 * machine holds each id */
		{
			struct pc_cl_peer_info pi[PC_CL_MAXPEER];
			int np = pc_cluster_peers(pi, PC_CL_MAXPEER), k;

			pc_tw_key(out, "peers");
			pc_tw_arr(out);
			for (k = 0; k < np; k++) {
				pc_tw_map(out);
				pc_tw_key(out, "node");
				pc_tw_i64(out, pi[k].node);
				pc_tw_key(out, "up");
				pc_tw_bool(out, (pi[k].up));
				pc_tw_key(out, "identity");
				pc_tw_str(out, pi[k].ident);
				pc_tw_key(out, "identity_ver");
				pc_tw_i64(out, pi[k].ident_ver);
				pc_tw_key(out, "free_mb");
				pc_tw_i64(out, pi[k].free_mb);
				pc_tw_end(out);
			}
			pc_tw_end(out);
		}
		pc_tw_key(out, "reserved_ids");
		pc_tw_i64(out, cs.reserved_ids);
		pc_tw_ctr(out, "cluster.pull_sent", "pull_sent", (unsigned long long)(cs.pull_sent));
		pc_tw_ctr(out, "cluster.pull_hits", "pull_hits", (unsigned long long)(cs.pull_hits));
		pc_tw_ctr(out, "cluster.pull_misses", "pull_misses", (unsigned long long)(cs.pull_misses));
		pc_tw_ctr(out, "cluster.pull_timeouts", "pull_timeouts", (unsigned long long)(cs.pull_timeouts));
		pc_tw_ctr(out, "cluster.pull_served", "pull_served", (unsigned long long)(cs.pull_served));
		pc_tw_ctr(out, "cluster.pull_fetching", "pull_fetching", (unsigned long long)(cs.pull_fetching));
		pc_tw_ctr(out, "cluster.pull_fetched", "pull_fetched", (unsigned long long)(cs.pull_fetched));
		{
			/* mgets that waited on remote lookups, the ones
			 * that ended with a key unanswered, the ones refused */
			unsigned long long mf, mu, mr;

			pc_mget_stats(1, &mf, &mu, &mr);
			pc_tw_ctr(out, "cluster.mget_fanout", "mget_fanout", mf);
			pc_tw_ctr(out, "cluster.mget_unanswered", "mget_unanswered", mu);
			pc_tw_ctr(out, "cluster.mget_refused", "mget_refused", mr);
		}
		pc_tw_ctr(out, "cluster.fwd_through", "fwd_through", (unsigned long long)(cs.fwd_through));
		pc_tw_ctr(out, "cluster.fwd_through_acked", "fwd_through_acked", (unsigned long long)(cs.fwd_through_acked));
		pc_tw_ctr(out, "cluster.tomb_sent", "tomb_sent", (unsigned long long)(cs.tomb_sent));
		/* on the group, the datagrams they cost, own copies */
		pc_tw_ctr(out, "cluster.tomb_group", "tomb_group", (unsigned long long)(cs.tomb_group));
		pc_tw_ctr(out, "cluster.tomb_dgrams", "tomb_dgrams", (unsigned long long)(cs.tomb_dgrams));
		pc_tw_ctr(out, "cluster.tomb_own", "tomb_own", (unsigned long long)(cs.tomb_own));
		pc_tw_ctr(out, "cluster.tomb_applied", "tomb_applied", (unsigned long long)(cs.tomb_applied));
		pc_tw_ctr(out, "cluster.neg_hits", "neg_hits", (unsigned long long)(cs.neg_hits));
		pc_tw_key(out, "neg_displaced");
		pc_tw_i64(out, (long long)cs.neg_displaced);
		pc_tw_ctr(out, "cluster.tomb_displaced", "tomb_displaced", (unsigned long long)(cs.tomb_displaced));
		/* the retained deletes: count, cost (outside the arena), the cap's early drops (a longer
		 * partition than is remembered may resurrect) and the replays to returning peers */
		{
			struct clretain_stats rt;
			struct timespec ts;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			clretain_get_stats(&rt, (unsigned int)ts.tv_sec);
			pc_tw_key(out, "tombstones_retained");
			pc_tw_map(out);
			pc_tw_key(out, "entries");
			pc_tw_i64(out, (long long)rt.entries);
			pc_tw_key(out, "bytes");
			pc_tw_i64(out, (long long)rt.bytes);
			pc_tw_key(out, "max");
			pc_tw_i64(out, rt.max);
			pc_tw_key(out, "retain_s");
			pc_tw_i64(out, rt.retain_s);
			pc_tw_key(out, "oldest_age_s");
			pc_tw_i64(out, rt.oldest_age_s);
			/* with their 5-minute windows, the span of the plane's other counters */
			pc_tw_ctr(out, "tomb.dropped_early", "dropped_early",
				rt.dropped_early);
			pc_tw_ctr(out, "tomb.dropped_early_expiries",
				"dropped_early_expiries", rt.dropped_early_expiries);
			pc_tw_ctr(out, "tomb.noted", "noted", rt.noted);
			pc_tw_ctr(out, "tomb.noted_expiries", "noted_expiries",
				rt.noted_expiries);
			pc_tw_key(out, "expiries");             /* of entries */
			pc_tw_i64(out, (long long)rt.expiries);
			/* of entries, numbered; forgotten because every member holds them; expiries
			 * held PC_TOMB_EXPIRY_HOLD_S while the fleet confirms */
			pc_tw_key(out, "numbered");
			pc_tw_i64(out, (long long)rt.numbered);
			pc_tw_key(out, "forgotten");
			pc_tw_i64(out, (long long)rt.forgotten);
			pc_tw_key(out, "forgotten_expiries");
			pc_tw_i64(out, (long long)rt.forgotten_expiries);
			pc_tw_key(out, "local");                /* this node's own drops */
			pc_tw_i64(out, (long long)rt.local);
			pc_tw_key(out, "replayed");
			pc_tw_i64(out, (long long)pc_cluster_tomb_catchup_sent());
			pc_tw_end(out);
		}
		/* what this node holds of each origin's numbered deletes: confirmed (applied,
		 * contiguous, durable), applied, the highest seen */
		{
			struct clack_stats ak;
			struct ack_list al = { out, 0 };

			pc_ack_get_stats(&ak);
			pc_tw_key(out, "acks");
			pc_tw_map(out);
			pc_tw_key(out, "durable_on_apply");
			pc_tw_bool(out, ak.durable_on_apply);
			pc_tw_key(out, "spaces");
			pc_tw_i64(out, ak.spaces);
			pc_tw_key(out, "applied");
			pc_tw_i64(out, (long long)ak.applied);
			pc_tw_key(out, "dups");
			pc_tw_i64(out, (long long)ak.dups);
			pc_tw_key(out, "far");
			pc_tw_i64(out, (long long)ak.far);
			pc_tw_key(out, "full");
			pc_tw_i64(out, (long long)ak.full);
			/* the holes asked for, and the asks answered here */
			pc_tw_key(out, "gap_asked");
			pc_tw_i64(out, (long long)cs.gap_asked);
			pc_tw_key(out, "gap_served");
			pc_tw_i64(out, (long long)cs.gap_served);
			pc_tw_key(out, "gap_resent");
			pc_tw_i64(out, (long long)cs.gap_resent);
			pc_tw_key(out, "gap_floored");
			pc_tw_i64(out, (long long)cs.gap_floored);
			pc_tw_key(out, "gap_dead");
			pc_tw_i64(out, (long long)cs.gap_dead);
			/* the tails asked for, and the floors a node that came
			 * back empty took from its bootstrap's sources */
			pc_tw_key(out, "gap_tail");
			pc_tw_i64(out, (long long)cs.gap_tail);
			pc_tw_key(out, "floors");
			pc_tw_i64(out, (long long)cs.acks_floors);
			/* rebuilds asked of members back from an absence,
			 * run here when asked, and asks already covered */
			pc_tw_key(out, "rebuild_sent");
			pc_tw_i64(out, (long long)cs.rebuild_sent);
			pc_tw_key(out, "rebuilds");
			pc_tw_i64(out, (long long)cs.rebuilds);
			pc_tw_key(out, "rebuild_covered");
			pc_tw_i64(out, (long long)cs.rebuild_covered);
			pc_tw_key(out, "reconcile_rewritten");
			pc_tw_i64(out, (long long)cs.reconcile_rewritten);
			pc_tw_key(out, "marks");
			pc_tw_arr(out);
			pc_ack_each(ack_list_cb, &al);
			pc_tw_end(out);
			/* what the members announce, and whether an origin may forget by it */
			{
				struct clackf_stats fs;
				struct ack_list fl = { out, 0 };
				int lack[16], nl = 0, j, gate;

				gate = pc_cluster_acks_gate(lack, 16, &nl);
				pc_ack_fleet_stats(&fs);
				pc_tw_key(out, "fleet");
				pc_tw_map(out);
				pc_tw_key(out, "confirming");
				pc_tw_bool(out, gate);
				pc_tw_key(out, "waiting_for");
				pc_tw_arr(out);
				for (j = 0; j < nl && j < 16; j++)
					pc_tw_i64(out, lack[j]);
				pc_tw_end(out);
				pc_tw_key(out, "members");
				pc_tw_i64(out, fs.members);
				pc_tw_key(out, "marks_held");
				pc_tw_i64(out, fs.marks);
				pc_tw_key(out, "reports");
				pc_tw_i64(out, (long long)fs.reports);
				pc_tw_key(out, "lowered");
				pc_tw_i64(out, (long long)fs.lowered);
				pc_tw_key(out, "resets");
				pc_tw_i64(out, (long long)fs.resets);
				pc_tw_key(out, "refused");
				pc_tw_i64(out, (long long)fs.refused);
				pc_tw_key(out, "marks");
				pc_tw_arr(out);
				pc_ack_fleet_each(fleet_list_cb, &fl);
				pc_tw_end(out);
				pc_tw_end(out);
			}
			/* the spaces whose deletes this node keeps by the marks (node 0: holder not
			 * known here; self: this node), and the member stuck on one */
			{
				struct pc_ack_hold hv[PC_ACK_HOLD_MAX];
				int hn = pc_cluster_acks_holds(hv, PC_ACK_HOLD_MAX), j;
				char h[17];

				pc_tw_key(out, "held_by");
				pc_tw_arr(out);
				for (j = 0; j < hn; j++) {
					snprintf(h, sizeof h, "%016llx",
						(unsigned long long)hv[j].oid);
					pc_tw_map(out);
					pc_tw_key(out, "origin");
					pc_tw_str(out, h);
					pc_tw_key(out, "incarn");
					pc_tw_i64(out, (long long)hv[j].incarn);
					pc_tw_key(out, "through");
					pc_tw_i64(out, (long long)hv[j].through);
					pc_tw_key(out, "head");
					pc_tw_i64(out, (long long)hv[j].head);
					pc_tw_key(out, "waiting");
					pc_tw_i64(out, (long long)(hv[j].head - hv[j].through));
					pc_tw_key(out, "node");
					pc_tw_i64(out, hv[j].node);
					pc_tw_key(out, "self");
					pc_tw_bool(out, hv[j].self);
					pc_tw_key(out, "stuck");
					if (hv[j].stuck_node) {
						pc_tw_map(out);
						pc_tw_key(out, "node");
						pc_tw_i64(out, hv[j].stuck_node);
						pc_tw_key(out, "behind");
						pc_tw_i64(out, (long long)(hv[j].head -
							hv[j].stuck_mark));
						pc_tw_key(out, "still_s");
						pc_tw_i64(out, (long long)hv[j].stuck_s);
						pc_tw_end(out);
					} else
						pc_tw_nil(out);
					pc_tw_end(out);
				}
				pc_tw_end(out);
			}
			pc_tw_end(out);
		}
		pc_tw_ctr(out, "cluster.bad_auth", "bad_auth", (unsigned long long)(cs.bad_auth));
		pc_tw_key(out, "wire_ver_refused");
		pc_tw_i64(out, (long long)cs.wire_ver_refused);
		{
			unsigned long long rd, ro, ri, rf, rh;

			pc_cluster_replay_counts(&rd, &ro, &ri, &rf, &rh);
			pc_tw_key(out, "replay");
			pc_tw_map(out);
			pc_tw_key(out, "dup");
			pc_tw_i64(out, (long long)rd);
			pc_tw_key(out, "old");
			pc_tw_i64(out, (long long)ro);
			pc_tw_key(out, "incarn");
			pc_tw_i64(out, (long long)ri);
			pc_tw_key(out, "unchecked");
			pc_tw_i64(out, (long long)rf);
			pc_tw_key(out, "host");
			pc_tw_i64(out, (long long)rh);
			pc_tw_end(out);
		}
		{
			struct pc_proxy_stats px;

			pc_proxy_get_stats(&px);
			since_px(&px);
			pc_tw_key(out, "placed_local");
			pc_tw_i64(out, (long long)px.placed_local);
			pc_tw_key(out, "placed_remote");
			pc_tw_i64(out, (long long)px.placed_remote);
			pc_tw_ctr(out, "cluster.fwd_sent", "fwd_sent", (unsigned long long)(px.fwd_sent));
			pc_tw_ctr(out, "cluster.fwd_served", "fwd_served", (unsigned long long)(px.fwd_served));
			{
				/* proxy holder hints */
				unsigned long long hs = 0, hl = 0;

				pc_proto_hint_stats(&hs, &hl);
				/* not "hints_sent": discovery's
				 * peer-address hints have that name */
				pc_tw_key(out, "holder_hints_sent");
				pc_tw_i64(out, (long long)hs);
				pc_tw_key(out, "holder_hint_local");
				pc_tw_i64(out, (long long)hl);
			}
			pc_tw_ctr(out, "cluster.migrated_out", "migrated_out", (unsigned long long)(px.migrated_out));
			pc_tw_ctr(out, "cluster.migrated_in", "migrated_in", (unsigned long long)(px.migrated_in));
			pc_tw_ctr(out, "cluster.migrate_lost", "migrate_lost", (unsigned long long)(px.migrate_lost));
			pc_tw_key(out, "migrate_retx");
			pc_tw_i64(out, (long long)px.migrate_retx);
			/* whether the rebalancer has anything to do, and why */
			{
				struct pc_rebalance rbs;

				pc_cluster_rebalance(&rbs);
				pc_tw_key(out, "rebalance");
				pc_tw_map(out);
				pc_tw_key(out, "state");
				pc_tw_str(out, rbs.settled ? "settled" : "leveling");
				pc_tw_key(out, "reason");
				pc_tw_bulk(out, rbs.reason, strlen(rbs.reason));
				pc_tw_key(out, "settled_since");
				pc_tw_i64(out, rbs.settled_since);
				pc_tw_key(out, "last_move");
				pc_tw_i64(out, rbs.last_move);
				pc_tw_key(out, "moved_records");
				pc_tw_i64(out, (long long)rbs.moved_records);
				pc_tw_key(out, "ticks_shed");
				pc_tw_i64(out, (long long)rbs.ticks_shed);
				pc_tw_end(out);
			}
			pc_tw_key(out, "migrate_dgrams");
			pc_tw_i64(out, (long long)px.migrate_dgrams);
			pc_tw_ctr(out, "cluster.repl_out", "repl_out", (unsigned long long)(px.repl_out));
		/* not_held = writes accepted, forwarded and not kept (placement leaves this node
		 * out); repaired = holder-set changes that armed a re-placement */
		pc_tw_key(out, "spread_not_held");
		pc_tw_i64(out, (long long)px.spread_not_held);
		pc_tw_key(out, "spread_repaired");
		pc_tw_i64(out, (long long)px.spread_repaired);
		pc_tw_key(out, "spread_reclaimed");
		pc_tw_i64(out, (long long)px.spread_reclaimed);
		pc_tw_key(out, "spread_possess_sent");
		pc_tw_i64(out, (long long)px.spread_possess_sent);
		pc_tw_key(out, "spread_possess_kept");
		pc_tw_i64(out, (long long)px.spread_possess_kept);
		pc_tw_key(out, "spread_ack_parked");
		pc_tw_i64(out, (long long)px.spread_ack_parked);
		pc_tw_key(out, "spread_ack_kept");
		pc_tw_i64(out, (long long)px.spread_ack_kept);
		pc_tw_key(out, "spread_ack_dropped");
		pc_tw_i64(out, (long long)px.spread_ack_dropped);
		pc_tw_key(out, "spread_ack_short");
		pc_tw_i64(out, (long long)px.spread_ack_short);
		pc_tw_key(out, "spread_ack_lost");
		pc_tw_i64(out, (long long)px.spread_ack_lost);
		pc_tw_key(out, "sweep_gate");
		pc_tw_map(out);
		pc_tw_key(out, "asked");
		pc_tw_i64(out, (long long)px.sweep_gate_asked);
		pc_tw_key(out, "answered");
		pc_tw_i64(out, (long long)px.sweep_gate_answered);
		pc_tw_key(out, "unanswered");
		pc_tw_i64(out, (long long)px.sweep_gate_unanswered);
		pc_tw_key(out, "clean");
		pc_tw_i64(out, (long long)px.sweep_gate_clean);
		pc_tw_key(out, "idle");
		pc_tw_i64(out, (long long)px.sweep_gate_idle);
		pc_tw_key(out, "slots_asked");
		pc_tw_i64(out, (long long)px.sweep_gate_slots_asked);
		pc_tw_key(out, "slots_diff");
		pc_tw_i64(out, (long long)px.sweep_gate_slots_diff);
		pc_tw_key(out, "digested");
		pc_tw_i64(out, (long long)px.sweep_gate_digested);
		pc_tw_key(out, "walks");
		pc_tw_i64(out, (long long)px.sweep_gate_walks);
		pc_tw_end(out);
		pc_tw_key(out, "spread_gap_repaired");
		pc_tw_i64(out, (long long)px.spread_gap_repaired);
		/* fwd_fails: forwards that could not be sent */
		pc_tw_key(out, "fwd_fails");
		pc_tw_i64(out, (long long)px.fwd_fails);
		pc_tw_key(out, "migrate_skipped_big");
		pc_tw_i64(out, (long long)px.migrate_skipped_big);
		pc_tw_ctr(out, "cluster.repl_pushed", "repl_pushed", (unsigned long long)(px.repl_pushed));
		pc_tw_key(out, "repl_groups");
		pc_tw_i64(out, (long long)px.repl_groups);
		pc_tw_ctr(out, "cluster.repl_skipped_dying", "repl_skipped_dying", (unsigned long long)(px.repl_skipped_dying));
		{
			/* write-path replication on the relay port */
			int rdirect;
			unsigned long long sd, sc, rg, rr;

			pc_cluster_repl_plane_figures(&rdirect, &sd, &sc, &rg, &rr);
			pc_tw_key(out, "repl_plane_peers_direct");
			pc_tw_i64(out, rdirect);
			pc_tw_key(out, "repl_plane_sent_direct");
			pc_tw_i64(out, (long long)sd);
			pc_tw_key(out, "repl_plane_sent_cluster");
			pc_tw_i64(out, (long long)sc);
			pc_tw_key(out, "repl_plane_rx_groups");
			pc_tw_i64(out, (long long)rg);
			pc_tw_key(out, "repl_plane_rx_refused");
			pc_tw_i64(out, (long long)rr);
		}
		{                                   /* groups by TCP */
			static const char *const k[6] = { "repl_plane_tcp_sent",
				"repl_plane_tcp_fallbacks", "repl_plane_tcp_lost",
				"repl_plane_tcp_rx", "repl_plane_tcp_connects",
				"repl_plane_tcp_connect_fail" };
			unsigned long long v[6];
			int j;

			pc_cluster_repl_tcp_figures(v);
			for (j = 0; j < 6; j++) {
				pc_tw_key(out, k[j]);
				pc_tw_i64(out, (long long)v[j]);
			}
		}
			/* copies refused as not-newer; only climbing means a sender
			 * looping on records nobody will take */
			pc_tw_key(out, "recv_older");
			pc_tw_i64(out, (long long)px.recv_older);
			/* refused because a tombstone carries a newer version:
			 * each is a resurrection that did not happen */
			pc_tw_key(out, "recv_tombstoned");
			pc_tw_i64(out, (long long)px.recv_tombstoned);
			/* refused at the arena ceiling; stale
			 * copies dropped for them */
			pc_tw_key(out, "recv_nomem");
			pc_tw_i64(out, (long long)px.recv_nomem);
			pc_tw_key(out, "recv_stale_dropped");
			pc_tw_i64(out, (long long)px.recv_stale_dropped);
			pc_tw_key(out, "recv_below_dropped");
			pc_tw_i64(out, (long long)px.recv_below_dropped);
			/* an unconfirmed bulk batch holds this peer's sweep mark for the
			 * next cycle: climbing means the plane fails, not that records are lost */
			pc_tw_key(out, "repl_bulk_lost");
			pc_tw_i64(out, (long long)px.repl_bulk_lost);
			/* the applier's lag, said nowhere else: rx_queue against rx_rcvbuf is
			 * the buffer filling; nonzero rx_drops_ps is loss awaiting the repair sweep */
			pc_tw_ctr(out, "cluster.rx_applied", "rx_applied", (unsigned long long)(cs.rx_applied));
			pc_tw_key(out, "rx_applied_ps");
			pc_tw_i64(out, (long long)cs.rx_applied_ps);
			pc_tw_key(out, "rx_older_ps");
			pc_tw_i64(out, (long long)cs.rx_older_ps);
			pc_tw_ctr(out, "cluster.rx_drops", "rx_drops", (unsigned long long)(cs.rx_drops));
			pc_tw_key(out, "rx_drops_ps");
			pc_tw_i64(out, (long long)cs.rx_drops_ps);
			pc_tw_key(out, "rx_queue");
			pc_tw_i64(out, (long long)cs.rx_queue);
			pc_tw_key(out, "rx_rcvbuf");
			pc_tw_i64(out, (long long)cs.rx_rcvbuf);
			/* the parked-request table: a peak at capacity with refusals
			 * means route clients or raise it */
			pc_tw_key(out, "pend_peak");
			pc_tw_i64(out, (long long)cs.pend_peak);
			pc_tw_key(out, "win_pend_peak");
			pc_tw_i64(out, (long long)cs.pend_peak_win);
			pc_tw_key(out, "pend_max");
			pc_tw_i64(out, (long long)cs.pend_max);
			pc_tw_key(out, "pend_used");
			pc_tw_i64(out, (long long)cs.pend_used);
			pc_tw_ctr(out, "cluster.pend_exhausted", "pend_exhausted", (unsigned long long)(cs.pend_exhausted));
			/* the other two refusal causes, split so "forward
			 * failed" does not point at a healthy network */
			pc_tw_ctr(out, "cluster.fwd_no_route", "fwd_no_route", (unsigned long long)(cs.fwd_no_route));
			pc_tw_ctr(out, "cluster.fwd_send_fail", "fwd_send_fail", (unsigned long long)(cs.fwd_send_fail));
			pc_tw_key(out, "fwd_send_errno");
			pc_tw_i64(out, (long long)cs.fwd_send_errno);
			/* replies this node, as an OWNER, could not ship -
			 * answered "holder rejected", and the log says why */
			pc_tw_key(out, "fwd_reply_too_big");
			pc_tw_i64(out, (long long)cs.fwd_reply_too_big);
			/* the EFFECTIVE receive buffer: a kernel clamp means dropped forwards */
			pc_tw_key(out, "rcvbuf");
			pc_tw_i64(out, (long long)pc_cluster_rcvbuf());
		pc_tw_key(out, "bulk_out");
			pc_tw_i64(out, (long long)px.bulk_out);
			pc_tw_key(out, "bulk_in");
			pc_tw_i64(out, (long long)px.bulk_in);
			/* the bootstrap pull */
			pc_tw_key(out, "boot_out");
			pc_tw_i64(out, (long long)px.boot_out);
			pc_tw_key(out, "boot_in");
			pc_tw_i64(out, (long long)px.boot_in);
			pc_tw_key(out, "boot_failed");
			pc_tw_i64(out, (long long)px.boot_failed);
			pc_tw_key(out, "demotes_sent");
			pc_tw_i64(out, (long long)px.demotes_sent);
			pc_tw_key(out, "demotes_applied");
			pc_tw_i64(out, (long long)px.demotes_applied);
			pc_tw_key(out, "demotes_kept");
			pc_tw_i64(out, (long long)px.demotes_kept);
			pc_tw_key(out, "loc_hits");
			pc_tw_i64(out, (long long)px.loc_hits);
		}
		pc_tw_end(out);
	}
}
