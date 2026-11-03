# perfcached for Alpine

`APKBUILD` builds six packages from one release tarball (PD-11, 0.7.0;
until 0.6.x it packaged the client library alone):

- `perfcached` - the clustered daemon, `perfcli`, `perfdump`, `perfload`,
  the OpenRC service `/etc/init.d/perfcached` (not enabled) and
  `/etc/perfcached/perfcached.conf.example`;
- `perfcached-standalone` - the standalone edition under the same names
  (one daemon per host: `apk del perfcached` first, then add it);
- `perfcached-dashboard` - the status page as its own service
  (`/etc/init.d/perfcached-dashboard`, its own account, not enabled);
- `libperfd` - `libperfd.so.0`, and `libperfd-dev` - `perfd.h`,
  `libperfd.a`, the `.so` link and `libperfd.pc`;
- `perfcached-doc` - GUIDE.md, PRODUCTION.md, the ChangeLog.

The daemon packages write `/usr/share/perfcached/INSTALL-MODE` = `package`:
the daemon's `upgrade_stage` then refuses, because `apk` upgrades it.  The
services run as the `perfcached` and `perfcached-dashboard` accounts the
packages create; the daemon's service sets the memlock and descriptor
limits, creates `/var/lib/perfcached`, copies the DMI uuid where the
daemon can read it, and runs the daemon's own config check first.

The release packages are signed with the perfcached release key.  Trust it
once, then install and start:

```
cp yury@kfi.net.au-6aca2bb6.rsa.pub /etc/apk/keys/
apk add ./perfcached-0.7.0-r0.apk ./libperfd-0.7.0-r0.apk
cp /etc/perfcached/perfcached.conf.example /etc/perfcached/perfcached.conf
chmod 640 /etc/perfcached/perfcached.conf && chown root:perfcached /etc/perfcached/perfcached.conf
# set the secrets (client and cluster must differ), then
rc-update add perfcached && rc-service perfcached start
```

The key's public half is in `keys/` here and on every release page.  To
build the packages yourself, put the source tarball
(`perfcached-<version>.tar.gz`, top directory `perfcached-<version>/`)
beside the APKBUILD and run `abuild checksum && abuild -r` with your own
key (`abuild-keygen -a`).  `tools/alpine-pkg-check.sh` is what CI runs:
it builds the packages with abuild in an alpine container, installs them
and starts the services through OpenRC.
