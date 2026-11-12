#!/bin/sh
# Rebuilds the OpenSSH package for the Nebula pad (Ingenic X2000, MIPS32r2,
# soft-float, Linux 4.4 / glibc 2.29 system). Needs zig 0.17.x, python3,
# and GNU-compatible tar/curl on macOS or Linux.
set -e

OPENSSL_VER=3.5.9
OPENSSH_VER=10.6p2
ZLIB_VER=1.4

HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${WORK:-$HOME/nebula-build}"
BIN="$HERE/build/bin"
PREFIX="$WORK/prefix"
SRC="$WORK/src"

mkdir -p "$SRC" "$PREFIX"
export CC="$BIN/mips-cc" AR="$BIN/mips-ar" RANLIB="$BIN/mips-ranlib"

cd "$SRC"
[ -f zlib-$ZLIB_VER.tar.gz ] || curl -sLO https://zlib.net/zlib-$ZLIB_VER.tar.gz
[ -f openssl-$OPENSSL_VER.tar.gz ] || curl -sLO https://www.openssl.org/source/openssl-$OPENSSL_VER.tar.gz
[ -f openssh-$OPENSSH_VER.tar.gz ] || curl -sLO https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-$OPENSSH_VER.tar.gz
for t in zlib-$ZLIB_VER openssl-$OPENSSL_VER openssh-$OPENSSH_VER; do
	[ -d $t ] || tar xzf $t.tar.gz
done

# zlib
cd "$SRC/zlib-$ZLIB_VER"
make distclean >/dev/null 2>&1 || true
CHOST=mipsel-linux-musl ./configure --static --prefix="$PREFIX"
make -j8 libz.a
make install

# OpenSSL (no asm, no threads: the atomics builtins fail to link on this target)
cd "$SRC/openssl-$OPENSSL_VER"
make distclean >/dev/null 2>&1 || true
./Configure linux-mips32 no-shared no-tests no-asm no-threads \
	--prefix="$PREFIX" --openssldir=/etc/ssl --libdir=lib
make -j8 build_libs
make install_dev

# OpenSSH (no syscall sandbox: seccomp prctl fails on the device kernel,
# rlimit sandbox cannot be configured when cross-compiling)
cd "$SRC/openssh-$OPENSSH_VER"
make distclean >/dev/null 2>&1 || true
LDFLAGS="-static -L$PREFIX/lib" ./configure --host=mipsel-linux-musl \
	--prefix=/usr --sysconfdir=/etc/ssh \
	--with-zlib="$PREFIX" --with-ssl-dir="$PREFIX" \
	--without-pam --without-selinux --without-kerberos5 \
	--without-libedit --without-ldns --without-audit --with-sandbox=no
make -j8 LDFLAGS="-L. -Lopenbsd-compat/ -L$PREFIX/lib -static -s" \
	LDFLAGS_NOPIE="-L. -Lopenbsd-compat/ -L$PREFIX/lib -static -s" \
	ssh sshd ssh-keygen sftp-server scp sshd-session sshd-auth

# mdns-announce (plain C, no libc features beyond musl)
"$BIN/mips-cc" -static -s -O2 -Wall -o "$HERE/mdns/mdns-announce" "$HERE/mdns/mdns-announce.c"

# Stage binaries into the package tree and build the .ipk
D="$HERE/package/data"
mkdir -p "$D/usr/sbin" "$D/usr/bin" "$D/usr/libexec"
cp sshd "$D/usr/sbin/"
cp ssh ssh-keygen scp "$D/usr/bin/"
cp sftp-server sshd-session sshd-auth "$D/usr/libexec/"
cp "$HERE/mdns/mdns-announce" "$D/usr/sbin/"
echo "Binaries staged. Now run: python3 $HERE/make-ipk.py"
