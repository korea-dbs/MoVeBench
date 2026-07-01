"""
pixel_benchmark.py  —  benchmark.py adapted for Pixel phone via adb

Host-side Python drives benchmarks that run on the connected Pixel phone.
Binaries on device:
  /data/local/sqlite3_libsql/MoVeBench/sqlite3
  /data/local/sqlite4_lsm/MoVeBench/sqlite4
  /data/local/sqlite4_lsm/MoVeBench/compact_db

Usage example:
  python pixel_benchmark.py \\
    --dataset-dir ./dataset \\
    --datasets sift,glove \\
    --adb-serial 2B261FDH200B0M \\
    --disk-device sda
"""

import subprocess
import argparse
import os
import re
import time
import threading
import tempfile
from datetime import datetime

DISK_DEVICE = "sda"   # Pixel phones usually expose UFS as sda
TOP_K = 10

_sql_counter = 0


# ─────────────────────────────────────────────────────────────────────────────
# adb helpers
# ─────────────────────────────────────────────────────────────────────────────

def _adb(serial=None):
    return ["adb"] + (["-s", serial] if serial else [])


def adb_push(local_path, device_path, serial=None):
    subprocess.run(
        _adb(serial) + ["push", local_path, device_path],
        check=True, capture_output=True, timeout=300
    )


def adb_shell(cmd_str, serial=None, timeout=20000):
    return subprocess.run(
        _adb(serial) + ["shell", cmd_str],
        capture_output=True, text=True, timeout=timeout
    )


def adb_rm(device_path, serial=None):
    try:
        adb_shell(f"rm -f {device_path}", serial=serial, timeout=10)
    except (subprocess.TimeoutExpired, OSError):
        pass


def device_file_size_mb(device_path, serial=None):
    try:
        r = adb_shell(f"stat -c %s {device_path} 2>/dev/null || echo 0", serial=serial)
        return int(r.stdout.strip().splitlines()[0]) / (1024 * 1024)
    except (OSError, ValueError, IndexError):
        return 0.0


def device_cleanup_db(db_path, serial=None, is_sqlite3=False):
    suffixes = ["", "-wal", "-shm"] if is_sqlite3 else ["", "-log", "-shm"]
    for s in suffixes:
        adb_rm(db_path + s, serial=serial)


def _tmp_device_sql(device_tmp_dir):
    global _sql_counter
    _sql_counter += 1
    return f"{device_tmp_dir}/_bench_{os.getpid()}_{_sql_counter}.sql"


# ─────────────────────────────────────────────────────────────────────────────
# DiskStatsMonitor  (reads /proc/diskstats from device via adb)
# ─────────────────────────────────────────────────────────────────────────────

class DiskStatsMonitor:
    def __init__(self, device, interval_s=1.0, log_path=None, serial=None):
        self.device = device
        self.interval_s = interval_s
        self.log_path = log_path
        self.serial = serial
        self._stop_event = threading.Event()
        self._thread = None
        self._samples = []
        self._error = None
        self._prev = None
        self._prev_ts = None

    def start(self):
        first = self._read_stats()
        if first is None:
            self._error = f"device {self.device} not found in /proc/diskstats"
            return self
        self._prev = first
        self._prev_ts = time.monotonic()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def stop(self):
        if self._thread is None:
            return self.summary()
        self._stop_event.set()
        self._thread.join(timeout=self.interval_s * 2 + 1.0)
        self._write_log()
        return self.summary()

    def _run(self):
        while not self._stop_event.wait(self.interval_s):
            cur = self._read_stats()
            ts = time.monotonic()
            if cur is None:
                self._error = f"device {self.device} disappeared from /proc/diskstats"
                return
            self._record_sample(cur, ts)
        cur = self._read_stats()
        ts = time.monotonic()
        if cur is not None:
            self._record_sample(cur, ts)

    def _record_sample(self, cur, ts):
        elapsed = ts - self._prev_ts
        if elapsed <= 0:
            self._prev, self._prev_ts = cur, ts
            return
        d = {k: cur[k] - self._prev[k] for k in cur}
        if any(d[k] < 0 for k in d):
            self._prev, self._prev_ts = cur, ts
            return
        self._samples.append({
            "wall_time":   datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "elapsed_s":   elapsed,
            "read_reqs":   d["read_reqs"],
            "read_bytes":  d["read_sectors"] * 512,
            "read_ms":     d["read_ms"],
            "write_reqs":  d["write_reqs"],
            "write_bytes": d["write_sectors"] * 512,
            "write_ms":    d["write_ms"],
            "busy_ms":     d["busy_ms"],
        })
        self._prev, self._prev_ts = cur, ts

    def _write_log(self):
        if not self.log_path:
            return
        os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
        with open(self.log_path, "w") as fp:
            fp.write(
                "wall_time,elapsed_s,read_mbps,write_mbps,read_iops,write_iops,"
                "read_latency_ms,write_latency_ms,latency_ms,disk_util\n"
            )
            for s in self._samples:
                e   = s["elapsed_s"]
                rm  = s["read_bytes"]  / 1048576 / e if e > 0 else 0.0
                wm  = s["write_bytes"] / 1048576 / e if e > 0 else 0.0
                ri  = s["read_reqs"]  / e if e > 0 else 0.0
                wi  = s["write_reqs"] / e if e > 0 else 0.0
                rl  = s["read_ms"]  / s["read_reqs"]  if s["read_reqs"]  > 0 else 0.0
                wl  = s["write_ms"] / s["write_reqs"] if s["write_reqs"] > 0 else 0.0
                tr  = s["read_reqs"] + s["write_reqs"]
                lat = (s["read_ms"] + s["write_ms"]) / tr if tr > 0 else 0.0
                util = s["busy_ms"] / (e * 10.0) if e > 0 else 0.0
                fp.write(
                    f"{s['wall_time']},{e:.3f},{rm:.3f},{wm:.3f},"
                    f"{ri:.3f},{wi:.3f},{rl:.3f},{wl:.3f},{lat:.3f},{util:.3f}\n"
                )

    def _read_stats(self):
        try:
            r = subprocess.run(
                _adb(self.serial) + ["shell", "cat", "/proc/diskstats"],
                capture_output=True, text=True, timeout=5
            )
            for line in r.stdout.splitlines():
                parts = line.split()
                if len(parts) < 14 or parts[2] != self.device:
                    continue
                return {
                    "read_reqs":     int(parts[3]),
                    "read_sectors":  int(parts[5]),
                    "read_ms":       int(parts[6]),
                    "write_reqs":    int(parts[7]),
                    "write_sectors": int(parts[9]),
                    "write_ms":      int(parts[10]),
                    "busy_ms":       int(parts[12]),
                }
        except (subprocess.TimeoutExpired, OSError, ValueError) as e:
            self._error = str(e)
        return None

    def summary(self):
        if not self._samples:
            return {
                "device": self.device, "available": False,
                "error": self._error or "no diskstats samples captured",
            }
        te  = sum(s["elapsed_s"]   for s in self._samples)
        trb = sum(s["read_bytes"]  for s in self._samples)
        twb = sum(s["write_bytes"] for s in self._samples)
        trr = sum(s["read_reqs"]   for s in self._samples)
        twr = sum(s["write_reqs"]  for s in self._samples)
        trm = sum(s["read_ms"]     for s in self._samples)
        twm = sum(s["write_ms"]    for s in self._samples)
        tbm = sum(s["busy_ms"]     for s in self._samples)
        mb  = 1048576
        return {
            "device": self.device, "available": True, "log_path": self.log_path,
            "samples": len(self._samples), "elapsed_s": te,
            "avg_read_mbps":        trb / mb / te if te > 0 else 0.0,
            "avg_write_mbps":       twb / mb / te if te > 0 else 0.0,
            "peak_read_mbps":  max(s["read_bytes"]  / mb / s["elapsed_s"] for s in self._samples),
            "peak_write_mbps": max(s["write_bytes"] / mb / s["elapsed_s"] for s in self._samples),
            "avg_read_iops":   trr / te if te > 0 else 0.0,
            "avg_write_iops":  twr / te if te > 0 else 0.0,
            "avg_read_latency_ms":  trm / trr if trr > 0 else 0.0,
            "avg_write_latency_ms": twm / twr if twr > 0 else 0.0,
            "avg_latency_ms": (trm + twm) / (trr + twr) if (trr + twr) > 0 else 0.0,
            "avg_disk_util":   tbm / (te * 10.0) if te > 0 else 0.0,
        }


