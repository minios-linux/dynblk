#!/bin/bash
set -euo pipefail
# Build a diagnostic ISO only. Never mount or format anything on the host.
here=$(dirname "$(readlink -f "$0")")
repo=$(readlink -f "$here/../../../..")
module=$(readlink -f "${1:?module path required}")
out=${2:?new output directory required}
mode=${3:-selfcontained}
if test "$mode" != selfcontained; then
    echo 'only the current selfcontained VM architecture is supported' >&2
    exit 2
fi
test_case=${4:-dual}
case "$test_case" in full|extra|checker|large|audit|lowmem|mount|dual) ;; *) exit 2 ;; esac
kernel=$(modinfo -F vermagic "$module" | cut -d ' ' -f1)
name=$(modinfo -F name "$module")
case "$kernel" in 6.12.*) ;; *) exit 2 ;; esac
# Refuse the historical module-parameter attachment ABI before creating a
# misleading diagnostic ISO; current attachment is through dynblk-control.
module_parameters=$(modinfo -p "$module")
if grep -Eq '^(filename|part_limit_bytes):' <<<"$module_parameters"; then
    echo 'obsolete dynblk module-parameter ABI detected' >&2
    exit 2
fi
if ! grep -aFq 'dynblk-control' "$module"; then
    echo 'dynblk control device marker is missing' >&2
    exit 2
fi
test ! -e "$out"
test -f "/boot/vmlinuz-$kernel"
test -f /usr/lib/grub/i386-pc/cdboot.img
mkdir -p "$out/root" "$out/iso/boot/grub"
source=$(readlink -f "$(dirname "$module")")
test -x "$source/dynblk"
sha256sum "$module" "$source/dynblk" \
    "$source"/{dynblk.c,dynblk_engine.c,dynblk_engine.h,dynblk_host.c,dynblk_host.h,dynblk_uapi.h,dynblk_cli.c,dynblk_mount.inc,dynblk_common.c,dynblk_common.h,dynblk_check.c,dynblk_check.h,Kbuild,dkms.conf,FORMAT.md,README.md,Makefile,LICENSE} \
    "$source"/lzo/{decompress.c,decompress.h,compat.h,README.md} \
    > "$out/source-before.sha256"
root=$out/root
mkdir -p "$root"/{bin,proc,sys,dev,tmp,back,mnt,lib/modules,etc,run/lock}
ln -s usr/sbin "$root/sbin"
copy_binary() {
    local binary=$1 lib dependencies
    cp --parents "$binary" "$root"
    dependencies=$(ldd "$binary" | awk '/=> \// {print $3} /^[[:space:]]*\// {print $1}')
    while read -r lib; do
        test -f "$lib" && cp -L --parents "$lib" "$root"
    done <<< "$dependencies"
}
copy_libraries() {
    local binary=$1 lib dependencies
    dependencies=$(ldd "$binary" | awk '/=> \// {print $3} /^[[:space:]]*\// {print $1}')
    while read -r lib; do
        test -f "$lib" && cp -L --parents "$lib" "$root"
    done <<< "$dependencies"
}
copy_binary /usr/bin/busybox
copy_libraries "$source/dynblk"
for app in $(/usr/bin/busybox --list); do ln -sf /usr/bin/busybox "$root/bin/$app"; done
for binary in /usr/sbin/mke2fs /usr/sbin/mkfs.ext4 /usr/sbin/e2fsck /usr/sbin/resize2fs /usr/bin/fallocate /usr/sbin/blkid /usr/bin/findmnt /usr/bin/mount /usr/bin/umount /usr/bin/env /usr/sbin/mkfs.vfat /usr/sbin/mkfs.exfat /usr/sbin/fsck.vfat /usr/sbin/fsck.exfat /usr/sbin/losetup /usr/sbin/blockdev; do
    copy_binary "$binary"
done
if test "$mode" = selfcontained; then
    # Runtime module management is supplied by the already copied BusyBox. Keep
    # /sbin/modprobe too: kernel request_module() invokes the configured absolute helper.
    # Host-side modinfo/modprobe below are only builders for this diagnostic ISO.
    for app in modprobe insmod rmmod; do
        test -L "$root/bin/$app"
        ln -sf /usr/bin/busybox "$root/usr/sbin/$app"
    done
    for binary in /usr/sbin/mkfs.ext2 /usr/sbin/mkfs.btrfs /usr/sbin/mkfs.ntfs /usr/sbin/dumpe2fs /usr/bin/btrfs; do
        copy_binary "$binary"
    done
    cp "/boot/config-$kernel" "$root/kernel.config"
fi
if test "$test_case" = dual; then
    copy_binary /usr/bin/qemu-img
    copy_binary /usr/bin/qemu-io
fi
if test "$test_case" = checker || test "$test_case" = large || test "$test_case" = dual; then
    # Optional userspace decoders are included only in checker-focused media;
    # the dynblk executable itself has no DT_NEEDED dependency on them.
    for library in /usr/lib/x86_64-linux-gnu/liblz4.so.1 /usr/lib/x86_64-linux-gnu/libzstd.so.1 /usr/lib/x86_64-linux-gnu/libz.so.1; do
        copy_binary "$library"
    done
