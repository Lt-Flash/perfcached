/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * upstats.c - the read-through's figures: pc_upstream_stats_tree, a
 * collection's /stats block (its upstreams and their health, the
 * write-through writer, the sweeps, the S351 windows), and
 * pc_upstream_stats_reset, the S358 baselines it subtracts.  Reads the
 * upstream's state through upstreamint.h.  Moved from upstream.c
 * unchanged (the 0.7.2 split, wave 1 - doc/MODULARITY.md).
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "compat/timer.h"                /* get_ticks */
#include "core/pcache_htable.h"
#include "ptree.h"
#include "winctr_tw.h"                  /* S351 */
#include "upstream.h"
#include "upstreamint.h"

void pc_upstream_stats_tree(struct pc_tw *out, const char *col, size_t collen)
{
	struct up_col *uc = col_of(col, collen);
	unsigned long long n, oth = 0;
	long long up = 0, ents = 0;
	int k, trk = 1;
	static const char *const sname[] = { "serving", "not reached yet",
		"master fallback", "not used" };
	static const char *const rname[] = { "", "master", "replica", "other" };

	if (!uc)
		return;
	for (k = 0; k < uc->nsrv; k++) {
		const struct up_srv *s = &uc->srv[k];

		up += __atomic_load_n(&s->conns_up, __ATOMIC_RELAXED);
		ents += (long long)pcache_ht_entries(s->shadow);
		oth += ST_GET(s->other_server);
		/* one upstream: its tracking, as 0.6.0 said it; several:
		 * every one in use tracked (S382) */
		if (__atomic_load_n(&s->inv_id, __ATOMIC_ACQUIRE) <= 0 &&
		    (uc->nsrv == 1 || srv_state(s) != PC_UPS_OFF))
			trk = 0;
	}
	pc_tw_key(out, "upstream");
	pc_tw_map(out);
	/* the first upstream's address, as 0.6.0 had its only one */
	pc_tw_key(out, "host");
	pc_tw_str(out, uc->srv[0].host);
	pc_tw_key(out, "port");
	pc_tw_i64(out, uc->srv[0].port);
	pc_tw_key(out, "db");
	pc_tw_i64(out, uc->srv[0].db);
	/* S382: each upstream - what it is, what it may do, its share */
	pc_tw_key(out, "role_required");
	pc_tw_str(out, uc->role_req == PC_UP_ROLE_REPLICA ? "replica" : "any");
	pc_tw_key(out, "master_fallback");
	pc_tw_bool(out, uc->master_fallback);
	{
		/* S382 part 2: the mode, and in failover who is active */
		int act, fo = uc->mode == PC_UP_MODE_FAILOVER;
		long long since;
		unsigned long long nfo;

		pthread_mutex_lock(&uc->mx);
		act = uc->active;
		since = uc->active_since;
		nfo = uc->failovers;
		pthread_mutex_unlock(&uc->mx);
		pc_tw_key(out, "mode");
		pc_tw_str(out, fo ? "failover" : "balance");
		if (fo) {
			pc_tw_key(out, "primary");
			pc_tw_str(out, uc->srv[uc->primary].addr);
			pc_tw_key(out, "preempt");
			pc_tw_bool(out, uc->preempt);
			pc_tw_key(out, "preempt_hold_s");
			pc_tw_i64(out, uc->hold_ms / 1000);
			pc_tw_key(out, "active");
			pc_tw_str(out, act >= 0 ? uc->srv[act].addr : "");
			pc_tw_key(out, "active_since_ms");
			pc_tw_i64(out, since);
			pc_tw_key(out, "failovers");
			pc_tw_i64(out, (long long)nfo);
		}
	}
	/* S384: reads by another perfcached's read-through - a daisy-chain
	 * cut here, at one hop (5 min window) */
	pc_tw_ctr_col(out, col, collen, "up.", "chained_reads", ST_GET(uc->chained));
	pc_tw_key(out, "upstreams");
	pc_tw_arr(out);
	for (k = 0; k < uc->nsrv; k++) {
		const struct up_srv *s = &uc->srv[k];
		int st = srv_state(s), ro = __atomic_load_n(&s->role,
			__ATOMIC_RELAXED);
		long long lag = __atomic_load_n(&s->lag_ms, __ATOMIC_RELAXED);
		char sub[24];               /* "up.s" + an int + "." */

		pc_tw_map(out);
		pc_tw_key(out, "addr");
		pc_tw_str(out, s->addr);
		pc_tw_key(out, "role");
		pc_tw_str(out, ro >= 0 && ro <= 3 ? rname[ro] : "");
		pc_tw_key(out, "state");
		pc_tw_str(out, st >= 0 && st <= 3 ? sname[st] : "");
		pc_tw_key(out, "why");
		pc_tw_str(out, srv_why(s));
		pc_tw_key(out, "connections_up");
		pc_tw_i64(out, __atomic_load_n(&s->conns_up, __ATOMIC_RELAXED));
		/* S382 part 2: the failover roles; S384: a perfcached */
		pc_tw_key(out, "primary");
		pc_tw_bool(out, uc->mode == PC_UP_MODE_FAILOVER && k == uc->primary);
		pc_tw_key(out, "active");
		pc_tw_bool(out, uc->mode == PC_UP_MODE_FAILOVER &&
			k == __atomic_load_n(&uc->active, __ATOMIC_RELAXED));
		pc_tw_key(out, "perfcached");
		pc_tw_bool(out, __atomic_load_n(&s->is_pc, __ATOMIC_RELAXED));
		pc_tw_key(out, "tracking");
		pc_tw_bool(out, __atomic_load_n(&s->inv_id, __ATOMIC_ACQUIRE) > 0);
		/* each with its five-minute window: the page's share of
		 * fetches and average fetch are the last five minutes' */
		snprintf(sub, sizeof sub, "up.s%d.", k);
		pc_tw_ctr_col(out, col, collen, sub, "fetches", ST_GET(s->fetches));
		pc_tw_ctr_col(out, col, collen, sub, "errors",
			ST_GET(s->errors) + ST_GET(s->timeouts));
		pc_tw_ctr_col(out, col, collen, sub, "fetch_us_sum",
			ST_GET(s->lat_us_sum));
		pc_tw_ctr_col(out, col, collen, sub, "fetch_n", ST_GET(s->lat_n));
		pc_tw_key(out, "lag_ms");
		if (lag >= 0)
			pc_tw_i64(out, lag);
		else
			pc_tw_nil(out);
		pc_tw_key(out, "shadow_entries");
		pc_tw_i64(out, (long long)pcache_ht_entries(s->shadow));
		pc_tw_key(out, "flushes");
		pc_tw_i64(out, (long long)ST_GET(s->flushes));
		pc_tw_end(out);
	}
	pc_tw_end(out);
	pc_tw_key(out, "connections_up");
	pc_tw_i64(out, up);
	pc_tw_key(out, "connects");
	pc_tw_i64(out, (long long)UPV(uc, connects));
	pc_tw_key(out, "held");
	pc_tw_i64(out, (long long)UPV(uc, held));
	pc_tw_key(out, "joined");
	pc_tw_i64(out, (long long)UPV(uc, joined));
	pc_tw_ctr_col(out, col, collen, "up.", "fetches", UPV(uc, fetches));
	pc_tw_ctr_col(out, col, collen, "up.", "fill_string", UPV(uc, fill_string));
	pc_tw_ctr_col(out, col, collen, "up.", "fill_hash", UPV(uc, fill_hash));
	pc_tw_ctr_col(out, col, collen, "up.", "fill_json", UPV(uc, fill_json));
	pc_tw_ctr_col(out, col, collen, "up.", "absent", UPV(uc, absent));
	pc_tw_key(out, "wrongtype");
	pc_tw_i64(out, (long long)UPV(uc, wrongtype));
	pc_tw_key(out, "too_big");
	pc_tw_i64(out, (long long)UPV(uc, toobig));
	pc_tw_ctr_col(out, col, collen, "up.", "errors", UPV(uc, errors));
	pc_tw_ctr_col(out, col, collen, "up.", "timeouts", UPV(uc, timeouts));
	pc_tw_ctr_col(out, col, collen, "up.", "shadow_hits", UPV(uc, shadow_hits));
	pc_tw_ctr_col(out, col, collen, "up.", "pull_fetches", UPV(uc, pull_fetches));   /* 12nt */
	pc_tw_ctr_col(out, col, collen, "up.", "fwd_through", UPV(uc, fwd_through));     /* 12nt TC-2 */
	pc_tw_ctr_col(out, col, collen, "up.", "negative_hits", UPV(uc, neg_hits));
	/* ST2: keys found present by TYPE + PTTL alone (a probe asked), the
	 * probes those markers answered, and fetches queued again because a
	 * read joined one after it had stopped at the marker */
	pc_tw_ctr_col(out, col, collen, "up.", "fill_present", UPV(uc, fill_present));
	pc_tw_ctr_col(out, col, collen, "up.", "present_hits", UPV(uc, present_hits));
	pc_tw_key(out, "refetches");
	pc_tw_i64(out, (long long)UPV(uc, refetches));
	/* S424: fetches an invalidation of their key overtook, fetched again
	 * for it, and kept after three rounds (S433: for upstream_negative_ms
	 * at most) */
	pc_tw_ctr_col(out, col, collen, "up.", "inflight_invalidated", UPV(uc, inv_inflight));
	pc_tw_ctr_col(out, col, collen, "up.", "inflight_refetches", UPV(uc, inv_refetch));
	pc_tw_ctr_col(out, col, collen, "up.", "inflight_kept", UPV(uc, inv_kept));
	/* S435: reads whose key went between the gate and the read (an
	 * invalidation or an apply on another thread), gated again */
	pc_tw_ctr_col(out, col, collen, "up.", "read_lost", UPV(uc, read_lost));
	pc_tw_key(out, "writes");                                    /* S348 */
	pc_tw_str(out, uc->refuse ? "refuse" : uc->through ? "through" : "allow");
	pc_tw_ctr_col(out, col, collen, "up.", "writes_refused", UPV(uc, writes_refused));
	if (uc->through) {
		/* S385: the writer - its target, RedisJSON there, and what it
		 * forwarded (each with its window: the page reads 5 min) */
		int wm = __atomic_load_n(&uc->w_master, __ATOMIC_RELAXED),
			wj = __atomic_load_n(&uc->w_json, __ATOMIC_RELAXED),
			ws = __atomic_load_n(&uc->w_state, __ATOMIC_RELAXED);
		char why[160];

		pthread_mutex_lock(&uc->mx);
		memcpy(why, uc->w_why, sizeof why);
		pc_tw_key(out, "write_through");
		pc_tw_map(out);
		pc_tw_key(out, "queued");          /* AR2: + what the writer holds */
		pc_tw_i64(out, uc->wq_n + __atomic_load_n(&uc->w_inflight,
			__ATOMIC_RELAXED));
		pthread_mutex_unlock(&uc->mx);
		pc_tw_key(out, "target");
		if (wm >= 0 && wm < uc->nsrv)
			pc_tw_str(out, uc->srv[wm].addr);
		else
			pc_tw_nil(out);
		pc_tw_key(out, "state");
		pc_tw_str(out, upws_name[ws >= 0 && ws <= UPWS_DOWN ? ws : 0]);
		pc_tw_key(out, "why");
		pc_tw_str(out, wm >= 0 ? "" : why);
		pc_tw_key(out, "json");
		pc_tw_str(out, wj < 0 ? "unknown" : wj ? "yes" : "no");
		pc_tw_key(out, "unknown");
		pc_tw_str(out, uc->unknown_pass ? "pass" : "refuse");
		pc_tw_key(out, "pass_max_kb");
		pc_tw_i64(out, uc->pass_max / 1024);
		pc_tw_key(out, "queue_max");                   /* AR2 */
		pc_tw_i64(out, uc->wq_max);
		{
			int b = bcast_state(uc);               /* AR1 */

			pc_tw_key(out, "bcast");
			pc_tw_str(out, b > 0 ? "on" : b < 0 ? "refused" : "off");
		}
		{
			long long o = __atomic_load_n(&uc->wt_owned,
				__ATOMIC_RELAXED);

			/* AR4: keys the fleet owned before through, here */
			pc_tw_key(out, "fleet_owned");
			if (o < 0)
				pc_tw_nil(out);
			else
				pc_tw_i64(out, o);
		}
		pc_tw_key(out, "copies_marked");                /* S418 */
		pc_tw_i64(out, (long long)ST_GET(uc->copies_marked));
		pc_tw_key(out, "copies_premarked");             /* S418 */
		pc_tw_i64(out, (long long)ST_GET(uc->copies_premarked));
		pc_tw_key(out, "upfill");                      /* AR4 */
		pc_tw_map(out);
		pc_tw_key(out, "state");
		pc_tw_str(out, updr_name[uc->upf_state]);
		pc_tw_key(out, "started");
		pc_tw_i64(out, uc->upf_t0);
		pc_tw_key(out, "finished");
		pc_tw_i64(out, uc->upf_t1);
		pc_tw_key(out, "found");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_found));
		pc_tw_key(out, "written");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_written));
		pc_tw_key(out, "redis_had");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_kept));
		/* S400: of redis_had, how Redis's value compared; a check's
		 * keys Redis lacks (a run would write them) */
		pc_tw_key(out, "mode");
		pc_tw_str(out, uc->upf_check ? "check" : "fill");
		pc_tw_key(out, "redis_same");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_same));
		pc_tw_key(out, "redis_retimed");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_retimed));
		pc_tw_key(out, "redis_differed");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_differed));
		pc_tw_key(out, "missing");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_missing));
		/* S402: how each verdict on a key Redis had was reached -
		 * compared_text + compared_walked + uncompared = redis_had */
		pc_tw_key(out, "bytes");               /* S401: values offered */
		pc_tw_i64(out, __atomic_load_n(&uc->upf_bytes, __ATOMIC_RELAXED));
		pc_tw_key(out, "compared_text");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_cmp_text));
		pc_tw_key(out, "compared_walked");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_cmp_walked));
		pc_tw_key(out, "uncompared");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_uncompared));
		pc_tw_key(out, "skipped");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_skipped));
		pc_tw_key(out, "gone");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_gone));
		pc_tw_key(out, "errors");
		pc_tw_i64(out, (long long)ST_GET(uc->upf_errors));
		pc_tw_end(out);
		pc_tw_ctr_col(out, col, collen, "upw.", "forwarded", UPV(uc, wt_fwd));
		/* AR3: of them, sent in one round with others */
		pc_tw_ctr_col(out, col, collen, "upw.", "pipelined",
			UPV(uc, wt_pipelined));
		pc_tw_ctr_col(out, col, collen, "upw.", "applied", UPV(uc, wt_ok));
		pc_tw_ctr_col(out, col, collen, "upw.", "not_applied", UPV(uc, wt_nil));
		pc_tw_ctr_col(out, col, collen, "upw.", "redis_errors", UPV(uc, wt_rerr));
		pc_tw_ctr_col(out, col, collen, "upw.", "failed", UPV(uc, wt_failed));
		pc_tw_ctr_col(out, col, collen, "upw.", "timeouts", UPV(uc, wt_timeouts));
		pc_tw_ctr_col(out, col, collen, "upw.", "refused", UPV(uc, wt_refused));
		pc_tw_ctr_col(out, col, collen, "upw.", "json_refused",
			UPV(uc, wt_json_refused));
		pc_tw_ctr_col(out, col, collen, "upw.", "read_back_values",
			UPV(uc, wt_rb_value));
		pc_tw_ctr_col(out, col, collen, "upw.", "read_back_gone",
			UPV(uc, wt_rb_gone));
		pc_tw_ctr_col(out, col, collen, "upw.", "read_back_released",
			UPV(uc, wt_rb_release));
		pc_tw_ctr_col(out, col, collen, "upw.", "passed_writes", UPV(uc, pass_w));
		pc_tw_ctr_col(out, col, collen, "upw.", "passed_reads", UPV(uc, pass_r));
		pc_tw_ctr_col(out, col, collen, "upw.", "pass_refused",
			UPV(uc, pass_refused));
		pc_tw_ctr_col(out, col, collen, "upw.", "checks", UPV(uc, wt_checks));
		/* AR2: refused at a full queue; answered "not applied" after
		 * waiting past upstream_timeout_ms, never sent */
		pc_tw_ctr_col(out, col, collen, "upw.", "queue_full", UPV(uc, wt_qfull));
		pc_tw_ctr_col(out, col, collen, "upw.", "queue_expired",
			UPV(uc, wt_qexpired));
		/* AR1: records dropped because Redis said the key changed, and
		 * the flushes after a gap in the tracking */
		pc_tw_ctr_col(out, col, collen, "upw.", "invalidated", UPV(uc, wt_inval));
		/* S422: refetches after the echo of this node's own write */
		pc_tw_ctr_col(out, col, collen, "upw.", "echo_refetches", UPV(uc, wt_echo));
		/* S423: fetches inside the echo window sent to the master */
		pc_tw_ctr_col(out, col, collen, "upw.", "echo_master_fetches",
			UPV(uc, wt_echo_master));
		/* S429: read-backs applied out of order, skipped */
		pc_tw_ctr_col(out, col, collen, "upw.", "stale_readbacks_skipped",
			UPV(uc, wt_stale));
		pc_tw_ctr_col(out, col, collen, "upw.", "inval_flushes",
			UPV(uc, wt_invall));
		/* S404: a written-through key's mark found here without its
		 * record - dropped, and the key read from Redis */
		pc_tw_ctr_col(out, col, collen, "upw.", "bare_marks",
			UPV(uc, wt_bare));
		{
			/* S403: the writer thread's CPU, and its share of the last
			 * five minutes - one thread carries every write-through
			 * write of the collection, and an upfill sweep with them */
			unsigned long long cu = __atomic_load_n(&uc->wt_cpu_us,
				__ATOMIC_RELAXED);
			unsigned int secs = 0;
			char wk[160], pct[24];
			long long d;
			int pn;

			snprintf(wk, sizeof wk, "col.%.*s.upw.writer_cpu_us",
				(int)collen, col);
			d = pc_win_delta(wk, cu, 1, &secs);
			pc_tw_key(out, "writer_cpu_us");
			pc_tw_i64(out, (long long)cu);
			pc_tw_key(out, "writer_busy_pct");
			if (d < 0 || !secs) {
				pc_tw_nil(out);
			} else {
				pn = snprintf(pct, sizeof pct, "%.1f",
					(double)d / ((double)secs * 1e4));
				pc_tw_num(out, pct, (size_t)pn);
			}
		}
		pc_tw_ctr_col(out, col, collen, "upw.", "us_sum", UPV(uc, wt_us_sum));
		pc_tw_ctr_col(out, col, collen, "upw.", "n", UPV(uc, wt_n));
		/* the slowest write in 5 min, queued to answered */
		pc_tw_key(out, "win_max_us");
		pc_tw_i64(out, (long long)pc_winmax_get(&uc->w_maxus, get_ticks()));
		/* by command, as sent to Redis (each with its window) */
		pc_tw_key(out, "by_command");
		pc_tw_arr(out);
		{
			static const char *const wcn[WC_N] = { "applied",
				"not_applied", "redis_errors", "failed", "us_sum", "n" };
			int r, f, nr = __atomic_load_n(&uc->nwcmd, __ATOMIC_ACQUIRE);

			for (r = 0; r < nr; r++) {
				const struct wt_cmd *wc = &uc->wcmd[r];
				char sub[48];

				snprintf(sub, sizeof sub, "upw.c.%s.", wc->name);
				pc_tw_map(out);
				pc_tw_key(out, "name");
				pc_tw_str(out, wc->name);
				pc_tw_key(out, "passed");
				pc_tw_bool(out, wc->passed);
				for (f = 0; f < WC_N; f++)
					pc_tw_ctr_col(out, col, collen, sub, wcn[f],
						ST_GET(wc->c[f]) - wc->base[f]);
				pc_tw_end(out);
			}
		}
		pc_tw_end(out);
		/* the last errors, the newest first: what Redis said, or why
		 * the write was not done or refused here */
		pc_tw_key(out, "last_errors");
		pc_tw_arr(out);
		{
			struct wt_err e[WT_NERR];
			unsigned int en, i;

			pthread_mutex_lock(&uc->mx);
			en = uc->werr_n;
			memcpy(e, uc->werr, sizeof e);
			pthread_mutex_unlock(&uc->mx);
			for (i = 0; i < en && i < WT_NERR; i++) {
				const struct wt_err *x = &e[(en - 1 - i) % WT_NERR];

				pc_tw_map(out);
				pc_tw_key(out, "at");
				pc_tw_i64(out, x->at);
				pc_tw_key(out, "age_s");      /* the page's "ago" */
				pc_tw_i64(out, (long long)time(NULL) - x->at);
				pc_tw_key(out, "kind");
				pc_tw_str(out, x->kind);
				pc_tw_key(out, "command");
				pc_tw_str(out, x->cmd);
				pc_tw_key(out, "key");
				pc_tw_str(out, x->key);
				pc_tw_key(out, "message");
				pc_tw_str(out, x->msg);
				pc_tw_end(out);
			}
		}
		pc_tw_end(out);
		pc_tw_end(out);
	}
	/* every upstream in use tracked (S382) */
	pc_tw_key(out, "tracking");
	pc_tw_bool(out, trk);
	{
		char run[41];

		pthread_mutex_lock(&uc->srv[0].run_mx);
		memcpy(run, uc->srv[0].inv_run, sizeof run);
		pthread_mutex_unlock(&uc->srv[0].run_mx);
		pc_tw_key(out, "server");
		pc_tw_str(out, run);
	}
	pc_tw_key(out, "tracking_other_server");
	pc_tw_i64(out, (long long)oth);
	pc_tw_key(out, "invalidations");
	pc_tw_i64(out, (long long)UPV(uc, invalidations));
	pc_tw_key(out, "flushes");
	pc_tw_i64(out, (long long)UPV(uc, flushes));
	pc_tw_key(out, "untracked_fills");
	pc_tw_i64(out, (long long)UPV(uc, untracked));
	/* S421: the re-warm after a tracking gap */
	pc_tw_key(out, "rewarm");
	pc_tw_map(out);
	pc_tw_key(out, "state");
	pc_tw_str(out, uc->rw_n ? "running" : "idle");
	pc_tw_key(out, "queued");
	pc_tw_i64(out, (long long)(uc->rw_n - uc->rw_head));
	pc_tw_key(out, "started");
	pc_tw_i64(out, (long long)uc->rw_started);
	pc_tw_key(out, "skipped");
	pc_tw_i64(out, (long long)uc->rw_skipped);
	pc_tw_key(out, "dropped");
	pc_tw_i64(out, (long long)uc->rw_dropped);
	pc_tw_key(out, "runs");
	pc_tw_i64(out, (long long)uc->rw_runs);
	pc_tw_key(out, "requeued");                                  /* S431 */
	pc_tw_i64(out, (long long)uc->rw_requeued);
	pc_tw_key(out, "max");
	pc_tw_i64(out, (long long)uc->rewarm_max);
	pc_tw_end(out);
	pc_tw_key(out, "drain");
	pc_tw_map(out);
	pc_tw_key(out, "state");
	pc_tw_str(out, updr_name[uc->drain_state]);
	pc_tw_key(out, "started");
	pc_tw_i64(out, uc->drain_t0);
	pc_tw_key(out, "finished");
	pc_tw_i64(out, uc->drain_t1);
	pc_tw_key(out, "read");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_scanned));
	pc_tw_key(out, "bytes");                       /* S401: values read */
	pc_tw_i64(out, __atomic_load_n(&uc->dr_bytes, __ATOMIC_RELAXED));
	pc_tw_key(out, "taken_over");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_pulled));
	pc_tw_key(out, "fleet_had");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_owned));
	pc_tw_key(out, "gone");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_absent));
	pc_tw_key(out, "wrongtype");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_wrongtype));
	pc_tw_key(out, "too_big");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_toobig));
	pc_tw_key(out, "elsewhere");                   /* 12nt TC-4: other nodes' */
	pc_tw_i64(out, (long long)ST_GET(uc->dr_elsewhere));
	pc_tw_key(out, "errors");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_errors));
	pc_tw_end(out);
	pc_tw_key(out, "promoted");
	pc_tw_i64(out, (long long)UPV(uc, promoted));
	pc_tw_key(out, "nofall_written");
	pc_tw_i64(out, (long long)UPV(uc, nofall_written));
	pc_tw_key(out, "nofall_hits");
	pc_tw_i64(out, (long long)UPV(uc, nofall_hits));
	pc_tw_key(out, "ownership_checks");
	pc_tw_i64(out, (long long)UPV(uc, own_checks));
	pc_tw_key(out, "ownership_found");
	pc_tw_i64(out, (long long)UPV(uc, own_found));
	pc_tw_key(out, "fill_failed");
	pc_tw_i64(out, (long long)UPV(uc, fill_failed));
	pc_tw_key(out, "shadow_entries");
	pc_tw_i64(out, ents);
	n = UPV(uc, lat_n);
	pc_tw_key(out, "fetch_us_avg");
	pc_tw_i64(out, n ? (long long)(UPV(uc, lat_us_sum) / n) : 0);
	/* S351: the sum and the count, each with its window - the page's
	 * average fetch is the last five minutes' */
	pc_tw_ctr_col(out, col, collen, "up.", "fetch_us_sum", UPV(uc, lat_us_sum));
	pc_tw_ctr_col(out, col, collen, "up.", "fetch_n", n);
	pc_tw_end(out);
}

