import subprocess
import argparse
import os
import re
import select
import time
import threading
from datetime import datetime

TIME_MARKER = "__TIME__"
DISK_DEVICE = "mmcblk0p1"
TOP_K = 10


def _diskstats_devices():
    devices = set()
    try:
        with open("/proc/diskstats") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 3:
                    devices.add(parts[2])
    except OSError:
        pass
    return devices


def _existing_path_for_mount(path):
    path = os.path.abspath(path)
    while not os.path.exists(path):
        parent = os.path.dirname(path)
        if parent == path:
            return "."
        path = parent
    return path


def detect_disk_device(path, fallback=DISK_DEVICE):
    devices = _diskstats_devices()
    if not devices:
        return fallback

    target = _existing_path_for_mount(path)
    try:
        proc = subprocess.run(
            ["findmnt", "-no", "SOURCE", "--target", target],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return fallback

    source = proc.stdout.strip().splitlines()[0] if proc.stdout.strip() else ""
    candidates = []
    if source.startswith("/dev/"):
        candidates.append(os.path.basename(source))
        candidates.append(os.path.basename(os.path.realpath(source)))

        try:
            proc = subprocess.run(
                ["lsblk", "-no", "PKNAME", source],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                check=True,
            )
            candidates.extend(x.strip() for x in proc.stdout.splitlines() if x.strip())
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass

    for device in candidates:
        if device in devices:
            return device
    return fallback


class DiskStatsMonitor:
    def __init__(self, device, interval_s=1.0, log_path=None):
        self.device = device
        self.interval_s = interval_s
        self.log_path = log_path
        self._stop_event = threading.Event()
        self._thread = None
        self._samples = []
        self._error = None
        self._prev = None
        self._prev_ts = None
        self._log_fp = None

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
            self._prev = cur
            self._prev_ts = ts
            return
        d_read_reqs = cur["read_reqs"] - self._prev["read_reqs"]
        d_read_sectors = cur["read_sectors"] - self._prev["read_sectors"]
        d_read_ms = cur["read_ms"] - self._prev["read_ms"]
        d_write_reqs = cur["write_reqs"] - self._prev["write_reqs"]
        d_write_sectors = cur["write_sectors"] - self._prev["write_sectors"]
        d_write_ms = cur["write_ms"] - self._prev["write_ms"]
        d_busy_ms = cur["busy_ms"] - self._prev["busy_ms"]
        if min(d_read_reqs, d_read_sectors, d_read_ms, d_write_reqs,
               d_write_sectors, d_write_ms, d_busy_ms) < 0:
            self._prev = cur
            self._prev_ts = ts
            return
        self._samples.append({
            "wall_time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "elapsed_s": elapsed,
            "read_reqs": d_read_reqs,
            "read_bytes": d_read_sectors * 512,
            "read_ms": d_read_ms,
            "write_reqs": d_write_reqs,
            "write_bytes": d_write_sectors * 512,
            "write_ms": d_write_ms,
            "busy_ms": d_busy_ms,
        })
        self._prev = cur
        self._prev_ts = ts

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
                elapsed = s["elapsed_s"]
                read_mbps = s["read_bytes"] / (1024 * 1024) / elapsed if elapsed > 0 else 0.0
                write_mbps = s["write_bytes"] / (1024 * 1024) / elapsed if elapsed > 0 else 0.0
                read_iops = s["read_reqs"] / elapsed if elapsed > 0 else 0.0
                write_iops = s["write_reqs"] / elapsed if elapsed > 0 else 0.0
                read_latency = s["read_ms"] / s["read_reqs"] if s["read_reqs"] > 0 else 0.0
                write_latency = s["write_ms"] / s["write_reqs"] if s["write_reqs"] > 0 else 0.0
                total_reqs = s["read_reqs"] + s["write_reqs"]
                latency_ms = (s["read_ms"] + s["write_ms"]) / total_reqs if total_reqs > 0 else 0.0
                disk_util = s["busy_ms"] / (elapsed * 10.0) if elapsed > 0 else 0.0
                fp.write(
                    f"{s['wall_time']},{elapsed:.3f},{read_mbps:.3f},{write_mbps:.3f},"
                    f"{read_iops:.3f},{write_iops:.3f},{read_latency:.3f},{write_latency:.3f},"
                    f"{latency_ms:.3f},{disk_util:.3f}\n"
                )

    def _read_stats(self):
        try:
            with open("/proc/diskstats") as f:
                for line in f:
                    parts = line.split()
                    if len(parts) < 14 or parts[2] != self.device:
                        continue
                    return {
                        "read_reqs": int(parts[3]),
                        "read_sectors": int(parts[5]),
                        "read_ms": int(parts[6]),
                        "write_reqs": int(parts[7]),
                        "write_sectors": int(parts[9]),
                        "write_ms": int(parts[10]),
                        "busy_ms": int(parts[12]),
                    }
        except OSError as e:
            self._error = str(e)
        return None

    def summary(self):
        if not self._samples:
            return {
                "device": self.device,
                "available": False,
                "error": self._error or "no diskstats samples captured",
            }
        total_elapsed = sum(s["elapsed_s"] for s in self._samples)
        total_read_bytes = sum(s["read_bytes"] for s in self._samples)
        total_write_bytes = sum(s["write_bytes"] for s in self._samples)
        total_read_reqs = sum(s["read_reqs"] for s in self._samples)
        total_write_reqs = sum(s["write_reqs"] for s in self._samples)
        total_read_ms = sum(s["read_ms"] for s in self._samples)
        total_write_ms = sum(s["write_ms"] for s in self._samples)
        total_busy_ms = sum(s["busy_ms"] for s in self._samples)
        mb = 1024 * 1024
        peak_read_mbps = max((s["read_bytes"] / mb / s["elapsed_s"]) for s in self._samples)
        peak_write_mbps = max((s["write_bytes"] / mb / s["elapsed_s"]) for s in self._samples)
        return {
            "device": self.device,
            "available": True,
            "log_path": self.log_path,
            "samples": len(self._samples),
            "elapsed_s": total_elapsed,
            "avg_read_mbps": total_read_bytes / mb / total_elapsed if total_elapsed > 0 else 0.0,
            "avg_write_mbps": total_write_bytes / mb / total_elapsed if total_elapsed > 0 else 0.0,
            "peak_read_mbps": peak_read_mbps,
            "peak_write_mbps": peak_write_mbps,
            "avg_read_iops": total_read_reqs / total_elapsed if total_elapsed > 0 else 0.0,
            "avg_write_iops": total_write_reqs / total_elapsed if total_elapsed > 0 else 0.0,
            "avg_read_latency_ms": total_read_ms / total_read_reqs if total_read_reqs > 0 else 0.0,
            "avg_write_latency_ms": total_write_ms / total_write_reqs if total_write_reqs > 0 else 0.0,
            "avg_latency_ms": (
                (total_read_ms + total_write_ms) / (total_read_reqs + total_write_reqs)
                if (total_read_reqs + total_write_reqs) > 0 else 0.0
            ),
            "avg_disk_util": total_busy_ms / (total_elapsed * 10.0) if total_elapsed > 0 else 0.0,
        }


def parse_time_stats(stderr_text):
    stats = {}
    kept = []
    for line in stderr_text.splitlines():
        if line.startswith(TIME_MARKER):
            m = re.search(r"real=([\d.]+)\s+user=([\d.]+)\s+sys=([\d.]+)", line)
            if m:
                stats["real_s"] = float(m.group(1))
                stats["user_s"] = float(m.group(2))
                stats["sys_s"] = float(m.group(3))
        else:
            kept.append(line)
    cleaned = "\n".join(kept)
    if stderr_text.endswith("\n"):
        cleaned += "\n"
    return cleaned, stats

def run_shell_one(shell, db, sql_file, env=None):
    """Run a SQL file through the sqlite4 shell."""
    with open(sql_file, 'r') as f:
        proc = subprocess.run(
            [shell, db],
            stdin=f,
            capture_output=True,
            text=True,
            env=env,
        )
    if proc.returncode != 0:
        stdout_tail = "\n".join(proc.stdout.strip().split("\n")[-5:]) if proc.stdout else ""
        print(f"STDERR: {proc.stderr}")
        print(f"STDOUT (last 5 lines): {stdout_tail}")
        raise RuntimeError(f"sqlite4 shell failed (rc={proc.returncode}) on {sql_file}")
    return proc.stdout


def run_shell(shell, db, sql_input, env=None):
    """Run SQL through shell in one session. Returns (stdout, stderr, time_stats)."""
    if not sql_input.rstrip().endswith(".quit"):
        sql_input = sql_input + "\n.quit\n"
    cmd = [shell, db]
    use_time = os.path.exists("/usr/bin/time")
    if use_time:
        cmd = ["/usr/bin/time", "-f", f"{TIME_MARKER} real=%e user=%U sys=%S"] + cmd
    proc = subprocess.run(
        cmd,
        input=sql_input,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=20000,
        env=env,
    )
    stderr_text, time_stats = parse_time_stats(proc.stderr)
    if proc.returncode != 0 and proc.returncode != 1:
        err_lines = [l for l in stderr_text.splitlines() if not l.startswith("[LSM]")]
        err_msg = "\n".join(err_lines[-10:]) if err_lines else stderr_text[-500:]
        raise RuntimeError(f"shell error (rc={proc.returncode}): {err_msg}")
    return proc.stdout, stderr_text, time_stats


def run_compact(compact_bin, db, env=None, lsm_compression="none"):
    timeout_s = 20000
    cmd = [compact_bin, db]
    if lsm_compression and lsm_compression != "none":
        cmd.append(lsm_compression)
    if os.path.exists("/usr/bin/time"):
        cmd = ["/usr/bin/time", "-f", f"{TIME_MARKER} real=%e user=%U sys=%S"] + cmd

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )

    stderr_lines = []
    deadline = time.monotonic() + timeout_s
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            proc.kill()
            proc.wait()
            raise subprocess.TimeoutExpired(cmd, timeout_s)

        ready, _, _ = select.select([proc.stderr], [], [], min(0.5, remaining))
        if ready:
            line = proc.stderr.readline()
            if line:
                stderr_lines.append(line)
                if not line.startswith(TIME_MARKER):
                    print(f"        {line.rstrip()}")
        elif proc.poll() is not None:
            rest = proc.stderr.read()
            if rest:
                stderr_lines.append(rest)
                for line in rest.splitlines():
                    if not line.startswith(TIME_MARKER):
                        print(f"        {line.rstrip()}")
            break

        if proc.poll() is not None:
            rest = proc.stderr.read()
            if rest:
                stderr_lines.append(rest)
                for line in rest.splitlines():
                    if not line.startswith(TIME_MARKER):
                        print(f"        {line.rstrip()}")
            break

    stderr_text = "".join(stderr_lines)
    stderr_text, time_stats = parse_time_stats(stderr_text)
    if proc.returncode != 0:
        err_lines = [l for l in stderr_text.splitlines() if l.strip()]
        err_msg = "\n".join(err_lines[-10:]) if err_lines else stderr_text[-500:]
        raise RuntimeError(f"compact_db failed (rc={proc.returncode}): {err_msg}")
    return stderr_text, time_stats


