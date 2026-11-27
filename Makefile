# perfcached - top-level Makefile: the daemon, its tools, libperfd and the test suites

CC      ?= gcc
# the daemon resets /stats on a schedule, and a suite spanning midnight would see its counters
# restart: every suite started from make runs with it off (statsschedtest unsets this)
export PERFCACHED_STATS_RESET ?= off

# config.mk (make menuconfig) first: it names the edition, the allocator and the build FLAVOR,
# and the flavor decides the optimisation flags.
-include config.mk

# the build flavor: release (-O2 -g, the default), debug (-Og -g3, frame pointers, arena checks),
# asan (debug + ASan/UBSan, runtimes linked statically); CFLAGS or SAN on the command line win.
PC_BUILD ?= release
ifeq ($(PC_BUILD),release)
CFLAGS  ?= -O2 -g
else ifeq ($(PC_BUILD),debug)
CFLAGS  ?= -Og -g3 -fno-omit-frame-pointer
else ifeq ($(PC_BUILD),asan)
CFLAGS  ?= -Og -g3 -fno-omit-frame-pointer
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer \
	$(if $(findstring clang,$(shell $(CC) --version 2>/dev/null)),-static-libsan,-static-libasan -static-libubsan)
else
$(error PC_BUILD must be release, debug or asan, not '$(PC_BUILD)' - run make menuconfig)
endif
ifneq ($(PC_BUILD),release)
CFLAGS  += -DPCACHE_ARENA_DEBUG
endif
PC_BUILD_EFF = $(if $(SAN),asan,$(PC_BUILD))
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64
# SAN: sanitizer flags injected into BOTH compile and link (a sanitizer that reaches only one
# of them links clean and checks nothing); `make check-asan` and CI set it.
CFLAGS  += $(SAN)
LDLIBS   = $(SAN) -lpthread -lsodium

# never hardcode ISA extensions here - runtime dispatch decides.

PC_BUILD_REV := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
CFLAGS += -DPC_BUILD_REV=\"$(PC_BUILD_REV)\" -DPC_BUILD_FLAVOR=\"$(PC_BUILD_EFF)\"

# The vendored core builds as close to upstream as possible: the two warning classes upstream
# trips under -Wextra are excepted; every other class still fails the build.
CORE_WNO = -Wno-unused-parameter -Wno-sign-compare
CORE_OBJS = src/core/pcache_mem.o src/core/pcache_htable.o src/core/pcache_arena.o \
	src/core/pcache_opool.o

# the EDITION (config.mk; clustered by default): standalone links cluster_stub.o in place of
# cluster.c and the cl*.c modules - a clustered daemon with no [cluster] section, minus the code.
PC_EDITION ?= clustered
CLUSTER_OBJS = src/cluster.o src/clmap.o src/clterm.o src/clsync.o src/clhist.o \
	src/clplace.o src/clsel.o src/clpeers.o src/clpend.o src/clloc.o \
	src/clboot.o src/clpush.o src/clwire.o src/clbulk.o src/clwork.o \
	src/clmig.o src/clapply.o src/clfwd.o src/clinit.o src/clres.o \
	src/clmemb.o src/clpull.o src/clhold.o src/clrecon.o src/clepoch.o src/clcol.o src/clplat.o src/clsend.o \
	src/clfleet.o src/clps.o src/clspread.o src/clcid.o \
	src/clmesh.o src/clane.o src/clorder.o src/clrp.o src/clrpt.o src/clreplay.o \
	src/clrecv.o src/cltomb.o src/cladmit.o src/clfound.o src/clddl.o src/clconn.o
ifeq ($(PC_EDITION),standalone)
EDITION_OBJS = src/cluster_stub.o
CFLAGS += -DPC_EDITION_STANDALONE
else ifeq ($(PC_EDITION),clustered)
EDITION_OBJS = $(CLUSTER_OBJS)
else
$(error PC_EDITION must be standalone or clustered, not '$(PC_EDITION)' - run make menuconfig)
endif

# the ALLOCATOR: libc, or mimalloc/jemalloc linked AHEAD of the C library to serve the whole process;
# mimalloc pinned to major 2 (3.0 measured slower on the JSON path), its insecure build preferred.
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

# MAIL, the alert mailer's transport: none (the default; -C refuses a [mail] section) or libcurl,
# a dependency only the builds that ask for it carry (libcurl4-openssl-dev, Alpine curl-dev).
PC_MAIL ?= none
ifeq ($(PC_MAIL),libcurl)
ifeq ($(shell pkg-config --exists libcurl 2>/dev/null && echo yes),)
$(error PC_MAIL=libcurl: libcurl not found by pkg-config - install libcurl4-openssl-dev (Alpine: curl-dev), or PC_MAIL=none)
endif
CFLAGS += -DPC_MAIL_CURL $(shell pkg-config --cflags libcurl)
LDLIBS += $(shell pkg-config --libs libcurl)
else ifneq ($(PC_MAIL),none)
$(error PC_MAIL must be none or libcurl, not '$(PC_MAIL)' - run make menuconfig)
endif

# TLS on the RESP and HTTP doors through OpenSSL (libssl-dev, Alpine openssl-dev), daemon only;
# auto (the default) builds it when pkg-config finds openssl; yes insists; no skips.
PC_TLS ?= auto
ifeq ($(PC_TLS),auto)
PC_TLS := $(if $(shell pkg-config --exists openssl 2>/dev/null && echo yes),yes,no)
endif
ifeq ($(PC_TLS),yes)
ifeq ($(shell pkg-config --exists openssl 2>/dev/null && echo yes),)
$(error PC_TLS=yes: openssl not found by pkg-config - install libssl-dev (Alpine: openssl-dev), or PC_TLS=no)
endif
CFLAGS += -DPC_TLS $(shell pkg-config --cflags openssl)
TLS_LIBS = $(shell pkg-config --libs openssl)
else ifneq ($(PC_TLS),no)
$(error PC_TLS must be auto, yes or no, not '$(PC_TLS)')
endif

# perfcached-dashboard: the status page in its own process over libperfd, on libmicrohttpd
# (libmicrohttpd-dev); auto builds it when pkg-config finds the library, yes insists, no skips.
PC_DASH ?= auto
ifeq ($(PC_DASH),auto)
PC_DASH := $(if $(shell pkg-config --exists libmicrohttpd 2>/dev/null && echo yes),yes,no)
endif
ifeq ($(PC_DASH),yes)
ifeq ($(shell pkg-config --exists libmicrohttpd 2>/dev/null && echo yes),)
$(error PC_DASH=yes: libmicrohttpd not found by pkg-config - install libmicrohttpd-dev, or PC_DASH=no)
endif
DASH_CFLAGS = $(shell pkg-config --cflags libmicrohttpd)
DASH_LIBS = $(shell pkg-config --libs libmicrohttpd)
DASH_BIN = perfcached-dashboard
DASH_TEST = dashlocktest
else ifneq ($(PC_DASH),no)
$(error PC_DASH must be auto, yes or no, not '$(PC_DASH)')
endif

all: shim core perfcached perfcli perfdump perfload libperfd.a $(DASH_BIN)

# choose the edition (writes config.mk)
menuconfig:
	@sh tools/menuconfig.sh

# the standalone edition's gate: every suite that needs no cluster (test/standalone.suites),
# in order, stopping at the first failure; CI runs it on the standalone build.
check-standalone: selftest verstamptest perfcached perfcli perfdump perfload pagedump \
		pcbench libtest wedgetest asynctest asyncbintest pubsubcli \
		udppushcli rpsbench syncfailshim.so allocshim.so liballoc noisetest \
		libperfd.a libperfd.so abicaller libtest-so
	./selftest
	./verstamptest
	@grep -vE '^#|^$$' test/standalone.suites | while read -r line; do \
		echo "== $$line"; sh -c "$$line" || exit 1; done

perfcached: src/main.o src/tls.o src/config.o src/cfgcore.o src/cfgcluster.o src/cfgops.o src/cfgcheck.o src/daemon.o src/doorlive.o src/listenfd.o src/cfgsig.o src/autosize.o src/addrwait.o src/dirlock.o src/proto.o src/httpdoor.o src/respdoor.o src/enumwalk.o src/noisechan.o src/json.o \
		src/pc_noise.o src/verbs.o src/jsonpath.o src/hashrec.o src/sha1.o src/ptree.o src/store.o src/storage.o \
		src/walprobe.o src/wal.o src/walscan.o src/statefile.o src/crc32c.o src/rdb.o src/recover.o $(EDITION_OBJS) \
		src/node.o src/clretain.o src/clack.o src/clackf.o src/ackstate.o src/clunk.o src/pcaddr.o \
		src/obs.o src/metrics.o src/events.o src/quiesce.o src/psindex.o src/pubsub.o src/upstream.o src/upjson.o src/upstats.o src/upconn.o src/upsentinel.o src/upcluster.o src/uptls.o src/uprespc.o src/respout.o src/respcl.o src/resppubsub.o src/respkv.o src/respjson.o src/respscript.o src/respsrv.o src/respscan.o src/pcglob.o src/statstree.o src/statscluster.o src/statsreset.o src/winctr.o src/memguard.o src/mail.o src/alerts.o src/hostmon.o src/hostlink.o src/iostat.o src/uring.o src/busypoll.o src/upgrade.o src/upgverify.o src/logrl.o src/relkey.o src/reload.o src/respmap.o src/mgetfan.o src/presence.o src/keeps.o $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) $(TLS_LIBS)

