#!/bin/bash
set -euo pipefail
export LC_ALL=C
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
here=$(dirname "$(readlink -f "$0")")
out=$(readlink -f "${1:?package VM artifact directory required}")
prefix=${2:?fresh dynblk-package prefix required}
case "$prefix" in dynblk-package-?*-) ;; *) exit 2 ;; esac
test ! -e "$out/exit-code"
if virsh -c qemu:///session dominfo "${prefix}dynblk" >/dev/null 2>&1; then exit 2; fi
testo=/build/testo/source/build/out/sbin/testo
iso=$(awk '{print $2}' "$out/iso.sha256")
sha256sum -c "$out/iso.sha256"
mkdir -p "$out/share" "$out/evidence"
scenario=$(readlink -f "${3:-$here/package.testo}")
test -f "$scenario"
args=(run "$scenario" --user --prefix "$prefix"
      --allowed-sharing-directory "$out/share" --param ISO "$iso" --param EVIDENCE "$out/evidence")
"$testo" "${args[@]}" --dry
trap '"$testo" clean --user --prefix "$prefix" --assume-yes' EXIT
set +e
stdbuf -oL -eL "$testo" "${args[@]}" --report-folder "$out/report" --junit-report "$out/junit.xml" \
    --record-tests --assume-yes 2>&1 | tee "$out/run.log"
result=${PIPESTATUS[0]}
set -e
printf '%s\n' "$result" > "$out/exit-code"
virsh -c qemu:///session dumpxml "${prefix}dynblk" > "$out/domain.xml"
disk=$(virsh -c qemu:///session domblklist "${prefix}dynblk" | awk '$1 == "hda" {print $2}')
if test -n "$disk"; then
    state=$(virsh -c qemu:///session domstate "${prefix}dynblk")
    if test "$state" != 'shut off'; then
        printf 'Disk capture skipped: domain state is %s; no live-image read attempted.\n' "$state" | tee "$out/extraction.log"
        test "$result" != 0 || result=1
    else
        qemu-img convert -O raw "$disk" "$out/scratch.raw"
        # rdump creates the source directory and refuses an existing one.
        mkdir "$out/guest"
        debugfs -R "rdump /evidence $out/guest" "$out/scratch.raw" 2> "$out/extraction.log"
    fi
fi
if test "$result" = 0; then
    # Screen recognition alone must not qualify missing or failed guest results.
    (cd "$out/guest/evidence" && sha256sum -c SHA256SUMS) > "$out/evidence-integrity.log"
    test -f "$out/guest/evidence/exit-code"
    test "$(cat "$out/guest/evidence/exit-code")" = 0
    test "$(cat "$out/guest/evidence/pipeline-status")" = '0 0'
    python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d["final"]["installed_not_loaded"] is True' "$out/guest/evidence/results.json"
fi
exit "$result"
