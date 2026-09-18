#!/bin/bash
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
out=${1:?fresh runtime root}; base=${2:?base}; old=${3:?old packages}; new=${4:?new packages}
here=$(dirname "$(readlink -f "$0")")
case "$out" in /build/minios-live/build/dynblk-vm-package-upgrade-*) ;; *) exit 2 ;; esac
test "$(id -u)" = 0
test "$(awk '$1 == 0 {print $2}' /proc/self/uid_map)" != 0
root=$out/rootfs
test -d "$root" && test ! -e "$root/etc"
tar --numeric-owner --exclude=./dev --exclude=./proc --exclude=./sys --exclude=./run -xpf "$base" -C "$root"
mkdir -p "$root"/{proc,sys,dev,run,tmp,opt/package-test,opt/packages,opt/dependencies,opt/upgrade,opt/kernel-update,evidence}
mount --make-rprivate /
mount -t proc proc "$root/proc"
mount --rbind /dev "$root/dev"; mount --make-rslave "$root/dev"; mount -o remount,bind,ro "$root/dev"
mount --rbind /sys "$root/sys"; mount --make-rslave "$root/sys"; mount -o remount,bind,ro "$root/sys"
cp "$here/package.sources" "$root/etc/apt/sources.list"
rm -f "$root/etc/resolv.conf"
cp -L /etc/resolv.conf "$root/etc/resolv.conf"
cp "$here/package.policy-rc.d" "$root/usr/sbin/policy-rc.d"
chmod 0755 "$root/usr/sbin/policy-rc.d"
for name in dynblk dynblk-dkms; do
    inputs=("$old/${name}_"*_amd64.deb); test "${#inputs[@]}" = 1
    cp "${inputs[0]}" "$root/opt/packages/$name.deb"
    inputs=("$new/${name}_"*_amd64.deb); test "${#inputs[@]}" = 1
    cp "${inputs[0]}" "$root/opt/upgrade/$name.deb"
done
chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive LC_ALL=C /bin/bash -c '
    set -euo pipefail
    apt-get update
    apt-get install -y --no-install-recommends apt python3 kmod systemd-sysv udev \
        build-essential e2fsprogs util-linux ca-certificates initramfs-tools mokutil openssl \
        linux-image-6.12.94+deb13-amd64=6.12.94-1 \
        linux-headers-6.12.94+deb13-amd64=6.12.94-1 linux-headers-amd64=6.12.94-1
    test ! -e /usr/sbin/dynblk && test ! -e /usr/sbin/dkms
    apt-get clean
    apt-get --download-only -y --no-install-recommends install /opt/packages/dynblk.deb /opt/packages/dynblk-dkms.deb
    cp /var/cache/apt/archives/*.deb /opt/dependencies/
    apt-get clean
    apt-get --download-only -y --no-install-recommends install \
        linux-image-6.12.107+deb13-amd64=6.12.107-1 \
        linux-headers-6.12.107+deb13-amd64=6.12.107-1 linux-headers-amd64=6.12.107-1
    cp /var/cache/apt/archives/*.deb /opt/kernel-update/
    apt-get clean
    test ! -e /usr/sbin/dynblk && test ! -e /usr/sbin/dkms
    test ! -e /lib/modules/6.12.107+deb13-amd64
    dpkg-query -W -f="\${binary:Package} \${Version}\n" > /opt/package-test/base-packages.txt
    apt-get check
    dpkg-deb -f /opt/packages/dynblk.deb Version > /opt/package-test/version
    dpkg-deb -f /opt/upgrade/dynblk.deb Version > /opt/package-test/upgrade-version
    dpkg --compare-versions "$(cat /opt/package-test/upgrade-version)" gt "$(cat /opt/package-test/version)"
    sha256sum /opt/packages/*.deb /opt/upgrade/*.deb /opt/dependencies/*.deb /opt/kernel-update/*.deb > /opt/package-test/packages.sha256
'
version=$(cat "$root/opt/package-test/version")
dpkg-deb --fsys-tarfile "$root/opt/packages/dynblk-dkms.deb" | tar -xOf - "./usr/src/dynblk-$version/dynblk.c" | sha256sum | cut -d ' ' -f1 > "$root/opt/package-test/kernel-source.sha256"
cp "$here/package.py" "$root/opt/package-test/package_lib.py"
cp "$here/upgrade.py" "$root/opt/package-test/upgrade.py"
cp "$here/upgrade.launch" "$root/opt/package-test/launch"
chmod 0755 "$root/opt/package-test/launch"
sed 's/dynblk_package_test=1/dynblk_upgrade_test=1/' "$here/package.service" > "$root/etc/systemd/system/dynblk-package-test.service"
mkdir -p "$root/etc/systemd/system/multi-user.target.wants"
ln -s ../dynblk-package-test.service "$root/etc/systemd/system/multi-user.target.wants/dynblk-package-test.service"
printf '%s\n' '/dev/disk/by-label/DYNBLK-PKG / ext4 defaults 0 0' > "$root/etc/fstab"
printf '%s\n' dynblk-upgrade > "$root/etc/hostname"
rm "$root/usr/sbin/policy-rc.d"
tar --numeric-owner --one-file-system --exclude=./proc --exclude=./sys --exclude=./dev \
    --exclude=./run --exclude=./tmp --exclude=./build --exclude=./workspace -C "$root" -czpf "$out/rootfs.tar.gz" .
sha256sum "$out/rootfs.tar.gz" > "$out/rootfs.sha256"
