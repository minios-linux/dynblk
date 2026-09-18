#!/bin/bash
# Assemble a diagnostic initramfs/ISO from existing artifacts; no host mounts.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C
here=$(dirname "$(readlink -f "$0")")
repo=$(readlink -f "$here/../../../..")
out=${1:?new absolute artifact directory required}
case "$out" in "$repo"/build/dynblk-vm-init-*) ;; *) exit 2 ;; esac
test ! -e "$out"
test "$(readlink -m "$out")" = "$out"
module=$repo/submodules/dynblk/dynblk.ko
cli=${DYNBLK_CLI:-$repo/linux-live/initramfs/livekit-mos/bin/dynblk}
kernel=$(modinfo -F vermagic "$module" | cut -d ' ' -f1)
test "$kernel" = 6.12.107+deb13-amd64
test -z "$(modinfo -F depends "$module")"
test -x "$cli"
cli_file=$(file "$cli")
grep -q 'statically linked' <<<"$cli_file"
cli_dynamic=$(readelf -d "$cli")
if grep -q '(NEEDED)' <<<"$cli_dynamic"; then
    echo 'dynblk initramfs CLI has dynamic dependencies' >&2
    exit 2
fi
livekit=$repo/linux-live/initramfs/livekit-mos
root=$out/root
mkdir -p "$root"/{bin,sbin,usr/bin,usr/sbin,proc,sys,dev,run,tmp,etc,back,upper,test-modules/raw} "$out/iso/boot/grub"
sha256sum "$module" "$repo/submodules/dynblk/dynblk.c" "$cli" \
    "$repo/submodules/dynblk/dynblk_cli.c" "$here/initramfs.init" "$livekit/bin/busybox" \
    >"$out/input-source.sha256"
cp "$livekit/bin/busybox" "$root/bin/busybox"
for app in $("$livekit/bin/busybox" --list); do
    test "$app" = busybox || ln -s busybox "$root/bin/$app"
done
for app in mke2fs blkid; do
    rm -f "$root/bin/$app"
    cp "$livekit/bin/$app" "$root/bin/$app"
done
rm -f "$root/bin/dynblk"
cp "$cli" "$root/bin/dynblk"
for app in modprobe insmod rmmod; do
    ln -sf ../bin/$app "$root/sbin/$app"
done
printf 'NAME=MiniOS-dynblk-native-test\nID=minios\n' >"$root/etc/initrd-release"
printf 'root:x:0:0:root:/root:/bin/sh\n' >"$root/etc/passwd"
printf 'root:x:0:\n' >"$root/etc/group"
for driver in crc32c_generic ext4 ata_piix sd_mod lz4 lz4hc lzo lzo-rle zstd deflate; do
    dependencies=$(modprobe -S "$kernel" --show-depends "$driver")
    printf '%s\n%s\n' "$driver" "$dependencies" >>"$out/modules.log"
    while read -r verb path rest; do
        if test "$verb" = insmod; then cp --parents "$path" "$root"; fi
    done <<<"$dependencies"
