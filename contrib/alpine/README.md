# libperfd for Alpine

`APKBUILD` builds the perfcached client library as two packages:

- `libperfd` - `libperfd.so.0` (the shared library), and
- `libperfd-dev` - `perfd.h`, `libperfd.a`, the `libperfd.so` link and
  `libperfd.pc` (pkg-config).

The release packages are signed with the perfcached release key.  Trust it
once, then install:

```
cp yury@kfi.net.au-6aca2bb6.rsa.pub /etc/apk/keys/
apk add ./libperfd-0.5.0-r0.apk ./libperfd-dev-0.5.0-r0.apk
cc -o app app.c $(pkg-config --cflags --libs libperfd)
```

The key's public half is in `keys/` here and on every release page.  To
build the packages yourself, put a perfcached source tarball
(`perfcached-<version>.tar.gz`, top directory `perfcached-<version>/`)
beside the APKBUILD and run `abuild checksum && abuild -r` with your own
key.