# ─────────────────────────────────────────────────────────────────────────────
# Core helpers
# ─────────────────────────────────────────────────────────────────────────────

def _parse_android_time(text):
    """Parse Android toybox 'time' output: 0m00.00s real  0m00.00s user  0m00.00s system"""
    stats = {}
    kept = []
    for line in text.splitlines():
        m = re.search(
            r"(\d+)m([\d.]+)s\s+real\s+(\d+)m([\d.]+)s\s+user\s+(\d+)m([\d.]+)s\s+system",
            line
        )
        if m:
            stats["real_s"] = int(m.group(1)) * 60 + float(m.group(2))
            stats["user_s"] = int(m.group(3)) * 60 + float(m.group(4))
            stats["sys_s"]  = int(m.group(5)) * 60 + float(m.group(6))
        else:
            kept.append(line)
    cleaned = "\n".join(kept)
    if text.endswith("\n"):
        cleaned += "\n"
    return cleaned, stats


def push_sql(sql_input, serial=None, device_tmp_dir="/data/local/tmp"):
    """Write SQL to a local temp file and push it to device. Returns device path."""
    if not sql_input.rstrip().endswith(".quit"):
        sql_input += "\n.quit\n"
    device_sql = _tmp_device_sql(device_tmp_dir)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".sql", delete=False) as f:
        f.write(sql_input)
        local_tmp = f.name
    try:
        adb_push(local_tmp, device_sql, serial=serial)
    finally:
        os.unlink(local_tmp)
    return device_sql


def run_shell_device(shell, db, device_sql, serial=None, env_vars=None,
                     device_tmp_dir="/data/local/tmp", timeout=20000):
    """Execute a pre-pushed SQL file on device. Returns (stdout, stderr, time_stats)."""
    time_file = f"{device_tmp_dir}/_time_{os.getpid()}_{_sql_counter}.txt"
    env_str = " ".join(f"{k}={v}" for k, v in (env_vars or {}).items())
    if env_str:
        env_str += " "
    cmd_str = f"{{ time {env_str}{shell} {db} < {device_sql}; }} 2>{time_file}"

    proc = adb_shell(cmd_str, serial=serial, timeout=timeout)
    adb_rm(device_sql, serial=serial)

    t_result = adb_shell(f"cat {time_file} 2>/dev/null", serial=serial, timeout=10)
    adb_rm(time_file, serial=serial)
    stderr_text, time_stats = _parse_android_time(t_result.stdout)

    if proc.returncode not in (0, 1):
        err_lines = [l for l in stderr_text.splitlines() if not l.startswith("[LSM]")]
        err_msg = "\n".join(err_lines[-10:]) if err_lines else stderr_text[-500:]
        raise RuntimeError(f"shell error (rc={proc.returncode}): {err_msg}")
    return proc.stdout, stderr_text, time_stats


def run_shell(shell, db, sql_input, serial=None, env_vars=None,
              device_tmp_dir="/data/local/tmp", timeout=20000):
    """Push SQL to device and run through shell. Returns (stdout, stderr, time_stats)."""
    device_sql = push_sql(sql_input, serial=serial, device_tmp_dir=device_tmp_dir)
    return run_shell_device(shell, db, device_sql, serial=serial, env_vars=env_vars,
                            device_tmp_dir=device_tmp_dir, timeout=timeout)