done
mkdir -p "$root/lib/modules/$kernel/updates/dkms"
cp /lib/modules/"$kernel"/modules.{order,builtin,builtin.modinfo} "$root/lib/modules/$kernel/"
cp "$module" "$root/lib/modules/$kernel/updates/dkms/dynblk.ko"
cp "$module" "$root/test-modules/raw/dynblk.ko"
# The bundled BusyBox 1.26.2 loads plain .ko files. Decompression happens only
# while assembling the image; no decompressor is required in the guest runtime.
find "$root/lib/modules" -type f -name '*.ko.xz' -exec unxz {} +
find "$root/lib/modules" -type f -name '*.ko.gz' -exec gunzip {} +
find "$root/lib/modules" -type f -name '*.ko.zst' -exec unzstd --rm {} +
depmod -b "$root" "$kernel"
test -s "$root/lib/modules/$kernel/modules.dep"
grep -q 'updates/dkms/dynblk.ko:' "$root/lib/modules/$kernel/modules.dep"
test -x "$root/bin/modprobe" && test -x "$root/bin/insmod" && test -x "$root/bin/rmmod"
test ! -e "$root/usr/bin/kmod"
cp "$here/initramfs.init" "$root/init"
chmod 0755 "$root/init" "$root/bin/"{busybox,dynblk,mke2fs,blkid}
find "$root" -type d -exec chmod 0755 {} +
find "$root/lib/modules" "$root/test-modules" -type f -exec chmod 0644 {} +
sha256sum "$here/initramfs.build.sh" "$here/initramfs.init" >"$out/harness.sha256"
sha256sum -c "$out/input-source.sha256" >"$out/input-stable.log"
(cd "$root" && find . -print0 | cpio --null -o -H newc --owner=0:0 | gzip -1) >"$out/iso/boot/initrd.gz"
cp "/boot/vmlinuz-$kernel" "$out/iso/boot/vmlinuz"
cat >"$out/iso/boot/grub/grub.cfg" <<'GRUB'
set timeout=0
set default=0
menuentry "Dynblk native initramfs acceptance" {
    linux /boot/vmlinuz console=ttyS0,115200 console=tty0 nomodeset panic=-1 dynblk_init_acceptance=1
    initrd /boot/initrd.gz
}
GRUB
sha256sum "$out/iso/boot/"{vmlinuz,initrd.gz} >"$out/boot-inputs.sha256"
grub_args=()
if test -n "${2:-}"; then
    test -f "$2/modinfo.sh" && test -f "$2/linux.mod"
    grub_args+=(--directory="$2")
fi
grub-mkrescue "${grub_args[@]}" -o "$out/acceptance.iso" "$out/iso"

# Build a self-contained UEFI El Torito image without requiring grub-efi on the
# host. OVMF's built-in shell executes startup.nsh from the FAT boot image, and
# the Linux EFI stub loads the same kernel/initrd used by the BIOS image.
efi="$out/efi.img"
truncate -s 64M "$efi"
mformat -i "$efi" -F ::
mcopy -i "$efi" "$out/iso/boot/vmlinuz" ::/vmlinuz.efi
mcopy -i "$efi" "$out/iso/boot/initrd.gz" ::/initrd.gz
cat >"$out/startup.nsh" <<'UEFI'
map -r
fs0:\vmlinuz.efi initrd=\initrd.gz console=ttyS0,115200 console=tty0 nomodeset panic=-1 dynblk_init_acceptance=1
UEFI
mcopy -i "$efi" "$out/startup.nsh" ::/startup.nsh
cp "$efi" "$out/iso/boot/efi.img"
xorriso -as mkisofs -R -J -V DYNBLK-UEFI -eltorito-alt-boot \
    -e boot/efi.img -no-emul-boot -o "$out/acceptance-uefi.iso" "$out/iso"

bios_iso="$repo/build/iso/dynblk-init-$(basename "$out").iso"
uefi_iso="$repo/build/iso/dynblk-init-$(basename "$out")-uefi.iso"
for iso in "$bios_iso" "$uefi_iso"; do test ! -e "$iso"; done
if test -w "$repo/build/iso"; then
    cp -n "$out/acceptance.iso" "$bios_iso"
    cp -n "$out/acceptance-uefi.iso" "$uefi_iso"
else
    sudo -n cp -n "$out/acceptance.iso" "$bios_iso"
    sudo -n cp -n "$out/acceptance-uefi.iso" "$uefi_iso"
fi
sha256sum "$bios_iso" >"$out/iso-bios.sha256"
cp "$out/iso-bios.sha256" "$out/iso.sha256"
sha256sum "$uefi_iso" >"$out/iso-uefi.sha256"
xorriso -indev "$bios_iso" -report_el_torito plain -report_system_area plain \
    >"$out/iso-inspection.log" 2>&1
xorriso -indev "$uefi_iso" -report_el_torito plain -report_system_area plain \
    >"$out/iso-uefi-inspection.log" 2>&1
printf 'BIOS_ISO=%s\nUEFI_ISO=%s\n' "$bios_iso" "$uefi_iso"