def build_compact_binary(sqlite4_dir):
    compact_bin = os.path.join(sqlite4_dir, "compact_db")
    compact_src = os.path.join(sqlite4_dir, "compact_db.c")
    libsqlite4 = os.path.join(sqlite4_dir, "libsqlite4.a")

    if os.path.isfile(compact_bin) and os.access(compact_bin, os.X_OK):
        return compact_bin
    if not os.path.isfile(compact_src):
        print(f"Warning: compact_db.c missing, cannot build {compact_bin}")
        return None
    if not os.path.isfile(libsqlite4):
        print(f"Warning: libsqlite4.a missing, cannot build {compact_bin}")
        return None

    cmd = [
        "gcc", "-O2", compact_src,
        "-I" + sqlite4_dir,
        "-I" + os.path.join(sqlite4_dir, "src"),
        "-L" + sqlite4_dir,
        "-lsqlite4", "-lpthread", "-lm", "-lz", "-llz4",
        "-o", compact_bin,
    ]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        err = "\n".join(proc.stderr.strip().splitlines()[-8:])
        print(f"Warning: failed to build compact_db:\n{err}")
        return None
    return compact_bin


def drop_caches(enabled=True):
    """Drop OS page cache. Requires sudo."""
    if not enabled:
        return
    try:
        subprocess.run(["sudo", "sh", "-c", "sync; echo 3 > /proc/sys/vm/drop_caches"],
                       check=True, timeout=10)
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired, PermissionError, OSError) as e:
        print(f"  WARNING: drop_caches failed: {e}")




