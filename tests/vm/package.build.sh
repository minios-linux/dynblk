#!/bin/bash
set -euo pipefail
export LC_ALL=C
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
here=$(dirname "$(readlink -f "$0")")
repo=$(readlink -f "$here/../../../..")
prep=$(readlink -f "${1:?prepared guest directory required}")
out=$(readlink -m "${2:?new output directory required}")
userspace=$(readlink -f "${3:?final dynblk deb required}")
dkms=$(readlink -f "${4:?final dynblk-dkms deb required}")
kernel=6.12.107+deb13-amd64
case "$prep" in "$repo"/build/dynblk-vm-package-*) ;; *) exit 2 ;; esac
case "$out" in "$repo"/build/dynblk-vm-package-*) ;; *) exit 2 ;; esac
test ! -e "$out"
test -f "$prep/base-install.log"
test "$(dpkg-deb -f "$userspace" Package)" = dynblk
test "$(dpkg-deb -f "$dkms" Package)" = dynblk-dkms
version=$(dpkg-deb -f "$userspace" Version)
test "$version" = "$(dpkg-deb -f "$dkms" Version)"
[[ "$version" =~ ^[A-Za-z0-9.+~:-]+$ ]]
mkdir -p "$out/inputs" "$out/bootstrap" "$out/iso/boot/grub" "$out/iso/payload"
sha256sum "$userspace" "$dkms" > "$out/packages-source.sha256"
cp "$userspace" "$out/inputs/dynblk.deb"
cp "$dkms" "$out/inputs/dynblk-dkms.deb"
cp "$out/inputs/"*.deb "$prep/rootfs/opt/packages/"
printf '%s\n' "$version" > "$prep/rootfs/opt/package-test/version"
for name in dynblk dynblk-dkms; do
    digest=$(sha256sum "$out/inputs/$name.deb" | cut -d ' ' -f1)
    printf '%s  /opt/packages/%s.deb\n' "$digest" "$name"
done > "$prep/rootfs/opt/package-test/packages.sha256"
dpkg-deb --fsys-tarfile "$dkms" | tar -xOf - "./usr/src/dynblk-$version/dynblk.c" | \
    sha256sum | cut -d ' ' -f1 > "$prep/rootfs/opt/package-test/kernel-source.sha256"
unshare --user --mount --pid --fork \
    --map-users="0:$(id -u):1" --map-users=1:100000:65535 \
    --map-groups="0:$(id -g):1" --map-groups=1:100000:65535 \
    /bin/bash "$here/package.container.sh" finalize "$prep"
cp --reflink=auto "$prep/rootfs.tar.gz" "$out/iso/payload/rootfs.tar.gz"
cp "$prep/base.sha256" "$prep/base-install.log" "$prep/dependency-resolution.log" "$out/"
sha256sum "$out/iso/payload/rootfs.tar.gz" > "$out/rootfs.sha256"
sha256sum "$prep/rootfs/opt/packages/"*.deb "$prep/rootfs/opt/dependencies/"*.deb > "$out/guest-debs.sha256"
boot=$out/bootstrap
mkdir -p "$boot"/{bin,proc,sys,dev,etc,lib/modules}
copy_binary() {
    local binary=$1 library dependencies
    cp --parents "$binary" "$boot"
    dependencies=$(ldd "$binary" | awk '/=> \// {print $3} /^[[:space:]]*\// {print $1}')
    while read -r library; do
        test -f "$library" && cp -L --parents "$library" "$boot"
    done <<< "$dependencies"
}
copy_binary /usr/bin/busybox
for app in $(/usr/bin/busybox --list); do ln -sf /usr/bin/busybox "$boot/bin/$app"; done
copy_binary /usr/sbin/mke2fs
copy_binary /usr/sbin/blkid
cp /etc/mke2fs.conf "$boot/etc/"
ln -s usr/sbin "$boot/sbin"
for driver in crc32c_generic ext4 iso9660 ata_piix sd_mod sr_mod; do
    dependencies=$(modprobe -S "$kernel" --show-depends "$driver")
    while read -r verb path rest; do
        if test "$verb" = insmod; then cp --parents "$path" "$boot"; fi
    done <<< "$dependencies"
done
cp /lib/modules/"$kernel"/modules.{order,builtin,builtin.modinfo} "$boot/lib/modules/$kernel/"
depmod -b "$boot" "$kernel"
cp "$here/package.bootstrap" "$boot/init"
chmod 0755 "$boot/init"
(cd "$boot" && find . -print0 | cpio --null -o -H newc --owner=0:0 | gzip -1) > "$out/iso/boot/initrd.gz"
cp "$prep/rootfs/boot/vmlinuz-$kernel" "$out/iso/boot/vmlinuz"
cp "$here/package.grub.cfg" "$out/iso/boot/grub/grub.cfg"
sha256sum "$here"/package.* "$out/iso/boot/vmlinuz" "$out/iso/boot/initrd.gz" > "$out/harness.sha256"
sha256sum -c "$out/packages-source.sha256" > "$out/packages-stable.log"
grub-mkrescue -o "$out/package.iso" "$out/iso"
iso="$repo/build/iso/dynblk-package-$(basename "$out").iso"
if test -w "$repo/build/iso"; then cp -n "$out/package.iso" "$iso"; else sudo -n cp -n "$out/package.iso" "$iso"; fi
sha256sum "$iso" > "$out/iso.sha256"
printf 'ISO=%s\n' "$iso"