def run_compact(compact_bin, db, serial=None, env_vars=None,
                device_tmp_dir="/data/local/tmp", lsm_compression="none"):
    """Run compact_db on device. Returns (stderr_text, time_stats)."""
    env_str = " ".join(f"{k}={v}" for k, v in (env_vars or {}).items())
    if env_str:
        env_str += " "
    compression_arg = f" {lsm_compression}" if lsm_compression and lsm_compression != "none" else ""
    time_file = f"{device_tmp_dir}/_time_{os.getpid()}_compact.txt"
    proc = adb_shell(
        f"{{ time {env_str}{compact_bin} {db}{compression_arg}; }} 2>{time_file}",
        serial=serial, timeout=20000
    )
    t_result = adb_shell(f"cat {time_file} 2>/dev/null", serial=serial, timeout=10)
    adb_rm(time_file, serial=serial)
    stderr_text, time_stats = _parse_android_time(t_result.stdout)

    for line in (proc.stderr + "\n" + stderr_text).splitlines():
        if line.strip():
            print(f"        {line.rstrip()}")

    if proc.returncode != 0:
        raise RuntimeError(f"compact_db failed (rc={proc.returncode})")
    return stderr_text, time_stats


def drop_caches(serial=None, enabled=True):
    """Drop OS page cache on device."""
    if not enabled:
        return
    try:
        r = adb_shell("sync; echo 3 > /proc/sys/vm/drop_caches", serial=serial, timeout=15)
        if r.returncode != 0:
            print(f"  WARNING: drop_caches failed: {r.stderr.strip()}")
    except (subprocess.TimeoutExpired, OSError) as e:
        print(f"  WARNING: drop_caches failed: {e}")


def read_sql(sql_path):
    with open(sql_path) as f:
        return f.read()


def prepare_insert_sql(sql_text, page_size_kb, is_sqlite3=False,
                       use_compaction=False, lsm_autoflush_mb=None,
                       lsm_automerge=None):
    pragmas = []
    if page_size_kb is not None:
        pragmas.append(f"PRAGMA page_size={page_size_kb * 1024};")
    if not is_sqlite3 and lsm_autoflush_mb is not None:
        pragmas.append(f"PRAGMA lsm_autoflush={lsm_autoflush_mb * 1024};")
    if not is_sqlite3 and lsm_automerge is not None:
        pragmas.append(f"PRAGMA lsm_automerge={lsm_automerge};")
    if not pragmas:
        return sql_text
    return "\n".join(pragmas) + "\n" + sql_text


def build_db_target(db_path, is_sqlite3=False, page_size_kb=None,
                    lsm_compression="none", use_compaction=False):
    if is_sqlite3 or page_size_kb is None:
        return db_path
    params = [f"page_size={page_size_kb * 1024}"]
    if lsm_compression and lsm_compression != "none":
        params.append(f"lsm_compression={lsm_compression}")
    return f"file:{db_path}?{'&'.join(params)}"


def parse_output_to_results(output, k):
    id_lines = []
    for line in output.strip().split("\n"):
        line = line.strip()
        if not line:
            continue
        try:
            id_lines.append(int(line))
        except ValueError:
            pass
    results = []
    for i in range(0, len(id_lines), k):
        results.append(set(id_lines[i:i + k]))
    return results


def load_groundtruth(path):
    results = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                results.append(set(int(x) for x in line.split(",") if x.strip()))
    return results


def config_label(name, page_size_kb, include_page_size=False):
    if include_page_size:
        return f"{name}_{page_size_kb}kb"
    return name


