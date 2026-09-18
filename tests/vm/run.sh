#!/bin/bash
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C
here=$(dirname "$(readlink -f "$0")")
out=$(readlink -f "${1:?artifact directory required}")
prefix=${2:?unique Testo prefix required}
case "$prefix" in dynblk-?*-) ;; *) exit 2 ;; esac
test ! -e "$out/exit-code"
if virsh -c qemu:///session dominfo "${prefix}dynblk" >/dev/null 2>&1; then
    printf 'Refusing reused Testo prefix: %s\n' "$prefix" >&2
    exit 2
fi
testo=/build/testo/source/build/out/sbin/testo
iso=$(awk '{print $2}' "$out/iso.sha256")
sha256sum -c "$out/iso.sha256"
mkdir -p "$out/share" "$out/evidence"
args=(run "$here/${3:-selfcontained}.testo" --user --prefix "$prefix"
    --allowed-sharing-directory "$out/share" --param ISO "$iso"
    --param EVIDENCE "$out/evidence")
"$testo" "${args[@]}" --dry
trap '"$testo" clean --user --prefix "$prefix" --assume-yes' EXIT
set +e
stdbuf -oL -eL "$testo" "${args[@]}" --report-folder "$out/report" \
    --junit-report "$out/junit.xml" --record-tests --assume-yes 2>&1 | tee "$out/run.log"
result=${PIPESTATUS[0]}
set -e
printf '%s\n' "$result" > "$out/exit-code"
# Forensic capture is best-effort evidence and must never overwrite the Testo result.
set +e
virsh -c qemu:///session dumpxml "${prefix}dynblk" > "$out/domain.xml"
disk=$(virsh -c qemu:///session domblklist "${prefix}dynblk" | awk '$1 == "hda" {print $2}')
if test -n "$disk"; then
    qemu-img convert -O raw "$disk" "$out/scratch.raw"
    # Read-only forensic extraction: an unclean capture can need journal replay.
    # Skip allocation bitmap loading, without repairing or changing the raw image.
    for log in guest.log results.json crash-state lzorle-fixtures.tar large-progress.json large-chunks.json large-metadata.json lowmem-results-512.json lowmem-results-1024.json lowmem-dmesg-512.txt lowmem-dmesg-1024.txt dmesg-failure.txt dmesg-before-cut.txt dmesg-before-cow.txt dmesg-before-gc.txt dmesg-before-checkpoint.txt dmesg-recovery.txt dmesg-lazy.txt dmesg-compaction.txt; do
        debugfs -c -R "cat /$log" "$out/scratch.raw" > "$out/evidence/$log" 2> "$out/evidence/debugfs-$log.log"
    done
fi
set -e
exit "$result"
