#!/bin/bash
# Add a real UEFI El Torito image to a prepared dynblk diagnostic ISO tree.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C
here=$(dirname "$(readlink -f "$0")")
repo=$(readlink -f "$here/../../../..")
input=$(readlink -f "${1:?prepared BIOS artifact directory required}")
out=$(readlink -m "${2:?fresh UEFI artifact directory required}")
reference=$(readlink -f "${3:?local MiniOS UEFI ISO required}")
case "$input" in "$repo"/build/dynblk-vm-init-*) ;; *) exit 2 ;; esac
case "$out" in "$repo"/build/dynblk-vm-init-*) ;; *) exit 2 ;; esac
test ! -e "$out" && test -f "$input/iso/boot/vmlinuz" && test -f "$input/iso/boot/initrd.gz"
test -f "$reference"
mkdir -p "$out"
cp -a "$input/iso" "$out/iso"
cp "$input/input-source.sha256" "$out/"
xorriso -osirrox on -indev "$reference" \
    -extract /minios/boot/grub/efi.img "$out/iso/boot/grub/efi.img" >/dev/null 2>&1
test -s "$out/iso/boot/grub/efi.img"
xorriso -as mkisofs -R -V DYNBLK-TEST -o "$out/acceptance-uefi.iso" \
    -eltorito-alt-boot -e boot/grub/efi.img -no-emul-boot "$out/iso" >/dev/null 2>&1
xorriso -indev "$out/acceptance-uefi.iso" -report_el_torito plain \
    >"$out/iso-inspection.log" 2>&1
grep -q "UEFI" "$out/iso-inspection.log"
sha256sum "$out/acceptance-uefi.iso" >"$out/iso.sha256"
printf 'ISO=%s\n' "$out/acceptance-uefi.iso"