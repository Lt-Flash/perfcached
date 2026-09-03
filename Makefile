# perfcached — top-level Makefile (skeleton; grows with the task plan)
#
# Targets fill in as milestones land:
#   S4  -> selftest        S10 -> perfcached + bench client
#   S27 -> matrix (four-arch build+selftest, run on the build host)

CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64
# SAN: sanitizer flags injected into BOTH compile and link (a sanitizer
# that reaches only one of them links clean and checks nothing).
# `make check-asan` sets it; CI uses the same spelling (S55).
CFLAGS  += $(SAN)
LDLIBS   = $(SAN) -lpthread -lsodium

# Per-arch bonus flags are added by configure-time probes later (S27);
# never hardcode ISA extensions here — runtime dispatch decides.

PC_BUILD_REV := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
CFLAGS += -DPC_BUILD_REV=\"$(PC_BUILD_REV)\"

# The vendored core builds as close to upstream as possible: the two
# warning classes upstream itself trips under -Wextra are excepted; every
# other class still fails the build.
CORE_WNO = -Wno-unused-parameter -Wno-sign-compare
CORE_OBJS = src/core/pcache_mem.o src/core/pcache_htable.o src/core/pcache_arena.o

all: shim core perfcached perfcli libperfd.a

# S5 config + S6 threads + S7 protocol + S25' Noise + S8 verbs/store
perfcached: src/main.o src/config.o src/daemon.o src/proto.o src/json.o \
		src/pc_noise.o src/verbs.o src/jsonpath.o src/store.o src/storage.o \
		src/walprobe.o src/wal.o src/rdb.o src/recover.o src/cluster.o \
		src/clmap.o src/clterm.o src/clsync.o src/clhist.o src/clplace.o src/clsel.o \
		src/obs.o src/metrics.o src/statuspage.o $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c src/*.h src/compat/*.h
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

# S1: the OpenSIPS compatibility shim the vendored core compiles against
shim: src/compat/compat.o

src/compat/compat.o: src/compat/compat.c src/compat/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

# S2: the vendored core (see tools/sync-core.sh; provenance in each file)
core: $(CORE_OBJS)

src/core/%.o: src/core/%.c src/core/*.h src/compat/*.h
	$(CC) $(CFLAGS) $(CORE_WNO) -c -o $@ $<

# the cluster map format: parses bytes off the network, so it is kept
# free of cluster.c and unit-tested on its own
clmaptest: test/clmaptest.c src/clmap.o
	$(CC) $(CFLAGS) -o $@ test/clmaptest.c src/clmap.o

# the mastership term: persistence plus the three ordering rules
cltermtest: test/cltermtest.c src/clterm.o
	$(CC) $(CFLAGS) -o $@ test/cltermtest.c src/clterm.o

# staging a map change past the backup: ack before publish
clsynctest: test/clsynctest.c src/clsync.o
	$(CC) $(CFLAGS) -o $@ test/clsynctest.c src/clsync.o

# the identity history: what separates a new node from a returning one
clhisttest: test/clhisttest.c src/clhist.o
	$(CC) $(CFLAGS) -o $@ test/clhisttest.c src/clhist.o

# placement over the map: weighted rendezvous, in integers so every node
# reaches the same answer
clplacetest: test/clplacetest.c src/clplace.o src/clmap.o src/pc_slot.h
	$(CC) $(CFLAGS) -o $@ test/clplacetest.c src/clplace.o src/clmap.o

# client-side selection: the spread property, without needing a fleet
clseltest: test/clseltest.c src/clsel.o src/clmap.o
	$(CC) $(CFLAGS) -o $@ test/clseltest.c src/clsel.o src/clmap.o

# S42f: the mastership state machine under seed-replayable schedules.
# Links the SHIPPED decision functions - the simulation is the protocol
# around them, never a reimplementation of them.
clustersim: test/clustersim.c src/clterm.o src/clmap.o
	$(CC) $(CFLAGS) -o $@ test/clustersim.c src/clterm.o src/clmap.o

# S4: the M1 gate - vendored selftests + the cross-thread storm
jwtest: test/jwtest.c src/json.o src/compat/compat.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/jwtest.c src/json.o \
		src/compat/compat.o $(LDLIBS)

slottest: test/slottest.c src/pc_slot.h src/pc_mix.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/slottest.c $(LDLIBS)

natbench: bench/natbench.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ bench/natbench.c libperfd.a $(LDLIBS)

keysnative: bench/keysnative.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ bench/keysnative.c libperfd.a $(LDLIBS)

insbench: bench/insbench.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ bench/insbench.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

selftest: test/selftest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/selftest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# Same storm with the bucket locks compiled out - MUST fail (proves the
# storm detects what the locks prevent; see src/compat/locking.h)
selftest_broken: test/selftest.c src/compat/compat.c src/core/pcache_mem.c \
		src/core/pcache_htable.c src/core/pcache_arena.c
	$(CC) $(CFLAGS) $(CORE_WNO) -DPC_COMPAT_BROKEN_LOCKS -o $@ $^ $(LDLIBS)

# The whole suite under ASan+UBSan, same spelling locally and in CI.
# detect_leaks=0: exit-time leak reports are not the target - per-thread
# scratch and the obs registries live for the process lifetime by
# design, and a leak-check failing SIGTERM shutdown would fail the
# daemontest for the wrong reason.  halt_on_error makes UBSan loud.
check-asan:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=0 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) SAN="-fsanitize=address,undefined -fno-omit-frame-pointer" \
		check

# The FAULT suites (S42).  Two kinds, both deliberately out of `check`
# because every one of them costs minutes of deliberate waiting on the
# liveness windows - a fault only counts once the fleet has NOTICED it.
#
#   container fleet (bench/containers-up.sh, real netns + root iptables):
#     partition, split brain, churn, a failed hand-over.  Each announces
#     a loud SKIP rather than passing quietly when the fleet is not
#     there, so this target cannot masquerade as coverage.
#   loopback fleets (nothing but the binary):
#     a node rejoining with a WAL, and the tombstone boundary.
check-fault: perfcached perfcli
	sh test/partitiontest.sh
	sh test/splitbraintest.sh
	sh test/churntest.sh
	sh test/reshardtest.sh
	sh test/stepdowntest.sh
	sh test/dualclaimtest.sh
	sh test/shardhandovertest.sh ./perfcached
	sh test/rejointest.sh ./perfcached
	sh test/tombstonetest.sh ./perfcached

# The FAST gate (S55 tiering, 2026-09-02): every unit binary plus the
# suites that finish in seconds - no cluster suites, whose fleets and
# quiet windows are where the minutes go.  This is what a branch push
# must pass, so a commit stays individually accountable; `check` in
# full rides tag pushes, where a feature set is declared ready.
check-fast: selftest selftest_broken memprobe noisetest wipetest perfcached \
		perfcli pcbench clmaptest cltermtest clsynctest clhisttest \
		clplacetest clseltest natbench slottest jwtest asyncbintest \
		clustersim
	./clustersim 500
	./slottest
	./jwtest
	./clmaptest
	./cltermtest
	./clsynctest
	./clhisttest
	./clplacetest
	./clseltest
	./memprobe
	./selftest
	./noisetest
	./wipetest
	sh test/configtest.sh ./perfcached
	sh test/daemontest.sh ./perfcached
	sh test/prototest.sh ./perfcached
	sh test/bintest.sh ./perfcached
	sh test/verbtest.sh ./perfcached
	sh test/httptest.sh ./perfcached
	sh test/asyncbin.sh ./perfcached ./asyncbintest
	@echo "--- broken-locks build (must fail) ---"
	@if ./selftest_broken; then \
		echo "check-fast FAILED: the broken-locks storm PASSED"; exit 1; \
	else echo "broken-locks storm failed as it must"; fi
	@echo "FAST CHECKS PASS (cluster/storage suites ride the full check)"

check: selftest selftest_broken memprobe noisetest wipetest perfcached perfcli pcbench \
		libtest failovertest asynctest asyncbintest clmaptest cltermtest clsynctest clhisttest clplacetest clseltest \
		natbench slottest jwtest clustersim
	./clustersim 5000
	./slottest
	./jwtest
	./clmaptest
	./cltermtest
	./clsynctest
	./clhisttest
	./clplacetest
	./clseltest
	./memprobe
	./selftest
	./noisetest
	./wipetest
	sh test/httptest.sh ./perfcached
	sh test/pressuretest.sh ./perfcached
	sh test/configtest.sh ./perfcached
	sh test/daemontest.sh ./perfcached
	sh test/prototest.sh ./perfcached
	sh test/bintest.sh ./perfcached
	sh test/noiseinterop.sh ./perfcached
	sh test/verbtest.sh ./perfcached
	sh test/scantest.sh ./perfcached
	sh test/keysyieldtest.sh ./perfcached
	sh test/grafanatest.sh ./perfcached
	sh test/storagetest.sh ./perfcached
	sh test/walprobetest.sh ./perfcached
	sh test/waltest.sh ./perfcached
	sh test/waldroptest.sh ./perfcached ./pcbench ./perfcli
	sh test/rdbtest.sh ./perfcached
	sh test/recoverytest.sh ./perfcached
	sh test/walctrltest.sh ./perfcached
	sh test/waloverruntest.sh ./perfcached
	sh test/walobstest.sh ./perfcached
	sh test/waldisktest.sh ./perfcached
	sh test/vertest.sh ./perfcached
	sh test/verwiretest.sh ./perfcached ./perfcli
	sh test/nodestatetest.sh ./perfcached ./perfcli
	sh test/readygatetest.sh ./perfcached ./perfcli
	sh test/clmapwiretest.sh ./perfcached ./perfcli
	sh test/shardswitchtest.sh ./perfcached ./perfcli
	sh test/backupsynctest.sh ./perfcached ./perfcli
	sh test/clustertest.sh ./perfcached
	sh test/clustercfgtest.sh ./perfcached
	sh test/proxytest.sh ./perfcached
	sh test/shardtest.sh ./perfcached
	sh test/slotplacetest.sh ./perfcached ./natbench ./perfcli
	sh test/clustermaptest.sh ./perfcached
	sh test/eagertest.sh ./perfcached
	sh test/resptest.sh ./perfcached
	sh test/resplistener.sh ./perfcached
	sh test/jsontest.sh ./perfcached
	sh test/fwdtest.sh ./perfcached
	sh test/clitest.sh ./perfcached ./perfcli
	sh test/libtest.sh ./perfcached ./libtest
	sh test/failovertest.sh ./perfcached ./failovertest
	sh test/phptest.sh ./perfcached
	sh test/admintest.sh ./perfcached
	sh test/asynctest.sh ./perfcached ./asynctest
	sh test/asyncbin.sh ./perfcached ./asyncbintest
	@echo "--- broken-locks build (must fail) ---"
	@if ./selftest_broken; then \
		echo "check FAILED: the broken-locks storm PASSED"; exit 1; \
	else echo "broken-locks storm failed as it must"; fi
	@echo "ALL CHECKS PASS"

# the CLI (redis-cli analogue).  ALL communication rides libperfd -
# the one client transport (08-26 decree); the CLI adds only words,
# display, and the line editor.
perfcli: cli/perfcli.c cli/lineedit.c libperfd.a
	$(CC) $(CFLAGS) -o $@ cli/perfcli.c cli/lineedit.c libperfd.a \
		$(LDLIBS)

# libperfd (deliverable B): the hiredis-analogue client library.  The
# archive bundles its json/noise/compat dependencies, so consumers link
# just the .a + -lsodium -lpthread.
lib/perfd.o: lib/perfd.c lib/perfd.h src/json.h src/pc_noise.h src/pc_slot.h \
		src/pc_mix.h
	$(CC) $(CFLAGS) -c -o $@ lib/perfd.c

libperfd.a: lib/perfd.o src/json.o src/pc_noise.o src/compat/compat.o
	ar rcs $@ $^

libtest: test/libtest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/libtest.c libperfd.a $(LDLIBS)

# S34: the cluster-aware client - failover onto a pre-warmed standby
failovertest: test/failovertest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/failovertest.c libperfd.a $(LDLIBS)

# S32: the event-loop surface, proven under the loop S31 actually targets.
# libevent is a TEST dependency only - libperfd itself still links nothing
# but libc and libsodium, and the suite skips (loudly) without it.
# Probe with pkg-config, then fall back to a plain link test - the
# earlier inline-C probe tripped over make's escaping and reported "no"
# on a host where libevent was installed, which is the wrong direction
# for a probe to fail in: it silently skips the test.
HAVE_LIBEVENT := $(shell pkg-config --exists libevent 2>/dev/null && echo yes || \
	{ echo 'int main(void){return 0;}' > .pcev.c && \
	  $(CC) .pcev.c -levent -o .pcev >/dev/null 2>&1 && echo yes; \
	  rm -f .pcev.c .pcev; })

asynctest: test/asynctest.c lib/perfd.h libperfd.a
ifeq ($(HAVE_LIBEVENT),yes)
	$(CC) $(CFLAGS) -o $@ test/asynctest.c libperfd.a $(LDLIBS) -levent
else
	@echo "asynctest: libevent headers not found - not built"
endif


# the load generator used by the bench rigs (not part of `all`)
pcbench: bench/pcbench.c
	$(CC) $(CFLAGS) -o $@ bench/pcbench.c -lpthread

# the mode matrix's real-client load generator (bench rig, not in `all`)
mmclient: bench/mmclient.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ bench/mmclient.c libperfd.a $(LDLIBS)

# N concurrent libperfd connections to one cluster
concbench: bench/concbench.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ bench/concbench.c libperfd.a $(LDLIBS)

# S34: where the spreading policies actually put clients
policybench: bench/policybench.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ bench/policybench.c libperfd.a $(LDLIBS)

# S35: the forward-hop cost, routed vs not (bench rig, not in `all`)
routebench: bench/routebench.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ bench/routebench.c libperfd.a $(LDLIBS)

# S3: memory-backing verification runner (see test/memprobe.c header)
scancost: bench/scancost.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ bench/scancost.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

memprobe: test/memprobe.c src/core/pcache_mem.o src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/memprobe.c src/core/pcache_mem.o \
		src/compat/compat.o $(LDLIBS)

# S28/S32: the BINARY async path - warm-up and reply decoding, which
# the OpenSIPS driver's async fetch is built on
asyncbintest: test/asyncbintest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/asyncbintest.c libperfd.a $(LDLIBS)

# S59(a): the secret-wipe proof needs config.c recompiled with the
# test-only wipe counter (same shape as selftest_broken's define)
wipetest: test/wipetest.c src/config.c src/compat/compat.c
	$(CC) $(CFLAGS) -DPC_TESTHOOKS -o $@ $^ $(LDLIBS)

# S25': Noise core proof (RFC 5869 vector + round-trip + tamper + wrong-PSK)
noisetest: test/noisetest.c src/pc_noise.o src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f src/*.o src/compat/*.o src/core/*.o lib/*.o perfcached \
		perfcli libperfd.a libtest failovertest selftest selftest_broken \
		memprobe noisetest pcbench routebench mmclient policybench concbench \
		asynctest clmaptest cltermtest clsynctest clhisttest clplacetest clseltest

# install (S24): binary + annotated example config + systemd unit.
# The live config is NEVER written - only the .example is refreshed.
PREFIX ?= /usr/local
install: perfcached perfcli libperfd.a
	install -D -m 755 perfcached $(DESTDIR)$(PREFIX)/bin/perfcached
	install -D -m 755 perfcli $(DESTDIR)$(PREFIX)/bin/perfcli
	install -D -m 644 libperfd.a $(DESTDIR)$(PREFIX)/lib/libperfd.a
	install -D -m 644 lib/perfd.h $(DESTDIR)$(PREFIX)/include/perfd.h
	install -D -m 644 contrib/perfcached.conf.example \
		$(DESTDIR)/etc/perfcached/perfcached.conf.example
	install -D -m 644 contrib/perfcached.service \
		$(DESTDIR)/etc/systemd/system/perfcached.service
	install -D -m 644 contrib/perfcached.sysusers \
		$(DESTDIR)/usr/lib/sysusers.d/perfcached.conf
	@echo "next: systemd-sysusers   # creates the perfcached account"
	@echo "      cp $(DESTDIR)/etc/perfcached/perfcached.conf.example" \
		"/etc/perfcached/perfcached.conf && edit the secrets"
	@echo "      see PRODUCTION.md before the first real deployment"

.PHONY: all shim core check check-fast check-fault check-asan clean install
