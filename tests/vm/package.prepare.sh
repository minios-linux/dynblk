#!/bin/bash
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
here=$(dirname "$(readlink -f "$0")")
out=$(readlink -m "${1:?new artifact-local preparation directory required}")
base=$(readlink -f "${2:-$HOME/.cache/sbuild/trixie-amd64.tar.gz}")
case "$out" in /build/minios-live/build/dynblk-vm-package-*) ;; *) exit 2 ;; esac
test ! -e "$out"
test -r "$base"
mkdir -p "$out/rootfs"
sha256sum "$base" > "$out/base.sha256"
# Only the new rootless container namespace gets these API-filesystem mounts.
exec unshare --user --mount --pid --fork \
    --map-users="0:$(id -u):1" --map-users=1:100000:65535 \
    --map-groups="0:$(id -g):1" --map-groups=1:100000:65535 \
    /bin/bash "$here/package.container.sh" base "$out" "$base"