fi
copy_binary /usr/bin/python3.13
ln -s python3.13 "$root/usr/bin/python3"
mkdir -p "$root/usr/lib"
cp -a /usr/lib/python3.13 "$root/usr/lib/"
mkdir -p "$root/usr/lib/x86_64-linux-gnu"
cp -a /usr/lib/x86_64-linux-gnu/gconv "$root/usr/lib/x86_64-linux-gnu/"
cp /etc/mke2fs.conf "$root/etc/"
for driver in crc32c_generic ext4 vfat exfat nls_cp437 nls_ascii nls_iso8859-1 nls_utf8 loop ata_piix sd_mod sr_mod virtio_pci virtio_blk; do
    dependencies=$(modprobe -S "$kernel" --show-depends "$driver")
    while read -r verb path rest; do
        if test "$verb" = insmod; then cp --parents "$path" "$root"; fi
    done <<< "$dependencies"
done
if test "$mode" = selfcontained; then
    for driver in ext2 btrfs ntfs3 lz4 lz4hc lzo lzo-rle zstd deflate; do
        dependencies=$(modprobe -S "$kernel" --show-depends "$driver")
        printf '%s\n%s\n' "$driver" "$dependencies" >> "$out/extra-modules.log"
        while read -r verb path rest; do
            if test "$verb" = insmod; then cp --parents "$path" "$root"; fi
        done <<< "$dependencies"
    done
fi
cp /lib/modules/"$kernel"/modules.{order,builtin,builtin.modinfo} "$root/lib/modules/$kernel/"
depmod -b "$root" "$kernel"
if test "$mode" = selfcontained; then
    test -s "$root/lib/modules/$kernel/modules.alias"
    test -x "$root/bin/modprobe"
    test ! -e "$root/usr/bin/kmod"
fi
cp "$module" "$root/module.ko"
mkdir -p "$root/cli"
cp "$module" "$root/cli/dynblk.ko"
chmod 0644 "$root/module.ko" "$root/cli/dynblk.ko"
make -C "$source" -o dynblk install DESTDIR="$root" | tee "$out/install.log"
cmp "$source/dynblk" "$root/usr/sbin/dynblk"
test ! -e "$root/usr/lib/dynblk"
test -L "$root/usr/sbin/mount.dynblk"
printf '%s\n' "$name" > "$root/module-name"
cp "$here/init-selfcontained" "$root/init"
cp "$here/selfcontained.py" "$root/selfcontained.py"
cp "$here/selfcontained.py" "$root/entrypoint.py"
if test "$test_case" = dual; then
    cp "$here/dual.py" "$root/entrypoint.py"
    cp "$here/perf.py" "$root/perf.py"
    cp "$here/reclaim.py" "$root/reclaim.py"
elif test "$test_case" = mount; then
    cp "$here/mount.py" "$root/entrypoint.py"
elif test "$test_case" = extra; then
    cp "$here/selfcontained-extra.py" "$root/entrypoint.py"
elif test "$test_case" = audit; then
    cp "$here/audit.py" "$root/entrypoint.py"
elif test "$test_case" = lowmem; then
    cp "$here/lowmem.py" "$root/entrypoint.py"
fi
if test "$test_case" = checker || test "$test_case" = large; then
    cp "$here/init-release" "$root/init"
    cp "$here/release-common.py" "$root/release_common.py"
    cp "$here/$test_case.py" "$root/entrypoint.py"
    sectors=16777216
    if test "$test_case" = large; then sectors=50331648; fi
    printf '%s\n' "$sectors" > "$root/release-sectors"
fi
sha256sum "$root/selfcontained.py" > "$out/selfcontained.sha256"
chmod +x "$root/init"
find "$root" -type d -exec chmod 0755 {} +
sha256sum "$module" "/boot/vmlinuz-$kernel" > "$out/inputs.sha256"
for input in "$root/cli/"*; do
    if test -f "$input"; then sha256sum "$input"; fi
done > "$out/snapshot.sha256"
sha256sum "$root/init" "$root/usr/sbin/dynblk" >> "$out/snapshot.sha256"
if test "$test_case" = checker || test "$test_case" = large; then
    sha256sum "$root/release_common.py" "$root/release-sectors" \
        "$root/usr/sbin/dynblk" "$here/$test_case.testo" >> "$out/snapshot.sha256"
fi
testo_case=selfcontained.testo
if test "$test_case" = mount; then testo_case=mount.testo; fi
if test "$test_case" = audit; then testo_case=audit.testo; fi
if test "$test_case" = lowmem; then testo_case=lowmem512.testo; fi
sha256sum "$root/selfcontained.py" "$root/entrypoint.py" "$here/$testo_case" "$here/grub-selfcontained.cfg" >> "$out/snapshot.sha256"
if test "$test_case" = lowmem; then
    sha256sum "$here/lowmem1024.testo" >> "$out/snapshot.sha256"
fi
sha256sum -c "$out/source-before.sha256" > "$out/source-stable.log"
(cd "$root" && find . -print0 | cpio --null -o -H newc --owner=0:0 | gzip -1) > "$out/iso/boot/initrd.gz"
cp "/boot/vmlinuz-$kernel" "$out/iso/boot/vmlinuz"
cp "$here/grub-selfcontained.cfg" "$out/iso/boot/grub/grub.cfg"
iso="$repo/build/iso/dynblk-acceptance-$(basename "$out").iso"
grub-mkrescue -o "$out/acceptance.iso" "$out/iso"
if test -w "$repo/build/iso"; then
    cp -n "$out/acceptance.iso" "$iso"
else
    sudo -n cp -n "$out/acceptance.iso" "$iso"
fi
sha256sum "$iso" | tee "$out/iso.sha256"
printf 'ISO=%s\n' "$iso"
