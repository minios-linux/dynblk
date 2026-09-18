#!/bin/bash
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C
here=$(dirname "$(readlink -f "$0")")
repo=$(readlink -f "$here/../../../..")
input=${1:?prepared ISO artifact directory required}
out=${2:?fresh run artifact directory required}
prefix=${3:?fresh dynblk-init prefix required}
firmware=${4:-bios}
case "$out" in "$repo"/build/dynblk-vm-init-*) ;; *) exit 2 ;; esac
case "$prefix" in dynblk-init-?*-) ;; *) exit 2 ;; esac
case "$firmware" in
    bios) scenario=initramfs.testo; checksum='iso-bios.sha256' ;;
    uefi) scenario=initramfs-uefi.testo; checksum='iso-uefi.sha256' ;;
    *) exit 2 ;;
esac
if test ! -f "$input/$checksum" && test "$firmware" = bios; then checksum=iso.sha256; fi
test ! -e "$out"
test "$(readlink -m "$out")" = "$out"
if virsh -c qemu:///session dominfo "${prefix}dynblk" >/dev/null 2>&1; then exit 2; fi
testo=/build/testo/source/build/out/sbin/testo
sha256sum -c "$input/$checksum"
iso=$(awk '{print $2}' "$input/$checksum")
mkdir -p "$out/share" "$out/evidence"
cp "$input/$checksum" "$out/iso.sha256"
cp "$input/input-source.sha256" "$out/"
cp "$here/$scenario" "$out/scenario.testo"
printf '%s\n' "$prefix" >"$out/prefix"
printf '%s\n' "$firmware" >"$out/firmware"
args=(run "$out/scenario.testo" --user --prefix "$prefix" --allowed-sharing-directory "$out/share"
      --param ISO "$iso" --param EVIDENCE "$out/evidence")
"$testo" --version >"$out/testo-version"
/usr/bin/testo --version >"$out/reference-version"
"$testo" "${args[@]}" --dry >"$out/dry.log" 2>&1
/usr/bin/testo "${args[@]}" --dry >"$out/reference-dry.log" 2>&1
cleanup() {
    local result=$?
    trap - EXIT
    "$testo" clean --user --prefix "$prefix" --assume-yes >"$out/cleanup.log" 2>&1 || result=1
    if virsh -c qemu:///session list --all --name | grep -q "^${prefix}"; then result=1; fi
    printf '%s\n' "$result" >"$out/exit-code"
    exit "$result"
}
trap cleanup EXIT
set +e
"$testo" "${args[@]}" --report-folder "$out/report" --junit-report "$out/junit.xml" \
    --record-tests --assume-yes >"$out/run.log" 2>&1
result=$?
set -e
printf '%s\n' "$result" >"$out/testo-exit-code"
virsh -c qemu:///session dumpxml "${prefix}dynblk" >"$out/domain.xml"
state=$(virsh -c qemu:///session domstate "${prefix}dynblk")
if test "$state" != 'shut off'; then
    printf 'No disk read: domain is %s\n' "$state" >"$out/extraction.log"
    exit 1
fi
disk=$(virsh -c qemu:///session domblklist "${prefix}dynblk" | awk '$1 == "hda" {print $2}')
test -n "$disk"
qemu-img convert -O raw "$disk" "$out/scratch.raw"
mkdir "$out/extracted"
debugfs -c -R "rdump /evidence $out/extracted" "$out/scratch.raw" 2>"$out/extraction.log"
test "$result" = 0
(cd "$out/extracted/evidence" && sha256sum -c checksums-1 checksums-2) >"$out/evidence-validation.log"
python3 "$here/initramfs.verify.py" "$out" >>"$out/evidence-validation.log"
