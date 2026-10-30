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
"""Splits the benchmark round trip into the time spent on the target and the time spent outside of it.

Pairs the RTT per ping (samples.txt of the benchmark application) by sequence number with the ping and the pong
captured on the target's interface (capture.pcap, see run_target.sh --capture):

    outside = pong captured - ping captured   (network and peer)
    target  = RTT - outside                    (SOME/IP stack and application on the target, both directions)

usage: target_time.py <capture.pcap> <samples.txt> [--payload-offset <bytes>] [--label <name>]
  --payload-offset: bytes before the benchmark header in the SOME/IP payload: 0 for the io-sock event module,
                    4 for the Vector stack (length field of the dynamic array).
"""

import argparse
import math
import struct

PING_MESSAGE_ID = 0xB0018001
PONG_MESSAGE_ID = 0xB0028002
SOMEIP_HEADER_SIZE = 16
SOMEIP_LENGTH_OFFSET = 8
ETHERNET_HEADER_SIZE = 14
ETHERTYPE_VLAN = 0x8100
ETHERTYPE_IPV4 = 0x0800
LINKTYPE_ETHERNET = 1
IPPROTO_UDP = 17
UDP_HEADER_SIZE = 8
MAGIC_MICROSECONDS = 0xA1B2C3D4
MAGIC_NANOSECONDS = 0xA1B23C4D


def read_pcap(path):
    """Yields (timestamp in s, UDP payload) of the IPv4/UDP packets of a classic pcap file with Ethernet frames."""
    with open(path, "rb") as file:
        header = file.read(24)
        for endian in ("<", ">"):
            magic = struct.unpack(endian + "I", header[:4])[0]
            if magic in (MAGIC_MICROSECONDS, MAGIC_NANOSECONDS):
                break
        else:
            raise ValueError(f"{path}: not a classic pcap file")
        resolution = 1e-9 if magic == MAGIC_NANOSECONDS else 1e-6
        if struct.unpack(endian + "I", header[20:24])[0] != LINKTYPE_ETHERNET:
            raise ValueError(f"{path}: only Ethernet captures are supported")
        while True:
            record = file.read(16)
            if len(record) < 16:
                return
            seconds, fraction, captured, _ = struct.unpack(endian + "IIII", record)
            payload = udp_payload(file.read(captured))
            if payload is not None:
                yield seconds + fraction * resolution, payload


def udp_payload(frame):
    offset = ETHERNET_HEADER_SIZE - 2
    ethertype = struct.unpack_from("!H", frame, offset)[0]
    while ethertype == ETHERTYPE_VLAN:
        offset += 4
        ethertype = struct.unpack_from("!H", frame, offset)[0]
    offset += 2
    if (ethertype != ETHERTYPE_IPV4) or (len(frame) < offset + 20) or (frame[offset + 9] != IPPROTO_UDP):
        return None
    return frame[offset + (frame[offset] & 0x0F) * 4 + UDP_HEADER_SIZE :]


def someip_messages(datagram):
    """Yields (message ID, payload) per SOME/IP message; payloads may be cut by the capture's snap length."""
    offset = 0
    while offset + SOMEIP_HEADER_SIZE <= len(datagram):
        message_id, length = struct.unpack_from("!II", datagram, offset)
        yield message_id, datagram[offset + SOMEIP_HEADER_SIZE : offset + SOMEIP_LENGTH_OFFSET + length]
        offset += SOMEIP_LENGTH_OFFSET + length


def capture_times(path, payload_offset):
    """Returns the first capture time per sequence number of pings and of pongs."""
    times = {PING_MESSAGE_ID: {}, PONG_MESSAGE_ID: {}}
    for timestamp, datagram in read_pcap(path):
        for message_id, payload in someip_messages(datagram):
            if (message_id in times) and (len(payload) >= payload_offset + 4):
                sequence = struct.unpack_from("!I", payload, payload_offset)[0]
                times[message_id].setdefault(sequence, timestamp)
    return times[PING_MESSAGE_ID], times[PONG_MESSAGE_ID]


def percentile(sorted_values, value):
    """Nearest-rank percentile, as in the benchmark applications."""
    rank = math.ceil(value / 100.0 * len(sorted_values))
    return sorted_values[min(max(rank, 1), len(sorted_values)) - 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture")
    parser.add_argument("samples")
    parser.add_argument("--payload-offset", type=int, default=0)
    parser.add_argument("--label", default="")
    arguments = parser.parse_args()

    ping_times, pong_times = capture_times(arguments.capture, arguments.payload_offset)
    runs = {}
    with open(arguments.samples, encoding="utf-8") as samples:
        for line in samples:
            payload, rate_hz, sequence, rtt_ns = (int(field) for field in line.split())
            run = runs.setdefault((payload, rate_hz), {"rtt": [], "outside": [], "target": []})
            run["rtt"].append(rtt_ns / 1000.0)
            if (sequence in ping_times) and (sequence in pong_times):
                outside_us = (pong_times[sequence] - ping_times[sequence]) * 1e6
                run["outside"].append(outside_us)
                run["target"].append(rtt_ns / 1000.0 - outside_us)

    print(f"{arguments.label} time per round trip in us; target = RTT - outside, outside = network + peer")
    print(
        f"{'payload':>8} {'rate_hz':>8} {'matched':>12} {'rtt_p50':>8} {'out_p50':>8} "
        f"{'tgt_min':>8} {'tgt_p50':>8} {'tgt_p90':>8} {'tgt_p99':>8} {'tgt_max':>8}"
    )
    for (payload, rate_hz), run in runs.items():
        matched = f"{len(run['target'])}/{len(run['rtt'])}"
        if not run["target"]:
            print(f"{payload:>8} {rate_hz:>8} {matched:>12}")
            continue
        rtt, outside, target = (sorted(run[key]) for key in ("rtt", "outside", "target"))
        print(
            f"{payload:>8} {rate_hz:>8} {matched:>12} {percentile(rtt, 50):>8.0f} {percentile(outside, 50):>8.0f} "
            + " ".join(f"{percentile(target, value):>8.0f}" for value in (0, 50, 90, 99, 100))
        )


if __name__ == "__main__":
    main()