def parse_diskann_stats(stderr_text):
    stats = {}
    def grab(pattern, key, conv=float):
        m = re.search(pattern, stderr_text)
        if m:
            stats[key] = conv(m.group(1))

    # Insert breakdown
    grab(r'insert statement total:\s*([\d.]+)\s+ms', 'insert_stmt_total_ms')
    grab(r'VDBE work:\s*([\d.]+)\s+ms',              'insert_vdbe_work_ms')
    grab(r'insert VDBE other:\s*([\d.]+)\s+ms',      'insert_vdbe_work_ms')
    grab(r'statement finish:\s*([\d.]+)\s+ms',        'insert_stmt_finish_ms')
    grab(r'shell db close:\s*([\d.]+)\s+ms',          'shell_close_ms')
    grab(r'shell statements:\s*(\d+)',                 'shell_stmt_count', int)
    grab(r'shell prepare:\s*([\d.]+)\s+ms',           'shell_prepare_ms')
    grab(r'shell step:\s*([\d.]+)\s+ms',              'shell_step_ms')
    grab(r'shell finalize:\s*([\d.]+)\s+ms',          'shell_finalize_ms')
    grab(r'shell other:\s*([\d.]+)\s+ms',             'shell_other_ms')
    grab(r'step top-level calls:\s*(\d+)',             'step_top_count', int)
    grab(r'step api total:\s*([\d.]+)\s+ms',          'step_api_ms')
    grab(r'step core total:\s*([\d.]+)\s+ms',         'step_core_ms')
    grab(r'step mutex enter:\s*([\d.]+)\s+ms',        'step_mutex_enter_ms')
    grab(r'step mutex leave:\s*([\d.]+)\s+ms',        'step_mutex_leave_ms')
    grab(r'step auto reset:\s*([\d.]+)\s+ms',         'step_auto_reset_ms')
    grab(r'step ready setup:\s*([\d.]+)\s+ms',        'step_ready_ms')
    grab(r'step vdbe list:\s*([\d.]+)\s+ms',          'step_vdbe_list_ms')
    grab(r'step vdbe exec:\s*([\d.]+)\s+ms',          'step_vdbe_exec_ms')
    grab(r'step profile:\s*([\d.]+)\s+ms',            'step_profile_ms')
    grab(r'step wal callback:\s*([\d.]+)\s+ms',       'step_wal_ms')
    grab(r'step transfer error:\s*([\d.]+)\s+ms',     'step_transfer_ms')
    grab(r'step api exit:\s*([\d.]+)\s+ms',           'step_api_exit_ms')
    grab(r'step reprepare:\s*([\d.]+)\s+ms',          'step_reprepare_ms')
    grab(r'step reset:\s*([\d.]+)\s+ms',              'step_reset_ms')
    grab(r'step wrapper other:\s*([\d.]+)\s+ms',      'step_wrapper_other_ms')
    grab(r'non-index insert remainder:\s*([\d.]+)\s+ms', 'non_index_insert_ms')
    grab(r'base table insert:\s*([\d.]+)\s+ms',       'non_index_insert_ms')
    grab(r'table insert:\s*([\d.]+)\s+ms',            'non_index_insert_ms')
    grab(r'shadow (?:row|table) insert:\s*([\d.]+)\s+ms', 'shadow_insert_ms')
    grab(r'vector index build:\s*([\d.]+)\s+ms',      'build_total_ms')
    grab(r'index build:\s*([\d.]+)\s+ms',             'build_total_ms')
    grab(r'graph build/update:\s*([\d.]+)\s+ms',      'graph_build_ms')
    grab(r'build graph traversal:\s*([\d.]+)\s+ms',   'build_traversal_ms')
    grab(r'build edge update:\s*([\d.]+)\s+ms',       'build_edge_update_ms')
    grab(r'build KV read path:\s*([\d.]+)\s+ms',      'build_read_ms')
    grab(r'build blob read path:\s*([\d.]+)\s+ms',    'build_read_ms')
    grab(r'build read I/O:\s*([\d.]+)\s+ms',          'build_read_ms')
    grab(r'build KV write path:\s*([\d.]+)\s+ms',     'build_write_ms')
    grab(r'build blob write path:\s*([\d.]+)\s+ms',   'build_write_ms')
    grab(r'build write I/O:\s*([\d.]+)\s+ms',         'build_write_ms')
    grab(r'build distance:\s*([\d.]+)\s+ms',          'build_dist_ms')
    grab(r'LSM auto-compaction during insert:\s*([\d.]+)\s+ms', 'insert_lsm_compact_ms')
    grab(r'LSM page compress:\s*([\d.]+)\s+ms',       'lsm_page_compress_ms')
    grab(r'LSM page decompress:\s*([\d.]+)\s+ms',     'lsm_page_decompress_ms')
    # Query stats
    grab(r'total:\s*([\d.]+)\s+ms',                   'search_total_ms')
    grab(r'context init:\s*([\d.]+)\s+ms',            'ctx_init_ms')
    grab(r'graph traversal:\s*([\d.]+)\s+ms',         'graph_ms')
    grab(r'query KV read path:\s*([\d.]+)\s+ms',      'query_read_ms')
    grab(r'query blob read path:\s*([\d.]+)\s+ms',    'query_read_ms')
    grab(r'query read I/O:\s*([\d.]+)\s+ms',          'query_read_ms')
    grab(r'blob open:\s*([\d.]+)\s+ms',               'blob_open_ms')
    grab(r'blob reopen:\s*([\d.]+)\s+ms',             'blob_reopen_ms')
    grab(r'blob read:\s*([\d.]+)\s+ms',               'blob_read_call_ms')
    m_cache = re.search(r'blob read:\s*[\d.]+\s+ms\s+\(cache hit/miss\s+(\d+)/(\d+)\)', stderr_text)
    if m_cache:
        stats['blob_cache_hits']   = int(m_cache.group(1))
        stats['blob_cache_misses'] = int(m_cache.group(2))
    grab(r'KV cursor open:\s*([\d.]+)\s+ms',          'kv_cursor_open_ms')
    grab(r'KV seek:\s*([\d.]+)\s+ms',                 'kv_seek_ms')
    grab(r'KV data:\s*([\d.]+)\s+ms',                 'kv_data_ms')
    grab(r'KV decode:\s*([\d.]+)\s+ms',               'kv_decode_ms')
    grab(r'KV memcpy:\s*([\d.]+)\s+ms',               'kv_memcpy_ms')
    grab(r'query distance:\s*([\d.]+)\s+ms',          'query_dist_ms')
    grab(r'result collect:\s*([\d.]+)\s+ms',          'result_ms')
    grab(r'context deinit:\s*([\d.]+)\s+ms',          'ctx_deinit_ms')
    grab(r'vector search total:\s*([\d.]+)\s+ms',     'vector_search_total_ms')
    grab(r'vector parse:\s*([\d.]+)\s+ms',            'vector_parse_ms')
    grab(r'index lookup/open:\s*([\d.]+)\s+ms',       'index_lookup_ms')
    grab(r'diskAnn call:\s*([\d.]+)\s+ms',            'diskann_call_ms')
    grab(r'vector cleanup:\s*([\d.]+)\s+ms',          'vector_cleanup_ms')
    grab(r'([\d.]+)\s+q/s',                           'qps')
    return stats


def extract_c_stat_blocks(stderr_text):
    blocks, cur, in_block = [], [], False
    for line in stderr_text.splitlines():
        s = line.rstrip()
        if s.startswith("=== diskAnn ") and s.endswith("==="):
            if cur:
                blocks.append("\n".join(cur))
                cur = []
            in_block = True
            cur.append(s)
            continue
        if in_block:
            cur.append(s)
            if s == "================================================":
                blocks.append("\n".join(cur))
                cur = []
                in_block = False
    if cur:
        blocks.append("\n".join(cur))
    return blocks


def format_io_summary(io_stats):
    if not io_stats.get("available"):
        return (f"disk={io_stats.get('device', DISK_DEVICE)} unavailable "
                f"({io_stats.get('error', 'unknown error')})")
    return (
        f"disk={io_stats['device']} "
        f"RBW={io_stats['avg_read_mbps']:.1f}MB/s "
        f"WBW={io_stats['avg_write_mbps']:.1f}MB/s "
        f"RIOPS={io_stats['avg_read_iops']:.0f} "
        f"WIOPS={io_stats['avg_write_iops']:.0f} "
        f"Latency={io_stats['avg_latency_ms']:.2f}ms "
        f"Util={io_stats['avg_disk_util']:.1f}%"
    )


# ─────────────────────────────────────────────────────────────────────────────
# run_one_config
# ─────────────────────────────────────────────────────────────────────────────

