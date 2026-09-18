#!/bin/bash
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
action=${1:?action required}
out=${2:?artifact-local preparation root required}
here=$(dirname "$(readlink -f "$0")")
case "$out" in /build/minios-live/build/dynblk-vm-package-*) ;; *) exit 2 ;; esac
test "$(id -u)" = 0
test "$(awk '$1 == 0 {print $2}' /proc/self/uid_map)" != 0
root=$out/rootfs
if test "$action" = base; then
    tar --numeric-owner --exclude=./dev --exclude=./proc --exclude=./sys --exclude=./run \
        -xpf "${3:?base tarball required}" -C "$root"
fi
mkdir -p "$root"/{proc,sys,dev,run,tmp,opt/package-test,opt/packages,opt/dependencies,evidence}
mount --make-rprivate /
mount -t proc proc "$root/proc"
mount --rbind /dev "$root/dev"
mount --make-rslave "$root/dev"
mount -o remount,bind,ro "$root/dev"
mount --rbind /sys "$root/sys"
mount --make-rslave "$root/sys"
mount -o remount,bind,ro "$root/sys"
cp "$here/package.sources" "$root/etc/apt/sources.list"
rm -f "$root/etc/resolv.conf"
cp -L /etc/resolv.conf "$root/etc/resolv.conf"
cp "$here/package.policy-rc.d" "$root/usr/sbin/policy-rc.d"
chmod 0755 "$root/usr/sbin/policy-rc.d"
if test "$action" = base; then
    chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive LC_ALL=C /bin/bash -c '
        set -euo pipefail
        apt-get update
        apt-get install -y --no-install-recommends apt python3 kmod systemd-sysv udev \
            build-essential e2fsprogs util-linux ca-certificates \
            linux-headers-amd64=6.12.107-1 linux-headers-6.12.107+deb13-amd64=6.12.107-1
        test ! -e /usr/sbin/dynblk
        test ! -e /usr/sbin/dkms
        dpkg-query -W -f="\${binary:Package} \${Version}\n"
    ' 2>&1 | tee "$out/base-install.log"
elif test "$action" = finalize; then
    test -f "$root/opt/packages/dynblk.deb"
    test -f "$root/opt/packages/dynblk-dkms.deb"
    chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive LC_ALL=C /bin/bash -c '
        set -euo pipefail
        apt-get clean
        apt-get --download-only -y --no-install-recommends install /opt/packages/dynblk.deb /opt/packages/dynblk-dkms.deb
        cp /var/cache/apt/archives/*.deb /opt/dependencies/
        test ! -e /usr/sbin/dynblk
        test ! -e /usr/sbin/dkms
        dpkg-query -W -f="\${binary:Package} \${Version}\n" > /opt/package-test/base-packages.txt
        apt-get check
        apt-get clean
    ' 2>&1 | tee "$out/dependency-resolution.log"
    rm -f "$root/usr/sbin/policy-rc.d"
    cp "$here/package.py" "$root/opt/package-test/package.py"
    cp "$here/package.launch" "$root/opt/package-test/launch"
    chmod 0755 "$root/opt/package-test/launch"
    cp "$here/package.service" "$root/etc/systemd/system/dynblk-package-test.service"
    mkdir -p "$root/etc/systemd/system/multi-user.target.wants"
    ln -sf ../dynblk-package-test.service "$root/etc/systemd/system/multi-user.target.wants/dynblk-package-test.service"
    printf '%s\n' '/dev/disk/by-label/DYNBLK-PKG / ext4 defaults 0 0' > "$root/etc/fstab"
    # Export actual distro state with namespace-correct numeric ownership, not a mocked dpkg tree.
    tar --numeric-owner --one-file-system --exclude=./proc --exclude=./sys --exclude=./dev \
        --exclude=./run --exclude=./tmp --exclude=./build --exclude=./workspace \
        -C "$root" -czpf "$out/rootfs.tar.gz" .
else
    exit 2
fi
