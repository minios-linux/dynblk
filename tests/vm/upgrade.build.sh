#!/bin/bash
# Compose test media from prepared files; no host devices are opened.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C
here=$(dirname "$(readlink -f "$0")")
repo=$(readlink -f "$here/../../../..")
prep=$(readlink -f "${1:?prepared runtime directory}")
out=$(readlink -m "${2:?fresh ISO artifact directory}")
case "$prep" in "$repo"/build/dynblk-vm-package-upgrade-*) ;; *) exit 2 ;; esac
case "$out" in "$repo"/build/dynblk-vm-package-upgrade-*) ;; *) exit 2 ;; esac
test ! -e "$out"
test -f "$prep/rootfs.tar.gz"
sha256sum -c "$prep/rootfs.sha256"
old=6.12.94+deb13-amd64
new=6.12.107+deb13-amd64
mkdir -p "$out/bootstrap" "$out/iso/boot/grub" "$out/iso/payload"
cp --reflink=auto "$prep/rootfs.tar.gz" "$out/iso/payload/rootfs.tar.gz"
cp "$prep/base.sha256" "$prep/rootfs.sha256" "$out/"
cp "$prep/rootfs/opt/package-test/packages.sha256" "$out/packages.sha256"
cp "$prep/rootfs/opt/package-test/base-packages.txt" "$out/base-packages.txt"
boot=$out/bootstrap
mkdir -p "$boot"/{bin,proc,sys,dev,etc,lib/modules}
copy_binary() {
    local binary=$1 library dependencies
    cp --parents "$binary" "$boot"
    dependencies=$(ldd "$binary" | awk '/=> \// {print $3} /^[[:space:]]*\// {print $1}')
    while read -r library; do test -f "$library" && cp -L --parents "$library" "$boot"; done <<< "$dependencies"
}
copy_binary /usr/bin/busybox
for app in $(/usr/bin/busybox --list); do ln -s /usr/bin/busybox "$boot/bin/$app"; done
copy_binary /usr/sbin/mke2fs
copy_binary /usr/sbin/blkid
cp /etc/mke2fs.conf "$boot/etc/"
ln -s usr/sbin "$boot/sbin"
for driver in crc32c_generic ext4 iso9660 ata_piix sd_mod sr_mod; do
    dependencies=$(modprobe -d "$prep/rootfs" -S "$old" --show-depends "$driver")
    while read -r verb path rest; do
        if test "$verb" = insmod; then
            case "$path" in "$prep/rootfs/"*) ;; *) exit 2 ;; esac
            relative=${path#"$prep/rootfs/"}
            install -Dm0644 "$path" "$boot/$relative"
        fi
    done <<< "$dependencies"
done
mkdir -p "$boot/lib/modules/$old"
cp "$prep/rootfs/lib/modules/$old/"modules.{order,builtin,builtin.modinfo} "$boot/lib/modules/$old/"
depmod -b "$boot" "$old"
sed "s/6.12.107+deb13-amd64/$old/g" "$here/package.bootstrap" > "$boot/init"
chmod 0755 "$boot/init"
(cd "$boot" && find . -print0 | cpio --null -o -H newc --owner=0:0 | gzip -1) > "$out/iso/boot/initrd.gz"
cp "$prep/rootfs/boot/vmlinuz-$old" "$out/iso/boot/vmlinuz"
cat > "$out/iso/boot/grub/grub.cfg" <<EOF
set timeout=0
set default=0
if search --no-floppy --label DYNBLK-PKG --set=installed; then
    if [ -e (\$installed)/opt/package-test/upgrade-state.json ]; then
        menuentry "Dynblk updated distro kernel" {
            set root=\$installed
            linux /boot/vmlinuz-$new console=ttyS0,115200 console=tty0 panic=-1 root=/dev/sda rw dynblk_disposable_acceptance=1 dynblk_package_test=1 dynblk_upgrade_test=1
            initrd /boot/initrd.img-$new
        }
    fi
fi
menuentry "Dynblk clean old-kernel bootstrap" {
    linux /boot/vmlinuz console=ttyS0,115200 console=tty0 panic=-1 root=/dev/sda rw dynblk_disposable_acceptance=1 dynblk_package_test=1 dynblk_upgrade_test=1
    initrd /boot/initrd.gz
}
EOF
sha256sum "$here"/upgrade.* "$here"/upgrade-uefi.testo "$here/package.py" "$here/package.bootstrap" "$here/package.run.sh" > "$out/harness.sha256"
sha256sum "$out/iso/boot/"*gz "$out/iso/boot/vmlinuz" "$out/iso/boot/grub/grub.cfg" > "$out/boot.sha256"
grub-mkrescue -o "$out/upgrade.iso" "$out/iso"
printf 'Prepared local ISO: %s\n' "$out/upgrade.iso"
