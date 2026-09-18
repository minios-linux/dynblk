# DynBlk VM tests

## Current native/VMDK smoke test

Build a matching Linux 6.12 module and CLI, then from the repository root:

```sh
PATH=/usr/sbin:$PATH bash tests/vm/build.sh "$PWD/dynblk.ko" /new/output selfcontained dual
python3 tests/vm/dual.run.py /new/output --memory 512
```

The runner creates its own new 8-GiB scratch image, boots an isolated guest and
records `dual-serial.log` and `dual-exit-code`. It does not use an existing VM.
Coverage includes both backends, five cache modes, codecs, reload, read-only,
mount cleanup, growth and split VMDK exchange with QEMU. `perf.py` adds a small
32-MiB write comparison with final synchronization; its speeds are diagnostic,
not regression thresholds or a substitute for steady-state benchmarking.

`reclaim.py` additionally verifies online reclaim on ext4/exFAT, preservation
of live data, opt-in compaction, and compatibility with QEMU after reclamation.
The primary acceptance runner is `dual.run.py` with the `dual` image profile.