def run_one_config(label, shell, compact_bin, insert_sql_path, query_sql_path,
                   gt_results, k, device_db_dir, serial=None,
                   is_sqlite3=False, use_compaction=True,
                   do_drop_cache=False,
                   page_size_kb=None, lsm_compression="none", disk_device=DISK_DEVICE,
                   lsm_autoflush_mb=None, lsm_automerge=None,
                   device_tmp_dir="/data/local/tmp",
                   shell_timeout=20000, search_only=False):
    db_path   = f"{device_db_dir}/bench_{label}.db"
    db_target = build_db_target(
        db_path, is_sqlite3=is_sqlite3, page_size_kb=page_size_kb,
        lsm_compression=lsm_compression, use_compaction=use_compaction,
    )
    if not search_only:
        device_cleanup_db(db_path, serial=serial, is_sqlite3=is_sqlite3)
    else:
        r = adb_shell(f"test -f {db_path} && echo OK", serial=serial)
        if "OK" not in r.stdout:
            raise FileNotFoundError(f"search-only DB not found on device: {db_path}")

    env_vars = {"DISKANN_IO_TIMING": "1"}
    result = {"label": label}
    need_compact = not search_only and not is_sqlite3 and use_compaction and compact_bin
    n_phases = 2 if search_only else (4 if need_compact else 3)

    print(f"\n{'='*60}")
    print(f"  Config: {label}")
    print(f"  Shell:  {shell}")
    if not is_sqlite3 and page_size_kb is not None:
        print(f"  DB:     {db_target}")
    if need_compact:
        print(f"  Compact:{compact_bin}")
    print(f"{'='*60}")

    result["insert_size_mb"]    = 0.0
    result["insert_time_s"]     = 0.0
    result["insert_time_stats"] = {}
    result["ins_stats"]         = {}

    if search_only:
        size_before = device_file_size_mb(db_path, serial=serial)
        print(f"  Using existing DB: {db_path} ({size_before:.1f} MB)")
    else:
        print(f"  [1/{n_phases}] Schema + Insert...")
        insert_sql = read_sql(insert_sql_path)
        prepared_sql = prepare_insert_sql(
            insert_sql, page_size_kb, is_sqlite3=is_sqlite3,
            use_compaction=use_compaction, lsm_autoflush_mb=lsm_autoflush_mb,
            lsm_automerge=lsm_automerge,
        )
        drop_caches(serial=serial, enabled=do_drop_cache)
        insert_mon = DiskStatsMonitor(disk_device, serial=serial).start()
        t0 = time.time()
        ins_out, ins_err, ins_time = run_shell(
            shell, db_target, prepared_sql,
            serial=serial, env_vars=env_vars,
            device_tmp_dir=device_tmp_dir, timeout=shell_timeout
        )
        t_insert = time.time() - t0
        result["insert_disk_io"] = insert_mon.stop()

        err_lines = [l for l in ins_err.splitlines() if l.startswith("Error:")]
        if err_lines:
            print(f"        !! {len(err_lines)} SQL errors during schema/insert:")
            for l in err_lines[:5]:
                print(f"           {l}")
            if len(err_lines) > 5:
                print(f"           ... ({len(err_lines)-5} more)")
            raise RuntimeError(f"schema/insert phase had {len(err_lines)} SQL errors")

        size_before = device_file_size_mb(db_path, serial=serial)
        result["insert_time_s"]     = round(t_insert, 2)
        result["insert_size_mb"]    = round(size_before, 1)
        result["insert_time_stats"] = ins_time
        ins_stats = parse_diskann_stats(ins_err)
        result["ins_stats"] = ins_stats
        print(f"        {t_insert:.1f}s, {size_before:.1f} MB")
        if ins_time:
            print(f"        time: real={ins_time.get('real_s',0):.2f}s  "
                  f"user={ins_time.get('user_s',0):.2f}s  sys={ins_time.get('sys_s',0):.2f}s")
        if ins_stats.get('build_total_ms') is not None:
            print(
                f"        Stmt={ins_stats.get('insert_stmt_total_ms',0)/1000:.1f}s  "
                f"Commit={ins_stats.get('insert_stmt_finish_ms',0)/1000:.1f}s  "
                f"Checkpt={ins_stats.get('step_wal_ms',0)/1000:.1f}s  "
                f"VecBuild={ins_stats.get('build_total_ms',0)/1000:.1f}s  "
                f"Shadow={ins_stats.get('shadow_insert_ms',0)/1000:.1f}s  "
                f"GraphBuild={ins_stats.get('graph_build_ms',0)/1000:.1f}s  "
                f"BuildTrav={ins_stats.get('build_traversal_ms',0)/1000:.1f}s  "
                f"EdgeUpd={ins_stats.get('build_edge_update_ms',0)/1000:.1f}s  "
                f"ReadPath={ins_stats.get('build_read_ms',0)/1000:.1f}s  "
                f"WritePath={ins_stats.get('build_write_ms',0)/1000:.1f}s  "
                f"Dist={ins_stats.get('build_dist_ms',0)/1000:.1f}s  "
                f"LSMComp={ins_stats.get('insert_lsm_compact_ms',0)/1000:.1f}s  "
                f"PgComp={ins_stats.get('lsm_page_compress_ms',0)/1000:.1f}s  "
                f"PgDecomp={ins_stats.get('lsm_page_decompress_ms',0)/1000:.1f}s"
            )
            if ins_stats.get('step_api_ms') is not None:
                print(
                    f"        StepApi={ins_stats.get('step_api_ms',0)/1000:.1f}s  "
                    f"StepCore={ins_stats.get('step_core_ms',0)/1000:.1f}s  "
                    f"StepExec={ins_stats.get('step_vdbe_exec_ms',0)/1000:.1f}s  "
                    f"StepReady={ins_stats.get('step_ready_ms',0)/1000:.1f}s  "
                    f"StepAutoReset={ins_stats.get('step_auto_reset_ms',0)/1000:.1f}s  "
                    f"StepReset={ins_stats.get('step_reset_ms',0)/1000:.1f}s  "
                    f"StepReprep={ins_stats.get('step_reprepare_ms',0)/1000:.1f}s"
                )
                print(
                    f"        StepMutex={ins_stats.get('step_mutex_enter_ms',0)/1000:.1f}s/"
                    f"{ins_stats.get('step_mutex_leave_ms',0)/1000:.1f}s  "
                    f"StepProfile={ins_stats.get('step_profile_ms',0)/1000:.1f}s  "
                    f"StepWal={ins_stats.get('step_wal_ms',0)/1000:.1f}s  "
                    f"StepXfer={ins_stats.get('step_transfer_ms',0)/1000:.1f}s  "
                    f"StepApiExit={ins_stats.get('step_api_exit_ms',0)/1000:.1f}s  "
                    f"StepOther={ins_stats.get('step_wrapper_other_ms',0)/1000:.1f}s"
                )
        print(f"        {format_io_summary(result['insert_disk_io'])}")
        for block in extract_c_stat_blocks(ins_err):
            print(block)

    # Compact (sqlite4 only)
    if need_compact:
        print(f"  [2/{n_phases}] Compacting...")
        drop_caches(serial=serial)
        t0 = time.time()
        compact_out, compact_time_stats = run_compact(
            compact_bin, db_path, serial=serial, env_vars=env_vars,
            device_tmp_dir=device_tmp_dir, lsm_compression=lsm_compression
        )
        t_compact = time.time() - t0
        compact_real_s = compact_time_stats.get("real_s", t_compact)
        size_after = device_file_size_mb(db_path, serial=serial)
        result["compact_time_s"]      = round(compact_real_s, 2)
        result["compact_wall_time_s"] = round(t_compact, 2)
        result["compact_size_mb"]     = round(size_after, 1)
        result["compact_time_stats"]  = compact_time_stats
        print(f"        {compact_real_s:.1f}s, {size_before:.1f} -> {size_after:.1f} MB")
        if compact_time_stats:
            print(f"        time: real={compact_time_stats.get('real_s',0):.2f}s  "
                  f"user={compact_time_stats.get('user_s',0):.2f}s  "
                  f"sys={compact_time_stats.get('sys_s',0):.2f}s")
        for line in compact_out.split("\n"):
            if line.startswith("Final:"):
                result["structure"] = line.strip()
                print(f"        {line.strip()}")
    else:
        result["compact_time_s"]  = 0.0
        result["compact_size_mb"] = round(size_before, 1)

    # Query
    phase_q = 1 if search_only else (3 if need_compact else 2)
    print(f"  [{phase_q}/{n_phases}] Querying...")
    query_sql = read_sql(query_sql_path)

    device_query_sql = push_sql(query_sql, serial=serial, device_tmp_dir=device_tmp_dir)
    drop_caches(serial=serial, enabled=do_drop_cache)
    query_mon = DiskStatsMonitor(disk_device, serial=serial).start()
    t0 = time.time()
    ann_out, q_err, q_time_stats = run_shell_device(
        shell, db_target, device_query_sql,
        serial=serial, env_vars=env_vars,
        device_tmp_dir=device_tmp_dir, timeout=shell_timeout
    )
    t_query = time.time() - t0
    result["query_disk_io"] = query_mon.stop()

    q_err_lines = [l for l in q_err.splitlines() if l.startswith("Error:")]
    if q_err_lines:
        print(f"        !! {len(q_err_lines)} SQL errors during query:")
        for l in q_err_lines[:5]:
            print(f"           {l}")
        if len(q_err_lines) > 5:
            print(f"           ... ({len(q_err_lines)-5} more)")

    ann_results = parse_output_to_results(ann_out, k)
    q = len(ann_results)
    qps = q / t_query if t_query > 0 else 0
    result["query_time_s"]     = round(t_query, 2)
    result["queries"]          = q
    result["query_per_sec"]    = round(qps, 1)
    result["query_time_stats"] = q_time_stats
    q_stats = parse_diskann_stats(q_err)
    result["q_stats"] = q_stats
    print(f"        {t_query:.2f}s ({qps:.0f} q/s), {q} queries returned")
    if q_time_stats:
        print(f"        time: real={q_time_stats.get('real_s',0):.2f}s  "
              f"user={q_time_stats.get('user_s',0):.2f}s  sys={q_time_stats.get('sys_s',0):.2f}s")
    if q_stats.get('graph_ms'):
        print(
            f"        Graph={q_stats.get('graph_ms',0):.0f}ms  "
            f"ReadPath={q_stats.get('query_read_ms',0):.0f}ms  "
            f"QueryDist={q_stats.get('query_dist_ms',0):.0f}ms  "
            f"Result={q_stats.get('result_ms',0):.0f}ms  "
            f"CtxDeinit={q_stats.get('ctx_deinit_ms',0):.0f}ms"
        )
        if q_stats.get('blob_read_call_ms') or q_stats.get('kv_seek_ms'):
            print(
                f"        BlobOpen={q_stats.get('blob_open_ms',0):.0f}ms  "
                f"BlobReopen={q_stats.get('blob_reopen_ms',0):.0f}ms  "
                f"BlobRead={q_stats.get('blob_read_call_ms',0):.0f}ms  "
                f"KVSeek={q_stats.get('kv_seek_ms',0):.0f}ms  "
                f"KVData={q_stats.get('kv_data_ms',0):.0f}ms  "
                f"KVDecode={q_stats.get('kv_decode_ms',0):.0f}ms"
            )
        if q_stats.get('lsm_page_compress_ms') or q_stats.get('lsm_page_decompress_ms'):
            print(
                f"        PgComp={q_stats.get('lsm_page_compress_ms',0):.0f}ms  "
                f"PgDecomp={q_stats.get('lsm_page_decompress_ms',0):.0f}ms"
            )
    print(f"        {format_io_summary(result['query_disk_io'])}")
    for block in extract_c_stat_blocks(q_err):
        print(block)

    # Recall
    phase_r = phase_q + 1
    print(f"  [{phase_r}/{n_phases}] Computing recall@{k}...")
    n_compare = min(len(ann_results), len(gt_results))
    if n_compare == 0:
        recall = 0.0
        print(f"        WARNING: no results to compare")
    else:
        total_hits     = sum(len(a & g) for a, g in zip(ann_results[:n_compare], gt_results[:n_compare]))
        total_possible = sum(len(g) for g in gt_results[:n_compare])
        recall = total_hits / total_possible if total_possible > 0 else 0.0
        print(f"        recall@{k} = {recall:.4f} ({recall*100:.2f}%)")

    result["recall"] = round(recall, 4)
    return result


