#!/usr/bin/env bash
# Builds the pacman package from the current checkout.
#   ci/makepkg.sh [version]
# Without a version the PKGBUILD's pkgver is used; the source tarball is
# always taken from this tree, never downloaded. makepkg refuses to run as
# root, so an unprivileged user is created when needed.
set -euo pipefail
cd "$(dirname "$0")/../packaging"

VER=${1:-$(sed -n 's/^pkgver=//p' PKGBUILD)}
sed -i "s/^pkgver=.*/pkgver=$VER/" PKGBUILD
git -C .. config --global --add safe.directory "$(cd .. && pwd)" 2>/dev/null || true
git -C .. archive --format=tar.gz --prefix="mvplayer-$VER/" -o "packaging/mvplayer-$VER.tar.gz" HEAD

if [[ $(id -u) -eq 0 ]]; then
    id builder &>/dev/null || useradd -m builder
    chown -R builder .
    su builder -c "makepkg --noconfirm --nodeps -f"
else
    makepkg --noconfirm -f
fi
ls -1 ./*.pkg.tar.zst
