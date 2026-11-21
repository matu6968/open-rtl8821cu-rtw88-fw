#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work="$root/build/toolchain-bootstrap"
mkdir -p "$work" "$1"
prefix=$(CDPATH= cd -- "$1" && pwd)
target=mipsel-elf
jobs=${TOOLCHAIN_JOBS:-4}

export CC="${HOST_CC:-gcc}" CXX="${HOST_CXX:-g++}"
export CFLAGS="${HOST_CFLAGS:--O2}" CXXFLAGS="${HOST_CXXFLAGS:--O2}"

get_source() {
    name=$1
    url=$2
    revision=$3
    local_source=$4
    if [ -z "$local_source" ]; then
        local_source="$work/$name-source"
        if [ ! -d "$local_source/.git" ]; then
            git init -q "$local_source"
        fi
        if ! git -C "$local_source" cat-file -e "$revision^{commit}" 2>/dev/null; then
            git -C "$local_source" fetch --depth=1 "$url" "$revision"
        fi
        git -C "$local_source" checkout --quiet --detach "$revision"
    fi
    if [ "$(git -C "$local_source" rev-parse HEAD)" != "$revision" ] ||
       ! git -C "$local_source" diff --quiet HEAD --; then
        echo "$name needs an unmodified checkout of $revision" >&2
        exit 1
    fi
    (CDPATH= cd -- "$local_source" && pwd)
}

if [ ! -x "$prefix/bin/$target-as" ] || [ ! -x "$prefix/bin/$target-ld" ] ||
   [ ! -x "$prefix/bin/$target-objdump" ]; then
    binutils=$(get_source binutils https://github.com/gnutools/binutils-gdb.git \
        6ce87bbc521cf46eaee9a1f7ef61cee2cdfb3e32 "${BINUTILS_SRC:-}")
    mkdir -p "$work/binutils"
    (
        cd "$work/binutils"
        "$binutils/configure" --prefix="$prefix" --target="$target" --disable-nls \
            --disable-werror --disable-gdb --disable-sim --disable-libdecnumber \
            --disable-readline --disable-gprof --disable-gold --without-isl
        make -j"$jobs"
        make install
    )
fi

if [ ! -x "$prefix/bin/$target-gcc" ]; then
    gcc=$(get_source gcc https://github.com/gnutools/gcc.git \
        71b50793e1e2b12b36baff23c1211e43b8c86ecd "${GCC_SRC:-}")
    mkdir -p "$work/gcc"
    (
        cd "$work/gcc"
        "$gcc/configure" --prefix="$prefix" --target="$target" --enable-languages=c \
            --disable-nls --disable-shared --disable-threads --disable-libssp \
            --disable-libgomp --disable-libquadmath --disable-libatomic --disable-libstdcxx \
            --disable-bootstrap --disable-multilib --disable-werror --without-headers \
            --without-isl --with-newlib --with-gnu-as --with-gnu-ld \
            --with-endian=little --with-arch=mips32r2 --with-abi=32
        make -j"$jobs" all-gcc
        make install-gcc
    )
fi

"$prefix/bin/$target-gcc" --version
"$prefix/bin/$target-objdump" --version