def read_sql(sql_path):
    with open(sql_path) as f:
        return f.read()


def split_schema_inserts(sql_text):
    """Split SQL text into schema (CREATE/PRAGMA) and INSERT statements."""
    schema = []
    inserts = []
    for line in sql_text.strip().split("\n"):
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.upper().startswith("INSERT"):
            inserts.append(stripped)
        else:
            schema.append(stripped)
    return schema, inserts


def build_schema_sql(schema_lines, page_size_kb):
    pragma = f"PRAGMA page_size={page_size_kb * 1024};"
    return "\n".join([pragma] + schema_lines)


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
    """Return the shell target used to open the database."""
    if is_sqlite3 or page_size_kb is None:
        return db_path
    params = [f"page_size={page_size_kb * 1024}"]
    if lsm_compression and lsm_compression != "none":
        params.append(f"lsm_compression={lsm_compression}")
    return f"file:{db_path}?{'&'.join(params)}"


def parse_output_to_results(output, k):
    """Parse shell output into list of sets of IDs, chunked by k."""
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
        results.append(set(id_lines[i:i+k]))
    return results


def load_groundtruth(path):
    """Load groundtruth file: one line per query, comma-separated IDs."""
    results = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                ids = set(int(x) for x in line.split(",") if x.strip())
                results.append(ids)
    return results


def file_size_mb(path):
    try:
        return os.path.getsize(path) / (1024 * 1024)
    except OSError:
        return 0.0


def cleanup_db(db_path, is_sqlite3=False):
    if is_sqlite3:
        suffixes = ["", "-wal", "-shm"]
    else:
        suffixes = ["", "-log", "-shm"]
    for suffix in suffixes:
        p = db_path + suffix
        if os.path.exists(p):
            os.remove(p)


