#!/usr/bin/python3
import errno
import json
import os
import pathlib
import subprocess

CLI = "/usr/sbin/dynblk"
BLOCK = 4096
RUNTIME_CHUNK = 128 * BLOCK
FIXED_INDEX_BYTES = 16513 * 24
PATTERN = bytes((0x5A,)) * (32 * BLOCK)


def run(*args, capture=False):
    result = subprocess.run(args, check=True, text=True,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout.strip() if capture else ""


def status(device):
    return json.loads(run(CLI, "status", device, "--json", capture=True))


def memtotal_mib():
    for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemTotal:"):
            return int(line.split()[1]) // 1024
    raise RuntimeError("MemTotal missing")


def main():
    ram_mib = memtotal_mib()
    if 430 <= ram_mib <= 560:
        capacity_gib, budget_mib, label = 16, 128, "512"
    elif 900 <= ram_mib <= 1120:
        capacity_gib, budget_mib, label = 32, 256, "1024"
    else:
        raise RuntimeError("unexpected RAM size: {} MiB".format(ram_mib))

    backing = pathlib.Path("/back/lowmem-{}".format(label))
    backing.mkdir()
    volume = backing / "volume000.db"
    run("insmod", "/module.ko")
    device = run(CLI, "create", str(volume), "--size", "{}GiB".format(capacity_gib),
                 "--compression", "lz4", "--execute", capture=True)

    initial = status(device)
    assert initial["capacity_bytes"] == capacity_gib << 30
    assert initial["map_memory_bytes"] == FIXED_INDEX_BYTES

    chunks = (capacity_gib << 30) // RUNTIME_CHUNK
    assert chunks == budget_mib * 256
    pairs = chunks // 2
    fd = os.open(device, os.O_RDWR | os.O_SYNC)
    try:
        for pair in range(pairs):
            offset = pair * 2 * RUNTIME_CHUNK + RUNTIME_CHUNK - 64 * 1024
            written = os.pwrite(fd, PATTERN, offset)
            if written != len(PATTERN):
                raise RuntimeError("short block write")
            if (pair + 1) % 2048 == 0:
                print("LOWMEM progress {}/{}".format(pair + 1, pairs), flush=True)
        os.fsync(fd)
    finally:
        os.close(fd)

    filled = status(device)
    expected_map = (budget_mib << 20) + FIXED_INDEX_BYTES
    expected_original = pairs * len(PATTERN)
    assert filled["map_memory_bytes"] == expected_map, filled
    assert filled["original_bytes"] == expected_original, filled
    assert not filled["fenced"], filled

    run(CLI, "unload", device, "--execute")

    # Recovery must honor an explicit mapping cap. Materializing every runtime
    # chunk above means one MiB less than the required budget cannot attach the
    # existing volume. The failed recovery is read-only and must leave neither
    # a block device nor a changed backing-file geometry behind.
    before_sizes = {path.name: path.stat().st_size for path in backing.glob("volume*.db")}
    undersized_budget = budget_mib - 1
    failed = subprocess.run(
        [CLI, "load", str(volume), "--map-memory-mb", str(undersized_budget), "--execute"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert failed.returncode != 0, failed.stdout
    assert os.strerror(errno.ENOSPC) in failed.stderr, failed.stderr
    assert not list(pathlib.Path("/sys/block").glob("dynblk[0-9]*"))
    assert not list(pathlib.Path("/dev").glob("dynblk[0-9]*"))
    after_sizes = {path.name: path.stat().st_size for path in backing.glob("volume*.db")}
    assert after_sizes == before_sizes, (before_sizes, after_sizes)

    device = run(CLI, "load", str(volume), "--map-memory-mb", str(budget_mib),
                 "--execute", capture=True)
    recovered = status(device)
    assert recovered["map_memory_bytes"] == expected_map, recovered
    assert recovered["original_bytes"] == expected_original, recovered
    assert not recovered["fenced"], recovered

    fd = os.open(device, os.O_RDONLY)
    try:
        for pair in (0, pairs // 2, pairs - 1):
            offset = pair * 2 * RUNTIME_CHUNK + RUNTIME_CHUNK - 64 * 1024
            assert os.pread(fd, len(PATTERN), offset) == PATTERN
    finally:
        os.close(fd)
    run(CLI, "unload", device, "--execute")

    dmesg = run("dmesg", capture=True) + "\n"
    pathlib.Path("/back/lowmem-dmesg-{}.txt".format(label)).write_text(dmesg)
    bad_markers = ("WARNING: CPU:", "BUG:", "Oops:", "KASAN:", "object pointer:",
                   "Out of memory", "oom-kill", "Killed process")
    if any(marker in dmesg for marker in bad_markers):
        raise RuntimeError("kernel warning detected during low-memory stress")

    evidence = {
        "ram_mib": ram_mib,
        "capacity_gib": capacity_gib,
        "mapping_budget_mib": budget_mib,
        "undersized_mapping_budget_mib": undersized_budget,
        "undersized_attach_errno": errno.ENOSPC,
        "runtime_chunks": chunks,
        "map_memory_bytes": recovered["map_memory_bytes"],
        "original_bytes": recovered["original_bytes"],
        "physical_bytes": recovered["physical_bytes"],
    }
    pathlib.Path("/back/lowmem-results-{}.json".format(label)).write_text(
        json.dumps(evidence, sort_keys=True) + "\n")
    # Testo powers the guest off immediately after the PASS marker. Make the
    # final forensic files durable before publishing that marker.
    os.sync()
    print("DYNBLK LOWMEM {} PASS".format(label), flush=True)


if __name__ == "__main__":
    main()
