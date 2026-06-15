"""
pixel_bench_test.py — sqlite-bench DiskANN workload simulation on Pixel phone via adb

Simulates DiskANN index build pattern for small / glove / coco workloads:
  1) fillseq    100k inserts  (fresh DB)
  2) overwrite  2M random writes on 100k key range  (back-edge updates)
  3) compact    sqlite4 only: full LSM compaction via compact_db
  4) drop cache
  5) readrandom 10k reads

Usage:
  python pixel_bench_test.py [--adb-serial <serial>] [--workloads small,glove,coco]
"""

import subprocess
import argparse
import re
import signal
import sys
import time

SQLITE3_RUNNER = "/data/local/sqlite3-runner"
SQLITE4_RUNNER = "/data/local/sqlite4-runner"
DEVICE_DB_DIR  = "/data/local/tmp"

WORKLOADS = {
    "small": 100,
    "glove": 5296,
    "coco":  12784,
}

NUM       = 100_000    # initial inserts
OVERWRITE = 2_000_000  # back-edge updates  (100k nodes × ~20 updates)
READS     = 10_000     # readrandom queries
KEY_RANGE = 100_000


def _adb(serial=None):
    return ["adb"] + (["-s", serial] if serial else [])


def adb_shell(cmd, serial=None, timeout=300):
    return subprocess.run(
        _adb(serial) + ["shell", cmd],
        capture_output=True, text=True, timeout=timeout
    )


def drop_caches(serial=None):
    r = adb_shell("sync; echo 3 > /proc/sys/vm/drop_caches",
                  serial=serial, timeout=15)
    if r.returncode != 0:
        print(f"    WARNING: drop_caches failed (rc={r.returncode})")


def db_size_mb(db_path, serial=None):
    r = adb_shell(
        f"du -k {db_path}* 2>/dev/null | awk '{{sum+=$1}} END{{print sum+0}}'",
        serial=serial, timeout=10
    )
    try:
        return int(r.stdout.strip()) / 1024.0
    except (ValueError, AttributeError):
        return 0.0


def parse_result_line(text):
    m = re.search(r'(\S+)\s*:\s*([\d.]+)\s+micros/op', text)
    if not m:
        return None
    result = {"name": m.group(1), "micros_per_op": float(m.group(2)), "mbps": None}
    m2 = re.search(r'([\d.]+)\s+MB/s', text)
    if m2:
        result["mbps"] = float(m2.group(1))
    return result


def run_step(label, runner, bench_args, serial=None, timeout=300):
    cmd = f"{runner} {bench_args}"
    print(f"    $ {cmd}")
    t0 = time.time()
    r  = adb_shell(cmd, serial=serial, timeout=timeout)
    elapsed = time.time() - t0
    combined = r.stdout + r.stderr
    result = None
    for line in combined.splitlines():
        parsed = parse_result_line(line)
        if parsed:
            result = parsed
    if result:
        mbps_str = f"  {result['mbps']:.1f} MB/s" if result["mbps"] else ""
        print(f"    {label:12s}: {result['micros_per_op']:>10.1f} µs/op{mbps_str}  ({elapsed:.1f}s)")
    elif r.returncode != 0:
        tail = combined[-500:].strip()
        print(f"    ERROR (rc={r.returncode}): {tail}")
    else:
        print(f"    (completed in {elapsed:.1f}s, no result parsed)")
        for line in combined.splitlines()[-5:]:
            if line.strip():
                print(f"      {line}")
    return result


def run_compact(compact_bin, db_path, serial=None):
    print(f"    $ {compact_bin} {db_path}")
    t0 = time.time()
    r  = adb_shell(f"{compact_bin} {db_path}", serial=serial, timeout=7200)
    elapsed = time.time() - t0
    combined = r.stdout + r.stderr
    if r.returncode != 0:
        print(f"    WARNING: compact_db failed (rc={r.returncode}): {combined[-300:].strip()}")
    else:
        print(f"    compact done  ({elapsed:.1f}s)")
    return elapsed


def run_workload(runner_name, runner_bin, wl_name, value_size, db_dir,
                 compact_bin=None, serial=None):
    db_path = f"{db_dir}/{runner_name.replace('-', '_')}_{wl_name}.db"
    adb_shell(f"rm -f {db_path}* 2>/dev/null", serial=serial)

    is_sqlite4 = "sqlite4" in runner_name
    do_compact = is_sqlite4 and compact_bin is not None
    n_phases   = 4 if do_compact else 3

    print(f"\n  [{runner_name}]  {wl_name}  value_size={value_size}B")

    print(f"  [1/{n_phases}] fillseq  ({NUM:,} inserts)")
    r_fill = run_step("fillseq", runner_bin,
        f"--benchmarks=fillseq --num={NUM} --value_size={value_size}"
        f" --progress=10000 --db={db_path}",
        serial=serial, timeout=1200)

    print(f"  [2/{n_phases}] overwrite  ({OVERWRITE:,} random writes, key_range={KEY_RANGE:,})")
    r_over = run_step("overwrite", runner_bin,
        f"--benchmarks=overwrite --num={OVERWRITE} --key_range={KEY_RANGE}"
        f" --use_existing_db=1 --value_size={value_size}"
        f" --progress=200000 --db={db_path}",
        serial=serial, timeout=7200)

    size_before_mb = db_size_mb(db_path, serial=serial)
    print(f"    DB size after write: {size_before_mb:.1f} MB")

    size_after_mb = size_before_mb
    if do_compact:
        print(f"  [3/{n_phases}] compact  (full LSM compaction)")
        run_compact(compact_bin, db_path, serial=serial)
        size_after_mb = db_size_mb(db_path, serial=serial)
        print(f"    DB size after compact: {size_after_mb:.1f} MB"
              f"  ({size_before_mb:.1f} → {size_after_mb:.1f})")

    print(f"  [drop cache]")
    drop_caches(serial=serial)

    print(f"  [{n_phases}/{n_phases}] readrandom  ({READS:,} reads, key_range={KEY_RANGE:,})")
    r_read = run_step("readrandom", runner_bin,
        f"--benchmarks=readrandom --reads={READS} --key_range={KEY_RANGE}"
        f" --value_size={value_size} --db={db_path}",
        serial=serial, timeout=1200)

    adb_shell(f"rm -f {db_path}* 2>/dev/null", serial=serial)

    return {
        "fillseq":       r_fill,
        "overwrite":     r_over,
        "readrandom":    r_read,
        "size_mb":       size_before_mb,
        "size_compact_mb": size_after_mb if do_compact else None,
    }


