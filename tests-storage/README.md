# Storage tests

Run `make test` from the repository root. The shared `dynblk_engine.c` is built
with ASan/UBSan, warnings and `-Werror`. Tests use private ordinary files; they
never attach host block devices or mount/format host filesystems.

`engine-tool.c` provides file I/O adapters and test-only codec encoders.
`fault.c` models failures around flush boundaries. The Python tests cover both
formats, QEMU interoperability, cache modes, limits, read-only operation,
discard, and reclaim with strictly opt-in relocation.

Dependencies: GCC, Python 3, zlib development headers, liblz4, libzstd and QEMU
image tools. Set `BUILD_DIR=/absolute/path` for a separate test build directory.
Kernel/device integration is covered by the disposable VM suite in `tests/vm`.