# ─────────────────────────────────────────────────────────────────────────────
# main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="LSM vector benchmark via adb (Pixel phone)")
    parser.add_argument("--dataset-dir",    type=str, default="./dataset")
    parser.add_argument("--datasets",       type=str, default="sift,glove,coco,cohere")
    parser.add_argument("--sqlite4-dir",    type=str,
                        default="/data/local/sqlite4_lsm/MoVeBench",
                        help="Device path containing sqlite4 and compact_db")
    parser.add_argument("--sqlite3-dir",    type=str,
                        default="/data/local/sqlite3_libsql/MoVeBench",
                        help="Device path containing sqlite3")
    parser.add_argument("--device-db-dir",  type=str, default="/data/local/tmp",
                        help="Device directory for database files")
    parser.add_argument("--device-tmp-dir", type=str, default="/data/local/tmp",
                        help="Device directory for temporary SQL files")
    parser.add_argument("--page-sizes",     type=str, default="4,16,32,64")
    parser.add_argument("--lsm-compression", type=str, default="none",
                        choices=["none", "zlib", "lz4"],
                        help="LSM storage page compression")
    parser.add_argument("--lsm-autoflush-mb", type=int, default=None,
                        help="Set LSM autoflush threshold in MB")
    parser.add_argument("--lsm-automerge", type=int, default=None,
                        help="Set LSM automerge segment threshold for LSMoVe configs")
    parser.add_argument("--lsm-use-compaction", type=int, default=0, choices=[0, 1],
                        help="1: run compact_db after insert, 0: skip")
    parser.add_argument("--drop-cache",     action="store_true",
                        help="Drop OS page cache on device before each timed phase (requires root)")
    parser.add_argument("--disk-device",    type=str, default=DISK_DEVICE,
                        help="Block device in /proc/diskstats (default: sda)")
    parser.add_argument("--search-only",    action="store_true",
                        help="Run search and recall only using existing bench_*.db files on device")
    parser.add_argument("--keep-db",        action="store_true",
                        help="Keep bench_*.db files on device after all runs")
    parser.add_argument("--adb-serial",     type=str, default=None,
                        help="adb device serial (from 'adb devices'); omit if only one device")
    parser.add_argument("--shell-timeout",  type=int, default=20000,
                        help="Timeout in seconds for each adb shell command (default: 20000)")
    args = parser.parse_args()

    serial         = args.adb_serial
    page_sizes_kb  = [int(x) for x in args.page_sizes.split(",")]
    dataset_names  = [x.strip() for x in args.datasets.split(",")]
    use_compaction = bool(args.lsm_use_compaction)
    include_page_size_in_label = len(page_sizes_kb) > 1

    if args.lsm_autoflush_mb is not None and args.lsm_autoflush_mb <= 0:
        print("Error: --lsm-autoflush-mb must be positive.")
        return 1
    if args.lsm_automerge is not None and args.lsm_automerge <= 1:
        print("Error: --lsm-automerge must be greater than 1.")
        return 1

    # Validate local dataset files
    datasets = []
    for name in dataset_names:
        insert_sql = os.path.join(args.dataset_dir, f"insert100k_{name}.sql")
        query_sql  = os.path.join(args.dataset_dir, f"query10k_{name}.sql")
        gt_file    = os.path.join(args.dataset_dir, f"groundtruth_{name}.txt")
        required   = [query_sql, gt_file] if args.search_only else [insert_sql, query_sql, gt_file]
        missing    = [f for f in required if not os.path.isfile(f)]
        if missing:
            print(f"Warning: skipping dataset '{name}', missing: {missing}")
            continue
        datasets.append((name, insert_sql, query_sql, gt_file))

    if not datasets:
        print("Error: no valid datasets found.")
        return 1

    # Check binaries on device
    configs = []
    if args.sqlite4_dir:
        shell_bin   = f"{args.sqlite4_dir}/sqlite4"
        compact_bin = f"{args.sqlite4_dir}/compact_db"
        r = adb_shell(f"test -x {shell_bin} && echo OK", serial=serial)
        if "OK" not in r.stdout:
            print(f"Warning: {shell_bin} not found on device, skipping sqlite4 configs")
        else:
            r2 = adb_shell(f"test -x {compact_bin} && echo OK", serial=serial)
            cb = compact_bin if "OK" in r2.stdout else None
            for ps_kb in page_sizes_kb:
                label = config_label("LSMoVe", ps_kb, include_page_size_in_label)
                configs.append((label, shell_bin, cb, False, ps_kb))

    if args.sqlite3_dir:
        shell_bin = f"{args.sqlite3_dir}/sqlite3"
        r = adb_shell(f"test -x {shell_bin} && echo OK", serial=serial)
        if "OK" not in r.stdout:
            print(f"Warning: {shell_bin} not found on device, skipping sqlite3 configs")
        else:
            for ps_kb in page_sizes_kb:
                label = config_label("libSQL", ps_kb, include_page_size_in_label)
                configs.append((label, shell_bin, None, True, ps_kb))

    if not configs:
        print("Error: no valid configurations found.")
        return 1

    print(f"Device:          adb serial={serial or 'default'}")
    print(f"Disk device:     {args.disk_device}")
    print(f"Datasets:        {', '.join(n for n, *_ in datasets)}")
    print(f"Configs:         {', '.join(c[0] for c in configs)}")
    print(f"LSM compression: {args.lsm_compression}")
    print(f"LSM autoflush:   {'default' if args.lsm_autoflush_mb is None else str(args.lsm_autoflush_mb) + ' MB'}")
    print(f"LSM automerge:   {'default' if args.lsm_automerge is None else args.lsm_automerge}")
    print(f"Compaction:      {'ON (use compact_db)' if use_compaction else 'OFF'}")
    print(f"DB dir:          {args.device_db_dir} (on device)")
    print(f"Total runs:      {len(datasets) * len(configs)}")

    all_results = {}
    for ds_name, insert_sql, query_sql, gt_file in datasets:
        print(f"\n{'#'*70}")
        print(f"  DATASET: {ds_name}")
        print(f"{'#'*70}")

        gt_results = load_groundtruth(gt_file)
        print(f"  Loaded {len(gt_results)} groundtruth queries")

        ds_results = []
        for label, shell, compact_bin, is_s3, ps_kb in configs:
            run_label = f"{ds_name}_{label}"
            result = run_one_config(
                run_label, shell, compact_bin, insert_sql, query_sql,
                gt_results, TOP_K, args.device_db_dir,
                serial=serial, is_sqlite3=is_s3,
                use_compaction=use_compaction, do_drop_cache=args.drop_cache,
                page_size_kb=ps_kb,
                lsm_compression=args.lsm_compression,
                lsm_autoflush_mb=args.lsm_autoflush_mb,
                lsm_automerge=args.lsm_automerge,
                disk_device=args.disk_device,
                device_tmp_dir=args.device_tmp_dir,
                shell_timeout=args.shell_timeout,
                search_only=args.search_only,
            )
            ds_results.append(result)

            db_path = f"{args.device_db_dir}/bench_{run_label}.db"
            if args.keep_db:
                print(f"  Kept DB {db_path} on device")
            else:
                device_cleanup_db(db_path, serial=serial, is_sqlite3=is_s3)
                print(f"  Cleaned up {db_path} on device")

        all_results[ds_name] = ds_results

    # Summary
    show_compact = use_compaction
    for ds_name, ds_results in all_results.items():
        ins_hdr = (
            f"{'Overall':>8} {'Stmt':>8} {'Commit':>8} {'Checkpt':>8} "
            f"{'VecBuild':>8} {'Shadow':>8} {'Trav':>8} {'EdgeUpd':>8} "
            f"{'Dist':>8} {'LSMComp':>8} {'PgComp':>8} {'PgDecomp':>8}"
        )
        ins_sub = (
            f"{'(s)':>8} {'(s)':>8} {'(s)':>8} {'(s)':>8} "
            f"{'(s)':>8} {'(s)':>8} {'(s)':>8} {'(s)':>8} "
            f"{'(s)':>8} {'(s)':>8} {'(s)':>8} {'(s)':>8}"
        )
        if show_compact:
            ins_hdr += f" {'Compact':>8}"
            ins_sub += f" {'(s)':>8}"
        q_hdr = (
            f"{'Overall':>8} {'Graph':>8} {'ReadPath':>8} {'Dist':>8} "
            f"{'Result':>8} {'PgComp':>8} {'PgDecomp':>8} {'Q/s':>8} {'Recall':>8}"
        )
        q_sub = (
            f"{'(s)':>8} {'(ms)':>8} {'(ms)':>8} {'(ms)':>8} {'(ms)':>8} "
            f"{'(ms)':>8} {'(ms)':>8} {'':>8} {'@k':>8}"
        )
        hdr = f"{'Config':>16} |{ins_hdr} |{q_hdr} | {'Size':>8}"
        sub = f"{'':>16} |{ins_sub} |{q_sub} | {'(MB)':>8}"
        w = len(hdr)
        title = f"SUMMARY: {ds_name} (k={TOP_K})"
        print(f"\n{'='*w}")
        print(f"{title:^{w}}")
        print(f"{'='*w}")
        ins_w = len(ins_hdr) + 1
        q_w   = len(q_hdr)   + 1
        print(f"{'':>16} |{'--- Insert ---':^{ins_w}} |{'--- Query ---':^{q_w}} |")
        print(hdr)
        print(sub)
        print(f"{'-'*w}")
        for r in ds_results:
            short_label = r['label'].replace(f"{ds_name}_", "")
            ist = r.get('ins_stats', {})
            qst = r.get('q_stats',  {})
            ins_vals = (
                f"{r['insert_time_s']:>8.1f} "
                f"{ist.get('insert_stmt_total_ms',0)/1000:>8.1f} "
                f"{ist.get('insert_stmt_finish_ms',0)/1000:>8.1f} "
                f"{ist.get('step_wal_ms',0)/1000:>8.1f} "
                f"{ist.get('build_total_ms',0)/1000:>8.1f} "
                f"{ist.get('shadow_insert_ms',0)/1000:>8.1f} "
                f"{ist.get('build_traversal_ms',0)/1000:>8.1f} "
                f"{ist.get('build_edge_update_ms',0)/1000:>8.1f} "
                f"{ist.get('build_dist_ms',0)/1000:>8.1f} "
                f"{ist.get('insert_lsm_compact_ms',0)/1000:>8.1f} "
                f"{ist.get('lsm_page_compress_ms',0)/1000:>8.1f} "
                f"{ist.get('lsm_page_decompress_ms',0)/1000:>8.1f}"
            )
            if show_compact:
                cs = (f"{r['compact_time_s']:>8.1f}" if r['compact_time_s'] > 0
                      else f"{'---':>8}")
                ins_vals += f" {cs}"
            q_vals = (
                f"{r['query_time_s']:>8.1f} "
                f"{qst.get('graph_ms',0):>8.1f} "
                f"{qst.get('query_read_ms',0):>8.1f} "
                f"{qst.get('query_dist_ms',0):>8.1f} "
                f"{qst.get('result_ms',0):>8.1f} "
                f"{qst.get('lsm_page_compress_ms',0):>8.1f} "
                f"{qst.get('lsm_page_decompress_ms',0):>8.1f} "
                f"{r['query_per_sec']:>8.0f} {r['recall']:>8.4f}"
            )
            print(f"{short_label:>16} |{ins_vals} |{q_vals} | {r['compact_size_mb']:>8.1f}")
        print(f"{'='*w}")


if __name__ == "__main__":
    main()
