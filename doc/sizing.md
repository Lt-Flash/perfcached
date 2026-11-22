# Sizing

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

What a set of collections costs on a host, per node - in eager mode every
node holds everything, so the figure is per node, not per fleet.  The
page's "memory budget" card computes the same arithmetic from the
daemon's exact figures; this is how to do it before the daemon exists.

1. **Index.**  Each collection's table is carved at creation.
   `buckets_log2` is 4..24 - the same range in a config file and at the
   `create` and `resize` verbs - but the cost is FLAT below 2^12 and 68
   bytes a bucket above it, because segments are fixed at 4,096 buckets
   and a table always carves whole ones:

   | buckets_log2 | buckets | index | per bucket |
   |---|---|---|---|
   | 4 .. 12 | 16 .. 4,096 | 1.0 MB | - |
   | 16 | 65,536 | 4.75 MB | 76 B |
   | 18 | 262,144 | 17.5 MB | 70 B |
   | 20 | 1,048,576 | 68.5 MB | 68.5 B |
   | 24 | 16,777,216 | 1.1 GB | 68 B |

   So anything below 12 buys nothing over 12, and the top of the range
   is measured in gigabytes - all of it carved before a single record.
   A table grows by splitting once it passes `grow_at_pct` of its slot
   capacity (`[daemon]`, default 75% - six slots a bucket, so 4.5
   entries), one bucket at a time, and growth adds index.  It shrinks
   too: once it has sat below `shrink_at_pct` (default a quarter of
   `grow_at_pct`) for `shrink_cooloff_s` (default 60 s) since it last
   split, was resized or started, it is resized down to the power of
   two midway between the two thresholds - one copy, never below one
   segment - and the old index goes back to the arena once every reader
   has moved on.  An index given back (a shrink, a resize, a `drop`) is
   kept for the next index; records can use its room again only once it
   borders the free space, which it does when nothing was carved after
   it (0.7.2).  `/stats` says which: `memory.arena_free` is the room
   records can still have, `memory.arena_free_tables` the given-back
   index space kept for tables (`perfcached_arena_regions_free_bytes`),
   and neither counts in `arena_used`.  A restart puts everything back.
   A table may also be widened PAST `grow_at_pct` by its
   own chains: a bucket whose six slots are full links an overflow
   bucket of its own (0.7.4; 64 bytes for up to five records, carved
   from the arena as needed), found under that bucket's own lock and
   never another's, and a split lays a bucket's chain into both halves.
   The records in chains are counted (`overflow`, and `leg_stuck` under
   its old name).  Once they exceed `leg_stuck_pct` of the entries
   (default 1%) the splitter aims at `grow_floor_pct` (default 50%)
   instead, which is the only thing that shortens them - those buckets
   are full.  Set `grow_floor_pct = 0` to turn it off; a table whose
   chains are quiet never notices it.  All of that
   is the `auto` **autoscale** mode, the default.  `autoscale = floor`
   is `auto` with a floor: it grows the same way, but never shrinks the
   table below the size the operator gave it - `buckets_log2` in its
   `[collection]` section, the size of a `create`, or an explicit
   `resize`, which becomes the new floor (an automatic resize never
   moves it).  A collection sized for the keys it expects keeps that
   size while it is still empty, and after a burst comes back down to
   it, not below; `/stats` carries the floor as `autoscale_floor_log2`.
   `autoscale = warn`
   (`[collection x]`, or `[daemon]` for the default) changes nothing and
   says what auto would do, in the log - at once, then at most every ten
   minutes - on the status page (an amber `warn -> 2^N` beside the
   collection) and in `/metrics`
   (`perfcached_collection_autoscale_target_buckets`); `autoscale = off`
   changes nothing and says nothing.  Under both, growth is blocked too:
   keys past the slots go to their buckets' chains, and the page shows
   that collection red, because every lookup in a long chain walks it -
   a bucket at a time, under that bucket's lock, so it costs that
   bucket's keys and nobody else's (before 0.7.4 the overflow leg was
   one lock for the whole table).  `perfcli -E ... autoscale <col>
   <auto|floor|warn|off>` sets it at runtime; it is persisted and outranks the
   config from then on.  In a fleet (0.7.2) the mode set this way is the
   fleet's: it reaches every member, a member that was away takes it when
   it is back, and the verb is refused while a member runs an older build
   (it names that member).  `/stats` shows each collection's
   `autoscale_set` and `autoscale_gen`, and `/members` each member's
   `autoscale_differs`; the alert `cluster.autoscale` says when a member
   holds other modes.  The sizing itself stays each member's: under `auto`
   a member sizes the table for the share of the keys it holds.  A create or a resize whose index would not fit under
   the ceiling in item 4 is refused, naming both figures.
   A read-through collection carves one more table for each upstream
   server, configured or discovered (a Redis Cluster's nodes, the servers
   a Sentinel or a master's ROLE names): its fetched copies, sized like
   the collection's own - 1.0 MB at the least.  A six-node cluster
   behind three collections is 18 such tables, 18 MB, before a copy is
   held.  A server whose table does not fit is not used, and the log
   says so; a copy that does not fit is not kept: the read that fetched
   it is answered the value all the same, and the next read of the key
   fetches again (`fill_failed` in the collection's upstream block counts
   them).  A change to such a key needs room for it here, and is refused
   without it.
2. **Records.**  A record is its key, its value and a 28-byte header,
   rounded UP to a cell class.  The classes step by at most 1.5 from
   64 bytes to 64 KB (64, 96, 128, 192, 256, 320, 384, 448, 512, 640,
   768, 896, 1 K, 1.5 K, 2 K, 3 K, 4 K, 6 K, 8 K, 12 K, 16 K, 24 K, 32 K,
   48 K, 64 K) and then to 96 K and 255.9 K - 27 classes - so the
   rounding is anywhere from 0 to 50 % - and
   up to a whole 256 KB chunk for the last two, which are cut one and
   two cells to a chunk.  **The largest record is 262,080 bytes**, key
   and header included: one cell per chunk, which is the most a 256 KB
   chunk can hold.  A larger value is refused, not truncated.  A measured mix - 100,000 records
   of 96 B, 768 B, 2 KB and 4 KB values, 62.9 MB of values - occupies
   94 MB.  Budget 1.5 x the key+value bytes for a mixed set, or bin by
   bin from your own size histogram.
3. **Residue after a burst.**  About 10 MB of warm free slots kept
   resident for the next growth, and up to 13.5 MB of chunks the size
   classes own (two 256 KB chunks for each of the 27 classes).
4. **The ceiling.**  1 + 2 + 3 at the PEAK must fit under `arena_mb` (or
   `arena_cap_mb`): a full arena refuses writes and evicts nothing.  An
   overwrite whose value fits the cell the key already holds still
   lands (`nomem_inplace`, inside `nomem`).  In a fleet, a node that
   cannot store a peer's newer copy of a key drops the older copy it
   holds instead of serving it (`nomem_stale_dropped`), and a read of
   that key pulls from a peer.  A full node answers correctly, but it is
   not a whole replica: size every eager node for the whole data set.
5. **What the host sees.**  The arena reserves `arena_cap_mb` (or
   `arena_mb` when no cap is set) of address space, charged for
   nothing, and commits `arena_mb` of it at start - populated, and
   pinned when the daemon may pin - so resident memory is `arena_mb`
   plus about 15 MB for the daemon from the first second.  Growth
   commits 2 MB groups inside the reservation as records arrive, all on
   the same tier; once idle the give-back returns the never-carved part
   of the commit, and after a burst it returns the drained groups, so
   resident follows `arena_committed` (in `stats` and on `/metrics`) to
   within a few 2 MB groups.  The arena's pages are counted in RSS when
   the tier is transparent huge pages (the default when no hugetlb pool
   exists); on a hugetlb pool they are in `HugetlbPages` instead and RSS
   shows only the daemon itself.

Read it back on the page: `held` and its parts (structure, class
chunks, warm free), the budget card, and `resident` from the kernel.
