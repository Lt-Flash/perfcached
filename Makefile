# perfcached — top-level Makefile (skeleton; grows with the task plan)
#
# Targets fill in as milestones land:
#   S4  -> selftest        S10 -> perfcached + bench client
#   S27 -> matrix (four-arch build+selftest, run on the build host)

CC      ?= gcc
# S358: the daemon resets /stats on a schedule (daily at local midnight by
# default); a suite that spans midnight would see its counters start again
# under it.  Every suite started from make runs with the schedule off;
# statsschedtest unsets this to test the schedule itself.
export PERFCACHED_STATS_RESET ?= off
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

# S250: the EDITION.  `make menuconfig` writes config.mk; without one the
# build is the clustered daemon, exactly as before.  standalone links
# cluster_stub.o in place of cluster.c and the cl*.c modules - the same
# behaviour as a clustered daemon with no [cluster] section, minus the code.
-include config.mk
PC_EDITION ?= clustered
CLUSTER_OBJS = src/cluster.o src/clmap.o src/clterm.o src/clsync.o src/clhist.o \
	src/clplace.o src/clsel.o src/clpeers.o src/clpend.o src/clloc.o \
	src/clboot.o src/clpush.o src/clwire.o src/clbulk.o src/clwork.o \
	src/clmig.o src/clapply.o src/clfwd.o src/clinit.o src/clres.o \
	src/clmemb.o src/clpull.o src/clcol.o src/clplat.o src/clsend.o \
	src/clfleet.o src/clps.o src/clspread.o src/clcid.o \
	src/clmesh.o src/clane.o
ifeq ($(PC_EDITION),standalone)
EDITION_OBJS = src/cluster_stub.o
CFLAGS += -DPC_EDITION_STANDALONE
else ifeq ($(PC_EDITION),clustered)
EDITION_OBJS = $(CLUSTER_OBJS)
else
$(error PC_EDITION must be standalone or clustered, not '$(PC_EDITION)' - run make menuconfig)
endif

# S282: the ALLOCATOR, also from config.mk (make menuconfig) or the command
# line.  libc (the default, every toolchain): the C library's own malloc.
# mimalloc / jemalloc: that library is linked AHEAD of the C library, so
# its malloc serves the whole process.  The C library itself is not a
# choice - it is the toolchain's (Debian gcc = glibc, Alpine gcc = musl).
# mimalloc is pinned to major 2: 3.0.1 measured slower on the JSON path
# (DESIGN 12he); the insecure build is preferred where both are installed
# (the secure one costs 5-10%, 12hd).  A sanitizer build keeps libc: the
# sanitizer's runtime answers malloc itself.
PC_ALLOC ?= libc
ifneq ($(SAN),)
PC_ALLOC_LIB =
PC_ALLOC_EFF = libc
else ifeq ($(PC_ALLOC),libc)
PC_ALLOC_LIB =
PC_ALLOC_EFF = libc
else ifeq ($(PC_ALLOC),mimalloc)
PC_ALLOC_LIB := $(firstword $(wildcard /usr/lib/libmimalloc-insecure.so.2 \
	/usr/lib/*/libmimalloc-insecure.so.2 /usr/local/lib/libmimalloc-insecure.so.2 \
	/usr/lib/libmimalloc.so.2 /usr/lib/*/libmimalloc.so.2 /usr/local/lib/libmimalloc.so.2))
