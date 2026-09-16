# perfdump file set, format version 1

A dump is a directory. `manifest.json` names every chunk file with its
collection, the bucket range its walker owned, its record and byte
counts, a CRC-32 of its uncompressed bytes, its compression, and whether
it is complete. A chunk is complete only once its trailer and its
manifest entry are written: a restart re-dumps what is not complete.

## Chunk files

`<collection>.<nnnn>.pcd`, or `.pcd.zst` when compressed (the same bytes
through `zstd`; `zstd -dc` recovers them). Little-endian throughout.

```
"PCD1"                              magic, 4 bytes
record*:
  u32 klen                          key length
  u32 vlen                          value length
  u64 expiry                        absolute unix time in milliseconds, 0 = never
  u64 version                       the record's Lamport version as dumped
  u8  flags                         reserved, 0
  key[klen] value[vlen]
trailer:
  u32 0xFFFFFFFF                    no record has a key this long
  u64 records                       the count in this file
```

The CRC-32 (IEEE, the zlib polynomial) covers everything from the magic
to the end of the trailer, uncompressed. `perfdump --inspect DIR` reads
every file back, decompressing through the `zstd` binary, and checks the
count against the trailer and the manifest and the checksum against the
manifest.

## Manifest

```json
{"format":"perfdump-format","format_version":1,"tool":"perfdump 0.1",
 "source":"192.0.2.10:6479","started":1788930000,"threads":4,
 "collections":[{"name":"sbcha","buckets":262144}],
 "files":[{"file":"sbcha.0000.pcd.zst","collection":"sbcha",
           "bucket_lo":0,"bucket_hi":65536,"records":12345,"bytes":3456789,
           "crc32":1234567890,"zstd":3,"complete":true}],
 "records":12345,"bytes":3456789}
```

`bucket_lo`/`bucket_hi` are the walker's range in the table's buckets at
the time of the dump; `bucket_hi` equal to the collection's bucket count
marks the walker that also carried the tail (the last bucket and the
overflow leg).

## What the walk promises

Each record once, with the value, expiry and version the daemon held
when its chunk was read, while the table is quiet. A table that grows
under the dump (the maintenance thread splits buckets once a collection
passes four records a bucket) can repeat a record: a split moves half a
bucket's records ahead of the walk's cursor, where the walk sees them
again. That is the cursor's at-least-once contract under a resize, the
same one Redis `SCAN` has, and `perfload` upserts by version, so a
repeated record costs bytes, never correctness. Records expiring during
the dump are absent or present as the walk found them; a value written
during the dump is present as of its chunk.

## Loading it back

`perfload DIR --to HOST:PORT` reads the manifest, verifies every
complete chunk's count and CRC-32, and sends the records in `restore`
batches with their expiry and version as the file holds them.  The
daemon rebases the expiry onto its own clock and installs the record
under the file's version, so the copy a node ends up with is the copy
the dump read, however many nodes or which mode the target has.

`DIR/perfload.done` is the loader's side file: one line per chunk whose
last batch was acknowledged (`file records target policy`).  A run
skips the chunks listed; `--restart` ignores the file.  The at-least-
once repeats a growing table can put into a dump cost nothing on the
way back: a record already held at the same version is refused by the
store as "as old" and counted `older`, never installed twice.
