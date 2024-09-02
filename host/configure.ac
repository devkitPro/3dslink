#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT([3dslink],[0.6.3],[https://github.com/devkitPro/3dslink/issues])
AC_CONFIG_SRCDIR([src/main.c])
AM_INIT_AUTOMAKE([subdir-objects])

AC_CANONICAL_BUILD
AC_CANONICAL_HOST

AC_PROG_CC

PKG_CHECK_MODULES([ZLIB], zlib, [
  LIBS="$ZLIB_LIBS $LIBS"
  CFLAGS="$ZLIB_CFLAGS $CFLAGS"
  AC_DEFINE([HAVE_LIBZ], [1], [Define if using zlib.])
], [
  AC_CHECK_LIB(z, compress)
])

case "$host" in
   *-*-mingw*)
      LIBS="$LIBS -lws2_32"
      CPPFLAGS="-D_WIN32_WINNT=0x600 $CPPFLAGS"
      ;;
esac

CFLAGS="$CFLAGS -std=gnu99"

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
