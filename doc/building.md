# Building from source

Part of the perfcached reference; [GUIDE.md](../GUIDE.md) is the task
guide and lists every reference document.

## What it needs

A C compiler, make and libsodium.  Where pkg-config finds OpenSSL
(libssl-dev; Alpine `openssl-dev`) the daemon is built with TLS for the
RESP and HTTP doors and `rediss://` upstreams (`make PC_TLS=yes` insists
on it, `PC_TLS=no` leaves it out).

```
make                 # perfcached, perfcli, perfdump, perfload, libperfd.a
                     # (+ perfcached-dashboard where libmicrohttpd-dev is installed)
make libperfd.so     # the shared library beside the archive
sudo make install    # binaries, libperfd, example config, systemd unit
sudo systemd-sysusers  # the perfcached account the unit runs as
```

## Choosing a build

`make menuconfig` chooses which daemon `make` builds and saves the
choice in `config.mk`:

| edition | build it for | what you get |
|---|---|---|
| `clustered` (the default) | anything that may grow beyond one node | every cluster mode: eager, store, shard, proxy, spread |
| `standalone` | a cache that will only ever be one node | no cluster code at all: the binary is about a quarter smaller, its static memory 13 MB less, and a `[cluster]` section in the config is refused rather than ignored |

Without a `config.mk` the build is `clustered`.  The `.deb` packages
come in both editions: `perfcached` and `perfcached-standalone`.  To
choose without the menu, for scripts:

```
make menuconfig                      # interactive (whiptail, dialog or plain prompts)
sh tools/menuconfig.sh standalone    # the same, non-interactive
make PC_EDITION=standalone           # one build, without writing config.mk
```

A standalone daemon reports `standalone` in `perfcached -V` and
`"edition"` in `/stats`.  Its tests are `make check-standalone`; the
clustered suites need the cluster code and `make check` says so.

**The allocator** is the second thing `make menuconfig` asks (or `make
PC_ALLOC=libc|mimalloc|jemalloc`): the C library's own malloc by
default, or mimalloc 2.x or jemalloc linked ahead of it.  The C library
itself is the toolchain's - glibc on Debian, musl on Alpine.
`perfcached -V` names a non-default allocator, and `/stats` carries
`process.allocator`.  After changing it on the command line rather than
through `make menuconfig`, `make clean` first.

**The build flavor** is the third question (or `make
PC_BUILD=release|debug|asan`): `release` (`-O2 -g`, what the packages
ship) by default; `debug` (`-Og -g3`, frame pointers, the arena's own
consistency checks); `asan` (debug plus AddressSanitizer and UBSan, their
runtimes linked statically so the target needs nothing installed; glibc
only).  A non-release build says so in `perfcached -V`
(`perfcached 0.7.5 (abc1234, asan)`), in `/stats` `process.build` and in
the dashboard's header.  `make clean` after changing it.  Release stays at
`-O2`: `-O3` was measured against it on one commit (single node, native and
RESP, 32- and 200-byte values) and bought nothing beyond the run-to-run
spread, for a 15% larger binary; `-Os` cost about 10% more CPU a request
(3% to 13% by cell) for a 21% smaller one.  The tree builds warning-free
at `-O3` and `-Os` (gcc 14) for anyone who wants either - set it in the
environment, `CFLAGS="-O3 -g" make`; on the make command line it would
replace the warning and feature flags the Makefile adds.  For a test
node, `contrib/systemd/` has two drop-ins for `perfcached.service.d/`:
`cores.conf` keeps the newest two cores, and `asan.conf` sends a sanitizer
report there.

**Alert mail** is the fourth: `none` (the default) or `libcurl` (`make
PC_MAIL=libcurl`; Debian/Ubuntu `libcurl4-openssl-dev`, Alpine
`curl-dev`), which lets a node send its alerts over SMTP (STARTTLS or
TLS).  A build without it refuses a `[mail]` section rather than ignore
it; its alerts are logged and kept on the dashboard all the same.

**TLS** is not a `make menuconfig` question: `PC_TLS=auto`, the
default, links OpenSSL into the daemon when pkg-config finds it (`yes`
insists, `no` leaves it out; libperfd never links it).  The `tls`
listeners and `rediss://` upstreams need it, and `perfcached -V` says
`tls: OpenSSL ...` or `tls: off`.

## Building and testing

```
make check             # every test suite (clustered edition)
make check-asan        # the same under AddressSanitizer + UBSan
make check-standalone  # the suite for the standalone edition
tools/matrix.sh        # build and test all four architectures (podman + qemu)
tools/alpine-check.sh  # Alpine (musl, gcc 15): both editions + check-standalone
```

Every release candidate passes the full suite, the sanitizer suite,
the four-architecture matrix and the Alpine check before it is tagged.
Debian and Alpine - glibc and musl - are both
built and tested; `bench/Containerfile.alpine` is the small image.