/* S358: the stats reset - every collection's counters count from here in
 * /stats (not /metrics, which does not carry them).  The daemon serialises
 * resets; a worker counting concurrently lands on either side of it. */
void pc_upstream_stats_reset(void)
{
	int i;

	for (i = 0; i < UP_MAX_COLS; i++) {
		struct up_col *uc = &up_cols[i];

		uc->base.held = ST_GET(uc->held);
		uc->base.fetches = ST_GET(uc->fetches);
		uc->base.fill_string = ST_GET(uc->fill_string);
		uc->base.fill_hash = ST_GET(uc->fill_hash);
		uc->base.fill_json = ST_GET(uc->fill_json);
		uc->base.absent = ST_GET(uc->absent);
		uc->base.wrongtype = ST_GET(uc->wrongtype);
		uc->base.toobig = ST_GET(uc->toobig);
		uc->base.errors = ST_GET(uc->errors);
		uc->base.timeouts = ST_GET(uc->timeouts);
		uc->base.connects = ST_GET(uc->connects);
		uc->base.shadow_hits = ST_GET(uc->shadow_hits);
		uc->base.pull_fetches = ST_GET(uc->pull_fetches);   /* 12nt */
		uc->base.fwd_through = ST_GET(uc->fwd_through);     /* 12nt TC-2 */
		uc->base.neg_hits = ST_GET(uc->neg_hits);
		uc->base.fill_present = ST_GET(uc->fill_present);     /* ST2 */
		uc->base.present_hits = ST_GET(uc->present_hits);
		uc->base.refetches = ST_GET(uc->refetches);
		uc->base.inv_inflight = ST_GET(uc->inv_inflight);   /* S424 */
		uc->base.inv_refetch = ST_GET(uc->inv_refetch);
		uc->base.inv_kept = ST_GET(uc->inv_kept);
		uc->base.read_lost = ST_GET(uc->read_lost);         /* S435 */
		uc->base.joined = ST_GET(uc->joined);
		uc->base.fill_failed = ST_GET(uc->fill_failed);
		uc->base.lat_us_sum = ST_GET(uc->lat_us_sum);
		uc->base.lat_n = ST_GET(uc->lat_n);
		uc->base.promoted = ST_GET(uc->promoted);
		uc->base.nofall_written = ST_GET(uc->nofall_written);
		uc->base.nofall_hits = ST_GET(uc->nofall_hits);
		uc->base.own_checks = ST_GET(uc->own_checks);
		uc->base.own_found = ST_GET(uc->own_found);
		uc->base.writes_refused = ST_GET(uc->writes_refused);
		uc->base.invalidations = ST_GET(uc->invalidations);
		uc->base.flushes = ST_GET(uc->flushes);
		uc->base.untracked = ST_GET(uc->untracked);
		uc->base.wt_fwd = ST_GET(uc->wt_fwd);
		uc->base.wt_ok = ST_GET(uc->wt_ok);
		uc->base.wt_nil = ST_GET(uc->wt_nil);
		uc->base.wt_rerr = ST_GET(uc->wt_rerr);
		uc->base.wt_failed = ST_GET(uc->wt_failed);
		uc->base.wt_timeouts = ST_GET(uc->wt_timeouts);
		uc->base.wt_refused = ST_GET(uc->wt_refused);
		uc->base.wt_json_refused = ST_GET(uc->wt_json_refused);
		uc->base.wt_us_sum = ST_GET(uc->wt_us_sum);
		uc->base.wt_n = ST_GET(uc->wt_n);
		uc->base.wt_rb_value = ST_GET(uc->wt_rb_value);
		uc->base.wt_rb_gone = ST_GET(uc->wt_rb_gone);
		uc->base.wt_rb_release = ST_GET(uc->wt_rb_release);
		uc->base.pass_w = ST_GET(uc->pass_w);
		uc->base.pass_r = ST_GET(uc->pass_r);
		uc->base.pass_refused = ST_GET(uc->pass_refused);
		uc->base.wt_checks = ST_GET(uc->wt_checks);
		uc->base.wt_qfull = ST_GET(uc->wt_qfull);       /* AR2 */
		uc->base.wt_pipelined = ST_GET(uc->wt_pipelined); /* AR3 */
		uc->base.wt_qexpired = ST_GET(uc->wt_qexpired);
		uc->base.wt_inval = ST_GET(uc->wt_inval);       /* AR1 */
		uc->base.wt_echo = ST_GET(uc->wt_echo);         /* S422 */
		uc->base.wt_echo_master = ST_GET(uc->wt_echo_master);   /* S423 */
		uc->base.wt_stale = ST_GET(uc->wt_stale);               /* S429 */
		uc->base.wt_invall = ST_GET(uc->wt_invall);
		uc->base.wt_bare = ST_GET(uc->wt_bare);         /* S404 */
		{
			int r, f, nr = __atomic_load_n(&uc->nwcmd, __ATOMIC_ACQUIRE);

			for (r = 0; r < nr; r++)
				for (f = 0; f < WC_N; f++)
					uc->wcmd[r].base[f] = ST_GET(uc->wcmd[r].c[f]);
		}
		if (uc->through) {
			pthread_mutex_lock(&uc->mx);
			uc->werr_n = 0;
			pthread_mutex_unlock(&uc->mx);
		}
	}
}
