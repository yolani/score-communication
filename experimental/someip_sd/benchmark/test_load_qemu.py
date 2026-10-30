# *******************************************************************************
# Copyright (c) 2026 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************
"""Functional check of the SOME/IP mixed load benchmark on two QNX QEMU VMs.

VM-A (10.0.3.1) runs the io-sock event module, the SD daemon and someip_load_lola, which publishes the TX event
classes of config/someip_event_workload.csv; VM-B (10.0.3.2) runs lg-someip someipd and someip_load_peer, which
publishes the RX event classes. Both directions run concurrently. QEMU timing is not representative: this test only
verifies that every event class flows end to end; loss and jitter are printed for information.

CPU time per thread group is sampled from procfs by someip_load_lola (CPU_RESULT): an idle baseline, a sweep over
CPU_SCALES and the main window. A least squares fit over these windows splits the CPU load of the event module (its
thread in io-sock) and of the rest of io-sock into a fixed part and a part per event.
"""

import json
import logging
import os
import pathlib
import sys

# Shared with the report tool for measurements on a target; a source of this test, next to this file.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from load_report import CPU_PREFIX, format_cpu, format_results, merge, parse

logger = logging.getLogger(__name__)

LOAD_DIR = "/opt/someip_load"
PEER_DIR = "/opt/someip_load_peer"
PEER_LOG = "/tmp/someip_load_peer.log"
MODULE_LOG_COMMAND = 'slog2info | grep -F "[someip_event]"'
WARMUP_S = 5
DURATION_S = 20
SETUP_TIMEOUT_S = 60
# Multiplies all rates of the workload.
SCALE = 1.0
CPU_BASELINE_S = 5
CPU_PHASE_S = 10
# Load factors of the CPU sweep, relative to SCALE; the peer follows via the TX sequence numbers.
CPU_SCALES = (0.25, 0.5)


class Side:
    """One QNX VM driven through its ITF serial console."""

    def __init__(self, console):
        self._console = console
        self._pid_files = []

    def sh(self, command, timeout=30):
        rc, out = self._console.run_sh_cmd_output(command, timeout=timeout)
        if rc != 0:
            raise RuntimeError(f"Command '{command}' failed with rc={rc}: {out}")
        return out

    def output(self, command):
        return self._console.run_sh_cmd_output(command)[1]

    def start(self, command):
        # Subshell, since the console wrapper appends further commands and a trailing '&;' is a syntax error.
        pid_file = f"/tmp/someip_load_process_{len(self._pid_files)}.pid"
        self._pid_files.append(pid_file)
        self.sh(f"( {command} & echo $! > {pid_file} )")
        return pid_file

    def stop(self, pid_file, timeout_s=10):
        """Terminates the process and waits until it has exited, so that its output is complete."""
        self.sh(
            f'( read pid < {pid_file}; kill -TERM "$pid"; attempt=0; '
            f'while kill -0 "$pid" 2>/dev/null && [ $attempt -lt {timeout_s} ]; do '
            f"sleep 1; attempt=$((attempt + 1)); done; rm {pid_file} )",
            timeout=timeout_s + 10,
        )
        self._pid_files.remove(pid_file)

    def cleanup(self):
        for pid_file in reversed(self._pid_files):
            self._console.run_sh_cmd_output(
                f'( if [ -r {pid_file} ]; then read pid < {pid_file}; kill -TERM "$pid" 2>/dev/null; rm {pid_file}; fi )'
            )
        self._pid_files.clear()


def _save_outputs(sd_side, results, cpu_results):
    """Stores the results and the CPU samples in bazel-testlogs/<test>/test.outputs; the VM is gone afterwards."""
    output_dir = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")
    if output_dir is None:
        return
    output = pathlib.Path(output_dir)
    (output / "results.jsonl").write_text("".join(json.dumps(result) + "\n" for result in results))
    (output / "cpu.jsonl").write_text("".join(json.dumps(result) + "\n" for result in cpu_results))
    rc, hogs = sd_side._console.run_sh_cmd_output("cat /tmp/someip_bench_hogs.txt")
    if rc == 0:
        (output / "hogs.txt").write_text(hogs)