PC_ALLOC_EFF = mimalloc
ifeq ($(PC_ALLOC_LIB),)
$(error PC_ALLOC=mimalloc: no libmimalloc(-insecure).so.2 found - install mimalloc 2.x (Alpine: apk add mimalloc2-insecure), or PC_ALLOC=libc)
endif
else ifeq ($(PC_ALLOC),jemalloc)
PC_ALLOC_LIB := $(firstword $(wildcard /usr/lib/libjemalloc.so.2 \
	/usr/lib/*/libjemalloc.so.2 /usr/local/lib/libjemalloc.so.2))
PC_ALLOC_EFF = jemalloc
ifeq ($(PC_ALLOC_LIB),)
$(error PC_ALLOC=jemalloc: no libjemalloc.so.2 found - install jemalloc (Alpine: apk add jemalloc; Debian: libjemalloc2), or PC_ALLOC=libc)
endif
else
$(error PC_ALLOC must be libc, mimalloc or jemalloc, not '$(PC_ALLOC)' - run make menuconfig)
endif
CFLAGS += -DPC_ALLOC_NAME=\"$(PC_ALLOC_EFF)\"
LDLIBS := $(PC_ALLOC_LIB) $(LDLIBS)

all: shim core perfcached perfcli perfdump perfload libperfd.a

# S250: choose the edition (writes config.mk)
menuconfig:
	@sh tools/menuconfig.sh

# S250: the standalone edition's gate - every make check suite that needs
# no cluster (test/standalone.suites), in order, stopping at the first
# failure.  Runs against whichever edition is built; CI runs it on the
# standalone build.
check-standalone: selftest verstamptest perfcached perfcli perfdump perfload \
		pcbench libtest wedgetest asynctest asyncbintest pubsubcli \
		udppushcli rpsbench syncfailshim.so allocshim.so liballoc noisetest
	./selftest
	./verstamptest
	@grep -vE '^#|^$$' test/standalone.suites | while read -r line; do \
		echo "== $$line"; sh -c "$$line" || exit 1; done

# S5 config + S6 threads + S7 protocol + S25' Noise + S8 verbs/store
perfcached: src/main.o src/config.o src/daemon.o src/addrwait.o src/dirlock.o src/proto.o src/json.o \
		src/pc_noise.o src/verbs.o src/jsonpath.o src/hashrec.o src/sha1.o src/ptree.o src/store.o src/storage.o \
		src/walprobe.o src/wal.o src/rdb.o src/recover.o $(EDITION_OBJS) \
		src/node.o src/clretain.o src/clunk.o \
		src/obs.o src/metrics.o src/statuspage.o src/events.o src/quiesce.o src/psindex.o src/pubsub.o src/upstream.o src/winctr.o src/memguard.o $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# src/core/*.h belongs here: the daemon's own sources read the core's
# constants (VAL_MAX is PCACHE_CELL_MAX), and without this prerequisite
# an incremental build after a core header change links objects that
# disagree about them - seen 2026-09-20, a binary whose store accepted a
# 218 KB value and whose GET refused it (S174).
src/%.o: src/%.c src/*.h src/core/*.h src/compat/*.h $(wildcard config.mk)
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

# S1: the OpenSIPS compatibility shim the vendored core compiles against
shim: src/compat/compat.o

src/compat/compat.o: src/compat/compat.c src/compat/*.h $(wildcard config.mk)
	$(CC) $(CFLAGS) -c -o $@ $<

# S2: the vendored core (see tools/sync-core.sh; provenance in each file)
core: $(CORE_OBJS)

src/core/%.o: src/core/%.c src/core/*.h src/compat/*.h $(wildcard config.mk)
	$(CC) $(CFLAGS) $(CORE_WNO) -c -o $@ $<

# the cluster map format: parses bytes off the network, so it is kept
# free of cluster.c and unit-tested on its own
clmaptest: test/clmaptest.c src/clmap.o
	$(CC) $(CFLAGS) -o $@ test/clmaptest.c src/clmap.o

# the mastership term: persistence plus the three ordering rules
cltermtest: test/cltermtest.c src/clterm.o
	$(CC) $(CFLAGS) -o $@ test/cltermtest.c src/clterm.o

# the peer table: liveness at the boundary, slot reuse, duplicate ids
clpeerstest: test/clpeerstest.c src/clpeers.o
	$(CC) $(CFLAGS) -o $@ test/clpeerstest.c src/clpeers.o

# parked requests + the completion queues: handles, occupancy, expiry
clpendtest: test/clpendtest.c src/clpend.o src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/clpendtest.c src/clpend.o src/compat/compat.o -lpthread

clmigtest: test/clmigtest.c src/clmig.o
	$(CC) $(CFLAGS) -o $@ test/clmigtest.c src/clmig.o

# S217: the sharded apply rings, without a daemon
clapplytest: test/clapplytest.c src/clapply.o
	$(CC) $(CFLAGS) -o $@ test/clapplytest.c src/clapply.o $(LDLIBS)

clfwdtest: test/clfwdtest.c src/clfwd.o
	$(CC) $(CFLAGS) -o $@ test/clfwdtest.c src/clfwd.o

clinittest: test/clinittest.c src/clinit.o src/fnv1a.h
	$(CC) $(CFLAGS) -o $@ test/clinittest.c src/clinit.o

# the locator and negative caches: TTL decay, the tombstone pair, 4-way
clloctest: test/clloctest.c src/clloc.o src/fnv1a.h
	$(CC) $(CFLAGS) -o $@ test/clloctest.c src/clloc.o

# S238: the retained-tombstone table
clretaintest: test/clretaintest.c src/clretain.o
	$(CC) $(CFLAGS) -o $@ test/clretaintest.c src/clretain.o $(LDLIBS)

# S90 id reservations: the expiry boundary, identity-before-address, the
# eviction choice - all of it invisible on a healthy fleet
clrestest: test/clrestest.c src/clres.o src/clres.h
	$(CC) $(CFLAGS) -o $@ test/clrestest.c src/clres.o

# the keepalive frames, checked DIFFERENTIALLY against the pre-extraction
# layout - MASTER_ALIVE is what master-down detection runs on
clmembtest: test/clmembtest.c src/clmemb.o src/clmemb.h src/clcodec.h
	$(CC) $(CFLAGS) -o $@ test/clmembtest.c src/clmemb.o

# S165: the unknown-command table, its frame and the fleet fold
clunktest: test/clunktest.c src/clunk.o src/clunk.h src/clcodec.h
	$(CC) $(CFLAGS) -o $@ test/clunktest.c src/clunk.o

# M15: the fleet's collection figures, the rules without the peer table
clfleettest: test/clfleettest.c src/clfleet.o src/clfleet.h src/clmemb.h
	$(CC) $(CFLAGS) -o $@ test/clfleettest.c src/clfleet.o

# M9b: the worker entry points and the write-path push on the real
# clwork/clpend/clsend/clpush/clmig objects;
# the test itself supplies cluster.c's accessors (clstate.h)
clworktest: test/clworktest.c src/clwork.o src/clpend.o src/clsend.o src/clpeers.o src/clwire.o src/clplace.o src/clmap.o src/clpull.o src/clfwd.o src/clpush.o src/clmig.o
	$(CC) $(CFLAGS) -o $@ test/clworktest.c src/clwork.o src/clpend.o src/clsend.o src/clpeers.o src/clwire.o src/clplace.o src/clmap.o src/clpull.o src/clfwd.o src/clpush.o src/clmig.o -lsodium -lpthread

# M16: the pub/sub relay plane on the real clps/clsend/clpeers/psindex/
# quiesce objects; the test supplies cluster.c's accessors and pubsub.c's
# callbacks, and captures the transmit - nothing binds a socket
clpstest: test/clpstest.c src/clps.o src/clsend.o src/clpeers.o src/clwire.o src/psindex.o src/quiesce.o src/clps.h
	$(CC) $(CFLAGS) -o $@ test/clpstest.c src/clps.o src/clsend.o src/clpeers.o src/clwire.o src/psindex.o src/quiesce.o -lsodium -lpthread

# M17: spread's decisions on the real clspread/clplace objects; placement's
# answers are the test's own
clspreadtest: test/clspreadtest.c src/clspread.o src/clplace.o src/clmap.o src/clspread.h
	$(CC) $(CFLAGS) -o $@ test/clspreadtest.c src/clspread.o src/clplace.o src/clmap.o

# M14: the send path (clsend), no socket - the transmit is captured
clsendtest: test/clsendtest.c src/clsend.o src/clpeers.o src/clwire.o src/clsend.h
	$(CC) $(CFLAGS) -o $@ test/clsendtest.c src/clsend.o src/clpeers.o src/clwire.o -lsodium

# the PULL plane: a store miss asks a peer, and the VERSION rides back
# with the bytes - checked against the pre-extraction layout
clpulltest: test/clpulltest.c src/clpull.o src/clpull.h src/clcodec.h
	$(CC) $(CFLAGS) -o $@ test/clpulltest.c src/clpull.o

# the collection announce: four ops in one frame, and RENAME carries a
# second name the other three do not
clcoltest: test/clcoltest.c src/clcol.o src/clcol.h src/clcodec.h
	$(CC) $(CFLAGS) -o $@ test/clcoltest.c src/clcol.o

# platform-derived identity: a clone must not inherit a member's identity
clplattest: test/clplattest.c src/clplat.o src/clplat.h
	$(CC) $(CFLAGS) -o $@ test/clplattest.c src/clplat.o -lsodium

# the bootstrap decision: rounds, deadlines, the handoff
clboottest: test/clboottest.c src/clboot.o
	$(CC) $(CFLAGS) -o $@ test/clboottest.c src/clboot.o -lpthread

# the write-path push groups: per-thread, the two send points, the flush
clpushtest: test/clpushtest.c src/clpush.o
	$(CC) $(CFLAGS) -o $@ test/clpushtest.c src/clpush.o -lpthread

# the sealed datagram and the beat frames: round trip, tamper, wrong key
clwiretest: test/clwiretest.c src/clwire.o
	$(CC) $(CFLAGS) -o $@ test/clwiretest.c src/clwire.o -lsodium

# the bulk plane's framing and batch handoff: round trip, truncation, handoff
clbulktest: test/clbulktest.c src/clbulk.o src/pc_noise.o src/compat/compat.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/clbulktest.c src/clbulk.o src/pc_noise.o \
		src/compat/compat.o $(LDLIBS)

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

# S226: the WAL pump's ring copy against the byte loop it replaced
walringtest: test/walringtest.c src/walring.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/walringtest.c

psindextest: test/psindextest.c src/psindex.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/psindextest.c src/psindex.o

psudptest: test/psudptest.c src/psudp.h lib/perfd_push.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/psudptest.c

psinteresttest: test/psinteresttest.c src/psinterest.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/psinteresttest.c

psrelaytest: test/psrelaytest.c src/psrelay.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/psrelaytest.c

slottest: test/slottest.c src/pc_slot.h src/pc_mix.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/slottest.c $(LDLIBS)

udppushcli: test/udppushcli.c lib/perfd.h lib/perfd_push.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/udppushcli.c libperfd.a $(LDLIBS)

pubsubcli: test/pubsubcli.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/pubsubcli.c libperfd.a $(LDLIBS)

natbench: bench/natbench.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ bench/natbench.c libperfd.a $(LDLIBS)

psqueuetest: test/psqueuetest.c src/pubsub.o src/psindex.o src/quiesce.o src/pubsub.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/psqueuetest.c src/pubsub.o src/psindex.o src/quiesce.o -lpthread

psengine: bench/psengine.c src/pubsub.o src/psindex.o src/quiesce.o src/pubsub.h
	$(CC) $(CFLAGS) -Isrc -o $@ bench/psengine.c src/pubsub.o src/psindex.o src/quiesce.o -lpthread

psbench: bench/psbench.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ bench/psbench.c libperfd.a $(LDLIBS)

# the pub/sub soak driver: publishers, subscribers (exact, pattern, UDP
# push) and churn that survive a node restart - see bench/pssoak.c
pssoak: bench/pssoak.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ bench/pssoak.c libperfd.a $(LDLIBS)

# RESP pub/sub against Redis with one client - see bench/rpsbench.c and
# bench/rpscmp.sh (which builds its own copy on the rig host)
rpsbench: bench/rpsbench.c
	$(CC) $(CFLAGS) -o $@ bench/rpsbench.c -lpthread
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
	$(MAKE) SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -DPCACHE_ARENA_DEBUG" \
		check

# One suite group under ASan+UBSan - the CI's per-group asan jobs (same
# flags as check-asan, which runs every group)
check-asan-g-%:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=0 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -DPCACHE_ARENA_DEBUG" \
		check-g-$*

# S133: scan a sanitizer log for findings, EXCLUDING the broken-locks
# range - which always contains some, by design.  Usage:
#   make check-asan 2>&1 | tee /tmp/asan.log
#   make check-asan-findings LOG=/tmp/asan.log
# Exit 1 if anything real is found, so it can gate.  A plain grep of the
# same file always matches and tells you nothing (S133, DESIGN 12df).
check-asan-findings:
	@test -n "$(LOG)" || { echo "usage: make check-asan-findings LOG=<file>"; exit 2; }
	@awk '/^--- broken-locks build/{skip=1} /^--- end broken-locks/{skip=0; next} \
	      !skip && /runtime error:|ERROR: AddressSanitizer/{print; n++} \
	      END{if (n) {printf "%d sanitizer finding(s) OUTSIDE the broken-locks control\n", n; exit 1} \
	          else print "no sanitizer findings outside the broken-locks control"}' "$(LOG)"

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
	sh test/restartwitnesstest.sh ./perfcached
	sh test/reconcileresizetest.sh ./perfcached
	sh test/tombstonetest.sh ./perfcached

# The FAST gate (S55 tiering, 2026-09-02): every unit binary plus the
# suites that finish in seconds - no cluster suites, whose fleets and
# quiet windows are where the minutes go.  This is what a branch push
# must pass, so a commit stays individually accountable; `check` in
# full rides tag pushes, where a feature set is declared ready.
check-fast: selftest selftest_broken memprobe noisetest noiseboundtest wipetest perfcached pubsubcli udppushcli \
		perfcli pcbench clmaptest cltermtest clsynctest clhisttest \
		clplacetest clseltest clpeerstest clpendtest clloctest clretaintest clboottest clpushtest clwiretest clbulktest clmigtest clfwdtest clinittest clrestest clmembtest clunktest clsendtest clfleettest clworktest clpstest clspreadtest clpulltest clcoltest clplattest natbench slottest jwtest ptreetest hashrectest walringtest asyncbintest routedpairtest movedhinttest hinttest \
		clustersim psrelaytest psindextest psqueuetest psinteresttest psudptest rpsbench legmetatest legwalkremovetest legdrainwalktest drainhinttest wintest scanremovetest histtest capracetest verstamptest arenadbgtest syncfailshim.so slowlistenshim.so fopenmapshim.so allocshim.so liballoc threadfreeze clapplytest evactest
	./clustersim 500
	./clapplytest
	./evactest
	./slottest
	./legmetatest
	./legwalkremovetest
	./legdrainwalktest
	./drainhinttest
	./wintest
	./scanremovetest
	./histtest
	./capracetest
	./verstamptest
	./arenadbgtest
	./psrelaytest
	./psindextest
	./psqueuetest
	./psinteresttest
	./psudptest
	./jwtest
	./ptreetest
	./hashrectest
	./walringtest
	sh test/readmetest.sh
	sh test/readytest.sh ./perfcached ./slowlistenshim.so
	sh test/cleardoortest.sh ./perfcached ./perfcli ./fopenmapshim.so
	sh test/standalonetest.sh ./perfcached ./noisetest
	sh test/maxclientstest.sh ./perfcached
	sh test/memguardtest.sh ./perfcached
	sh test/nativerefusetest.sh ./perfcached
	sh test/hsparktest.sh ./perfcached ./noisetest
	sh test/idlebuftest.sh ./perfcached
	sh test/sweepwalktest.sh ./perfcached
	sh test/pubsubtest.sh ./perfcached ./pubsubcli
	sh test/pushbatchtest.sh ./perfcached
	sh test/wiredraintest.sh ./perfcached
	sh test/expireleaktest.sh ./perfcached
	sh test/scanresizetest.sh ./perfcached
	sh test/refusedlogtest.sh ./perfcached
	sh test/prefetchtest.sh ./perfcached
	sh test/pubsubpressuretest.sh ./perfcached ./rpsbench
	sh test/psnotifytest.sh ./perfcached
	sh test/pubsubrelaytest.sh ./perfcached
	sh test/jdeltest.sh ./perfcached
	sh test/xsstest.sh ./perfcached
	sh test/deltombtest.sh ./perfcached
	sh test/routedpairtest.sh ./perfcached ./routedpairtest
	sh test/expiretest.sh ./perfcached
	sh test/delprobetest.sh ./perfcached
	sh test/plaintextdoortest.sh ./perfcached ./perfcli
	sh test/movedhinttest.sh ./perfcached ./movedhinttest
	sh test/respmovedtest.sh ./perfcached
	sh test/syncfailtest.sh ./perfcached ./syncfailshim.so
	sh test/stalltest.sh ./perfcached ./threadfreeze ./perfcli
	sh test/stallmastertest.sh ./perfcached ./threadfreeze ./perfcli
	sh test/applythreadstest.sh ./perfcached
	sh test/relayrxtest.sh ./perfcached
	sh test/relayinteresttest.sh ./perfcached
	sh test/udppushtest.sh ./perfcached ./udppushcli
	sh test/httpidletest.sh ./perfcached
	sh test/httpsweeptest.sh ./perfcached
	sh test/metricsresettest.sh ./perfcached
	sh test/statsschedtest.sh ./perfcached
	sh test/doorwraptest.sh ./perfcached
	sh test/keyspacetest.sh ./perfcached
	sh test/cmdstatstest.sh ./perfcached
	sh test/clientstest.sh ./perfcached
	./clmaptest
	./cltermtest
	./clsynctest
	./clhisttest
	./clplacetest
	./clseltest
	./clpeerstest
	./clpendtest
	./clloctest
	./clretaintest
	./clrestest
	./clmembtest
	./clunktest
	./clsendtest
	./clfleettest
	./clworktest
	./clpstest
	./clspreadtest
	./clpulltest
	./clcoltest
	./clplattest
	./clboottest
	./clpushtest
	./clwiretest
	./clbulktest
	./clmigtest
	./clfwdtest
	./clinittest
	./memprobe
	./selftest
	./noisetest
	./noiseboundtest
	./wipetest
	sh test/configtest.sh ./perfcached
	sh test/daemontest.sh ./perfcached
	sh test/prototest.sh ./perfcached
	sh test/bintest.sh ./perfcached
	sh test/verbtest.sh ./perfcached
	sh test/httptest.sh ./perfcached
	sh test/fleetcolstest.sh ./perfcached
	sh test/unknowntest.sh ./perfcached
	sh test/pintest.sh ./perfcached
	sh test/slowredacttest.sh ./perfcached
	sh test/unknownfleettest.sh ./perfcached
	sh test/synctest.sh
	sh test/asyncbin.sh ./perfcached ./asyncbintest
# S133: this step runs a build whose LOCKS ARE DELIBERATELY BROKEN and
# requires it to fail.  Under the sanitizers it therefore emits genuine
# findings - a racing splitter reading a half-compacted bucket - and those
# findings are the point, not a defect.  The markers bracket them so a scan
# of a sanitizer log can exclude the range mechanically; `make
# check-asan-findings` does exactly that.  Read them before concluding
# anything from a `runtime error:` line in one of these logs.
	@echo "--- broken-locks build (must fail) ---"
	@if ./selftest_broken; then \
		echo "check-fast FAILED: the broken-locks storm PASSED"; exit 1; \
	else echo "broken-locks storm failed as it must"; fi
	@echo "--- end broken-locks (its sanitizer findings are expected) ---"
	@echo "FAST CHECKS PASS (cluster/storage suites ride the full check)"

# S108: a suite that exists but is not run here is a break-check nobody
# verifies - clonetest proved a dead refusal for a month that way.  Every
# test/*.sh must be named in this file, or check refuses to start.
# S141: the same suite on its own, for the edit-test loop it was built
# for.  It is in `check` as well - a suite only this target runs is a
# suite CI never runs, which is what check-wired exists to prevent.
statlint:
	sh test/statlint.sh

spreadroutetest: perfcached failovertest
	sh test/spreadroutetest.sh ./perfcached ./failovertest

restarttest: perfcached
	sh test/restarttest.sh ./perfcached

spreadtest: perfcached
	sh test/spreadtest.sh ./perfcached
	sh test/spreadcoldtest.sh ./perfcached

fastgate: perfcached
	sh test/fastgate.sh ./perfcached

# S148: the coverage sketch counts DISTINCT served keys, which is the half
# a hit RATE cannot express.  Epochs are driven through compat_ticks_offset,
# so it asserts real cardinalities without sleeping for a window.
reachtest: test/reachtest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/reachtest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# S151: client-only totals and reach are a slot RANGE; this proves the
# arithmetic, the reset, and that an unregistered range reads as before.
origintest: test/origintest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/origintest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# S173: a record in the overflow leg must walk out with the same
# version, flags and write tick as one in a bucket.
# RV-3: the arena compiled WITH its debug invariants, into this binary
# only - the tree's own pcache_arena.o stays the release object
arenadbgtest: test/arenadbgtest.c src/core/pcache_arena.c src/core/pcache_mem.o \
		src/core/pcache_htable.o src/compat/compat.o
	$(CC) $(CFLAGS) $(CORE_WNO) -DPCACHE_ARENA_DEBUG -o $@ $^ $(LDLIBS)

capracetest: test/capracetest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/capracetest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# S234: includes the table itself, to hold a bucket lock - so it links
# every core object BUT the table
verstamptest: test/verstamptest.c src/core/pcache_htable.c src/core/pcache_htable.h $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) $(CORE_WNO) -o $@ test/verstamptest.c \
		$(filter-out src/core/pcache_htable.o,$(CORE_OBJS)) \
		src/compat/compat.o $(LDLIBS)

legmetatest: test/legmetatest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/legmetatest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# S198: a walk whose callback removes each record still hands over
# every record - the shape of a proxy migration.
legwalkremovetest: test/legwalkremovetest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/legwalkremovetest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# S330: a leg walk hands over every record while the drain moves records
# out of the chain it is walking.
legdrainwalktest: test/legdrainwalktest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/legdrainwalktest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

drainhinttest: test/drainhinttest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/drainhinttest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# S351: the five-minute counter windows, on an injected clock
wintest: test/wintest.c src/winctr.o src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/wintest.c src/winctr.o src/compat/compat.o $(LDLIBS)

# S199: the cursored scan (the drop's walk) removes as it goes, one pass.
scanremovetest: test/scanremovetest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/scanremovetest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# S183: the latency histogram's resolution - a percentile must estimate
# the value, not name a power of two above it.
histtest: test/histtest.c src/obs.o src/json.o src/ptree.o src/clunk.o src/compat/compat.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/histtest.c src/obs.o src/json.o src/ptree.o src/clunk.o \
		src/compat/compat.o $(LDLIBS)

# S155: the index geometry - a segment is one slot and a sixteenth - and
# the oracle behind S150: what index_bytes quotes is what regions_bytes moves by.
indextest: test/indextest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/indextest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# S150 B: the quiescent-state primitive a table retirement waits on.
quiesctest: test/quiesctest.c src/quiesce.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/quiesctest.c src/quiesce.o -lpthread

# S150 C: a retired table's slots come back - taken before the frontier
# moves, punched out past the cool-off - and RSS says so.
regiontest: test/regiontest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/regiontest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# S313: the hash record's pure functions
hashrectest: test/hashrectest.c src/hashrec.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/hashrectest.c src/hashrec.o -lm

# S318: HGET and HSET ops/s (and the daemon's CPU per op) on a ~50 KB and
# a ~500 B hash - one daemon, one RESP client, plaintext loopback.  The
# number the hash path's copy and lock work is judged by; a bench, not a
# gate, so it is not under make check.
hashbench: perfcached
	sh test/hashbench.sh ./perfcached

# S317: the native door's value codec
ptreetest: test/ptreetest.c src/ptree.o src/json.o src/compat/compat.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/ptreetest.c src/ptree.o src/json.o \
		src/compat/compat.o $(LDLIBS)

# S190: the survivors of a mass expiry, moved out of the chunks they pin
evactest: test/evactest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/evactest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

check-wired:
	@for t in test/*.sh; do grep -q "sh $$t" Makefile || { echo "check: $$t is not run by make check - wire it in or remove it"; exit 1; }; done

# The binaries every suite group needs - check and each group build them.
CHECK_BINS = selftest selftest_broken memprobe noisetest noiseboundtest \
	wipetest perfcached perfcli perfdump perfload pcbench libtest failovertest \
	routedpairtest movedhinttest hinttest wedgetest asynctest asyncbintest clmaptest \
	cltermtest clsynctest clhisttest clplacetest clseltest clpeerstest \
	clpendtest clloctest clretaintest clboottest clpushtest clwiretest \
	clbulktest clmigtest clfwdtest clinittest clrestest clmembtest clunktest \
	clsendtest clfleettest clworktest clpstest clspreadtest clpulltest \
	clcoltest clplattest natbench slottest jwtest ptreetest hashrectest walringtest clustersim \
	reachtest origintest indextest quiesctest regiontest legmetatest \
	legwalkremovetest legdrainwalktest drainhinttest wintest scanremovetest histtest capracetest verstamptest \
	arenadbgtest syncfailshim.so slowlistenshim.so fopenmapshim.so allocshim.so liballoc netcutshim.so lingershim.so threadfreeze clapplytest \
	evactest

# The full ladder, in GROUPS so CI can run them as separate jobs (2026-10-03:
# one monolithic check had grown to ~90 min plain and ~95 under ASan, past
# GitHub's limit, and one red suite cost a full rerun).  Each group is
# balanced from the v0.4.1 check-asan timings to ~13-17 min under ASan;
# within a group the suites keep their old order.  `make check` still runs
# every group, one after another; `make check-g-<group>` runs one, and
# `make check-asan-g-<group>` one under ASan+UBSan.  check-wired keeps every
# test/*.sh in some group.
CHECK_GROUPS = core durability cluster1 cluster2 repl1 repl2 repl3

check-edition-guard:
	@if [ "$(PC_EDITION)" = standalone ]; then \
		echo "make check: this is the STANDALONE edition - half of these suites need the cluster; its gate is make check-standalone"; \
		exit 1; fi

check: check-wired check-edition-guard $(CHECK_BINS)
	@for g in $(CHECK_GROUPS); do $(MAKE) --no-print-directory -j1 check-g-$$g || exit 1; done

check-g-core: check-edition-guard $(CHECK_BINS)
	./clustersim 5000
	./clapplytest
	./evactest
	./reachtest
	./origintest
	./indextest
	./legmetatest
	./legwalkremovetest
	./legdrainwalktest
	./drainhinttest
	./wintest
	./scanremovetest
	./histtest
	./capracetest
	./verstamptest
	./arenadbgtest
	./quiesctest
	./regiontest
	./slottest
	./jwtest
	./ptreetest
	./hashrectest
	./walringtest
	./clmaptest
	./cltermtest
	./clsynctest
	./clhisttest
	./clplacetest
	./clseltest
	./clpeerstest
	./clpendtest
	./clloctest
	./clretaintest
	./clrestest
	./clmembtest
	./clunktest
	sh test/hashtest.sh ./perfcached
	sh test/hashfwdtest.sh ./perfcached
	sh test/cmptest.sh ./perfcached
	sh test/symlocktest.sh ./perfcached
	sh test/jsonalloctest.sh ./perfcached ./allocshim.so
	sh test/liballoctest.sh ./perfcached ./liballoc ./allocshim.so
	sh test/ratelimittest.sh ./perfcached
	sh test/difftest.sh ./perfcached
	sh test/upstreamtest.sh ./perfcached
	./clsendtest
	./clfleettest
	./clworktest
	./clpstest
	./clspreadtest
	./clpulltest
	./clcoltest
	./clplattest
	./clboottest
	./clpushtest
	./clwiretest
	./clbulktest
	./clmigtest
	./clfwdtest
	./clinittest
	./memprobe
	./selftest
	./noisetest
	./noiseboundtest
	./wipetest
	sh test/httptest.sh ./perfcached
	sh test/httpsweeptest.sh ./perfcached
	sh test/metricsresettest.sh ./perfcached
	sh test/statsschedtest.sh ./perfcached
	sh test/doorwraptest.sh ./perfcached
	sh test/gossiptest.sh ./perfcached
	sh test/reservetest.sh ./perfcached
	sh test/clonetest.sh ./perfcached
	sh test/pressuretest.sh ./perfcached
	sh test/heldtest.sh ./perfcached
	sh test/tailtest.sh ./perfcached
	sh test/openstatstest.sh ./perfcached ./perfcli
	sh test/growtest.sh ./perfcached
	sh test/coltest.sh ./perfcached
	sh test/colnametest.sh ./perfcached
	sh test/enabletest.sh ./perfcached
	sh test/eventtest.sh ./perfcached ./perfcli
	sh test/colresizetest.sh ./perfcached ./perfcli
	sh test/indexceiltest.sh ./perfcached
	sh test/rxlagtest.sh ./perfcached
	sh test/hugetlbtest.sh ./perfcached
	sh test/configtest.sh ./perfcached
	sh test/daemontest.sh ./perfcached
	sh test/prototest.sh ./perfcached
	sh test/bintest.sh ./perfcached
	sh test/noiseinterop.sh ./perfcached
	sh test/verbtest.sh ./perfcached
	sh test/scantest.sh ./perfcached
	sh test/keysyieldtest.sh ./perfcached
	sh test/keysretiretest.sh ./perfcached ./perfcli
	sh test/keysbudgettest.sh ./perfcached
	sh test/shrinktest.sh ./perfcached ./perfcli
	sh test/grafanatest.sh ./perfcached
	sh test/storagetest.sh ./perfcached
	sh test/pinbothtest.sh ./perfcached
	sh test/colstattest.sh ./perfcached
	sh test/resizeheldtest.sh ./perfcached ./threadfreeze ./netcutshim.so
	sh test/autoscaletest.sh ./perfcached ./perfcli
	sh test/vertest.sh ./perfcached
	sh test/verwiretest.sh ./perfcached ./perfcli
	sh test/nodestatetest.sh ./perfcached ./perfcli
	sh test/readygatetest.sh ./perfcached ./perfcli
	sh test/fastgate.sh ./perfcached
	sh test/unknowntest.sh ./perfcached
	sh test/pintest.sh ./perfcached
	sh test/slowredacttest.sh ./perfcached
	sh test/aheadtest.sh ./perfcached
	sh test/addrwaittest.sh ./perfcached
	sh test/legdraintest.sh ./perfcached
	sh test/bigvaltest.sh ./perfcached
	sh test/jdeltest.sh ./perfcached
	sh test/xsstest.sh ./perfcached
	sh test/deltombtest.sh ./perfcached
	sh test/pagejstest.sh ./perfcached
	sh test/winstatstest.sh ./perfcached ./perfcli
	sh test/pagelayouttest.sh ./perfcached
	sh test/winmaxtest.sh ./perfcached
	sh test/utf8statstest.sh ./perfcached
	sh test/statlint.sh
	sh test/lamporttest.sh ./perfcached
	sh test/resptest.sh ./perfcached
	sh test/setnxtest.sh ./perfcached
	sh test/typestest.sh ./perfcached
	sh test/resplistener.sh ./perfcached
	sh test/jsontest.sh ./perfcached
	sh test/clitest.sh ./perfcached ./perfcli
	sh test/libtest.sh ./perfcached ./libtest
	sh test/expiretest.sh ./perfcached
	sh test/plaintextdoortest.sh ./perfcached ./perfcli
	sh test/fuzztest.sh ./perfcached
	sh test/admintest.sh ./perfcached
# S133: this step runs a build whose LOCKS ARE DELIBERATELY BROKEN and
# requires it to fail.  Under the sanitizers it therefore emits genuine
# findings - a racing splitter reading a half-compacted bucket - and those
# findings are the point, not a defect.  The markers bracket them so a scan
# of a sanitizer log can exclude the range mechanically; `make
# check-asan-findings` does exactly that.  Read them before concluding
# anything from a `runtime error:` line in one of these logs.
	@echo "--- broken-locks build (must fail) ---"
	@if ./selftest_broken; then \
		echo "check FAILED: the broken-locks storm PASSED"; exit 1; \
	else echo "broken-locks storm failed as it must"; fi
	@echo "--- end broken-locks (its sanitizer findings are expected) ---"
	@echo "ALL CHECKS PASS"

check-g-durability: check-edition-guard $(CHECK_BINS)
	sh test/waluniformtest.sh ./perfcached ./perfcli
	sh test/dumptest.sh ./perfcached ./perfdump
	sh test/loadtest.sh ./perfcached ./perfdump ./perfload
	sh test/dumpresizetest.sh ./perfcached ./perfcli ./perfdump
	sh test/probetest.sh ./perfcached
	sh test/legwalktest.sh ./perfcached
	sh test/walprobetest.sh ./perfcached
	sh test/waltest.sh ./perfcached
	sh test/waldroptest.sh ./perfcached ./pcbench ./perfcli
	sh test/rdbtest.sh ./perfcached
	sh test/recoverytest.sh ./perfcached
	sh test/walctrltest.sh ./perfcached
	sh test/waloverruntest.sh ./perfcached
	sh test/healtest.sh ./perfcached ./syncfailshim.so
	sh test/healroutinetest.sh ./perfcached ./syncfailshim.so
	sh test/rdbkilltest.sh ./perfcached
	sh test/walsizetest.sh ./perfcached
	sh test/walresidenttest.sh ./perfcached
	sh test/walringsizetest.sh ./perfcached
	sh test/walordertest.sh ./perfcached
	sh test/expirereaptest.sh ./perfcached
	sh test/emptywraptest.sh ./perfcached
	sh test/walobstest.sh ./perfcached
	sh test/waldisktest.sh ./perfcached
	sh test/backupsynctest.sh ./perfcached ./perfcli
	sh test/dirlocktest.sh ./perfcached
	sh test/loadfactortest.sh ./perfcached
	sh test/restarttest.sh ./perfcached
# S224: the WAL asks for a snapshot early enough - the rate trigger keeps an
# 8 MB ring under a ~4 s snapshot from overrunning; the quarter alone cannot.
	sh test/snaptriggertest.sh ./perfcached
	sh test/statedirtest.sh ./perfcached
	sh test/syncfailtest.sh ./perfcached ./syncfailshim.so
	sh test/asynctest.sh ./perfcached ./asynctest
	sh test/asyncbin.sh ./perfcached ./asyncbintest

check-g-cluster1: check-edition-guard $(CHECK_BINS)
	sh test/splitkeeptest.sh ./perfcached
	sh test/upstreamclustertest.sh ./perfcached
	sh test/resizefleettest.sh ./perfcached
	sh test/refusedpeertest.sh ./perfcached
	sh test/clmapwiretest.sh ./perfcached ./perfcli
	sh test/clientsfleettest.sh ./perfcached
	sh test/unknownfleettest.sh ./perfcached
	sh test/clustertest.sh ./perfcached
# S221: the only suite that runs TWO BUILDS against each other.  It
# compiles three one-line variants of the tree under test (~4 s each)
# into a temporary directory - that is the point of it, not overhead:
# nothing else here can tell a wire incompatibility from a config one.
	sh test/epochtest.sh ./perfcached
# S222: a clean stop snapshots, so a node restarted ALONE after the fleet was
# stopped serves the whole keyspace; and the privileged fleetstop verb.
	sh test/stoptest.sh ./perfcached ./perfcli
	sh test/lifeguardtest.sh ./perfcached
	sh test/idsrctest.sh ./perfcached
	sh test/boottest.sh ./perfcached
	sh test/failovertest.sh ./perfcached ./failovertest
	sh test/wedgetest.sh ./perfcached ./wedgetest
	sh test/fleetresettest.sh ./perfcached

check-g-cluster2: check-edition-guard $(CHECK_BINS)
	sh test/masterlosstest.sh ./perfcached
	sh test/standbypassovertest.sh ./perfcached ./netcutshim.so
	sh test/colsfleettest.sh ./perfcached
	sh test/fleetcolstest.sh ./perfcached
	sh test/gonecardtest.sh ./perfcached
	sh test/clustercfgtest.sh ./perfcached
	sh test/mcastttltest.sh ./perfcached
	sh test/unicasttest.sh ./perfcached
	sh test/dnsseedtest.sh ./perfcached
	sh test/lanetest.sh ./perfcached
	sh test/foundtest.sh ./perfcached
	sh test/foreigntest.sh ./perfcached
	sh test/udpblocktest.sh ./perfcached
	sh test/lanebyetest.sh ./perfcached ./lingershim.so
	sh test/maxmemberstest.sh ./perfcached
	sh test/clusteridtest.sh ./perfcached ./netcutshim.so
	sh test/clustermaptest.sh ./perfcached
	sh test/stalltest.sh ./perfcached ./threadfreeze ./perfcli
	sh test/stallmastertest.sh ./perfcached ./threadfreeze ./perfcli

check-g-repl1: check-edition-guard $(CHECK_BINS)
	sh test/eagerparttest.sh ./perfcached ./netcutshim.so
	sh test/ceilingreplicatest.sh ./perfcached
	sh test/reconciledigtest.sh ./perfcached
	sh test/shardswitchtest.sh ./perfcached ./perfcli
	sh test/replicastest.sh ./perfcached
	sh test/spreadroutetest.sh ./perfcached ./failovertest
# S223: kills a three-node eager fleet and restarts it four ways - together,
# one by one, with snapshots, and in the staggered order a power failure
# produces - then repeats two of them on a build with the witness rule
# removed, which must lose keys.  ~14 minutes: the scenarios are timed,
# because a witness is defined by how long it has been up.
	sh test/outagetest.sh ./perfcached
	sh test/slotplacetest.sh ./perfcached ./natbench ./perfcli
	sh test/eagermisstest.sh ./perfcached
	sh test/backfilltest.sh ./perfcached

check-g-repl2: check-edition-guard $(CHECK_BINS)
	sh test/restartsharetest.sh ./perfcached
	sh test/spreadparttest.sh ./perfcached ./netcutshim.so
	sh test/spreadtest.sh ./perfcached
	sh test/spreadcoldtest.sh ./perfcached
	sh test/bigreplicatest.sh ./perfcached
	sh test/shardtest.sh ./perfcached
	sh test/delprobetest.sh ./perfcached
	sh test/movedhinttest.sh ./perfcached ./movedhinttest
	sh test/respmovedtest.sh ./perfcached
	sh test/sweepclocktest.sh ./perfcached

check-g-repl3: check-edition-guard $(CHECK_BINS)
	sh test/ceilingtest.sh ./perfcached
	sh test/expirerejointest.sh ./perfcached
	sh test/sweepgatetest.sh ./perfcached ./netcutshim.so
	sh test/proxytest.sh ./perfcached
	sh test/eagertest.sh ./perfcached
	sh test/eagerrestarttest.sh ./perfcached
	sh test/stalebackfilltest.sh ./perfcached
	sh test/fwdtest.sh ./perfcached
	sh test/routedpairtest.sh ./perfcached ./routedpairtest
	sh test/hinttest.sh ./perfcached ./hinttest
	sh test/proxylocatortest.sh ./perfcached
	sh test/applythreadstest.sh ./perfcached

# the CLI (redis-cli analogue).  ALL communication rides libperfd -
# the one client transport (08-26 decree); the CLI adds only words,
# display, and the line editor.
perfcli: cli/perfcli.c cli/lineedit.c libperfd.a
	$(CC) $(CFLAGS) -o $@ cli/perfcli.c cli/lineedit.c libperfd.a \
		$(LDLIBS)

# perfdump (S112): the parallel dumper, on libperfd and the daemon's own
# JSON tokenizer (inside the archive); compression through the zstd binary
perfdump: cli/perfdump.c libperfd.a
	$(CC) $(CFLAGS) -o $@ cli/perfdump.c libperfd.a $(LDLIBS)

# perfload (S113): the parallel loader, the reverse of perfdump - the
# `restore` verb carries each record's own version and absolute expiry
perfload: cli/perfload.c libperfd.a
	$(CC) $(CFLAGS) -o $@ cli/perfload.c libperfd.a $(LDLIBS)

# libperfd (deliverable B): the hiredis-analogue client library.  The
# archive bundles its json/noise/compat dependencies, so consumers link
# just the .a + -lsodium -lpthread.
lib/perfd.o: lib/perfd.c $(wildcard lib/perfd-*.inc) lib/perfd.h src/json.h \
		src/ptree.h src/pc_noise.h src/pc_slot.h src/pc_mix.h
	$(CC) $(CFLAGS) -c -o $@ lib/perfd.c

libperfd.a: lib/perfd.o src/json.o src/ptree.o src/pc_noise.o src/compat/compat.o
	ar rcs $@ $^

# S295: proxy holder hints, through libperfd with route_hints on
hinttest: test/hinttest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/hinttest.c libperfd.a $(LDLIBS)

libtest: test/libtest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/libtest.c libperfd.a $(LDLIBS)

# S34: the cluster-aware client - failover onto a pre-warmed standby
failovertest: test/failovertest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/failovertest.c libperfd.a $(LDLIBS)

routedpairtest: test/routedpairtest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/routedpairtest.c libperfd.a $(LDLIBS)

movedhinttest: test/movedhinttest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/movedhinttest.c libperfd.a $(LDLIBS)

wedgetest: test/wedgetest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/wedgetest.c libperfd.a $(LDLIBS)

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

noiseboundtest: test/noiseboundtest.c src/pc_noise.o src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# S215: the device that refuses a sync - an LD_PRELOAD shim for
# test/syncfailtest.sh.  Never sanitized: it is preloaded into the daemon,
# and a second copy of a sanitizer runtime in one process is an error.
# S284: libperfd's allocations per call, under allocshim.so (liballoctest.sh)
liballoc: test/liballoc.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/liballoc.c libperfd.a $(LDLIBS)

# S283: counts the daemon's heap allocations - an LD_PRELOAD shim for
# test/jsonalloctest.sh.  Never sanitized, like syncfailshim.so.
allocshim.so: test/allocshim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $<

syncfailshim.so: test/syncfailshim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $<

# RV-4a: cuts a node off from named peers for test/eagerparttest.sh
netcutshim.so: test/netcutshim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $< -lpthread

lingershim.so: test/lingershim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $<

# test/readytest.sh: holds every worker's listen() so "ready" must wait for it
slowlistenshim.so: test/slowlistenshim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $< -ldl

# S339, test/cleardoortest.sh: shows perfcli a config at /etc/perfcached.conf
# without writing to the host's /etc
fopenmapshim.so: test/fopenmapshim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $< -ldl

# S216: the daemon's parent for test/stalltest.sh - it stops ONE named
# thread of its child with ptrace.  Never sanitized: it is a launcher.
threadfreeze: test/threadfreeze.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -o $@ $<

clean:
	rm -f src/*.o src/compat/*.o src/core/*.o lib/*.o perfcached \
		perfcli perfdump perfload libperfd.a libtest failovertest routedpairtest movedhinttest hinttest capracetest verstamptest arenadbgtest noiseboundtest syncfailshim.so slowlistenshim.so fopenmapshim.so allocshim.so liballoc netcutshim.so lingershim.so threadfreeze clapplytest evactest wedgetest selftest selftest_broken \
		memprobe noisetest pcbench routebench mmclient policybench concbench \
		asynctest clmaptest cltermtest clsynctest clhisttest clplacetest clseltest clpeerstest clpendtest clloctest clretaintest clboottest clpushtest clwiretest clbulktest clrestest clmembtest clunktest clsendtest clfleettest clworktest clpstest clspreadtest clpulltest clcoltest clplattest

# install (S24): binary + annotated example config + systemd unit.
# The live config is NEVER written - only the .example is refreshed.
PREFIX ?= /usr/local
install: perfcached perfcli perfdump perfload libperfd.a
	install -D -m 755 perfcached $(DESTDIR)$(PREFIX)/bin/perfcached
	install -D -m 755 perfcli $(DESTDIR)$(PREFIX)/bin/perfcli
	install -D -m 755 perfdump $(DESTDIR)$(PREFIX)/bin/perfdump
	install -D -m 755 perfload $(DESTDIR)$(PREFIX)/bin/perfload
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

.PHONY: all menuconfig check-standalone shim core check check-wired check-fast check-fault check-asan check-asan-findings hashbench clean install \
	check-edition-guard check-g-core check-g-durability check-g-cluster1 check-g-cluster2 \
	check-g-repl1 check-g-repl2 check-g-repl3
