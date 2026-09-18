# Linux LZO Userspace Adapter

This directory adapts the GPL-2.0-only Linux v6.12 safe LZO1X decoder for the
native dynblk offline checker. The adapted decoder is compiled directly into the
`dynblk` executable; there is no decoder helper process or Python runtime path.
The upstream compressor is retained only for tests.

Upstream: https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/refs/tags/v6.12/lib/lzo/

## Provenance

`decompress.c` retains the scalar decoding and bounds/error logic from Linux
`lib/lzo/lzo1x_decompress_safe.c`. Kernel headers and exports were replaced by
small userspace compatibility definitions, while architecture-specific fast paths
were omitted. The exported userspace symbol is renamed to
`dynblk_lzo1x_decompress_safe` so linking liblzo2 for ordinary LZO cannot interpose
or replace the Linux-derived RLE-capable decoder.

`compress-test.c` retains the scalar Linux v6.12 compressor path, including
`lzorle1x_1_compress`, solely to generate deterministic test streams.
SHA256 of the unmodified upstream v6.12 source inputs:

```text
9ab624768b316abbb8c51d3572fab3d8cc648d04bbc00972d840234018118bd0  lzo1x_decompress_safe.c
eb127f424ec425a2aa87b0264ecc2c96c29535aaf9b20105c789a0b222c6702c  lzo1x_compress.c
```

These hashes identify the upstream inputs, not the adapted sources or final
binary. The GPL text is in the repository-root `LICENSE` and the package ships
this matching decoder source.

## Runtime Contract

The native checker accepts compressed output lengths of exactly 4096 or 65536
bytes and rejects malformed/truncated/trailing streams unless the complete input
is consumed and the expected decoded length is produced. `lzo-rle` always uses
the in-process Linux-derived decoder; ordinary `lzo` continues to use liblzo2.
No external executable, shell or PATH lookup participates in decoding.

## Testing

`make test-userspace` builds `test.c` with ASan/UBSan and strict warnings. Its
deterministic coverage is 2048 ordinary/RLE round trips, 120085 truncation
rejections and 20000 malformed-input/bounds trials. Native black-box checker tests
exercise real 4 KiB and shared 64 KiB RLE payloads alongside the other codecs.
These tests cover the userspace decoder path; kernel crypto/VFS integration is
covered separately by the integration tests.
