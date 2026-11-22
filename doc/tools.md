# Tools

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

## perfcli

The redis-cli analogue.  Word commands mirror the verb set; `-a` runs
the Noise handshake (client principal) for listeners that ask for the
secret, and `--encrypt` keeps the connection encrypted after it
(`[listen] encrypt` in [config.md](config.md)):

    perfcli -p 6479 set sessions user:17 "some value" 300
    perfcli -a 's3cret' get sessions user:17
    printf 'ping\nstats\n' | perfcli -q          # pipe mode
    perfcli -j '{"method":"keys","params":{"col":"sessions","match":"user:*"}}'

`help` inside the REPL lists everything; jset takes raw JSON.  Every
command, its arguments and its reply shape:
[PERFCLI-COMMANDS.md](../PERFCLI-COMMANDS.md).  The
REPL has its own line editor - arrows + history (persisted 0600 in
`~/.perfcli_history`, duplicates collapsed), emacs keys
(Ctrl-A/E/B/F/W/U/K/L), Ctrl-R reverse search, Ctrl-C discards the
line, Ctrl-D quits.

`pretty on` (or `-P`) re-indents every JSON result - together with the
JSON path verbs:

    $ perfcli -p 6479 jset sessions user:18 '$' \
        '{"name":"ann","roles":["admin","ops"],"quota":{"used":3,"max":10}}'
    {"set":true}
    $ perfcli -p 6479 -P jget sessions user:18 '$.quota'
    {
      "found": true,
      "value": {
        "used": 3,
        "max": 10
      }
    }

## perfdump

A parallel dumper over the client door, shaped like mydumper: one node's
collections walked by N connections, each owning a range of the table's
buckets, into a directory of chunk files with a manifest.

    perfdump --from 192.0.2.10:6479 -a <client secret> --out /var/backups/pc-$(date +%F)
    perfdump --inspect /var/backups/pc-2026-09-09

`--threads` (default 4) connections per collection, `--count` buckets per
call (default 1024, halved on the daemon's refusal), `--chunk-mb` (64)
before compression, `--zstd` level (3; 0 = raw), `--rate` records per
second per connection so a dump never starves production,
`--collections a,b` to pick.  Every chunk carries a CRC-32 and a trailer,
the manifest is rewritten atomically as each chunk completes, and
`--inspect` reads every file back and checks both.  The format is
[doc/perfdump-format.md](perfdump-format.md); `perfload` loads it
back.  On an eager fleet any READY node holds everything, so dump from
one; a node still pulling its bootstrap is refused as a source.  A
table that grows under the dump can repeat a record: growth splits
buckets in place, and a split can move records ahead of the cursor
(at-least-once, as Redis SCAN is); the loader upserts by version, so a
repeat costs bytes.  A RESIZE is different - an operator's `resize`, or
the automatic shrink after mass deletes, swaps in a table of another
shape - and a walk carried across one used to repeat records or
silently LOSE them (measured: a shrink between two chunks lost 2,200 of
3,000).  Since 0.4.0 every chunk names the table it walked, the
daemon refuses a chunk once the collection has been swapped, and
perfdump dumps that collection again from scratch (it says so, and gives
up after five swaps in a row).  Needs the `zstd` binary for compressed
dumps.

## perfload

The loader, the reverse of perfdump: every record goes back with the
value, its type (string, JSON, hash, rate limit or lock), the VERSION and
the absolute expiry the dump held, up to the store's record limit, through the
daemon's write path (the WAL, the eager push), so a fleet of any size
or mode takes it as if a client had written it - only with the dump's
history.  Load a three-node eager fleet's dump into a two-node shard
fleet, or the other way round.  The dump format is version 2 from 0.7.5;
perfload and `perfdump --inspect` refuse an older dump - take it again
from the source.

    perfload /var/backups/pc-2026-09-09 --to 192.0.2.10:6479 -a <client secret> --enable <enable>
    perfload /var/backups/pc-2026-09-09 --to 192.0.2.10:6479 -a <client secret> --dry-run
    perfload /var/backups/pc-2026-09-09 --to 192.0.2.10:6479 -a <client secret> --enable <enable> --collection-map sessions=sessions_restored

Every chunk's count and checksum is verified before anything is sent
(`--no-verify` skips it).  `--threads` (default 4) connections stream
`restore` batches of `--batch` records (256) at `--depth` calls in
flight (8); `--rate` caps records per second over all threads.  On a
shard fleet each record goes straight to its owner (the library
routes it); on an eager or plain fleet the chunks are spread across
the members and the push carries the rest.  A record the daemon does
not own comes back named and is re-sent to the node it names.

`--policy newer` (default) is the store's own rule: a record older
than, or as old as, the copy the fleet holds loses and is counted.
`--policy skip` leaves every existing key alone.  `--policy overwrite`
installs the loaded value under a fresh version, so every replica
takes it - a fleet rolled back to a dump.  A record whose expiry has
passed is skipped and counted, never re-based.  `--collections a,b`
picks, `--collection-map a=b` loads a's chunks into b; the target must
already have the collection.

A chunk is marked done in `DIR/perfload.done` once its last batch is
acknowledged, and the next run skips it (`--restart` reloads
everything); with the default policy a `--restart` over a finished load
stores nothing and counts everything older, so a loader killed halfway
is simply run again.  The final line has records, bytes, elapsed,
records per second and the counts (stored, older, existing, expired,
refused, bad, rerouted); on a fleet a second line says how long the
members took to settle after the load.  `--via-set` loads through
plain pipelined `set` instead - the comparator: it re-bases TTLs and
assigns fresh versions, the two things a loader must not do.
Measured on the build host: a million 224-byte records in 0.8 s on
eight threads, 1.4x faster than plain SET on the same connections.