# src/core/*.h belongs here: the daemon's sources read the core's constants (VAL_MAX is
# PCACHE_CELL_MAX), and objects built before a core header change would disagree about them.
src/%.o: src/%.c src/*.h src/core/*.h src/compat/*.h $(wildcard config.mk)
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

# the compatibility shim the vendored core compiles against
shim: src/compat/compat.o

src/compat/compat.o: src/compat/compat.c src/compat/*.h $(wildcard config.mk)
	$(CC) $(CFLAGS) -c -o $@ $<

# the vendored core (see tools/sync-core.sh; provenance in each file)
core: $(CORE_OBJS)

src/core/%.o: src/core/%.c src/core/*.h src/compat/*.h $(wildcard config.mk)
	$(CC) $(CFLAGS) $(CORE_WNO) -c -o $@ $<

# the cluster map format: parses bytes off the network, so it is kept
# free of cluster.c and unit-tested on its own
clmaptest: test/clmaptest.c src/clmap.o
	$(CC) $(CFLAGS) -o $@ test/clmaptest.c src/clmap.o

# the page's bytes for the tests, from the object the dashboard links
pagedump: test/pagedump.c src/statuspage.o src/statuspage.h
	$(CC) $(CFLAGS) -o $@ test/pagedump.c src/statuspage.o

# the mastership term: persistence plus the three ordering rules
cltermtest: test/cltermtest.c src/clterm.o src/statefile.o
	$(CC) $(CFLAGS) -o $@ test/cltermtest.c src/clterm.o src/statefile.o

# the peer table: liveness at the boundary, slot reuse, duplicate ids
clpeerstest: test/clpeerstest.c src/pcaddr.o src/clpeers.o
	$(CC) $(CFLAGS) -o $@ test/clpeerstest.c src/pcaddr.o src/clpeers.o

# parked requests + the completion queues: handles, occupancy, expiry
clpendtest: test/clpendtest.c src/clpend.o src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/clpendtest.c src/clpend.o src/compat/compat.o -lpthread

clmigtest: test/clmigtest.c src/clmig.o
	$(CC) $(CFLAGS) -o $@ test/clmigtest.c src/clmig.o

# the sharded apply rings, without a daemon
clapplytest: test/clapplytest.c src/pcaddr.o src/clapply.o
	$(CC) $(CFLAGS) -o $@ test/clapplytest.c src/pcaddr.o src/clapply.o $(LDLIBS)

clfwdtest: test/clfwdtest.c src/clfwd.o
	$(CC) $(CFLAGS) -o $@ test/clfwdtest.c src/clfwd.o

clinittest: test/clinittest.c src/clinit.o src/fnv1a.h
	$(CC) $(CFLAGS) -o $@ test/clinittest.c src/clinit.o

# the locator and negative caches: TTL decay, the tombstone pair, 4-way
clloctest: test/clloctest.c src/clloc.o src/fnv1a.h
	$(CC) $(CFLAGS) -o $@ test/clloctest.c src/clloc.o

# the retained-tombstone table
clretaintest: test/clretaintest.c src/clretain.o
	$(CC) $(CFLAGS) -o $@ test/clretaintest.c src/clretain.o $(LDLIBS)

# the watermarks - contiguous, durable, gaps, the model
clacktest: test/clacktest.c src/clack.o
	$(CC) $(CFLAGS) -o $@ test/clacktest.c src/clack.o

# the members' announced marks - incarnations, the lowest, the bounds
clackftest: test/clackftest.c src/clackf.o
	$(CC) $(CFLAGS) -o $@ test/clackftest.c src/clackf.o

# the address type
pcaddrtest: test/pcaddrtest.c src/pcaddr.o
	$(CC) $(CFLAGS) -o $@ test/pcaddrtest.c src/pcaddr.o $(LDLIBS)
# the upstream's RESP reply parser, without a daemon
uprespctest: test/uprespctest.c src/uprespc.o src/uprespc.h
	$(CC) $(CFLAGS) -o $@ test/uprespctest.c src/uprespc.o $(LDLIBS)
# the RESP door's reply writers
respouttest: test/respouttest.c src/respout.o src/respout.h src/json.o src/compat/compat.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/respouttest.c src/respout.o src/json.o \
		src/compat/compat.o $(LDLIBS)
# the RESP door's CLUSTER commands, the fleet stubbed
respcltest: test/respcltest.c src/respcl.o src/respout.o src/json.o src/compat/compat.o src/verbsint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/respcltest.c src/respcl.o src/respout.o \
		src/json.o src/compat/compat.o $(LDLIBS) -lpthread
# the RESP door's pub/sub, the engine stubbed
resppubsubtest: test/resppubsubtest.c src/resppubsub.o src/respout.o src/json.o src/compat/compat.o src/verbsint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/resppubsubtest.c src/resppubsub.o \
		src/respout.o src/json.o src/compat/compat.o $(LDLIBS)
# the RESP door's key-value commands over a fake store
# (no compat.o: the test owns the clock)
respkvtest: test/respkvtest.c src/respkv.o src/respout.o src/json.o src/verbsint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/respkvtest.c src/respkv.o src/respout.o \
		src/json.o $(LDLIBS)

# RedisJSON on the RESP door, the JSON core stubbed
respjsontest: test/respjsontest.c src/respjson.o src/respout.o src/jsonpath.o src/json.o src/verbsint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/respjsontest.c src/respjson.o src/respout.o \
		src/jsonpath.o src/json.o $(LDLIBS)

# EVAL / EVALSHA / SCRIPT, the lock core stubbed, SHA1 real
respscripttest: test/respscripttest.c src/respscript.o src/respout.o src/sha1.o src/json.o src/verbsint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/respscripttest.c src/respscript.o src/respout.o \
		src/sha1.o src/json.o $(LDLIBS)

# PING / AUTH / HELLO / INFO / TIME, the store and observers stubbed
respsrvtest: test/respsrvtest.c src/respsrv.o src/respout.o src/json.o src/verbsint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/respsrvtest.c src/respsrv.o src/respout.o \
		src/json.o $(LDLIBS)

# SCAN and KEYS' turn over a fake table
respscantest: test/respscantest.c src/respscan.o src/pcglob.o src/respout.o src/json.o src/verbsint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/respscantest.c src/respscan.o src/pcglob.o \
		src/respout.o src/json.o $(LDLIBS)

# the read-through's figures, the upstream's state stubbed
upstatstest: test/upstatstest.c src/upstats.o src/ptree.o src/json.o src/winctr.o src/compat/compat.o src/upstreamint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/upstatstest.c src/upstats.o src/ptree.o src/json.o \
		src/winctr.o src/compat/compat.o $(LDLIBS)

# the read-through's RESP client over a socketpair
upconntest: test/upconntest.c src/upconn.o src/uprespc.o src/upstreamint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/upconntest.c src/upconn.o src/uprespc.o $(LDLIBS)

# the reconcile's pacing, pure
clrecontest: test/clrecontest.c src/clrecon.o src/clrecon.h src/clpeers.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/clrecontest.c src/clrecon.o

# the metrics door, proto.c's staging captured
httpdoortest: test/httpdoortest.c src/httpdoor.o src/ptree.o src/json.o src/pcaddr.o src/protoint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/httpdoortest.c src/httpdoor.o src/ptree.o src/json.o \
		src/pcaddr.o $(LDLIBS)

# one mget waiting on several lookups, the cluster and the store stubbed
mgetaggtest: test/mgetaggtest.c src/mgetfan.o src/ptree.o src/json.o src/respout.o src/mgetfan.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/mgetaggtest.c src/mgetfan.o src/ptree.o \
		src/json.o src/respout.o $(LDLIBS)

# the RESP door's framing, the verb layer stubbed
respdoortest: test/respdoortest.c src/respdoor.o src/json.o src/protoint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/respdoortest.c src/respdoor.o src/json.o $(LDLIBS)

# the cooperative KEYS walk, the connection's life stubbed
enumwalktest: test/enumwalktest.c src/enumwalk.o src/json.o src/protoint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/enumwalktest.c src/enumwalk.o src/json.o $(LDLIBS)

# the Noise channel's responder side, against pc_noise's initiator
noisechantest: test/noisechantest.c src/noisechan.o src/pc_noise.o src/pcaddr.o src/protoint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/noisechantest.c src/noisechan.o src/pc_noise.o src/pcaddr.o $(LDLIBS)

# the cluster wire epoch, clstate.h stubbed
clepochtest: test/clepochtest.c src/clepoch.o src/clepoch.h src/clstate.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/clepochtest.c src/clepoch.o
clfoundtest: test/clfoundtest.c src/clfound.o src/pcaddr.o src/clfound.h src/clstate.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/clfoundtest.c src/clfound.o src/pcaddr.o

cladmittest: test/cladmittest.c src/cladmit.o src/clpeers.o src/clmemb.o src/pcaddr.o src/cladmit.h src/clstate.h src/clconn.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/cladmittest.c src/cladmit.o src/clpeers.o src/clmemb.o src/pcaddr.o -lpthread
clconntest: test/clconntest.c src/clconn.o src/pcaddr.o src/clconn.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/clconntest.c src/clconn.o src/pcaddr.o
clddltest: test/clddltest.c src/clddl.o src/clcol.o src/pcaddr.o src/clddl.h src/clstate.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/clddltest.c src/clddl.o src/clcol.o src/pcaddr.o -lpthread

cfgkvtest: test/cfgkvtest.c src/config.o src/cfgcore.o src/cfgcluster.o src/cfgops.o src/cfgcheck.o src/pcaddr.o src/relkey.o src/compat/compat.o src/configint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/cfgkvtest.c src/config.o src/cfgcore.o src/cfgcluster.o src/cfgops.o src/cfgcheck.o src/pcaddr.o src/relkey.o src/compat/compat.o $(LDLIBS)

cfgchecktest: test/cfgchecktest.c src/cfgcheck.o src/config.o src/cfgcore.o src/cfgcluster.o src/cfgops.o src/pcaddr.o src/relkey.o src/configint.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/cfgchecktest.c src/cfgcheck.o src/config.o src/cfgcore.o src/cfgcluster.o src/cfgops.o src/pcaddr.o src/relkey.o $(LDLIBS)

doorlivetest: test/doorlivetest.c src/doorlive.o src/pcaddr.o src/doorlive.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/doorlivetest.c src/doorlive.o src/pcaddr.o $(LDLIBS)

listenfdtest: test/listenfdtest.c src/listenfd.o src/pcaddr.o src/listenfd.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/listenfdtest.c src/listenfd.o src/pcaddr.o $(LDLIBS)

cfgsigtest: test/cfgsigtest.c src/cfgsig.o src/cfgsig.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/cfgsigtest.c src/cfgsig.o

autosizetest: test/autosizetest.c src/autosize.o src/autosize.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/autosizetest.c src/autosize.o

hostmontest: test/hostmontest.c src/hostmon.o src/hostmon.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/hostmontest.c src/hostmon.o
# the link monitor against a fake /sys and /proc
hostlinktest: test/hostlinktest.c src/hostlink.o src/hostlink.h src/hostmon.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/hostlinktest.c src/hostlink.o

crc32ctest: test/crc32ctest.c src/crc32c.o src/crc32c.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/crc32ctest.c src/crc32c.o

walscantest: test/walscantest.c src/walscan.o src/crc32c.o src/walfmt.h src/wal.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/walscantest.c src/walscan.o src/crc32c.o

upgverifytest: test/upgverifytest.c src/upgverify.o src/relkey.o src/upgrade.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/upgverifytest.c src/upgverify.o src/relkey.o $(LDLIBS)

clanetest: test/clanetest.c src/clane.o src/pcaddr.o src/clane.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/clanetest.c src/clane.o src/pcaddr.o -lpthread

logrltest: test/logrltest.c src/logrl.o src/pcaddr.o src/logrl.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/logrltest.c src/logrl.o src/pcaddr.o -lpthread

clmeshtest: test/clmeshtest.c src/clmesh.o src/pcaddr.o src/clmesh.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/clmeshtest.c src/clmesh.o src/pcaddr.o -lpthread

# the key glob
pcglobtest: test/pcglobtest.c src/pcglob.o src/pcglob.h
	$(CC) $(CFLAGS) -o $@ test/pcglobtest.c src/pcglob.o

# the stats reset and its schedule, the planes stubbed
statsresettest: test/statsresettest.c src/statsreset.o src/statsreset.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/statsresettest.c src/statsreset.o $(LDLIBS)

# id reservations: the expiry boundary, identity-before-address, the
# eviction choice - all of it invisible on a healthy fleet
clrestest: test/clrestest.c src/clres.o src/clres.h
	$(CC) $(CFLAGS) -o $@ test/clrestest.c src/clres.o

# the keepalive frames, checked DIFFERENTIALLY against the pre-extraction
# layout - MASTER_ALIVE is what master-down detection runs on
clmembtest: test/clmembtest.c src/clmemb.o src/clmemb.h src/clcodec.h
	$(CC) $(CFLAGS) -o $@ test/clmembtest.c src/clmemb.o

# the unknown-command table, its frame and the fleet fold
clunktest: test/clunktest.c src/clunk.o src/clunk.h src/clcodec.h
	$(CC) $(CFLAGS) -o $@ test/clunktest.c src/clunk.o

# the fleet's collection figures, the rules without the peer table
clfleettest: test/clfleettest.c src/clfleet.o src/clfleet.h src/clmemb.h
	$(CC) $(CFLAGS) -o $@ test/clfleettest.c src/clfleet.o

# the worker entry points and the write-path push on the real clwork/clpend/clsend/clpush/clmig
# objects; the test itself supplies cluster.c's accessors (clstate.h)
clworktest: test/clworktest.c src/pcaddr.o src/clwork.o src/clpend.o src/clsend.o src/clpeers.o src/clwire.o src/clplace.o src/clmap.o src/clpull.o src/clfwd.o src/clpush.o src/clmig.o src/iostat.o src/clrp.o src/clrpt.o
	$(CC) $(CFLAGS) -o $@ test/clworktest.c src/pcaddr.o src/clwork.o src/clpend.o src/clsend.o src/clpeers.o src/clwire.o src/clplace.o src/clmap.o src/clpull.o src/clfwd.o src/clpush.o src/clmig.o src/iostat.o src/clrp.o src/clrpt.o -lsodium -lpthread

# the pub/sub relay plane on the real clps/clsend/clpeers/psindex/quiesce objects; the test supplies
# the accessors and callbacks and captures the transmit - nothing binds a socket
clpstest: test/clpstest.c src/pcaddr.o src/clps.o src/clsend.o src/clpeers.o src/clwire.o src/clreplay.o src/psindex.o src/quiesce.o src/iostat.o src/clrp.o src/clrpt.o src/clmig.o src/clps.h
	$(CC) $(CFLAGS) -o $@ test/clpstest.c src/pcaddr.o src/clps.o src/clsend.o src/clpeers.o src/clwire.o src/clreplay.o src/psindex.o src/quiesce.o src/iostat.o src/clrp.o src/clrpt.o src/clmig.o -lsodium -lpthread

# spread's decisions on the real clspread/clplace objects; placement's
# answers are the test's own
clspreadtest: test/clspreadtest.c src/pcaddr.o src/clspread.o src/clplace.o src/clmap.o src/clspread.h
	$(CC) $(CFLAGS) -o $@ test/clspreadtest.c src/pcaddr.o src/clspread.o src/clplace.o src/clmap.o

# the send path (clsend), no socket - the transmit is captured
clsendtest: test/clsendtest.c src/pcaddr.o src/clsend.o src/clpeers.o src/clwire.o src/iostat.o src/clsend.h
	$(CC) $(CFLAGS) -o $@ test/clsendtest.c src/pcaddr.o src/clsend.o src/clpeers.o src/clwire.o src/iostat.o -lsodium -lpthread

# the PULL plane: a store miss asks a peer, and the VERSION rides back
# with the bytes - checked against the pre-extraction layout
clpulltest: test/clpulltest.c src/clpull.o src/clpull.h src/clcodec.h
	$(CC) $(CFLAGS) -o $@ test/clpulltest.c src/clpull.o
# the hold-check frames
clholdtest: test/clholdtest.c src/clhold.o src/clhold.h src/clmsg.h
	$(CC) $(CFLAGS) -o $@ test/clholdtest.c src/clhold.o

# the collection announce: four ops in one frame, and RENAME carries a
# second name the other three do not
clcoltest: test/clcoltest.c src/clcol.o src/clcol.h src/clcodec.h
	$(CC) $(CFLAGS) -o $@ test/clcoltest.c src/clcol.o

# platform-derived identity: a clone must not inherit a member's identity
clplattest: test/clplattest.c src/clplat.o src/clplat.h
	$(CC) $(CFLAGS) -o $@ test/clplattest.c src/clplat.o -lsodium

# the bootstrap decision: rounds, deadlines, the handoff
clboottest: test/clboottest.c src/pcaddr.o src/clboot.o
	$(CC) $(CFLAGS) -o $@ test/clboottest.c src/pcaddr.o src/clboot.o -lpthread

# the write-path push groups: per-thread, the two send points, the flush
clpushtest: test/clpushtest.c src/pcaddr.o src/clpush.o
	$(CC) $(CFLAGS) -o $@ test/clpushtest.c src/pcaddr.o src/clpush.o -lpthread

# the sealed datagram and the beat frames: round trip, tamper, wrong key
clwiretest: test/clwiretest.c src/clwire.o
	$(CC) $(CFLAGS) -o $@ test/clwiretest.c src/clwire.o -lsodium -lpthread
# a sealed frame is taken once - the stream window, incarnations, the header
clreplaytest: test/clreplaytest.c src/clreplay.o src/clwire.o
	$(CC) $(CFLAGS) -o $@ test/clreplaytest.c src/clreplay.o src/clwire.o -lsodium -lpthread
# replication groups over TCP - framing, the queue's bound, refusals
clrpttest: test/clrpttest.c src/clrpt.o src/pcaddr.o src/clrpt.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/clrpttest.c src/clrpt.o src/pcaddr.o $(LDLIBS)

# the bulk plane's framing and batch handoff: round trip, truncation, handoff
clbulktest: test/clbulktest.c src/pcaddr.o src/clbulk.o src/pc_noise.o src/compat/compat.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/clbulktest.c src/pcaddr.o src/clbulk.o src/pc_noise.o \
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

# the mastership state machine under seed-replayable schedules, linking the SHIPPED decision
# functions: the simulation is the protocol around them, never a reimplementation of them.
clustersim: test/clustersim.c src/clterm.o src/clmap.o src/statefile.o
	$(CC) $(CFLAGS) -o $@ test/clustersim.c src/clterm.o src/clmap.o src/statefile.o

# the reply writer, including the heap mode CLUSTER SLOTS needs
jwtest: test/jwtest.c src/json.o src/compat/compat.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/jwtest.c src/json.o \
		src/compat/compat.o $(LDLIBS)

# the WAL pump's ring copy against the byte loop it replaced
walringtest: test/walringtest.c src/walring.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/walringtest.c
rpcqtest: test/rpcqtest.c src/rpcq.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/rpcqtest.c

# the node's state set from two threads (node.c, its test hook)
nodetest: test/nodetest.c src/node.c src/cluster.h src/compat/compat.c
	$(CC) $(CFLAGS) -DPC_TESTHOOKS -Isrc -o $@ test/nodetest.c src/node.c src/compat/compat.c $(LDLIBS)
clprobetest: test/clprobetest.c src/clprobe.h src/pcaddr.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/clprobetest.c src/pcaddr.o $(LDLIBS)
thrtabtest: test/thrtabtest.c src/thrtab.h src/clapply.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/thrtabtest.c
promlabeltest: test/promlabeltest.c src/promlabel.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/promlabeltest.c
upjsontest: test/upjsontest.c src/upjson.o src/json.o src/compat/compat.o src/upjson.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/upjsontest.c src/upjson.o src/json.o src/compat/compat.o $(LDLIBS)

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
		src/core/pcache_htable.c src/core/pcache_arena.c src/core/pcache_opool.c
	$(CC) $(CFLAGS) $(CORE_WNO) -DPC_COMPAT_BROKEN_LOCKS -o $@ $^ $(LDLIBS)

# The whole suite under ASan+UBSan, same spelling locally and in CI.  detect_leaks=0: per-thread
# scratch and the obs registries live for the process by design; halt_on_error makes UBSan loud.
check-asan:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=0$${LD_PRELOAD:+:verify_asan_link_order=0} \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -DPCACHE_ARENA_DEBUG" \
		check

# One suite group under ASan+UBSan - the CI's per-group asan jobs (same
# flags as check-asan, which runs every group)
check-asan-g-%:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=0$${LD_PRELOAD:+:verify_asan_link_order=0} \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -DPCACHE_ARENA_DEBUG" \
		check-g-$*

# scan a sanitizer log (LOG=) for findings, EXCLUDING the broken-locks range, which always holds
# some by design; exit 1 if anything real is found, so it can gate.
check-asan-findings:
	@test -n "$(LOG)" || { echo "usage: make check-asan-findings LOG=<file>"; exit 2; }
	@awk '/^--- broken-locks build/{skip=1} /^--- end broken-locks/{skip=0; next} \
	      !skip && /runtime error:|ERROR: AddressSanitizer/{print; n++} \
	      END{if (n) {printf "%d sanitizer finding(s) OUTSIDE the broken-locks control\n", n; exit 1} \
	          else print "no sanitizer findings outside the broken-locks control"}' "$(LOG)"

# The fault suites, out of `check` (each waits minutes on liveness windows): fault1/2 drive the
# container fleet (root), fault3/4 loopback daemons; one that cannot run SKIPs, a failure in CI.
check-fault:
	@for g in fault1 fault2 fault3 fault4; do $(MAKE) --no-print-directory -j1 check-g-$$g || exit 1; done

check-g-fault1: perfcached perfcli
	sh test/partitiontest.sh
	sh test/splitbraintest.sh
	sh test/churntest.sh

check-g-fault2: perfcached perfcli
	sh test/reshardtest.sh
	sh test/stepdowntest.sh
	sh test/dualclaimtest.sh

# The FAST gate: every unit binary plus the suites that finish in seconds - what a branch push
# must pass; `check` in full rides tag pushes.
check-fast: pagedump $(DASH_BIN) $(DASH_TEST) selftest selftest_broken memprobe noisetest noiseboundtest wipetest perfcached pubsubcli udppushcli \
		perfcli pcbench clmaptest cltermtest clsynctest clhisttest \
		clplacetest clseltest clpeerstest clpendtest clloctest clretaintest clacktest clackftest pcaddrtest uprespctest respouttest respcltest resppubsubtest respkvtest mgetaggtest respjsontest respscripttest respsrvtest respscantest pcglobtest upstatstest upconntest clrecontest httpdoortest respdoortest enumwalktest noisechantest clepochtest clfoundtest cladmittest clddltest clconntest perfdkindtest cfgkvtest cfgchecktest doorlivetest listenfdtest cfgsigtest autosizetest hostmontest hostlinktest crc32ctest walscantest upgverifytest clanetest logrltest clmeshtest statsresettest clboottest clpushtest clwiretest clreplaytest clrpttest clbulktest clmigtest clfwdtest clinittest clrestest clmembtest clunktest clsendtest clfleettest clworktest clpstest clspreadtest clpulltest clholdtest clcoltest clplattest natbench slottest jwtest ptreetest hashrectest walringtest rpcqtest clprobetest nodetest asyncbintest routedpairtest movedhinttest hinttest workerbaltest respreadtest frozentest quietlinktest asyncstatetest libperfd.a thrtabtest promlabeltest obstest upjsontest perfdlinktest apooltest perfdurltest perfdexectest cliupgtest dumpreadytest dumpchunktest loadrefusetest \
		clustersim psrelaytest psindextest psqueuetest psinteresttest psudptest rpsbench legmetatest legwalkremovetest htfuzztest cellpoisontest chainhinttest clordertest draincarrytest chainwalktest wintest scanremovetest histtest capracetest verstamptest arenadbgtest syncfailshim.so slowlistenshim.so fopenmapshim.so allocshim.so liballoc threadfreeze clapplytest evactest arenacaptest addexptest respmaptest
	./clustersim 500
	./clapplytest
	./respmaptest
	./evactest
	./arenacaptest
	./addexptest
	./slottest
	./legmetatest
	./legwalkremovetest
	./htfuzztest
	./cellpoisontest
	./chainhinttest
	./clordertest
	./draincarrytest
	./chainwalktest
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
	./rpcqtest
	./apooltest
	./nodetest
	./clprobetest
	./thrtabtest
	./promlabeltest
	./obstest
	./upjsontest
	sh test/readmetest.sh
	# cluster.c, upstream.c and verbs.c within their line budgets
	sh test/sizeratchet.sh
	sh test/readytest.sh ./perfcached ./slowlistenshim.so
	sh test/cleardoortest.sh ./perfcached ./perfcli ./fopenmapshim.so
	sh test/standalonetest.sh ./perfcached ./noisetest
	sh test/maxclientstest.sh ./perfcached
# a slow consumer's close says so (it read "peer closed")
	sh test/slowreadtest.sh ./perfcached
# busy_poll_us - the switch, the per-worker check, the refusal
	sh test/busypolltest.sh ./perfcached
# CPU, disk, host memory and arena alarms; process.cpus
	sh test/hostalerttest.sh ./perfcached
	sh test/memguardtest.sh ./perfcached
	sh test/nativerefusetest.sh ./perfcached
	sh test/hsparktest.sh ./perfcached ./noisetest
	# libperfd seals once per flush - the server opens far fewer records than requests
	sh test/sealbatchtest.sh ./perfcached ./natbench
	# authenticated plaintext by default, Noise where asked or required
	sh test/plainauthtest.sh ./perfcached ./perfcli ./natbench
	sh test/idlebuftest.sh ./perfcached
	sh test/sweepwalktest.sh ./perfcached
	sh test/pubsubtest.sh ./perfcached ./pubsubcli
	sh test/pushbatchtest.sh ./perfcached
	sh test/wiredraintest.sh ./perfcached
	sh test/expireleaktest.sh ./perfcached
	sh test/scanresizetest.sh ./perfcached
	sh test/resizedeltest.sh ./perfcached
	sh test/refusedlogtest.sh ./perfcached
	sh test/prefetchtest.sh ./perfcached
	sh test/pubsubpressuretest.sh ./perfcached ./rpsbench
	sh test/psnotifytest.sh ./perfcached
	sh test/pubsubrelaytest.sh ./perfcached
	sh test/jdeltest.sh ./perfcached
	sh test/xsstest.sh ./perfcached ./perfcached-dashboard
	sh test/deltombtest.sh ./perfcached
	sh test/routedpairtest.sh ./perfcached ./routedpairtest
	sh test/expiretest.sh ./perfcached
	sh test/delprobetest.sh ./perfcached
	sh test/plaintextdoortest.sh ./perfcached ./perfcli
	sh test/movedhinttest.sh ./perfcached ./movedhinttest
	sh test/workerbaltest.sh ./perfcached ./workerbaltest
	sh test/respreadtest.sh ./perfcached ./respreadtest ./syncfailshim.so ./threadfreeze
	sh test/frozentest.sh ./perfcached ./frozentest
	sh test/quietlinktest.sh ./perfcached ./quietlinktest
	sh test/asyncstatetest.sh ./perfcached ./asyncstatetest
	sh test/upclustertest.sh ./perfcached
	sh test/tlsdoortest.sh ./perfcached
	sh test/uptlstest.sh ./perfcached
	sh test/respmovedtest.sh ./perfcached
	sh test/syncfailtest.sh ./perfcached ./syncfailshim.so
	sh test/stalltest.sh ./perfcached ./threadfreeze ./perfcli
	sh test/stallmastertest.sh ./perfcached ./threadfreeze ./perfcli
	sh test/applythreadstest.sh ./perfcached
	sh test/pullordertest.sh ./perfcached
	sh test/relayrxtest.sh ./perfcached
	sh test/replplanetest.sh ./perfcached
	sh test/waltest.sh ./perfcached
	sh test/relayinteresttest.sh ./perfcached
	sh test/udppushtest.sh ./perfcached ./udppushcli
	sh test/httpidletest.sh ./perfcached
	sh test/httpsweeptest.sh ./perfcached
	sh test/metricsresettest.sh ./perfcached
	sh test/statsschedtest.sh ./perfcached
	sh test/doorwraptest.sh ./perfcached
	sh test/keyspacetest.sh ./perfcached
	sh test/cmdstatstest.sh ./perfcached
	sh test/hotkeytest.sh ./perfcached
	sh test/reloadtest.sh ./perfcached ./perfcli
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
	./clacktest
	./clackftest
	./pcaddrtest
	./uprespctest
	./respouttest
	./respcltest
	./resppubsubtest
	./respkvtest
	./mgetaggtest
	./respjsontest
	./respscripttest
	./respsrvtest
	./respscantest
	./pcglobtest
	./upstatstest
	./upconntest
	./clrecontest
	./httpdoortest
	./respdoortest
	./enumwalktest
	./noisechantest
	./clepochtest
	./clfoundtest
	./cladmittest
	./clddltest
	./clconntest
	./perfdkindtest
	./perfdlinktest
	./perfdurltest
	./perfdexectest
	./cliupgtest
	$(if $(DASH_TEST),./$(DASH_TEST))
	./dumpreadytest
	./dumpchunktest
	./loadrefusetest
	./cfgkvtest
	./cfgchecktest
	./doorlivetest
	./listenfdtest
	./cfgsigtest
	./autosizetest
	./hostmontest
	./hostlinktest
	./crc32ctest
	./walscantest
	./upgverifytest
	./clanetest
	./logrltest
	./clmeshtest
	./statsresettest
	./clrestest
	./clmembtest
	./clunktest
	./clsendtest
	./clfleettest
	./clworktest
	./clpstest
	./clspreadtest
	./clpulltest
	./clholdtest
	./clcoltest
	./clplattest
	./clboottest
	./clpushtest
	./clwiretest
	./clreplaytest
	./clrpttest
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
	sh test/stagetest.sh ./perfcached ./perfcli ./syncfailshim.so
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
	sh test/mitboundtest.sh libperfd.a
	sh test/asyncbin.sh ./perfcached ./asyncbintest
# this build's locks are DELIBERATELY BROKEN and it must fail: its sanitizer findings are the point,
# and the markers let `make check-asan-findings` exclude them.
	@echo "--- broken-locks build (must fail) ---"
	@if ./selftest_broken; then \
		echo "check-fast FAILED: the broken-locks storm PASSED"; exit 1; \
	else echo "broken-locks storm failed as it must"; fi
	@echo "--- end broken-locks (its sanitizer findings are expected) ---"
	@echo "FAST CHECKS PASS (cluster/storage suites ride the full check)"

# statlint on its own, for the edit-test loop; it is in `check` as well - a suite only this
# target runs is a suite CI never runs.
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

# the coverage sketch counts DISTINCT served keys; epochs are driven through compat_ticks_offset,
# so it asserts real cardinalities without sleeping for a window.
reachtest: test/reachtest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/reachtest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# client-only totals and reach are a slot RANGE; this proves the
# arithmetic, the reset, and that an unregistered range reads as before.
origintest: test/origintest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/origintest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# a record in the overflow leg walks out with the same version, flags and write tick as one in a
# bucket; the arena is compiled WITH its debug invariants, into this binary only
arenadbgtest: test/arenadbgtest.c src/core/pcache_arena.c src/core/pcache_mem.o \
		src/core/pcache_htable.o src/core/pcache_opool.o src/compat/compat.o
	$(CC) $(CFLAGS) $(CORE_WNO) -DPCACHE_ARENA_DEBUG -o $@ $^ $(LDLIBS)

capracetest: test/capracetest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/capracetest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# includes the table itself, to hold a bucket lock - so it links
# every core object BUT the table
verstamptest: test/verstamptest.c src/core/pcache_htable.c src/core/pcache_htable.h $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) $(CORE_WNO) -o $@ test/verstamptest.c \
		$(filter-out src/core/pcache_htable.o,$(CORE_OBJS)) \
		src/compat/compat.o $(LDLIBS)

legmetatest: test/legmetatest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/legmetatest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# a walk whose callback removes each record still hands over
# every record - the shape of a proxy migration.
legwalkremovetest: test/legwalkremovetest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/legwalkremovetest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# ASan's view of the arena's cells, read back from its shadow (an
# ASan build checks; a plain one says it skipped)
cellpoisontest: test/cellpoisontest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/cellpoisontest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# the shadow-model fuzzer - the same hooked table, so the maintenance
# thread's work runs inside a read's window too
htfuzztest: test/htfuzztest.c src/core/pcache_htable.c src/core/*.h \
		src/core/pcache_mem.o src/core/pcache_arena.o src/core/pcache_opool.o \
		src/compat/compat.o
	$(CC) $(CFLAGS) $(CORE_WNO) -DPCACHE_TEST_HOOKS -o $@ test/htfuzztest.c \
		src/core/pcache_htable.c src/core/pcache_mem.o \
		src/core/pcache_arena.o src/core/pcache_opool.o src/compat/compat.o \
		$(LDLIBS)

# a walk hands over a chain exactly once while removes move its tail
chainwalktest: test/chainwalktest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/chainwalktest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# a set and a delete of one key on two receive threads
draincarrytest: test/draincarrytest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/draincarrytest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS) -lpthread

clordertest: test/clordertest.c src/clorder.o $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/clordertest.c src/clorder.o $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# a chained record is reaped when due
chainhinttest: test/chainhinttest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/chainhinttest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# the five-minute counter windows, on an injected clock
wintest: test/wintest.c src/winctr.o src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/wintest.c src/winctr.o src/compat/compat.o $(LDLIBS)

# the cursored scan (the drop's walk) removes as it goes, one pass.
scanremovetest: test/scanremovetest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/scanremovetest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# the latency histogram's resolution - a percentile must estimate
# the value, not name a power of two above it.
histtest: test/histtest.c src/pcaddr.o src/obs.o src/json.o src/ptree.o src/clunk.o src/compat/compat.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/histtest.c src/pcaddr.o src/obs.o src/json.o src/ptree.o src/clunk.o \
		src/compat/compat.o $(LDLIBS)
# what the command rows and the query log keep of a client's bytes
obstest: test/obstest.c src/pcaddr.o src/obs.o src/json.o src/ptree.o src/clunk.o src/compat/compat.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/obstest.c src/pcaddr.o src/obs.o src/json.o src/ptree.o src/clunk.o \
		src/compat/compat.o $(LDLIBS)

# the index geometry - a segment is one slot and a sixteenth - and that what index_bytes
# quotes is what regions_bytes moves by.
indextest: test/indextest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/indextest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# the quiescent-state primitive a table retirement waits on.
quiesctest: test/quiesctest.c src/quiesce.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/quiesctest.c src/quiesce.o -lpthread

# the RESP door's DB-index map at runtime, without a daemon
respmaptest: test/respmaptest.c src/respmap.o src/quiesce.o src/compat/compat.o src/respmap.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/respmaptest.c src/respmap.o src/quiesce.o src/compat/compat.o -lpthread

# a retired table's slots come back - taken before the frontier
# moves, punched out past the cool-off - and RSS says so.
regiontest: test/regiontest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/regiontest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# a carve while a tail punch is in flight lands outside it
tailflighttest: test/tailflighttest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/tailflighttest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# the hash record's pure functions
hashrectest: test/hashrectest.c src/hashrec.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/hashrectest.c src/hashrec.o -lm

# HGET and HSET ops/s (and the daemon's CPU per op) on a ~50 KB and a ~500 B hash; a bench,
# not a gate, so it is not under make check.
hashbench: perfcached
	sh test/hashbench.sh ./perfcached

# the native door's value codec
ptreetest: test/ptreetest.c src/ptree.o src/json.o src/compat/compat.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/ptreetest.c src/ptree.o src/json.o \
		src/compat/compat.o $(LDLIBS)

# the survivors of a mass expiry, moved out of the chunks they pin
evactest: test/evactest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/evactest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# a punched slot read as used by the capacity scan
arenacaptest: test/arenacaptest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/arenacaptest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

# INCR and EXPIRE on a record past its deadline, not swept yet
addexptest: test/addexptest.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/addexptest.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

check-wired:
	@for t in test/*.sh; do grep -q "sh $$t" Makefile || { echo "check: $$t is not run by make check - wire it in or remove it"; exit 1; }; done

# The binaries every suite group needs - check and each group build them.
CHECK_BINS = $(DASH_BIN) $(DASH_TEST) pagedump rollload selftest selftest_broken memprobe noisetest noiseboundtest libtest-so abicaller \
	wipetest perfcached perfcli perfdump perfload pcbench libtest execcmp failovertest \
	routedpairtest movedhinttest hinttest workerbaltest respreadtest privmovetest frozentest quietlinktest asyncstatetest mgetextest wedgetest asynctest asyncbintest clmaptest \
	cltermtest clsynctest clhisttest clplacetest clseltest clpeerstest \
	clpendtest clloctest clretaintest clacktest clackftest pcaddrtest uprespctest respouttest respcltest resppubsubtest respkvtest mgetaggtest respjsontest respscripttest respsrvtest respscantest pcglobtest upstatstest upconntest clrecontest httpdoortest respdoortest enumwalktest noisechantest clepochtest clfoundtest cladmittest clddltest clconntest perfdkindtest perfdlinktest perfdurltest perfdexectest cliupgtest dumpreadytest dumpchunktest loadrefusetest cfgkvtest cfgchecktest doorlivetest listenfdtest cfgsigtest autosizetest hostmontest hostlinktest crc32ctest walscantest upgverifytest clanetest logrltest clmeshtest statsresettest clboottest clpushtest clwiretest clreplaytest clrpttest \
	clbulktest clmigtest clfwdtest clinittest clrestest clmembtest clunktest \
	clsendtest clfleettest clworktest clpstest clspreadtest clpulltest clholdtest \
	clcoltest clplattest natbench slottest jwtest ptreetest hashrectest walringtest rpcqtest apooltest apoollivetest nodetest clprobetest thrtabtest promlabeltest obstest clustersim upjsontest \
	reachtest origintest indextest quiesctest regiontest tailflighttest legmetatest \
	legwalkremovetest htfuzztest cellpoisontest chainhinttest clordertest draincarrytest chainwalktest wintest scanremovetest histtest capracetest verstamptest \
	arenadbgtest syncfailshim.so slowlistenshim.so fopenmapshim.so allocshim.so liballoc netcutshim.so lingershim.so threadfreeze clinject getbusy clapplytest threadfailshim.so \
	evactest arenacaptest addexptest respmaptest

# The full ladder in GROUPS, one CI job each: `make check` runs every group, check-g-<group> one,
# check-asan-g-<group> one under ASan+UBSan; check-wired keeps every test/*.sh in some group.
CHECK_GROUPS = core1 core2 durability cluster1 cluster2 cluster3 repl1 repl2 repl3 fault3 fault4

check-edition-guard:
	@if [ "$(PC_EDITION)" = standalone ]; then \
		echo "make check: this is the STANDALONE edition - half of these suites need the cluster; its gate is make check-standalone"; \
		exit 1; fi

check: check-wired check-edition-guard $(CHECK_BINS)
	@for g in $(CHECK_GROUPS); do $(MAKE) --no-print-directory -j1 check-g-$$g || exit 1; done

check-g-core1: check-edition-guard $(CHECK_BINS)
	./clustersim 5000
	./clapplytest
	./evactest
	./arenacaptest
	./addexptest
	./reachtest
	./origintest
	./indextest
	./legmetatest
	./legwalkremovetest
	./htfuzztest
	./cellpoisontest
	./chainhinttest
	./clordertest
	./draincarrytest
	./chainwalktest
	./wintest
	./scanremovetest
	./histtest
	./capracetest
	./verstamptest
	./arenadbgtest
	./quiesctest
	./respmaptest
	./regiontest
	./tailflighttest
	./slottest
	./jwtest
	./ptreetest
	./hashrectest
	./walringtest
	./rpcqtest
	./apooltest
	./nodetest
	./clprobetest
	./thrtabtest
	./promlabeltest
	./obstest
	./upjsontest
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
	./clacktest
	./clackftest
	./pcaddrtest
	./uprespctest
	./respouttest
	./respcltest
	./resppubsubtest
	./respkvtest
	./mgetaggtest
	./respjsontest
	./respscripttest
	./respsrvtest
	./respscantest
	./pcglobtest
	./upstatstest
	./upconntest
	./clrecontest
	./httpdoortest
	./respdoortest
	./enumwalktest
	./noisechantest
	./clepochtest
	./clfoundtest
	./cladmittest
	./clddltest
	./clconntest
	./perfdkindtest
	./perfdlinktest
	./perfdurltest
	./perfdexectest
	./cliupgtest
	$(if $(DASH_TEST),./$(DASH_TEST))
	./dumpreadytest
	./dumpchunktest
	./loadrefusetest
	./cfgkvtest
	./cfgchecktest
	./doorlivetest
	./listenfdtest
	./cfgsigtest
	./autosizetest
	./hostmontest
	./hostlinktest
	./crc32ctest
	./walscantest
	./upgverifytest
	./clanetest
	./logrltest
	./clmeshtest
	./statsresettest
	./clrestest
	./clmembtest
	./clunktest
	sh test/hashtest.sh ./perfcached
	sh test/hashfwdtest.sh ./perfcached
	sh test/cmptest.sh ./perfcached
	sh test/symlocktest.sh ./perfcached
	sh test/jsonalloctest.sh ./perfcached ./allocshim.so
	sh test/liballoctest.sh ./perfcached ./liballoc ./allocshim.so
	sh test/buildflavortest.sh ./perfcached
	sh test/mailtest.sh ./perfcached
	sh test/alerttest.sh ./perfcached
	sh test/ratelimittest.sh ./perfcached
	sh test/difftest.sh ./perfcached
	sh test/upstreamtest.sh ./perfcached
	# a read-through collection and its companion are not renamed live
	sh test/uprenametest.sh ./perfcached
	# a read-through copy never outlives the upstream's key
	sh test/upttltest.sh ./perfcached
# several upstreams, replicas only - the master refused, the reads
# spread, one lost or promoted, the master fallback
	sh test/upreplicatest.sh ./perfcached
# the upstream password per collection - two Redis, two passwords
	sh test/upsecrettest.sh ./perfcached
# no ring of read-throughs - a perfcached upstream refused, or one hop
	sh test/uploopguardtest.sh ./perfcached
# upstream_mode = failover - primary, preempt, the hold
	sh test/upfailovertest.sh ./perfcached
# upstream_writes = through - the master written first, the keys read
# back, unknown commands classed by Redis, no master, failover, no RedisJSON
	sh test/upwritethroughtest.sh ./perfcached
	sh test/upechomastertest.sh ./perfcached
	# the IPv6 guard - no AAAA query from a host without IPv6
	sh test/ipv6guardtest.sh ./perfcached
	sh test/upinvflighttest.sh ./perfcached
# many writers on one key, written through - the read-backs applied in
# the writer's order, a late one skipped
	sh test/upwtordertest.sh ./perfcached
# fleet_owned counts no write in flight
	sh test/upownedtest.sh ./perfcached
# an upstream answering out of the ordinary - errors, odd COMMAND INFO numbers, late replies
	sh test/upfaulttest.sh ./perfcached
# a resize under the upfill sweep: the upstream threads re-find their tables
	sh test/upfillresizetest.sh ./perfcached
	sh test/upclustertest.sh ./perfcached
	sh test/updiscovertest.sh ./perfcached
	sh test/upsentineltest.sh ./perfcached
# a read whose fetched copy finds no room in the arena answers the value
	sh test/upnoroomtest.sh ./perfcached
	sh test/upnoroomshardtest.sh ./perfcached
	sh test/tlsdoortest.sh ./perfcached
	sh test/uptlstest.sh ./perfcached
	./clsendtest
	./clfleettest
	./clworktest
	./clpstest
	./clspreadtest
	./clpulltest
	./clholdtest
	./clcoltest
	./clplattest
	./clboottest
	./clpushtest
	./clwiretest
	./clreplaytest
	./clrpttest
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
	sh test/stagetest.sh ./perfcached ./perfcli ./syncfailshim.so
	sh test/daemontest.sh ./perfcached
	sh test/prototest.sh ./perfcached
	sh test/bintest.sh ./perfcached
	sh test/noiseinterop.sh ./perfcached
	sh test/verbtest.sh ./perfcached
	sh test/scantest.sh ./perfcached
	sh test/keysyieldtest.sh ./perfcached

# the second half of core, split at keysyieldtest by the suites' timings: core was the longest job
check-g-core2: check-edition-guard $(CHECK_BINS)
	sh test/keysretiretest.sh ./perfcached ./perfcli
	sh test/keysbudgettest.sh ./perfcached
	sh test/shrinktest.sh ./perfcached ./perfcli
	# a resize whose copies find no room waits; it never drops what it could not copy
	sh test/resizeroomtest.sh ./perfcached ./perfcli
	# a dropped table's index goes back to the records' room when it borders the frontier
	sh test/arenafreetest.sh ./perfcached
	# the client doors on IPv6, the fleet on IPv4
	sh test/v6doortest.sh ./perfcached ./perfcli
	sh test/grafanatest.sh ./perfcached
	sh test/storagetest.sh ./perfcached
	sh test/pinbothtest.sh ./perfcached
	# a thread start-up cannot create stops the start; a SIGHUP during start-up is a reload
	sh test/startuptest.sh ./perfcached ./threadfailshim.so
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
	sh test/xsstest.sh ./perfcached ./perfcached-dashboard
	sh test/deltombtest.sh ./perfcached
# cluster.peer_absent only when the retained deletes did not cover the
# absence - a short freeze under a biting cap raises nothing
	sh test/tombgaptest.sh ./perfcached
	# numbered deletes logged, counted, confirmed when synced; the marks over a restart
	sh test/tombacktest.sh ./perfcached
	# a member on yes among members on no keeps the time rule; the fleet switched one at a time confirms
	sh test/tombmixtest.sh ./perfcached ./netcutshim.so
	# a hole over deletes the origin has superseded is floored with no rebuild, its loss table full
	sh test/tombsupertest.sh ./perfcached ./netcutshim.so
	# a delete forgotten once every member holds it; a frozen member keeps what it missed
	sh test/tombforgettest.sh ./perfcached
	sh test/tombspacetest.sh ./perfcached
	# a lost delete is a hole the member asks for
	sh test/tombholetest.sh ./perfcached ./netcutshim.so
	# a joiner floors its spaces at what its bootstrap's sources confirmed
	sh test/tombjointest.sh ./perfcached
	# a member back from an absence the deletes no longer cover is asked to rebuild
	sh test/tombrebuildtest.sh ./perfcached ./netcutshim.so
	# on a multicast fleet a delete goes to the group once
	sh test/tombmcasttest.sh ./perfcached
	# a stuck member is seen - /members, /stats held_by, cluster.acks_stuck
	sh test/tombstucktest.sh ./perfcached
	# a member confirms only what its WAL synced; one restored from an older snapshot walks
	sh test/tombsynctest.sh ./perfcached ./syncfailshim.so
	sh test/waldropacktest.sh ./perfcached ./syncfailshim.so
	sh test/tombrestoretest.sh ./perfcached
	# a member nobody can vouch for (no witness while the fleet lived on, or away longer than
	# resync_after_s) resyncs: it drops what it stored before and pulls the fleet's copy
	sh test/tombresynctest.sh ./perfcached ./netcutshim.so
	sh test/pagejstest.sh ./perfcached
	sh test/dashtest.sh ./perfcached ./perfcached-dashboard
	# a learned member that left the fleet: shown left, not dialled
	sh test/dashlefttest.sh ./perfcached ./perfcached-dashboard
	sh test/sketchtest.sh ./perfcached ./perfcached-dashboard
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
	sh test/libtest.sh ./perfcached ./libtest-so
	sh test/exectest.sh ./perfcached ./execcmp
	sh test/abitest.sh ./perfcached ./abicaller
	sh test/expiretest.sh ./perfcached
	sh test/plaintextdoortest.sh ./perfcached ./perfcli
	sh test/fuzztest.sh ./perfcached
	sh test/admintest.sh ./perfcached
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
	sh test/walmarktest.sh ./perfcached
	# a start that restored nothing resumes the WAL sequence above the snapshot's marker
	sh test/walmarkertest.sh ./perfcached
	# a durable file's directory is synced: the WAL's new segments, the collections file
	sh test/dirsynctest.sh ./perfcached ./syncfailshim.so ./perfcli
	sh test/waldroptest.sh ./perfcached ./pcbench ./perfcli ./syncfailshim.so
	# fsync = sync: a reply waits for its write's sync, on both doors, in order; always does not
	sh test/walsynctest.sh ./perfcached ./syncfailshim.so
	# a full log waits - no overwrite, no heal - and /stats answers while writers are held
	sh test/walfulltest.sh ./perfcached ./pcbench ./perfcli ./syncfailshim.so
	# the segments follow the load - grow under a fill, kept across a restart, given back when quiet
	sh test/walgrowtest.sh ./perfcached ./pcbench ./perfcli
	sh test/rdbtest.sh ./perfcached
	sh test/recoverytest.sh ./perfcached
	# a cache collection (durable = no) - off the WAL and the snapshot, empty after a restart
	sh test/cachecoltest.sh ./perfcached
	sh test/downgradetest.sh ./perfcached
	sh test/switchtest.sh ./perfcached
	sh test/walctrltest.sh ./perfcached
	sh test/waloverruntest.sh ./perfcached
	sh test/healtest.sh ./perfcached ./syncfailshim.so
	sh test/healroutinetest.sh ./perfcached ./syncfailshim.so
	sh test/rdbkilltest.sh ./perfcached
	sh test/walsizetest.sh ./perfcached
	sh test/walresidenttest.sh ./perfcached
	sh test/walringsizetest.sh ./perfcached
	sh test/walordertest.sh ./perfcached
	# a snapshot whose walk a resize swaps and drains under holds every record
	sh test/snapdraintest.sh ./perfcached ./perfcli
	sh test/expirereaptest.sh ./perfcached
	sh test/emptywraptest.sh ./perfcached
	sh test/walobstest.sh ./perfcached
	sh test/waldisktest.sh ./perfcached
	sh test/backupsynctest.sh ./perfcached ./perfcli
	sh test/dirlocktest.sh ./perfcached
	sh test/loadfactortest.sh ./perfcached
	sh test/restarttest.sh ./perfcached
# the WAL asks for a snapshot early enough - the rate trigger keeps an
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
	sh test/ddlstalltest.sh ./perfcached ./syncfailshim.so
	sh test/termstalltest.sh ./perfcached ./syncfailshim.so
	# the reconcile asks by hold-check; a fleet with an older member by pull
	sh test/reconcileholdtest.sh ./perfcached
	sh test/refusedpeertest.sh ./perfcached
	sh test/clmapwiretest.sh ./perfcached ./perfcli
	sh test/clientsfleettest.sh ./perfcached
	sh test/unknownfleettest.sh ./perfcached
	sh test/clustertest.sh ./perfcached
# the only suite that runs TWO BUILDS against each other: it compiles three one-line variants of
# the tree (~4 s each), the only way here to tell a wire incompatibility from a config one.
	sh test/epochtest.sh ./perfcached
# a clean stop snapshots, so a node restarted ALONE after the fleet was
# stopped serves the whole keyspace; and the privileged fleetstop verb.
	sh test/stoptest.sh ./perfcached ./perfcli
# a term followed but not persisted is never claimed again; a refused directory sync fails a store
	sh test/termsavetest.sh ./perfcached ./syncfailshim.so
	sh test/lifeguardtest.sh ./perfcached
	sh test/idsrctest.sh ./perfcached
	sh test/boottest.sh ./perfcached
	sh test/failovertest.sh ./perfcached ./failovertest
	sh test/wedgetest.sh ./perfcached ./wedgetest
	sh test/fleetresettest.sh ./perfcached
# three nodes, [mail] on one - the others relay to it, a fleet event is mailed once; builds a
# PC_MAIL=libcurl copy of the tree when the binary under test has no SMTP client.
	sh test/alertfleettest.sh ./perfcached
	sh test/iostattest.sh ./perfcached
# [daemon] io - the startup check and the fallback to epoll
	sh test/iouringtest.sh ./perfcached
# the workers' busy share; an epoll node out of worker time is told its lever
	sh test/iobusytest.sh ./perfcached ./pcbench
	sh test/hostalerttest.sh ./perfcached

# the roll under load - a tag job of its own
check-roll: check-edition-guard $(CHECK_BINS)
	sh test/upgradeloadtest.sh ./perfcached ./perfcli ./rollload
	# the shard variant: one owner per key, WAL on each node, the owner's share away while it restarts
	MODE=shard sh test/upgradeloadtest.sh ./perfcached ./perfcli ./rollload

# the PHP extension in lang/php, over the tree's libperfd.so; phpize's output stays in lang/php
# (.gitignore).  CI: the php job, on tags and on CHECK_GROUPS=php
PHPVER ?= 8.3
PHPIZE ?= phpize$(PHPVER)
PHP_CONFIG ?= php-config$(PHPVER)
PHP_BIN ?= php$(PHPVER)
phpext: libperfd.so lang/php/perfcached.c lang/php/php_perfcached.h lang/php/config.m4
	cd lang/php && $(PHPIZE) >/dev/null && \
		./configure --with-php-config=$(PHP_CONFIG) --with-perfcached=$(CURDIR) >/dev/null && \
		$(MAKE) >/dev/null
# symfony/cache and symfony/lock for phpsymfonytest: fetched here, outside tools/netns-run.sh; the
# target is the last package added, so a tree vendored before it fetches again
phpvendor: .phpvendor/vendor/symfony/lock/LockFactory.php
.phpvendor/vendor/symfony/lock/LockFactory.php:
	mkdir -p .phpvendor
	[ -f .phpvendor/composer.phar ] || curl -fsSL --retry 3 -o .phpvendor/composer.phar https://getcomposer.org/download/latest-2.x/composer.phar
	cd .phpvendor && COMPOSER_HOME=$(CURDIR)/.phpvendor/home COMPOSER_ALLOW_SUPERUSER=1 $(PHP_BIN) composer.phar require -q \
		--no-interaction --no-progress --ignore-platform-reqs symfony/cache:5.4.* symfony/lock:5.4.* psr/log:^1.1
check-php: phpext phpvendor perfcached
	sh test/phpexttest.sh ./perfcached $(PHP_BIN) lang/php/modules/perfcached.so
	sh test/phpfpmtest.sh ./perfcached php-fpm$(PHPVER) lang/php/modules/perfcached.so
	sh test/phpsymfonytest.sh ./perfcached $(PHP_BIN) lang/php/modules/perfcached.so .phpvendor/vendor/autoload.php
	sh test/phpfleettest.sh ./perfcached $(PHP_BIN) lang/php/modules/perfcached.so .phpvendor/vendor/autoload.php

# the formation ladder's 64 started at once - CI's form64 job
check-form64: check-edition-guard perfcached
	FORM_SIZES=64 FORM_KILL=0 sh test/formladdertest.sh ./perfcached

check-g-cluster2: check-edition-guard $(CHECK_BINS) perfcached.release
	# a floor's rebuild ask is no absence - never a resync
	sh test/rebuildfloortest.sh ./perfcached ./clinject
	sh test/handovertest.sh ./perfcached ./perfcli ./clinject
	# a master that yielded is silent until admitted: no watchdog re-send of its old claim
	sh test/yieldquiettest.sh ./perfcached ./perfcli ./clinject
	# one id, two live holders after a merge: the higher identity re-joins for another
	sh test/idduptest.sh ./perfcached ./perfcli ./clinject
	# two writers, one version: an eager tie goes to the greater bytes, and epoch 3's digest repairs it
	sh test/eagertietest.sh ./perfcached ./perfcli ./netcutshim.so
	# a hole in the deletes of an origin's run that is over is floored by the member
	sh test/tombdeadtest.sh ./perfcached ./netcutshim.so
	sh test/gapfloortest.sh ./perfcached ./netcutshim.so
	# the roll driver on a three-member loopback fleet, the switch hook in place of root
	sh test/upgraderolltest.sh ./perfcached ./perfcli
	# the committed wire epoch and its commit
	sh test/epochcommittest.sh ./perfcached ./perfcli
	# the RESP DB-index map changed on the running fleet
	sh test/respmapfleettest.sh ./perfcached ./perfcli
	# a collection's autoscale mode is the fleet's
	sh test/autoscalefleettest.sh ./perfcached
	sh test/masterlosstest.sh ./perfcached
	sh test/standbypassovertest.sh ./perfcached ./netcutshim.so
	sh test/colsfleettest.sh ./perfcached
	sh test/fleetcolstest.sh ./perfcached
	sh test/gonecardtest.sh ./perfcached ./perfcli

# cluster2's second half, its own group since 2026-11-25: one job was the longest of every pipeline
check-g-cluster3: check-edition-guard $(CHECK_BINS) perfcached.release
	sh test/clustercfgtest.sh ./perfcached
	sh test/mcastttltest.sh ./perfcached
	sh test/unicasttest.sh ./perfcached
	sh test/dnsseedtest.sh ./perfcached
	sh test/lanetest.sh ./perfcached
	sh test/foundtest.sh ./perfcached
	sh test/admitfoundtest.sh ./perfcached ./netcutshim.so
	sh test/admitconnecttest.sh ./perfcached ./netcutshim.so ./perfcli
	# the formation ladder, 3 to 16 started at once, and the master killed mid-queue at 24
	sh test/formladdertest.sh ./perfcached
	sh test/stagerelaytest.sh ./perfcached
	sh test/foreigntest.sh ./perfcached
	sh test/udpblocktest.sh ./perfcached
	# the lane's asks only from a known address (clinject plays the stranger)
	sh test/lanegatetest.sh ./perfcached ./clinject
	# the data plane only from a known address; a joiner's pull at once
	sh test/datagatetest.sh ./perfcached ./clinject
	# one sealed forwarded INCR sent twice is applied once
	sh test/replaytest.sh ./perfcached ./clinject
	# an id claimed from two addresses - counted and said, not refused
	sh test/idcollidetest.sh ./perfcached ./clinject
	# the multicast socket takes the group's datagrams only: another local
	# address's unicast is dropped, not served (clinject plays it)
	sh test/mcaststraytest.sh ./perfcached ./clinject
	# an address heard only from a JOIN_REQ or a hint lapses after its trial
	sh test/meshtrialtest.sh ./perfcached ./clinject
	# a sweep's target leaves inside its ack pump and a new address takes the slot: nothing goes to it
	sh test/pumpslottest.sh ./perfcached ./clinject
	# the election defers to a higher address only so long
	sh test/electboundtest.sh ./perfcached
	# a member elected while its bootstrap pull is out stays RECOVERING until the pull settles
	sh test/promotepulltest.sh ./perfcached
	sh test/lanebyetest.sh ./perfcached ./lingershim.so
	sh test/maxmemberstest.sh ./perfcached
	sh test/clusteridtest.sh ./perfcached ./netcutshim.so
	sh test/clustermaptest.sh ./perfcached
	sh test/stalltest.sh ./perfcached ./threadfreeze ./perfcli
	sh test/stallmastertest.sh ./perfcached ./threadfreeze ./perfcli
	sh test/bulkgatetest.sh ./perfcached ./perfcli
	sh test/eagerdoubttest.sh ./perfcached ./perfcli

check-g-repl1: check-edition-guard $(CHECK_BINS)
	sh test/eagerparttest.sh ./perfcached ./netcutshim.so
	sh test/ceilingreplicatest.sh ./perfcached
	sh test/reconciledigtest.sh ./perfcached ./clinject
	sh test/shardswitchtest.sh ./perfcached ./perfcli
	sh test/replicastest.sh ./perfcached
	sh test/spreadroutetest.sh ./perfcached ./failovertest
# kills a three-node eager fleet and restarts it four ways, then twice on a build without the
# witness rule, which must lose keys; ~14 min, timed because a witness is defined by its uptime.
	sh test/outagetest.sh ./perfcached
	sh test/slotplacetest.sh ./perfcached ./natbench ./perfcli
	sh test/eagermisstest.sh ./perfcached
	sh test/backfilltest.sh ./perfcached

check-g-repl2: check-edition-guard $(CHECK_BINS)
	# a read-through collection in shard mode fetches at the owner
	sh test/upthroughshardtest.sh ./perfcached
	# in spread the K holders fetch and hold
	sh test/upthroughspreadtest.sh ./perfcached
	# a read-only read-through collection fetches there too
	sh test/upreadplacetest.sh ./perfcached
	# a hash read through a non-owner or non-holder reads the key's node
	sh test/uphashplacetest.sh ./perfcached
	# EXISTS, TYPE, TTL and PTTL through a non-owner or non-holder: the key's node's answer
	sh test/existsplacetest.sh ./perfcached
	sh test/restartsharetest.sh ./perfcached
	sh test/spreadparttest.sh ./perfcached ./netcutshim.so
	sh test/spreadtest.sh ./perfcached
	sh test/spreadcoldtest.sh ./perfcached
	sh test/bigreplicatest.sh ./perfcached
	sh test/shardtest.sh ./perfcached
	# a shard join moves the joiner's whole share: big values past one batch, a walk past one tick
	sh test/reshardtailtest.sh ./perfcached
	# a read whose pull or forward to the owner times out is refused as retryable, never a miss
	sh test/readbusytest.sh ./perfcached ./getbusy
	sh test/delprobetest.sh ./perfcached
	# a delete whose forward to the owner cannot start is refused, as a set is
	sh test/delfwdfailtest.sh ./perfcached ./netcutshim.so
	# a proxy write whose fleet probe cannot park is refused, never placed unprobed
	sh test/probefulltest.sh ./perfcached
	sh test/movedhinttest.sh ./perfcached ./movedhinttest
	sh test/workerbaltest.sh ./perfcached ./workerbaltest
	sh test/respreadtest.sh ./perfcached ./respreadtest ./syncfailshim.so ./threadfreeze
	sh test/privmovetest.sh ./perfcached ./privmovetest ./syncfailshim.so ./perfcli
	sh test/frozentest.sh ./perfcached ./frozentest
	sh test/quietlinktest.sh ./perfcached ./quietlinktest
	sh test/asyncstatetest.sh ./perfcached ./asyncstatetest
	sh test/respmovedtest.sh ./perfcached
	sh test/sweepclocktest.sh ./perfcached
	# an mget across owners, every mode, a frozen peer, the guards
	sh test/mgetfantest.sh ./perfcached ./mgetextest

# fault3 and fault4 (check-fault above) sit after CHECK_BINS: a rule's prerequisites expand
# when it is read, and above the variable's definition the list was empty.
check-g-fault3: check-edition-guard $(CHECK_BINS)
	sh test/shardhandovertest.sh ./perfcached
	sh test/rejointest.sh ./perfcached
	sh test/restartwitnesstest.sh ./perfcached
	sh test/rejoinwitnesstest.sh ./perfcached

check-g-fault4: check-edition-guard $(CHECK_BINS)
	sh test/reconcileresizetest.sh ./perfcached
	sh test/tombstonetest.sh ./perfcached
	sh test/reconcilefasttest.sh ./perfcached

check-g-repl3: check-edition-guard $(CHECK_BINS)
	sh test/ceilingtest.sh ./perfcached
	sh test/expirerejointest.sh ./perfcached
	sh test/resizeexpirytest.sh ./perfcached
	# a peer back from a long silence is caught up, and the replay ends
	sh test/tombpacetest.sh ./perfcached
	# a silence while a catch-up is owed joins it: one replay on the return, not two
	sh test/tombowedtest.sh ./perfcached
	sh test/sweepgatetest.sh ./perfcached ./netcutshim.so
	# a sweep whose walk could not run is no clean cycle: the mark stays
	sh test/walkfailtest.sh ./perfcached ./netcutshim.so
	sh test/proxytest.sh ./perfcached
	sh test/eagertest.sh ./perfcached
	sh test/eagerrestarttest.sh ./perfcached
	# T600: a peer ahead for good - the pulls' empty answers settle the returning node
	sh test/eagercatchuptest.sh ./perfcached
	sh test/recovereddifftest.sh ./perfcached
	sh test/stalebackfilltest.sh ./perfcached
	sh test/fwdtest.sh ./perfcached
	sh test/routedpairtest.sh ./perfcached ./routedpairtest
	# the async pool: owner routing, wait/cancel and a late reply, a node restart, eviction, fork
	sh test/apoollivetest.sh ./perfcached ./apoollivetest
	sh test/hinttest.sh ./perfcached ./hinttest
	sh test/proxylocatortest.sh ./perfcached
	sh test/respparkmaptest.sh ./perfcached
	sh test/parkcaptest.sh ./perfcached
	sh test/applythreadstest.sh ./perfcached
	sh test/pullordertest.sh ./perfcached

# the CLI (redis-cli analogue).  ALL communication rides libperfd, the one client transport;
# the CLI adds only words, display, and the line editor.
perfcli: cli/perfcli.c cli/perfcli-upgrade.inc cli/lineedit.c libperfd.a
	$(CC) $(CFLAGS) -o $@ cli/perfcli.c cli/lineedit.c libperfd.a \
		$(LDLIBS)

# the roll driver's parsing and spawning, perfcli.c compiled in (its main renamed)
cliupgtest: test/cliupgtest.c cli/perfcli.c cli/perfcli-upgrade.inc cli/lineedit.c libperfd.a
	$(CC) $(CFLAGS) -o $@ test/cliupgtest.c cli/lineedit.c libperfd.a $(LDLIBS)

# perfdump: the parallel dumper, on libperfd and the daemon's own
# JSON tokenizer (inside the archive); compression through the zstd binary
perfdump: cli/perfdump.c cli/spawnio.c cli/spawnio.h libperfd.a
	$(CC) $(CFLAGS) -o $@ cli/perfdump.c cli/spawnio.c libperfd.a $(LDLIBS)

# perfdump's readiness guard, perfdump.c compiled in (its main renamed)
dumpreadytest: test/dumpreadytest.c cli/perfdump.c cli/spawnio.c cli/spawnio.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/dumpreadytest.c cli/spawnio.c libperfd.a $(LDLIBS)

# perfdump's chunk bookkeeping, perfdump.c compiled in (its main renamed)
dumpchunktest: test/dumpchunktest.c cli/perfdump.c cli/spawnio.c cli/spawnio.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/dumpchunktest.c cli/spawnio.c libperfd.a $(LDLIBS)

# perfload: the parallel loader, the reverse of perfdump - the
# `restore` verb carries each record's own version and absolute expiry
perfload: cli/perfload.c cli/spawnio.c cli/spawnio.h libperfd.a
	$(CC) $(CFLAGS) -o $@ cli/perfload.c cli/spawnio.c libperfd.a $(LDLIBS)

# perfload's verdict on a restore reply, perfload.c compiled in (its main renamed)
loadrefusetest: test/loadrefusetest.c cli/perfload.c cli/spawnio.c cli/spawnio.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/loadrefusetest.c cli/spawnio.c libperfd.a $(LDLIBS)

# perfcached-dashboard: the page's bytes are the daemon's own (src/statuspage.o), its data comes
# over libperfd, its HTTP layer is libmicrohttpd.  Built only when PC_DASH says so (above).
perfcached-dashboard: cli/perfcached-dashboard.c src/statuspage.o libperfd.a lib/perfd.h src/statuspage.h
	$(CC) $(CFLAGS) $(DASH_CFLAGS) -o $@ cli/perfcached-dashboard.c src/statuspage.o \
		libperfd.a $(DASH_LIBS) $(LDLIBS) -lm

# the dashboard's fleet lock, perfcached-dashboard.c compiled in (its main renamed)
dashlocktest: test/dashlocktest.c cli/perfcached-dashboard.c src/statuspage.o libperfd.a lib/perfd.h src/statuspage.h
	$(CC) $(CFLAGS) $(DASH_CFLAGS) -o $@ test/dashlocktest.c src/statuspage.o \
		libperfd.a $(DASH_LIBS) $(LDLIBS) -lm

# libperfd: the archive and the .so share PIC objects (the archive's users are often shared objects);
# lib/shim leads the include path, so no GPL daemon code rides in; the .so exports perfd_* only.
PERFD_VER   := $(shell sed -n 's/^\#define PERFD_VERSION "\(.*\)"/\1/p' lib/perfd.h)
PERFD_SOVER := 0
PERFD_SRCS  := lib/perfd.c src/json.c src/ptree.c src/pc_noise.c lib/perfd-log.c src/pcaddr.c
PERFD_HDRS  := $(wildcard lib/perfd-*.inc) lib/perfd.h lib/shim/compat/dprint.h \
		src/json.h src/ptree.h src/pc_noise.h src/pc_slot.h src/pc_mix.h src/pcaddr.h
PERFD_PICS  := $(patsubst %.c,lib/obj-pic/%.o,$(notdir $(PERFD_SRCS)))
PERFD_CPPFLAGS := -Ilib/shim -Isrc -Ilib
vpath %.c lib src
lib/obj-pic/%.o: %.c $(PERFD_HDRS)
	@mkdir -p lib/obj-pic
	$(CC) $(CFLAGS) -fPIC $(PERFD_CPPFLAGS) -c -o $@ $<

# the flavor this make would build, and its flags (test/buildflavortest.sh)
print-build:
	@echo '$(PC_BUILD_EFF)|$(CFLAGS)|$(SAN)'

# what the library is compiled from - test/mitboundtest.sh holds every
# file the compiler reads for it to the MIT licence
print-libperfd:
	@echo '$(PERFD_SRCS)|$(PERFD_CPPFLAGS)'

libperfd.a: $(PERFD_PICS)
	rm -f $@
	ar rcs $@ $^

libperfd.so.$(PERFD_VER): $(PERFD_PICS) lib/perfd.map
	$(CC) $(CFLAGS) -shared -Wl,-soname,libperfd.so.$(PERFD_SOVER) \
		-Wl,--version-script=lib/perfd.map -Wl,--no-undefined \
		-o $@ $(PERFD_PICS) -lsodium -lpthread
libperfd.so: libperfd.so.$(PERFD_VER)
	ln -sf $< libperfd.so.$(PERFD_SOVER)
	ln -sf libperfd.so.$(PERFD_SOVER) $@

# proxy holder hints, through libperfd with route_hints on
hinttest: test/hinttest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/hinttest.c libperfd.a $(LDLIBS)
# readbusytest's binary GET: hit, miss or the error's kind per key
getbusy: test/getbusy.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/getbusy.c libperfd.a $(LDLIBS)

libtest: test/libtest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/libtest.c libperfd.a $(LDLIBS)

# libperfd's own server_kind(), compiled in from lib/perfd.c
perfdkindtest: test/perfdkindtest.c lib/perfd.c $(PERFD_HDRS)
	$(CC) $(CFLAGS) $(PERFD_CPPFLAGS) -o $@ test/perfdkindtest.c $(filter-out lib/perfd.c,$(PERFD_SRCS)) $(LDLIBS)

# a link's own state, perfd.c compiled in: parked replies, the id FIFO, the frame bound
perfdlinktest: test/perfdlinktest.c lib/perfd.c $(PERFD_HDRS)
	$(CC) $(CFLAGS) $(PERFD_CPPFLAGS) -o $@ test/perfdlinktest.c $(filter-out lib/perfd.c,$(PERFD_SRCS)) $(LDLIBS)

# perfd:// URLs and the seed dial's backoff, perfd.c compiled in
perfdurltest: test/perfdurltest.c lib/perfd.c $(PERFD_HDRS)
	$(CC) $(CFLAGS) $(PERFD_CPPFLAGS) -o $@ test/perfdurltest.c $(filter-out lib/perfd.c,$(PERFD_SRCS)) $(LDLIBS)

# perfd_exec's command table, perfd.c compiled in
perfdexectest: test/perfdexectest.c lib/perfd.c $(PERFD_HDRS)
	$(CC) $(CFLAGS) $(PERFD_CPPFLAGS) -o $@ test/perfdexectest.c $(filter-out lib/perfd.c,$(PERFD_SRCS)) $(LDLIBS)

# perfd_exec beside the RESP door of one daemon (test/exectest.sh)
execcmp: test/execcmp.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/execcmp.c libperfd.a $(LDLIBS)

# load through libperfd while a fleet is rolled (test/upgradeloadtest.sh)
rollload: test/rollload.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/rollload.c libperfd.a $(LDLIBS)

# a caller built against the FROZEN header of libperfd.so.0, linked
# to the library the tree builds now (test/abitest.sh)
abicaller: test/abi/abicaller.c test/abi/perfd-abi0.h libperfd.so
	$(CC) $(CFLAGS) -Itest/abi -o $@ test/abi/abicaller.c -L. -lperfd -Wl,-rpath,'$$ORIGIN' $(LDLIBS)

libtest-so: test/libtest.c lib/perfd.h libperfd.so
	$(CC) $(CFLAGS) -o $@ test/libtest.c -L. -lperfd -Wl,-rpath,'$$ORIGIN' $(LDLIBS)

# the cluster-aware client - failover onto a pre-warmed standby
failovertest: test/failovertest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/failovertest.c libperfd.a $(LDLIBS)

routedpairtest: test/routedpairtest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/routedpairtest.c libperfd.a $(LDLIBS)

# the async pool against a stand-in server, no daemon
apooltest: test/apooltest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/apooltest.c libperfd.a $(LDLIBS)

apoollivetest: test/apoollivetest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/apoollivetest.c libperfd.a $(LDLIBS)

movedhinttest: test/movedhinttest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/movedhinttest.c libperfd.a $(LDLIBS)

# libperfd places its connections across the daemon's workers
workerbaltest: test/workerbaltest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/workerbaltest.c libperfd.a $(LDLIBS)
respreadtest: test/respreadtest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/respreadtest.c libperfd.a $(LDLIBS)

privmovetest: test/privmovetest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/privmovetest.c libperfd.a $(LDLIBS)

frozentest: test/frozentest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/frozentest.c libperfd.a $(LDLIBS)

quietlinktest: test/quietlinktest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/quietlinktest.c libperfd.a $(LDLIBS)

asyncstatetest: test/asyncstatetest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/asyncstatetest.c libperfd.a $(LDLIBS)

# libperfd's perfd_mget and perfd_mget_ex against a node of a fleet
# (test/mgetfantest.sh)
mgetextest: test/mgetextest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/mgetextest.c libperfd.a $(LDLIBS)

wedgetest: test/wedgetest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/wedgetest.c libperfd.a $(LDLIBS)

# the event-loop surface: libevent is a TEST dependency only (libperfd links libc and libsodium),
# and the suite skips loudly without it; pkg-config first, then a plain link test.
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

# where the spreading policies actually put clients
policybench: bench/policybench.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ bench/policybench.c libperfd.a $(LDLIBS)

# the forward-hop cost, routed vs not (bench rig, not in `all`)
routebench: bench/routebench.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ bench/routebench.c libperfd.a $(LDLIBS)

# what one pcache_ht_scan() call costs, in-process (bench rig, not in `all`)
scancost: bench/scancost.c $(CORE_OBJS) src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ bench/scancost.c $(CORE_OBJS) \
		src/compat/compat.o $(LDLIBS)

memprobe: test/memprobe.c src/core/pcache_mem.o src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ test/memprobe.c src/core/pcache_mem.o \
		src/compat/compat.o $(LDLIBS)

# the BINARY async path - warm-up and reply decoding, which an
# event-loop client's async fetch is built on
asyncbintest: test/asyncbintest.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -o $@ test/asyncbintest.c libperfd.a $(LDLIBS)

# the secret-wipe proof needs config.c recompiled with the
# test-only wipe counter (same shape as selftest_broken's define)
wipetest: test/wipetest.c src/config.c src/cfgcore.c src/cfgcluster.c src/cfgops.c src/cfgcheck.c src/pcaddr.c src/relkey.c src/compat/compat.c
	$(CC) $(CFLAGS) -DPC_TESTHOOKS -o $@ $^ $(LDLIBS)

# Noise core proof (RFC 5869 vector + round-trip + tamper + wrong-PSK)
noisetest: test/noisetest.c src/pc_noise.o src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

noiseboundtest: test/noiseboundtest.c src/pc_noise.o src/compat/compat.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# libperfd's allocations per call, under allocshim.so (liballoctest.sh)
liballoc: test/liballoc.c lib/perfd.h libperfd.a
	$(CC) $(CFLAGS) -Ilib -o $@ test/liballoc.c libperfd.a $(LDLIBS)

# counts the daemon's heap allocations, preloaded for test/jsonalloctest.sh.  Never sanitized, like
# syncfailshim.so: a second sanitizer runtime in one process is an error.
allocshim.so: test/allocshim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $<

syncfailshim.so: test/syncfailshim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $<

# cuts a node off from named peers for test/eagerparttest.sh
netcutshim.so: test/netcutshim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $< -lpthread

lingershim.so: test/lingershim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $<

# test/readytest.sh: holds every worker's listen() so "ready" must wait for it
slowlistenshim.so: test/slowlistenshim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $< -ldl
# test/startuptest.sh: the daemon's Nth thread is not created
threadfailshim.so: test/threadfailshim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $< -ldl

# test/cleardoortest.sh: shows perfcli a config at /etc/perfcached.conf
# without writing to the host's /etc
fopenmapshim.so: test/fopenmapshim.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -o $@ $< -ldl

# the daemon's parent for test/stalltest.sh - it stops ONE named
# thread of its child with ptrace.  Never sanitized: it is a launcher.
threadfreeze: test/threadfreeze.c
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -o $@ $<

# a cluster frame sealed as a node seals it, from an address
# the suite picks - the gates' suites play a sender the fleet does not know
clinject: test/clinject.c src/clwire.o src/pc_noise.o src/clmemb.o src/clpull.o src/clfwd.o src/clinit.o src/pcaddr.o src/compat/compat.o
	$(CC) $(CFLAGS) -Isrc -o $@ test/clinject.c src/clwire.o src/pc_noise.o src/clmemb.o src/clpull.o src/clfwd.o src/clinit.o src/pcaddr.o src/compat/compat.o $(LDLIBS)

clean:
	rm -rf lib/obj lib/obj-pic
	-[ ! -f lang/php/configure ] || (cd lang/php && $(PHPIZE) --clean >/dev/null)
	rm -f perfcached.release perfcached.release.tmp
	rm -f libperfd.so libperfd.so.* libtest-so abicaller src/*.o src/compat/*.o src/core/*.o lib/*.o perfcached \
		perfcli perfdump perfload libperfd.a libtest execcmp failovertest routedpairtest apooltest apoollivetest movedhinttest hinttest workerbaltest respreadtest privmovetest frozentest quietlinktest asyncstatetest clordertest draincarrytest mgetextest htfuzztest cellpoisontest capracetest verstamptest arenadbgtest noiseboundtest syncfailshim.so slowlistenshim.so fopenmapshim.so allocshim.so liballoc netcutshim.so lingershim.so threadfreeze clinject getbusy clapplytest evactest arenacaptest addexptest respmaptest wedgetest selftest selftest_broken threadfailshim.so \
		memprobe noisetest pcbench routebench mmclient policybench concbench \
		asynctest clmaptest cltermtest clsynctest clhisttest clplacetest clseltest clpeerstest clpendtest clloctest clretaintest clacktest clackftest pcaddrtest uprespctest respouttest respcltest resppubsubtest respkvtest mgetaggtest respjsontest respscripttest respsrvtest respscantest pcglobtest upstatstest upconntest clrecontest httpdoortest respdoortest enumwalktest noisechantest clepochtest clfoundtest cladmittest clddltest clconntest perfdkindtest perfdlinktest perfdurltest perfdexectest cliupgtest dumpreadytest dumpchunktest dashlocktest loadrefusetest cfgkvtest cfgchecktest doorlivetest listenfdtest cfgsigtest autosizetest hostmontest hostlinktest crc32ctest walscantest upgverifytest clanetest logrltest clmeshtest statsresettest clboottest clpushtest clwiretest clreplaytest clrpttest clbulktest clrestest clmembtest clunktest clsendtest clfleettest clworktest clpstest clspreadtest clpulltest clholdtest clcoltest clplattest

# install: binary + annotated example config + systemd unit.
# The live config is NEVER written - only the .example is refreshed.
PREFIX ?= /usr/local
# what a release says about itself (version, revision, edition, wire epochs) for `upgrade plan`;
# the preprocessor alone reads the constants, so a cross build writes what a native one does.
perfcached.release: src/version.h src/cluster.h src/clmemb.h
	printf '#include "version.h"\n#include "cluster.h"\nPCREL_version=PC_VERSION\nPCREL_wire_epoch_min=PC_CL_EPOCH_MIN\nPCREL_wire_epoch_max=PC_CL_EPOCH_MAX\n' \
		| $(CC) $(CFLAGS) -E -P -I src -x c - | sed -n 's/^PCREL_//p' | tr -d '" ' > $@.tmp
	printf 'revision=%s\nedition=%s\n' '$(PC_BUILD_REV)' '$(PC_EDITION)' >> $@.tmp
	[ $$(grep -cE '^(version=[0-9][^=]*|wire_epoch_min=[0-9]+|wire_epoch_max=[0-9]+)$$' $@.tmp) = 3 ] \
		|| { echo "perfcached.release: could not read the version and epochs:"; cat $@.tmp; rm -f $@.tmp; exit 1; }
	mv $@.tmp $@

install-lib: libperfd.a libperfd.so
	install -D -m 644 lib/perfd.h $(DESTDIR)$(PREFIX)/include/perfd.h
	install -D -m 644 libperfd.a $(DESTDIR)$(PREFIX)/lib/libperfd.a
	install -D -m 755 libperfd.so.$(PERFD_VER) $(DESTDIR)$(PREFIX)/lib/libperfd.so.$(PERFD_VER)
	ln -sf libperfd.so.$(PERFD_VER) $(DESTDIR)$(PREFIX)/lib/libperfd.so.$(PERFD_SOVER)
	ln -sf libperfd.so.$(PERFD_SOVER) $(DESTDIR)$(PREFIX)/lib/libperfd.so
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libperfd\nDescription: the perfcached client library\nVersion: %s\nRequires.private: libsodium\nLibs: -L$${libdir} -lperfd\nLibs.private: -lpthread\nCflags: -I$${includedir}\n' \
		'$(PREFIX)' '$(PERFD_VER)' > $(DESTDIR)$(PREFIX)/lib/pkgconfig/libperfd.pc

install: perfcached perfcli perfdump perfload install-lib perfcached.release $(DASH_BIN)
	install -D -m 755 perfcached $(DESTDIR)$(PREFIX)/bin/perfcached
	install -D -m 644 perfcached.release $(DESTDIR)$(PREFIX)/share/perfcached/RELEASE
	install -D -m 755 perfcli $(DESTDIR)$(PREFIX)/bin/perfcli
	install -D -m 755 perfdump $(DESTDIR)$(PREFIX)/bin/perfdump
	install -D -m 755 perfload $(DESTDIR)$(PREFIX)/bin/perfload
ifeq ($(PC_DASH),yes)
	# the dashboard's unit is installed DISABLED - one enabled node
	# shows the whole fleet; its config carries the client secret
	install -D -m 755 perfcached-dashboard $(DESTDIR)$(PREFIX)/bin/perfcached-dashboard
	install -D -m 644 contrib/perfcached-dashboard.conf.example \
		$(DESTDIR)/etc/perfcached/perfcached-dashboard.conf.example
	install -D -m 644 contrib/perfcached-dashboard.service \
		$(DESTDIR)/etc/systemd/system/perfcached-dashboard.service
	install -D -m 644 contrib/perfcached-dashboard.sysusers \
		$(DESTDIR)/usr/lib/sysusers.d/perfcached-dashboard.conf
endif
	install -D -m 644 contrib/perfcached.conf.example \
		$(DESTDIR)/etc/perfcached/perfcached.conf.example
	install -D -m 644 contrib/perfcached.service \
		$(DESTDIR)/etc/systemd/system/perfcached.service
	install -D -m 755 contrib/perfcached-upgrade $(DESTDIR)$(PREFIX)/sbin/perfcached-upgrade
	install -D -m 644 contrib/perfcached-upgrade.path \
		$(DESTDIR)/etc/systemd/system/perfcached-upgrade.path
	sed 's|@SBINDIR@|$(PREFIX)/sbin|g' contrib/perfcached-upgrade.service \
		> $(DESTDIR)/etc/systemd/system/perfcached-upgrade.service
	chmod 644 $(DESTDIR)/etc/systemd/system/perfcached-upgrade.service
	install -D -m 644 contrib/upgrade.conf.example \
		$(DESTDIR)/etc/perfcached/upgrade.conf.example
	install -D -m 644 contrib/perfcached.sysusers \
		$(DESTDIR)/usr/lib/sysusers.d/perfcached.conf
	@echo "next: systemd-sysusers   # creates the perfcached account"
	@echo "      cp $(DESTDIR)/etc/perfcached/perfcached.conf.example" \
		"/etc/perfcached/perfcached.conf && edit the secrets"
	@echo "      see PRODUCTION.md before the first real deployment"

.PHONY: all menuconfig print-libperfd print-build check-standalone shim core check perfcached.release check-wired check-fast check-fault check-asan check-asan-findings hashbench clean install check-roll \
	check-edition-guard check-g-core1 check-g-core2 check-g-durability check-g-cluster1 check-g-cluster2 check-g-cluster3 \
	check-g-repl1 check-g-repl2 check-g-repl3 check-g-fault1 check-g-fault2 check-g-fault3 check-g-fault4