def _print_diagnostics(sd_side, peer_side):
    for title, side, command in (
        ("event module", sd_side, f"{MODULE_LOG_COMMAND} | tail -40"),
        (
            "io-sock",
            sd_side,
            "slog2info | grep -F io_sock | grep -v -F '[someip_event]' | tail -40; pidin -p io-sock threads",
        ),
        ("SD daemon", sd_side, "tail -40 /tmp/someip_bench_sd_daemon.log"),
        ("load application", sd_side, "tail -40 /tmp/someip_bench_bench.txt"),
        ("load peer", peer_side, f"tail -40 {PEER_LOG}"),
        ("peer someipd", peer_side, "tail -40 /tmp/someip_load_someipd.log"),
    ):
        print(f"\n[{title}]\n{side.output(command)}")


def test_mixed_load_benchmark(console_a, console_b, capsys):
    sd_side, peer_side = Side(console_a), Side(console_b)
    try:
        # Background output must not reach the QNX console, it would interleave with the command parsing of the ITF.
        config = f"{PEER_DIR}/etc/lg_load_peer.json"
        cpu_scales = ",".join(f"{scale:g}" for scale in CPU_SCALES)
        peer_side.start(f"{PEER_DIR}/bin/someipd {config} > /tmp/someip_load_someipd.log 2>&1")
        peer_side.sh("sleep 1")
        peer = peer_side.start(
            f"{PEER_DIR}/bin/someip_load_peer {config} --workload {PEER_DIR}/etc/workload.txt --scale {SCALE}"
            f" --cpu-scales {cpu_scales} > {PEER_LOG} 2>&1"
        )

        # The SD side uses the same script as measurements on a real target.
        cpu_s = 1 + CPU_BASELINE_S + len(CPU_SCALES) * (1 + CPU_PHASE_S) + 1
        timeout = 10 + 30 + cpu_s + SETUP_TIMEOUT_S + WARMUP_S + DURATION_S + 2 + 30
        out = sd_side.sh(
            f"/bin/sh {LOAD_DIR}/bin/run_target.sh 10.0.3.1 --app someip_load_lola"
            f" --workload {LOAD_DIR}/etc/workload.txt --warmup {WARMUP_S} --duration {DURATION_S}"
            f" --setup-timeout {SETUP_TIMEOUT_S} --scale {SCALE} --cpu-baseline {CPU_BASELINE_S}"
            f" --cpu-phase {CPU_PHASE_S} --cpu-scales {cpu_scales}",
            timeout=timeout,
        )
        peer_side.stop(peer)
        sd_results, peer_results = parse(out), parse(peer_side.output(f"cat {PEER_LOG}"))
        cpu_results = parse(out, CPU_PREFIX)
        _save_outputs(sd_side, sd_results + peer_results, cpu_results)
        assert sd_results, f"no results of the benchmarked side:\n{out}"
        assert len(peer_results) == len(sd_results), f"expected {len(sd_results)} peer results: {peer_results}"
        rows = merge(sd_results, peer_results)

        # Bypasses pytest's capture; visible with bazel test --test_output=all or streamed.
        with capsys.disabled():
            print("QEMU timing is not representative, compare trends and ratios")
            print(format_results(rows))
            print(format_cpu(cpu_results))
        for row in rows:
            assert row["sender"].get("sent", 0) > 0, f"nothing sent: {row}"
            assert row["receiver"].get("received", 0) > 0, f"nothing received: {row}"
            assert row["receiver"].get("duplicates", 0) == 0, f"duplicates: {row}"
        assert len(cpu_results) == 2 + len(CPU_SCALES), f"expected baseline, sweep and load CPU results:\n{out}"
        load_cpu = cpu_results[-1]["groups"]
        assert "module" in load_cpu, f"event module thread not found: {cpu_results[-1]}"
        assert load_cpu["module"]["cpu_ms"] > 0, f"no CPU time of the event module under load: {cpu_results[-1]}"
    except Exception:
        _print_diagnostics(sd_side, peer_side)
        raise
    finally:
        for side in (sd_side, peer_side):
            side.cleanup()