def parse_diskann_stats(stderr_text):
    """Parse DiskANN insert/query stats from stderr output."""
    stats = {}
    def grab(pattern, key, conv=float):
        m = re.search(pattern, stderr_text)
        if m:
            stats[key] = conv(m.group(1))

    # Insert breakdown
    grab(r'insert statement total:\s*([\d.]+)\s+ms', 'insert_stmt_total_ms')
    grab(r'VDBE work:\s*([\d.]+)\s+ms', 'insert_vdbe_work_ms')
    grab(r'insert VDBE other:\s*([\d.]+)\s+ms', 'insert_vdbe_work_ms')
    grab(r'statement finish:\s*([\d.]+)\s+ms', 'insert_stmt_finish_ms')
    grab(r'KV commit total:\s*([\d.]+)\s+ms', 'insert_kv_commit_total_ms')
    grab(r'KV commit LSM work:\s*([\d.]+)\s+ms', 'insert_kv_commit_lsm_ms')
    grab(r'KV commit no LSM:\s*([\d.]+)\s+ms', 'insert_kv_commit_no_lsm_ms')
    grab(r'Btree commit total:\s*([\d.]+)\s+ms', 'insert_btree_commit_total_ms')
    grab(r'shell db close:\s*([\d.]+)\s+ms', 'shell_close_ms')
    grab(r'shell statements:\s*(\d+)', 'shell_stmt_count', int)
    grab(r'shell prepare:\s*([\d.]+)\s+ms', 'shell_prepare_ms')
    grab(r'shell step:\s*([\d.]+)\s+ms', 'shell_step_ms')
    grab(r'shell finalize:\s*([\d.]+)\s+ms', 'shell_finalize_ms')
    grab(r'shell other:\s*([\d.]+)\s+ms', 'shell_other_ms')
    grab(r'step top-level calls:\s*(\d+)', 'step_top_count', int)
    grab(r'step api total:\s*([\d.]+)\s+ms', 'step_api_ms')
    grab(r'step core total:\s*([\d.]+)\s+ms', 'step_core_ms')
    grab(r'step mutex enter:\s*([\d.]+)\s+ms', 'step_mutex_enter_ms')
    grab(r'step mutex leave:\s*([\d.]+)\s+ms', 'step_mutex_leave_ms')
    grab(r'step auto reset:\s*([\d.]+)\s+ms', 'step_auto_reset_ms')
    grab(r'step ready setup:\s*([\d.]+)\s+ms', 'step_ready_ms')
    grab(r'step vdbe list:\s*([\d.]+)\s+ms', 'step_vdbe_list_ms')
    grab(r'step vdbe exec:\s*([\d.]+)\s+ms', 'step_vdbe_exec_ms')
    grab(r'step profile:\s*([\d.]+)\s+ms', 'step_profile_ms')
    grab(r'step wal callback:\s*([\d.]+)\s+ms', 'step_wal_ms')
    grab(r'step transfer error:\s*([\d.]+)\s+ms', 'step_transfer_ms')
    grab(r'step api exit:\s*([\d.]+)\s+ms', 'step_api_exit_ms')
    grab(r'step reprepare:\s*([\d.]+)\s+ms', 'step_reprepare_ms')
    grab(r'step reset:\s*([\d.]+)\s+ms', 'step_reset_ms')
    grab(r'step wrapper other:\s*([\d.]+)\s+ms', 'step_wrapper_other_ms')
    grab(r'non-index insert remainder:\s*([\d.]+)\s+ms', 'non_index_insert_ms')
    grab(r'base table insert:\s*([\d.]+)\s+ms', 'non_index_insert_ms')
    grab(r'table insert:\s*([\d.]+)\s+ms', 'non_index_insert_ms')
    grab(r'shadow (?:row|table) insert:\s*([\d.]+)\s+ms', 'shadow_insert_ms')
    grab(r'vector index build:\s*([\d.]+)\s+ms', 'build_total_ms')
    grab(r'index build:\s*([\d.]+)\s+ms', 'build_total_ms')
    grab(r'graph build/update:\s*([\d.]+)\s+ms', 'graph_build_ms')
    grab(r'build graph traversal:\s*([\d.]+)\s+ms', 'build_traversal_ms')
    grab(r'build edge update:\s*([\d.]+)\s+ms', 'build_edge_update_ms')
    grab(r'build KV read path:\s*([\d.]+)\s+ms', 'build_read_ms')
    grab(r'build blob read path:\s*([\d.]+)\s+ms', 'build_read_ms')
    grab(r'build read I/O:\s*([\d.]+)\s+ms', 'build_read_ms')
    grab(r'build KV write path:\s*([\d.]+)\s+ms', 'build_write_ms')
    grab(r'build blob write path:\s*([\d.]+)\s+ms', 'build_write_ms')
    grab(r'build write I/O:\s*([\d.]+)\s+ms', 'build_write_ms')
    grab(r'build distance:\s*([\d.]+)\s+ms', 'build_dist_ms')
    grab(r'LSM auto-compaction during insert:\s*([\d.]+)\s+ms', 'insert_lsm_compact_ms')
    grab(r'LSM page compress:\s*([\d.]+)\s+ms', 'lsm_page_compress_ms')
    grab(r'LSM page decompress:\s*([\d.]+)\s+ms', 'lsm_page_decompress_ms')
    # Query stats
    grab(r'total:\s*([\d.]+)\s+ms', 'search_total_ms')
    grab(r'context init:\s*([\d.]+)\s+ms', 'ctx_init_ms')
    grab(r'graph traversal:\s*([\d.]+)\s+ms', 'graph_ms')
    grab(r'query KV read path:\s*([\d.]+)\s+ms', 'query_read_ms')
    grab(r'query blob read path:\s*([\d.]+)\s+ms', 'query_read_ms')
    grab(r'query read I/O:\s*([\d.]+)\s+ms', 'query_read_ms')
    grab(r'blob open:\s*([\d.]+)\s+ms', 'blob_open_ms')
    grab(r'blob reopen:\s*([\d.]+)\s+ms', 'blob_reopen_ms')
    grab(r'blob read:\s*([\d.]+)\s+ms', 'blob_read_call_ms')
    m_cache = re.search(r'blob read:\s*[\d.]+\s+ms\s+\(cache hit/miss\s+(\d+)/(\d+)\)', stderr_text)
    if m_cache:
        stats['blob_cache_hits'] = int(m_cache.group(1))
        stats['blob_cache_misses'] = int(m_cache.group(2))
    grab(r'KV cursor open:\s*([\d.]+)\s+ms', 'kv_cursor_open_ms')
    grab(r'KV seek:\s*([\d.]+)\s+ms', 'kv_seek_ms')
    grab(r'KV data:\s*([\d.]+)\s+ms', 'kv_data_ms')
    grab(r'KV decode:\s*([\d.]+)\s+ms', 'kv_decode_ms')
    grab(r'KV memcpy:\s*([\d.]+)\s+ms', 'kv_memcpy_ms')
    grab(r'query distance:\s*([\d.]+)\s+ms', 'query_dist_ms')
    grab(r'result collect:\s*([\d.]+)\s+ms', 'result_ms')
    grab(r'context deinit:\s*([\d.]+)\s+ms', 'ctx_deinit_ms')
    grab(r'vector search total:\s*([\d.]+)\s+ms', 'vector_search_total_ms')
    grab(r'vector parse:\s*([\d.]+)\s+ms', 'vector_parse_ms')
    grab(r'index lookup/open:\s*([\d.]+)\s+ms', 'index_lookup_ms')
    grab(r'diskAnn call:\s*([\d.]+)\s+ms', 'diskann_call_ms')
    grab(r'vector cleanup:\s*([\d.]+)\s+ms', 'vector_cleanup_ms')
    grab(r'([\d.]+)\s+q/s', 'qps')
    return stats


def extract_c_stat_blocks(stderr_text):
    """Return formatted DiskANN stat blocks emitted by the C code."""
    lines = stderr_text.splitlines()
    blocks = []
    cur = []
    in_block = False

    for line in lines:
        stripped = line.rstrip()
        if stripped.startswith("=== diskAnn ") and stripped.endswith("==="):
            if cur:
                blocks.append("\n".join(cur))
                cur = []
            in_block = True
            cur.append(stripped)
            continue

        if in_block:
            cur.append(stripped)
            if stripped == "================================================":
                blocks.append("\n".join(cur))
                cur = []
                in_block = False

    if cur:
        blocks.append("\n".join(cur))

    return blocks