def print_summary(all_results):
    print(f"\n{'='*110}")
    print(f"  SUMMARY")
    print(f"{'='*110}")
    print(f"  {'Workload':>8}  {'Runner':>16}  {'fillseq µs/op':>14}"
          f"  {'overwrite µs/op':>16}  {'read µs/op':>11}  {'DB MB (write)':>14}  {'DB MB (compact)':>16}")
    print(f"  {'-'*106}")
    for wl_name, runners in all_results.items():
        for runner_name, res in runners.items():
            def fmt(r, w):
                return f"{r['micros_per_op']:>{w}.1f}" if r else f"{'N/A':>{w}}"
            compact_str = (f"{res['size_compact_mb']:>16.1f}"
                           if res["size_compact_mb"] is not None
                           else f"{'---':>16}")
            print(f"  {wl_name:>8}  {runner_name:>16}  {fmt(res['fillseq'], 14)}"
                  f"  {fmt(res['overwrite'], 16)}  {fmt(res['readrandom'], 11)}"
                  f"  {res['size_mb']:>14.1f}  {compact_str}")
    print(f"{'='*110}")


def cleanup_device(serial, db_dir):
    print("\nInterrupted — cleaning up device...")
    adb_shell("killall sqlite3-runner sqlite4-runner compact_db 2>/dev/null; true", serial=serial)
    adb_shell(f"rm -f {db_dir}/sqlite3_runner_*.db* {db_dir}/sqlite4_runner_*.db* 2>/dev/null",
              serial=serial)
    print("Cleanup done.")


def main():
    parser = argparse.ArgumentParser(
        description="DiskANN workload simulation on Pixel phone via adb"
    )
    parser.add_argument("--adb-serial",     type=str, default=None)
    parser.add_argument("--sqlite3-runner", type=str, default=SQLITE3_RUNNER)
    parser.add_argument("--sqlite4-runner", type=str, default=SQLITE4_RUNNER)
    parser.add_argument("--compact-bin",    type=str, default="/data/local/sqlite4_lsm/sync_NORMAL/autoflush_256/compact_db",
                        help="Path to compact_db on device (sqlite4 only); pass empty string to disable")
    parser.add_argument("--db-dir",         type=str, default=DEVICE_DB_DIR)
    parser.add_argument("--workloads",      type=str, default="small,glove,coco")
    args = parser.parse_args()

    serial  = args.adb_serial
    runners = [
        ("sqlite3-runner", args.sqlite3_runner),
        ("sqlite4-runner", args.sqlite4_runner),
    ]
    wl_names = [w.strip() for w in args.workloads.split(",")]

    compact_bin = args.compact_bin if args.compact_bin else None
    if compact_bin:
        r = adb_shell(f"test -x {compact_bin} && echo OK", serial=serial)
        if "OK" not in r.stdout:
            print(f"WARNING: compact_db not found at {compact_bin}, disabling compaction")
            compact_bin = None

    for rname, rpath in runners:
        r = adb_shell(f"test -x {rpath} && echo OK", serial=serial)
        if "OK" not in r.stdout:
            print(f"ERROR: {rname} not found at {rpath} on device")
            return 1

    def _sigint_handler(sig, frame):
        cleanup_device(serial, args.db_dir)
        sys.exit(1)

    signal.signal(signal.SIGINT, _sigint_handler)

    all_results = {}
    for wl_name in wl_names:
        if wl_name not in WORKLOADS:
            print(f"WARNING: unknown workload '{wl_name}', skipping")
            continue
        value_size = WORKLOADS[wl_name]

        print(f"\n{'#'*60}")
        print(f"# Workload: {wl_name}  (value_size={value_size}B)")
        print(f"{'#'*60}")

        wl_results = {}
        for rname, rpath in runners:
            res = run_workload(rname, rpath, wl_name, value_size,
                               args.db_dir, compact_bin=compact_bin, serial=serial)
            wl_results[rname] = res
        all_results[wl_name] = wl_results

    print_summary(all_results)
    return 0


if __name__ == "__main__":
    exit(main())
