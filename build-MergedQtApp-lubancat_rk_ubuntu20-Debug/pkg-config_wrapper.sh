#!/bin/sh
PKG_CONFIG_SYSROOT_DIR=/opt/sysroot_ubuntu20
export PKG_CONFIG_SYSROOT_DIR
PKG_CONFIG_LIBDIR=/opt/sysroot_ubuntu20/usr/lib/aarch64-linux-gnu/pkgconfig:/opt/sysroot_ubuntu20/usr/lib/pkgconfig:/opt/sysroot_ubuntu20/usr/share/pkgconfig
export PKG_CONFIG_LIBDIR
exec pkg-config "$@"
