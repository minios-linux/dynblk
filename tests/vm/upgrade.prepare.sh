#!/bin/bash
# Prepare a disposable distro root and download, never install, dynblk inputs.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
here=$(dirname "$(readlink -f "$0")")
out=$(readlink -m "${1:?fresh package-upgrade preparation directory}")
old=$(readlink -f "${2:?old clean package directory}")
new=$(readlink -f "${3:?new clean package directory}")
case "$out" in /build/minios-live/build/dynblk-vm-package-upgrade-*) ;; *) exit 2 ;; esac
test ! -e "$out"
test -d "$(dirname "$out")"
base=$HOME/.cache/sbuild/trixie-amd64.tar.gz
test -r "$base"
mkdir "$out" "$out/rootfs"
sha256sum "$base" > "$out/base.sha256"
exec unshare --user --mount --pid --fork \
    --map-users="0:$(id -u):1" --map-users=1:100000:65535 \
    --map-groups="0:$(id -g):1" --map-groups=1:100000:65535 \
    /bin/bash "$here/upgrade.namespace.sh" "$out" "$base" "$old" "$new"
