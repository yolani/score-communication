#!/usr/bin/env python3
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
"""Report of the SOME/IP mixed load benchmark: merges the results of both sides into one table per event class, and
tabulates and fits the CPU_RESULT windows of the benchmarked side.

usage: load_report.py <target results.txt> [<peer results.txt>]
  target results: LOAD_RESULT and CPU_RESULT lines of run_target.sh --app someip_load_lola
  peer results:   LOAD_RESULT lines of someip_load_peer (run_host_load_peer.sh)
Also used by test_load_qemu.py.
"""

import json
import pathlib
import sys

RESULT_PREFIX = "LOAD_RESULT "
CPU_PREFIX = "CPU_RESULT "
CPU_GROUPS = (
    ("module", "module"),
    ("io-sock", "io-sock"),
    ("sd daemon", "someip_sd_daemon"),
    ("app", "someip_load_lola"),
)


def parse(output, prefix=RESULT_PREFIX):
    """The JSON objects of all lines starting with prefix."""
    return [json.loads(line[len(prefix) :]) for line in output.splitlines() if line.startswith(prefix)]


def merge(sd_results, peer_results):
    """One row per event class: the sender's counts and the receiver's statistics."""
    peer_by_class = {result["class"]: result for result in peer_results}
    rows = []
    for sd in sd_results:
        peer = peer_by_class.get(sd["class"], {})
        sender, receiver = (sd, peer) if sd["direction"] == "TX" else (peer, sd)
        rows.append({"sd": sd, "peer": peer, "sender": sender, "receiver": receiver})
    return rows


def format_results(rows):
    header = (
        f"{'dir':>3} {'frequency':>19} {'payload':>11} {'bytes':>5} {'rate_hz':>7} {'streams':>7} {'sent':>7} "
        f"{'recv':>7} {'lost':>6} {'loss%':>6} {'dup':>4} {'jit_p50':>8} {'jit_p99':>8} {'lag_max':>8}"
    )
    lines = [
        "",
        "SOME/IP mixed load benchmark, jitter and send lag in ms",
        "TX: sent by the benchmarked side, received by the peer; RX: sent by the peer, received by the benchmarked side",
        "RX sent and lost are derived from the sequence numbers received in the window (the peer sends continuously)",
        header,
        "-" * len(header),
    ]
    for row in rows:
        sd, sender, receiver = row["sd"], row["sender"], row["receiver"]
        received = receiver.get("received", 0)
        # TX counts only samples sent in the measurement window, so the loss is exact; RX loss are sequence gaps.
        if sd["direction"] == "TX":
            sent = sender.get("sent", 0)
            lost = sent - received
        else:
            lost = receiver.get("lost", 0)
            sent = received + lost
        jitter = receiver.get("jitter_us", {})
        lines.append(
            f"{sd['direction']:>3} {sd['frequency']:>19} {sd['payload_bucket']:>11} {sd['payload']:>5} "
            f"{sd['rate_hz']:>7g} {sd['streams']:>7} {sent:>7} {received:>7} {lost:>6} "
            f"{100.0 * lost / max(1, lost + received):>6.2f} {receiver.get('duplicates', 0):>4} "
            + " ".join(
                f"{value / 1000:>8.2f}" if value is not None else f"{'-':>8}"
                for value in (jitter.get("p50"), jitter.get("p99"), sender.get("max_send_lag_us"))
            )
        )
    return "\n".join(lines) + "\n"


def events_per_s(result):
    return (result["tx_events"] + result["rx_events"]) / result["window_s"]


def fit(points):
    """Least squares line y = intercept + slope * x; None with fewer than two distinct x."""
    if len({x for x, _ in points}) < 2:
        return None
    count = len(points)
    mean_x = sum(x for x, _ in points) / count
    mean_y = sum(y for _, y in points) / count
    slope = sum((x - mean_x) * (y - mean_y) for x, y in points) / sum((x - mean_x) ** 2 for x, _ in points)
    return mean_y - slope * mean_x, slope


def format_cpu(cpu_results):
    header = (
        f"{'phase':>12} {'ev/s':>7} "
        + " ".join(f"{name + ' %':>12}" for name, _ in CPU_GROUPS)
        + f" {'busy %':>7} {'module us/ev':>12}"
    )
    lines = [
        "",
        "CPU time per thread group from QNX procfs, % of one CPU",
        "module: event module thread in io-sock; io-sock: all other io-sock threads; busy: all CPUs minus idle",
        header,
        "-" * len(header),
    ]
    for result in cpu_results:
        groups = result["groups"]
        busy = 100.0 * result["cpus"] - groups.get("idle", {}).get("pct", 0.0)
        rate = events_per_s(result)
        module_us = (
            groups.get("module", {}).get("cpu_ms", 0.0) * 1000.0 / max(1, result["tx_events"] + result["rx_events"])
        )
        lines.append(
            f"{result['phase']:>12} {rate:>7.0f} "
            + " ".join(f"{groups.get(group, {}).get('pct', 0.0):>12.2f}" for _, group in CPU_GROUPS)
            + f" {busy:>7.1f} "
            + (f"{module_us:>12.2f}" if rate > 0 else f"{'-':>12}")
        )
    for name, group in CPU_GROUPS[:2]:
        # CPU ns per second over events per second: slope in ns (= us/1000) per event, intercept in % of one CPU.
        line = fit(
            [(events_per_s(result), result["groups"].get(group, {}).get("pct", 0.0) * 1.0e7) for result in cpu_results]
        )
        if line is not None:
            lines.append(f"{name}: {line[0] / 1.0e7:.2f} % fixed + {line[1] / 1000.0:.2f} us CPU per event")
    if cpu_results:
        busy_threads = [
            f"{thread['tid']} '{thread['name']}' {thread['pct']:.2f}"
            for thread in cpu_results[-1]["threads"]
            if thread["pct"] >= 0.05
        ]
        lines.append("io-sock threads >= 0.05 % in the load window (tid 'name' %): " + ", ".join(busy_threads))
    return "\n".join(lines) + "\n"


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__)
        return 1
    target = pathlib.Path(argv[1]).read_text()
    peer = pathlib.Path(argv[2]).read_text() if len(argv) == 3 else ""
    sd_results = parse(target)
    if sd_results:
        print(format_results(merge(sd_results, parse(peer))))
    cpu_results = parse(target, CPU_PREFIX)
    if cpu_results:
        print(format_cpu(cpu_results))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
