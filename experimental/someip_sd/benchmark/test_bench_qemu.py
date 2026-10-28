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
"""Functional check of the SOME/IP round-trip benchmark on two QNX QEMU VMs.

VM-A (10.0.3.1) runs the io-sock event module, the SD daemon and the benchmark application; VM-B (10.0.3.2) runs
lg-someip someipd and the reflector. QEMU timing is not representative: this test only verifies that the
benchmark runs end to end and reports results; numbers are printed for information.
"""

import json
import logging
import os
import pathlib

logger = logging.getLogger(__name__)

SD_DIR = "/opt/someip_sd"
PEER_DIR = "/opt/someip_bench_peer"
MODULE_LOG_COMMAND = 'slog2info | grep -F "[someip_event]"'
RESULT_PREFIX = "BENCH_RESULT "
# Short runs keep the test fast; payload sizes cover the smallest and the largest supported notification.
RUNS = [
    {"payload": 16, "rate_hz": 100, "count": 300},
    {"payload": 1400, "rate_hz": 100, "count": 300},
]
MAX_LOSS_RATIO = 0.01


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

    def start(self, command):
        # Subshell, since the console wrapper appends further commands and a trailing '&;' is a syntax error.
        pid_file = f"/tmp/someip_bench_process_{len(self._pid_files)}.pid"
        self._pid_files.append(pid_file)
        self.sh(f"( {command} & echo $! > {pid_file} )")

    def cleanup(self):
        for pid_file in reversed(self._pid_files):
            self._console.run_sh_cmd_output(
                f'( if [ -r {pid_file} ]; then read pid < {pid_file}; kill -TERM "$pid" 2>/dev/null; rm {pid_file}; fi )'
            )
        self._pid_files.clear()


def _parse_results(output):
    return [json.loads(line[len(RESULT_PREFIX) :]) for line in output.splitlines() if line.startswith(RESULT_PREFIX)]


def _save_outputs(sd_side, results):
    """Stores the results and the CPU samples in bazel-testlogs/<test>/test.outputs; the VM is gone afterwards."""
    output_dir = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")
    if output_dir is None:
        return
    output = pathlib.Path(output_dir)
    (output / "results.jsonl").write_text("".join(json.dumps(result) + "\n" for result in results))
    rc, hogs = sd_side._console.run_sh_cmd_output("cat /tmp/someip_bench_hogs.txt")
    if rc == 0:
        (output / "hogs.txt").write_text(hogs)


def _print_diagnostics(sd_side, peer_side):
    for title, side, command in (
        ("event module", sd_side, MODULE_LOG_COMMAND),
        ("io-sock", sd_side, "slog2info | grep -F io_sock | grep -v -F '[someip_event]' | tail -40; pidin -p io-sock"),
        ("SD daemon", sd_side, "cat /tmp/someip_bench_sd_daemon.log"),
        ("reflector", peer_side, "cat /tmp/someip_bench_reflector.log"),
        ("peer someipd", peer_side, "cat /tmp/someip_bench_someipd.log"),
    ):
        _, out = side._console.run_sh_cmd_output(command)
        print(f"\n[{title}]\n{out}")


def _format_results(results):
    columns = ("min", "p50", "p90", "p99", "p999", "max", "mean")
    header = f"{'payload':>8} {'rate_hz':>8} {'sent':>6} {'recv':>6} {'lost':>6} {'dup':>4} " + " ".join(
        f"{column:>8}" for column in columns
    )
    lines = [
        "",
        "SOME/IP round-trip benchmark, RTT in ms (QEMU timing is not representative)",
        header,
        "-" * len(header),
    ]
    for result in results:
        rtt = result.get("rtt_us", {})
        lines.append(
            f"{result['payload']:>8} {result['rate_hz']:>8} {result['sent']:>6} {result['received']:>6} "
            f"{result['lost']:>6} {result['duplicates']:>4} "
            + " ".join(f"{rtt[column] / 1000:>8.2f}" if column in rtt else f"{'-':>8}" for column in columns)
        )
    return "\n".join(lines) + "\n"


def test_round_trip_benchmark(console_a, console_b, capsys):
    sd_side, peer_side = Side(console_a), Side(console_b)
    try:
        # Background output must not reach the QNX console, it would interleave with the command parsing of the ITF.
        peer_side.start(f"{PEER_DIR}/bin/someipd {PEER_DIR}/etc/lg_reflector.json > /tmp/someip_bench_someipd.log 2>&1")
        peer_side.sh("sleep 1")
        peer_side.start(
            f"{PEER_DIR}/bin/someip_bench_reflector {PEER_DIR}/etc/lg_reflector.json"
            " > /tmp/someip_bench_reflector.log 2>&1"
        )

        # The SD side uses the same script as measurements on a real target.
        run_args = " ".join(f"--run {run['payload']}:{run['rate_hz']}:{run['count']}" for run in RUNS)
        timeout = 10 + 30 + sum(5 + run["count"] // run["rate_hz"] for run in RUNS) + 30
        out = sd_side.sh(f"/bin/sh {SD_DIR}/bin/run_target.sh 10.0.3.1 {run_args} --warmup 20", timeout=timeout)
        results = _parse_results(out)
        _save_outputs(sd_side, results)
        assert len(results) == len(RUNS), f"expected {len(RUNS)} results:\n{out}"

        # Bypasses pytest's capture; visible with bazel test --test_output=all or streamed.
        with capsys.disabled():
            print(_format_results(results))
        for result in results:
            assert result["received"] > 0, f"no pong received: {result}"
            assert result["lost"] <= MAX_LOSS_RATIO * result["sent"], f"too many lost round trips: {result}"
            assert result["duplicates"] == 0, f"duplicate pongs: {result}"
    except Exception:
        _print_diagnostics(sd_side, peer_side)
        raise
    finally:
        for side in (sd_side, peer_side):
            side.cleanup()