def format_io_summary(io_stats):
    if not io_stats.get("available"):
        return f"disk={io_stats.get('device', DISK_DEVICE)} unavailable ({io_stats.get('error', 'unknown error')})"
    return (
        f"disk={io_stats['device']} "
        f"RBW={io_stats['avg_read_mbps']:.1f}MB/s "
        f"WBW={io_stats['avg_write_mbps']:.1f}MB/s "
        f"RIOPS={io_stats['avg_read_iops']:.0f} "
        f"WIOPS={io_stats['avg_write_iops']:.0f} "
        f"Latency={io_stats['avg_latency_ms']:.2f}ms "
        f"Util={io_stats['avg_disk_util']:.1f}%"
    )


def run_one_config(label, shell, compact_bin, insert_sql_path, query_sql_path,
                   gt_results, k, db_dir, is_sqlite3=False, use_compaction=True,
                   do_drop_cache=False, page_size_kb=None,
                   lsm_compression="none", disk_device=DISK_DEVICE,
                   search_only=False):
    db_path = os.path.join(db_dir, f"bench_{label}.db")
    db_target = build_db_target(
        db_path,
        is_sqlite3=is_sqlite3,
        page_size_kb=page_size_kb,
        lsm_compression=lsm_compression,
        use_compaction=use_compaction,
    )
    if not search_only:
        cleanup_db(db_path, is_sqlite3)
    elif not os.path.exists(db_path):
        raise FileNotFoundError(f"search-only DB not found: {db_path}")
    child_env = os.environ.copy()
    child_env["DISKANN_IO_TIMING"] = "1"

    result = {"label": label}
    need_compact = not search_only and not is_sqlite3 and use_compaction and compact_bin
    n_phases = 2 if search_only else (4 if need_compact else 3)

    print(f"\n{'='*60}")
    print(f"  Config: {label}")
    print(f"  Shell:   {shell}")
    if not is_sqlite3 and page_size_kb is not None:
        print(f"  DB open: {db_target}")
    if need_compact:
        print(f"  Compact: {compact_bin}")
    print(f"{'='*60}")

    size_before = file_size_mb(db_path)
    result["insert_size_mb"] = round(size_before, 1)
    result["insert_time_s"] = 0.0
    result["insert_time_stats"] = {}
    result["ins_stats"] = {}

    if search_only:
        print(f"  Using existing DB: {db_path} ({size_before:.1f} MB)")
    else:
        print(f"  [1/{n_phases}] Schema + Insert...")

        # Keep schema and inserts in one shell session so LSM lock state and any
        # transaction statements in the SQL file stay in their original order.
        insert_sql = read_sql(insert_sql_path)
        drop_caches(do_drop_cache)
        insert_mon = DiskStatsMonitor(disk_device).start()
        t0 = time.time()
        ins_out, ins_err, ins_time = run_shell(shell, db_target, insert_sql, env=child_env)
        t_insert = time.time() - t0
        result["insert_disk_io"] = insert_mon.stop()

        # Check for silent SQL errors (shell continues past errors but sets gHasError)
        err_lines = [l for l in ins_err.splitlines() if l.startswith("Error:")]
        if err_lines:
            print(f"        !! {len(err_lines)} SQL errors during schema/insert:")
            for l in err_lines[:5]:
                print(f"           {l}")
            if len(err_lines) > 5:
                print(f"           ... ({len(err_lines)-5} more)")
            raise RuntimeError(f"schema/insert phase had {len(err_lines)} SQL errors")

        size_before = file_size_mb(db_path)
        result["insert_time_s"] = round(t_insert, 2)
        result["insert_size_mb"] = round(size_before, 1)
        result["insert_time_stats"] = ins_time
        ins_stats = parse_diskann_stats(ins_err)
        result["ins_stats"] = ins_stats
        print(f"        {t_insert:.1f}s, {size_before:.1f} MB")
        if ins_time:
            print(
                f"        time: real={ins_time.get('real_s', 0):.2f}s  "
                f"user={ins_time.get('user_s', 0):.2f}s  "
                f"sys={ins_time.get('sys_s', 0):.2f}s"
            )
        if ins_stats.get('build_total_ms') is not None:
            stmt_s = ins_stats.get('insert_stmt_total_ms', 0) / 1000
            commit_s = ins_stats.get(
                'insert_kv_commit_no_lsm_ms',
                ins_stats.get(
                    'insert_btree_commit_total_ms',
                    ins_stats.get('insert_stmt_finish_ms', 0)
                )
            ) / 1000
            finish_s = ins_stats.get('insert_stmt_finish_ms', 0) / 1000
            commit_total_s = ins_stats.get('insert_kv_commit_total_ms', 0) / 1000
            commit_lsm_s = ins_stats.get('insert_kv_commit_lsm_ms', 0) / 1000
            wal_s = ins_stats.get('step_wal_ms', 0) / 1000
            build_s = ins_stats.get('build_total_ms', 0) / 1000
            shadow_s = ins_stats.get('shadow_insert_ms', 0) / 1000
            graph_s = ins_stats.get('graph_build_ms', 0) / 1000
            traversal_s = ins_stats.get('build_traversal_ms', 0) / 1000
            edge_update_s = ins_stats.get('build_edge_update_ms', 0) / 1000
            dist_s = ins_stats.get('build_dist_ms', 0) / 1000
            lsm_compact_s = ins_stats.get('insert_lsm_compact_ms', 0) / 1000
            pg_comp_s = ins_stats.get('lsm_page_compress_ms', 0) / 1000
            pg_decomp_s = ins_stats.get('lsm_page_decompress_ms', 0) / 1000
            print(
                f"        Stmt={stmt_s:.1f}s  Commit={commit_s:.1f}s  "
                f"Checkpt={wal_s:.1f}s  VecBuild={build_s:.1f}s  "
                f"Shadow={shadow_s:.1f}s  GraphBuild={graph_s:.1f}s  "
                f"BuildTrav={traversal_s:.1f}s  EdgeUpd={edge_update_s:.1f}s  "
                f"Dist={dist_s:.1f}s  LSMComp={lsm_compact_s:.1f}s  PgComp={pg_comp_s:.1f}s  "
                f"PgDecomp={pg_decomp_s:.1f}s"
            )
            if ins_stats.get('insert_kv_commit_total_ms') is not None:
                print(
                    f"        Finish={finish_s:.1f}s  CommitTotal={commit_total_s:.1f}s  "
                    f"CommitLSM={commit_lsm_s:.1f}s"
                )
            elif ins_stats.get('insert_btree_commit_total_ms') is not None:
                print(f"        Finish={finish_s:.1f}s")
            if ins_stats.get('step_api_ms') is not None:
                print(
                    f"        StepApi={ins_stats.get('step_api_ms', 0)/1000:.1f}s  "
                    f"StepCore={ins_stats.get('step_core_ms', 0)/1000:.1f}s  "
                    f"StepExec={ins_stats.get('step_vdbe_exec_ms', 0)/1000:.1f}s  "
                    f"StepReady={ins_stats.get('step_ready_ms', 0)/1000:.1f}s  "
                    f"StepAutoReset={ins_stats.get('step_auto_reset_ms', 0)/1000:.1f}s  "
                    f"StepReset={ins_stats.get('step_reset_ms', 0)/1000:.1f}s  "
                    f"StepReprep={ins_stats.get('step_reprepare_ms', 0)/1000:.1f}s"
                )
                print(
                    f"        StepMutex={ins_stats.get('step_mutex_enter_ms', 0)/1000:.1f}s/"
                    f"{ins_stats.get('step_mutex_leave_ms', 0)/1000:.1f}s  "
                    f"StepProfile={ins_stats.get('step_profile_ms', 0)/1000:.1f}s  "
                    f"StepWal={ins_stats.get('step_wal_ms', 0)/1000:.1f}s  "
                    f"StepXfer={ins_stats.get('step_transfer_ms', 0)/1000:.1f}s  "
                    f"StepApiExit={ins_stats.get('step_api_exit_ms', 0)/1000:.1f}s  "
                    f"StepOther={ins_stats.get('step_wrapper_other_ms', 0)/1000:.1f}s"
                )
        print(f"        {format_io_summary(result['insert_disk_io'])}")
        for block in extract_c_stat_blocks(ins_err):
            print(block)

    # Compact (LSMoVe only)
    if need_compact:
        print(f"  [2/{n_phases}] Compacting...")
        drop_caches()
        t0 = time.time()
        compact_out, compact_time_stats = run_compact(
            compact_bin, db_path, env=child_env, lsm_compression=lsm_compression
        )
        t_compact = time.time() - t0
        compact_real_s = compact_time_stats.get("real_s", t_compact)
        size_after = file_size_mb(db_path)
        result["compact_time_s"] = round(compact_real_s, 2)
        result["compact_wall_time_s"] = round(t_compact, 2)
        result["compact_size_mb"] = round(size_after, 1)
        result["compact_time_stats"] = compact_time_stats
        print(f"        {compact_real_s:.1f}s, {size_before:.1f} -> {size_after:.1f} MB")
        if compact_time_stats:
            print(
                f"        time: real={compact_time_stats.get('real_s', 0):.2f}s  "
                f"user={compact_time_stats.get('user_s', 0):.2f}s  "
                f"sys={compact_time_stats.get('sys_s', 0):.2f}s"
            )
        for line in compact_out.split("\n"):
            if line.startswith("Final:"):
                result["structure"] = line.strip()
                print(f"        {line.strip()}")
    else:
        result["compact_time_s"] = 0.0
        result["compact_size_mb"] = round(size_before, 1)

    # Query (timed)
    phase_q = 1 if search_only else (3 if need_compact else 2)
    print(f"  [{phase_q}/{n_phases}] Querying...")
    query_sql = read_sql(query_sql_path)

    drop_caches(do_drop_cache)
    query_mon = DiskStatsMonitor(disk_device).start()
    t0 = time.time()
    ann_out, q_err, q_time_stats = run_shell(shell, db_target, query_sql, env=child_env)
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
    result["query_time_s"] = round(t_query, 2)
    result["queries"] = q
    result["query_per_sec"] = round(qps, 1)
    result["query_time_stats"] = q_time_stats
    q_stats = parse_diskann_stats(q_err)
    result["q_stats"] = q_stats
    print(f"        {t_query:.2f}s ({qps:.0f} q/s), {q} queries returned")
    if q_time_stats:
        print(
            f"        time: real={q_time_stats.get('real_s', 0):.2f}s  "
            f"user={q_time_stats.get('user_s', 0):.2f}s  "
            f"sys={q_time_stats.get('sys_s', 0):.2f}s"
        )
    if q_stats.get('graph_ms'):
        print(
            f"        SearchTotal={q_stats.get('search_total_ms', 0):.0f}ms  "
            f"CtxInit={q_stats.get('ctx_init_ms', 0):.0f}ms  "
            f"Graph={q_stats.get('graph_ms', 0):.0f}ms  "
            f"QueryDist={q_stats.get('query_dist_ms', 0):.0f}ms  "
            f"Result={q_stats.get('result_ms', 0):.0f}ms  "
            f"CtxDeinit={q_stats.get('ctx_deinit_ms', 0):.0f}ms"
        )
        if q_stats.get('blob_read_call_ms') or q_stats.get('kv_seek_ms'):
            print(
                f"        BlobOpen={q_stats.get('blob_open_ms', 0):.0f}ms  "
                f"BlobReopen={q_stats.get('blob_reopen_ms', 0):.0f}ms  "
                f"BlobRead={q_stats.get('blob_read_call_ms', 0):.0f}ms  "
                f"KVSeek={q_stats.get('kv_seek_ms', 0):.0f}ms  "
                f"KVData={q_stats.get('kv_data_ms', 0):.0f}ms  "
                f"KVDecode={q_stats.get('kv_decode_ms', 0):.0f}ms  "
                f"KVMemcpy={q_stats.get('kv_memcpy_ms', 0):.0f}ms"
            )
        if q_stats.get('vector_search_total_ms'):
            print(
                f"        VecSearch={q_stats.get('vector_search_total_ms', 0):.0f}ms  "
                f"VecParse={q_stats.get('vector_parse_ms', 0):.0f}ms  "
                f"IdxLookup={q_stats.get('index_lookup_ms', 0):.0f}ms  "
                f"DiskAnnCall={q_stats.get('diskann_call_ms', 0):.0f}ms  "
                f"VecCleanup={q_stats.get('vector_cleanup_ms', 0):.0f}ms"
            )
        if q_stats.get('lsm_page_compress_ms') or q_stats.get('lsm_page_decompress_ms'):
            print(
                f"        PgComp={q_stats.get('lsm_page_compress_ms', 0):.0f}ms  "
                f"PgDecomp={q_stats.get('lsm_page_decompress_ms', 0):.0f}ms"
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
        total_hits = 0
        total_possible = 0
        for ann, gt in zip(ann_results[:n_compare], gt_results[:n_compare]):
            total_hits += len(ann & gt)
            total_possible += len(gt)
        recall = total_hits / total_possible if total_possible > 0 else 0.0
        print(f"        recall@{k} = {recall:.4f} ({recall*100:.2f}%)")

    result["recall"] = round(recall, 4)
    return result


def main():
    parser = argparse.ArgumentParser(description="LSM vector benchmark")
    parser.add_argument("--dataset-dir", type=str, default=os.path.expanduser("./dataset"),
                        help="Directory with SQL files (default: ./dataset)")
    parser.add_argument("--datasets", type=str, default="sift,glove,coco,cohere",
                        help="Comma-separated dataset names (default: sift,glove,coco,cohere)")
    parser.add_argument("--sqlite4-dir", type=str, default="./LSMoVe",
                        help="Directory containing the LSMoVe/sqlite4 shell and optional compact_db")
    parser.add_argument("--sqlite3-dir", type=str, default="./libSQL",
                        help="Directory containing sqlite3")
    parser.add_argument("--db-dir", type=str, default=".")
    parser.add_argument("--page-sizes", type=str, default="4,16,32,64")
    parser.add_argument("--lsm-compression", type=str, default="none", choices=["none", "zlib", "lz4"],
                        help="LSM storage page compression for LSMoVe configs")
    parser.add_argument("--lsm-autoflush-mb", type=int, default=None,
                        help="Set LSM autoflush threshold in MB for LSMoVe configs")
    parser.add_argument("--lsm-automerge", type=int, default=None,
                        help="Set LSM automerge segment threshold for LSMoVe configs")
    parser.add_argument("--lsm-use-compaction", type=int, default=0, choices=[0, 1],
                        help="1: use compact_db after insert for LSMoVe configs, 0: skip compact_db")
    parser.add_argument("--drop-cache", action="store_true",
                        help="Drop OS page cache before each timed phase (requires sudo)")
    parser.add_argument("--disk-device", type=str, default="auto",
                        help="Block device name from /proc/diskstats, or 'auto' to detect from --db-dir")
    parser.add_argument("--search-only", action="store_true",
                        help="Run search and recall only using existing bench_*.db files")
    parser.add_argument("--keep-db", action="store_true",
                        help="Keep generated bench_*.db files after all runs")
    args = parser.parse_args()

    page_sizes_kb = [int(x) for x in args.page_sizes.split(",")]
    dataset_names = [x.strip() for x in args.datasets.split(",")]
    if args.lsm_autoflush_mb is not None and args.lsm_autoflush_mb <= 0:
        print("Error: --lsm-autoflush-mb must be positive.")
        return 1
    if args.lsm_automerge is not None and args.lsm_automerge <= 1:
        print("Error: --lsm-automerge must be greater than 1.")
        return 1

    # Validate datasets
    datasets = []
    for name in dataset_names:
        insert_sql = os.path.join(args.dataset_dir, f"insert100k_{name}.sql")
        query_sql = os.path.join(args.dataset_dir, f"query10k_{name}.sql")
        gt_file = os.path.join(args.dataset_dir, f"groundtruth_{name}.txt")
        required = [query_sql, gt_file] if args.search_only else [insert_sql, query_sql, gt_file]
        missing = [f for f in required if not os.path.isfile(f)]
        if missing:
            print(f"Warning: skipping dataset '{name}', missing: {missing}")
            continue
        datasets.append((name, insert_sql, query_sql, gt_file))

    if not datasets:
        print("Error: no valid datasets found.")
        return 1

    use_compaction = bool(args.lsm_use_compaction)

    # Build configs: (label, shell, compact_bin_or_None, is_sqlite3, page_size_kb)
    configs = []

    if args.sqlite4_dir:
        shell = os.path.join(args.sqlite4_dir, "sqlite4")
        if not os.path.isfile(shell):
            print("Warning: LSMoVe sqlite4 shell missing, skipping LSMoVe configs")
        else:
            compact_bin = build_compact_binary(args.sqlite4_dir) if use_compaction else None
            if use_compaction and compact_bin is None:
                print("Warning: compact_db unavailable, skipping LSMoVe configs")
            else:
                for ps_kb in page_sizes_kb:
                    configs.append((f"lsm_{ps_kb}kb", shell, compact_bin, False, ps_kb))

    if args.sqlite3_dir:
        shell = os.path.join(args.sqlite3_dir, "sqlite3")
        if not os.path.isfile(shell):
            print("Warning: sqlite3 binary missing, skipping sqlite3 configs")
        else:
            for ps_kb in page_sizes_kb:
                configs.append((f"sqlite3_{ps_kb}kb", shell, None, True, ps_kb))

    if not configs:
        print("Error: no valid configurations found.")
        return 1

    disk_device = (
        detect_disk_device(args.db_dir) if args.disk_device == "auto" else args.disk_device
    )
    print(f"Datasets:     {', '.join(n for n, _, _, _ in datasets)}")
    print(f"Configs:      {', '.join(cfg[0] for cfg in configs)}")
    print(f"LSM compression: {args.lsm_compression}")
    print(f"LSM autoflush: {'default' if args.lsm_autoflush_mb is None else str(args.lsm_autoflush_mb) + ' MB'}")
    print(f"LSM automerge: {'default' if args.lsm_automerge is None else args.lsm_automerge}")
    print(f"Compaction:   {'ON (use compact_db)' if use_compaction else 'OFF'}")
    print(f"Disk device:  /dev/{disk_device}" + (" (auto)" if args.disk_device == "auto" else ""))
    print(f"DB dir:       {args.db_dir}")
    print(f"Total runs:   {len(datasets) * len(configs)}")

    # Run all dataset x config combinations
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
            insert_sql_prepared = None
            if not args.search_only:
                insert_sql_text = read_sql(insert_sql)
                prepared_sql = prepare_insert_sql(
                    insert_sql_text,
                    ps_kb,
                    is_sqlite3=is_s3,
                    use_compaction=use_compaction,
                    lsm_autoflush_mb=args.lsm_autoflush_mb,
                    lsm_automerge=args.lsm_automerge,
                )
                insert_sql_prepared = os.path.join(args.db_dir, f".schema_{run_label}.sql")
                with open(insert_sql_prepared, "w") as f:
                    f.write(prepared_sql + "\n")
            result = run_one_config(
                run_label, shell, compact_bin, insert_sql_prepared, query_sql,
                gt_results, TOP_K, args.db_dir, is_sqlite3=is_s3,
                use_compaction=use_compaction, do_drop_cache=args.drop_cache,
                page_size_kb=ps_kb, lsm_compression=args.lsm_compression,
                disk_device=disk_device,
                search_only=args.search_only
            )
            ds_results.append(result)

            db_path = os.path.join(args.db_dir, f"bench_{run_label}.db")
            if insert_sql_prepared and os.path.exists(insert_sql_prepared):
                os.remove(insert_sql_prepared)
            if args.keep_db:
                print(f"  Kept DB {db_path}")
            else:
                cleanup_db(db_path, is_sqlite3=is_s3)
                print(f"  Cleaned up {db_path}")

        all_results[ds_name] = ds_results

    # Summary per dataset
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
            f"{'Overall':>8} {'Graph':>8} {'Dist':>8} "
            f"{'Result':>8} {'PgComp':>8} {'PgDecomp':>8} {'Q/s':>8} {'Recall':>8}"
        )
        q_sub = (
            f"{'(s)':>8} {'(ms)':>8} {'(ms)':>8} {'(ms)':>8} "
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
        q_w = len(q_hdr) + 1
        print(f"{'':>16} |{'--- Insert ---':^{ins_w}} |{'--- Query ---':^{q_w}} |")
        print(hdr)
        print(sub)
        print(f"{'-'*w}")
        for r in ds_results:
            short_label = r['label'].replace(f"{ds_name}_", "")
            ist = r.get('ins_stats', {})
            stmt_s = ist.get('insert_stmt_total_ms', 0) / 1000
            finish_s = ist.get(
                'insert_kv_commit_no_lsm_ms',
                ist.get(
                    'insert_btree_commit_total_ms',
                    ist.get('insert_stmt_finish_ms', 0)
                )
            ) / 1000
            wal_s = ist.get('step_wal_ms', 0) / 1000
            build_s = ist.get('build_total_ms', 0) / 1000
            shadow_s = ist.get('shadow_insert_ms', 0) / 1000
            traversal_s = ist.get('build_traversal_ms', 0) / 1000
            edge_update_s = ist.get('build_edge_update_ms', 0) / 1000
            dist_s = ist.get('build_dist_ms', 0) / 1000
            lsm_compact_s = ist.get('insert_lsm_compact_ms', 0) / 1000
            pg_comp_s = ist.get('lsm_page_compress_ms', 0) / 1000
            pg_decomp_s = ist.get('lsm_page_decompress_ms', 0) / 1000
            qst = r.get('q_stats', {})
            ins_vals = (f"{r['insert_time_s']:>8.1f} "
                        f"{stmt_s:>8.1f} {finish_s:>8.1f} {wal_s:>8.1f} "
                        f"{build_s:>8.1f} "
                        f"{shadow_s:>8.1f} {traversal_s:>8.1f} {edge_update_s:>8.1f} "
                        f"{dist_s:>8.1f} {lsm_compact_s:>8.1f} "
                        f"{pg_comp_s:>8.1f} {pg_decomp_s:>8.1f}")
            if show_compact:
                compact_str = f"{r['compact_time_s']:>8.1f}" if r['compact_time_s'] > 0 else f"{'---':>8}"
                ins_vals += f" {compact_str}"
            q_vals = (
                f"{r['query_time_s']:>8.1f} "
                f"{qst.get('graph_ms', 0):>8.1f} "
                f"{qst.get('query_dist_ms', 0):>8.1f} "
                f"{qst.get('result_ms', 0):>8.1f} "
                f"{qst.get('lsm_page_compress_ms', 0):>8.1f} "
                f"{qst.get('lsm_page_decompress_ms', 0):>8.1f} "
                f"{r['query_per_sec']:>8.0f} {r['recall']:>8.4f}"
            )
            print(f"{short_label:>16} |{ins_vals} |{q_vals} | {r['compact_size_mb']:>8.1f}")
        print(f"{'='*w}")


if __name__ == "__main__":
    main()
