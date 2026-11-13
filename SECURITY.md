# Security policy

## Reporting a vulnerability

Please do not open a public issue for a security problem.  Report it
privately through GitHub instead: the repository's **Security** tab,
**Report a vulnerability**
(<https://github.com/Lt-Flash/perfcached/security/advisories/new>).
Only the maintainer sees the report.

Please include:

- the output of `perfcached -V` (and `perfcli -V` when a client is
  involved), the edition and how it was installed;
- what an attacker can do, from where (the native door, the RESP door,
  the HTTP door, the cluster port, a local user), and with which
  secret, if any;
- the steps or a proof of concept, and the config with every secret,
  password and token removed.

The answer, the fix, the release that carries it and a credit (if you
want one) are coordinated in the advisory's private thread.

## Supported versions

Fixes ship in a new release.  The supported release is the latest one
on the [releases page](https://github.com/Lt-Flash/perfcached/releases);
an older fleet takes a fix by upgrading within the support window
(README.md, "Compatibility").

## Known and documented

The native door authenticates every client but does not encrypt unless
`[listen] encrypt = yes` says so, and the RESP and HTTP doors are
plaintext unless their listener says `tls` (README.md, "Known limits"):
run a door in the clear on a trusted segment, behind its allow list and
secrets.  A report that such a door carries traffic in the clear is
already known; a way past a door's authentication, allow list or TLS is
not, and is welcome.
