#!/usr/bin/env python3
"""65s synthetic 720p30 integration and measured encoder process cost."""
import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import time


def usage(pid):
    try:
        stat = Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1].split()
        cpu = (int(stat[11]) + int(stat[12])) / os.sysconf("SC_CLK_TCK")
        rss = int(stat[21]) * os.sysconf("SC_PAGE_SIZE")
        return cpu, rss
    except (FileNotFoundError, ProcessLookupError):
        return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    process = subprocess.Popen([str(args.binary.resolve()), "--sustained"], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, bufsize=1)
    phase = "streaming"
    samples = {"streaming": [], "simultaneous": []}
    baselines = {}
    started = time.monotonic()
    complete = None
    try:
        while process.poll() is None:
            ready, _, _ = select.select([process.stdout], [], [], 0.2)
            if ready:
                line = process.stdout.readline()
                if line.startswith("{"):
                    message = json.loads(line)
                    if message["phase"] == "complete":
                        complete = message
                    else:
                        phase = message["phase"]
                        print(line.strip(), flush=True)
            pids = [process.pid]
            try:
                children = Path(f"/proc/{process.pid}/task/{process.pid}/children").read_text().split()
                for child in children:
                    command = Path(f"/proc/{child}/cmdline").read_bytes()
                    if b"--internal-stream-worker" in command:
                        pids.append(int(child))
            except (FileNotFoundError, ProcessLookupError):
                pass
            cpu_delta, rss = 0.0, 0
            now = time.monotonic()
            for pid in pids:
                measured = usage(pid)
                if measured:
                    cpu, memory = measured
                    old = baselines.get(pid)
                    if old:
                        cpu_delta += max(0, cpu - old[0])
                    baselines[pid] = (cpu, now)
                    rss += memory
            if phase in samples:
                samples[phase].append((now, cpu_delta, rss))
            assert now - started < 85, "sustained fixture exceeded bounded deadline"
        remaining, stderr = process.communicate(timeout=5)
        for line in remaining.splitlines():
            if not line.startswith("{"):
                continue
            message = json.loads(line)
            if message["phase"] == "complete":
                complete = message
        assert process.returncode == 0, stderr
        assert complete and complete["duration"] >= 64, stderr
        result = {"media": complete, "encoder_cost": {}}
        for name, data in samples.items():
            duration = data[-1][0] - data[0][0]
            result["encoder_cost"][name] = {
                "cpu_one_core_percent": round(sum(sample[1] for sample in data) / duration * 100, 2),
                "peak_parent_worker_rss_mib": round(max(sample[2] for sample in data) / 1048576, 2),
                "measured_seconds": round(duration, 2),
            }
        print(json.dumps(result, sort_keys=True), flush=True)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


if __name__ == "__main__":
    main()
