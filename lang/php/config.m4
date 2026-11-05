dnl SPDX-License-Identifier: MIT
PHP_ARG_WITH([perfcached],
  [for perfcached, the client over libperfd],
  [AS_HELP_STRING([--with-perfcached@<:@=DIR@:>@],
    [perfcached: libperfd's prefix, or a perfcached source tree with libperfd.so built])],
  [yes])

if test "$PHP_PERFCACHED" != "no"; then
  PERFD_INC=
  PERFD_LIB=
  for i in "$PHP_PERFCACHED" /usr/local /usr; do
    if test -f "$i/lib/perfd.h" && test -f "$i/libperfd.so"; then
      PERFD_INC="$i/lib"; PERFD_LIB="$i"; break
    fi
    if test -f "$i/include/perfd.h"; then
      PERFD_INC="$i/include"; PERFD_LIB="$i/$PHP_LIBDIR"; break
    fi
  done
  if test -z "$PERFD_INC"; then
    AC_MSG_ERROR([perfd.h not found: install libperfd-dev, or give --with-perfcached=DIR])
  fi
  if test "$PHP_THREAD_SAFETY" = "yes"; then
    AC_MSG_ERROR([perfcached needs an NTS PHP: a libperfd handle belongs to one thread])
  fi
  PHP_ADD_INCLUDE([$PERFD_INC])
  PHP_ADD_LIBRARY_WITH_PATH([perfd], [$PERFD_LIB], [PERFCACHED_SHARED_LIBADD])
  PHP_SUBST([PERFCACHED_SHARED_LIBADD])
  PHP_NEW_EXTENSION([perfcached], [perfcached.c], [$ext_shared])
  PHP_ADD_EXTENSION_DEP([perfcached], [json])
  PHP_ADD_EXTENSION_DEP([perfcached], [spl])
fi
