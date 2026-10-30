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
"""Generates the synthetic mixed load of the SOME/IP load benchmark from an event workload CSV.

Every CSV row is an event class (direction, frequency_bucket, payload_bucket); it becomes `streams` synthetic events
with the class's max_payload_bytes, sent at a representative rate of the frequency bucket. Direction is seen from the
benchmarked side: TX events are published by the benchmarked application, RX events by the remote peer.

Outputs: the workload read by both load applications, the benchmarked side's mw_com_config.json and SD daemon
configuration, and the peer's lg-someip configuration.
"""

import argparse
import csv
import json
import pathlib

# Representative rate per frequency bucket; nonperiodic events are modeled as 1 Hz.
RATE_HZ = {
    "<1 Hz": 1,
    "1-<10 Hz": 5,
    "10-<50 Hz": 20,
    "50-<100 Hz": 50,
    ">=100 Hz": 100,
    "Unknown/nonperiodic": 1,
}
# TODO: payloads are capped at 1400 B (event module limit) until SOME/IP-TP is implemented.
MAX_PAYLOAD = 1400
# Sequence number and send time, see bench_protocol.h.
MIN_PAYLOAD = 12
# Events per service, limited by event_module_protocol::kMaxEvents.
MAX_EVENTS_PER_SERVICE = 64

INSTANCE = 0x0001
EVENTGROUP = 0x0001
FIRST_EVENT = 0x8001
FIRST_SERVICE = {"TX": 0xC000, "RX": 0xD000}
# Local UDP ports of the providers and consumers on the benchmarked side and on the peer.
SD_TX_PORT, SD_RX_PORT, PEER_RX_PORT, PEER_TX_PORT = 31000, 32000, 33000, 34000
# The event module subscribes TX events with 8 samples (tx_forwarder.cpp); one slot more for the next send.
TX_SAMPLE_SLOTS = 10
RX_SAMPLE_SLOTS = 8
SD_UNICAST, PEER_UNICAST = "10.0.3.1", "10.0.3.2"
PEER_APPLICATION = {"name": "someip-load-peer", "id": "0x127a"}
SERVICE_DISCOVERY = {
    "enable": "true",
    "multicast": "224.244.224.245",
    "port": "30491",
    "protocol": "udp",
    "initial_delay_min": "10",
    "initial_delay_max": "100",
    "repetitions_base_delay": "200",
    "repetitions_max": "3",
    "ttl": "3",
    "cyclic_offer_delay": "2000",
    "request_response_delay": "1500",
}


def _label(text):
    return text.replace(" ", "")


def build_workload(rows):
    """Returns the classes and the services; each service holds the events of one class."""
    classes, services = [], []
    next_service = dict(FIRST_SERVICE)
    for row in rows:
        direction = row["direction"]
        payload = min(MAX_PAYLOAD, max(MIN_PAYLOAD, int(row["max_payload_bytes"])))
        rate_hz = RATE_HZ[row["frequency_bucket"]]
        index = len(classes)
        classes.append(
            {
                "index": index,
                "direction": direction,
                "frequency": _label(row["frequency_bucket"]),
                "payload_bucket": _label(row["payload_bucket"]),
                "max_payload_bytes": int(row["max_payload_bytes"]),
                "payload": payload,
                "rate_hz": rate_hz,
                "streams": int(row["streams"]),
            }
        )
        remaining = int(row["streams"])
        while remaining > 0:
            count = min(remaining, MAX_EVENTS_PER_SERVICE)
            services.append(
                {
                    "direction": direction,
                    "class": index,
                    "service": next_service[direction],
                    "number": next_service[direction] - FIRST_SERVICE[direction],
                    "events": [FIRST_EVENT + event for event in range(count)],
                    "payload": payload,
                    "rate_hz": rate_hz,
                }
            )
            next_service[direction] += 1
            remaining -= count
    return classes, services


def workload_text(classes, services):
    lines = [
        "# class <index> <TX|RX> <frequency_bucket> <payload_bucket> <csv max_payload_bytes> <streams>",
        "# stream <class> <service> <instance> <eventgroup> <event> <payload bytes> <rate_hz>",
    ]
    for item in classes:
        lines.append(
            f"class {item['index']} {item['direction']} {item['frequency']} {item['payload_bucket']} "
            f"{item['max_payload_bytes']} {item['streams']}"
        )
    for service in services:
        for event in service["events"]:
            lines.append(
                f"stream {service['class']} {service['service']} {INSTANCE} {EVENTGROUP} {event} "
                f"{service['payload']} {service['rate_hz']}"
            )
    return "\n".join(lines) + "\n"


def _hex(value):
    return f"0x{value:04x}"


def mw_com_config(services):
    service_types, instances = [], []
    for service in services:
        type_name = f"/someip/{service['service']:04x}"
        events = [f"event_{event:04x}" for event in service["events"]]
        service_types.append(
            {
                "serviceTypeName": type_name,
                "version": {"major": 1, "minor": 0},
                "bindings": [
                    {
                        "binding": "SHM",
                        "serviceId": service["service"],
                        "events": [{"eventName": name, "eventId": index + 1} for index, name in enumerate(events)],
                    }
                ],
            }
        )
        slots = TX_SAMPLE_SLOTS if service["direction"] == "TX" else RX_SAMPLE_SLOTS
        instances.append(
            {
                "instanceSpecifier": f"someip/{service['service']:04x}/{INSTANCE:04x}",
                "serviceTypeName": type_name,
                "version": {"major": 1, "minor": 0},
                "instances": [
                    {
                        "instanceId": INSTANCE,
                        "asil-level": "QM",
                        "binding": "SHM",
                        "events": [
                            {"eventName": name, "numberOfSampleSlots": slots, "maxSubscribers": 3} for name in events
                        ],
                    }
                ],
            }
        )
    return {"serviceTypes": service_types, "serviceInstances": instances}


def _lg_service(service, is_provider, port):
    events = [_hex(event) for event in service["events"]]
    return {
        "service": _hex(service["service"]),
        "instance": _hex(INSTANCE),
        "is-provider": "true" if is_provider else "false",
        "unreliable": str(port + service["number"]),
        "events": [{"event": event, "is_field": "false"} for event in events],
        "eventgroups": [{"eventgroup": _hex(EVENTGROUP), "events": events}],
    }


def lg_config(services, unicast, applications, provided_direction, provider_port, consumer_port):
    return {
        "unicast": unicast,
        "iptype": "IPv4",
        "logging": {"level": "info"},
        "applications": applications,
        "services": [
            _lg_service(service, True, provider_port)
            if service["direction"] == provided_direction
            else _lg_service(service, False, consumer_port)
            for service in services
        ],
        "service-discovery": SERVICE_DISCOVERY,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=pathlib.Path)
    parser.add_argument("--workload", type=pathlib.Path, required=True)
    parser.add_argument("--mw-com-config", type=pathlib.Path, required=True)
    parser.add_argument("--sd-daemon-config", type=pathlib.Path, required=True)
    parser.add_argument("--peer-config", type=pathlib.Path, required=True)
    args = parser.parse_args()

    with args.csv.open(newline="") as csv_file:
        classes, services = build_workload(list(csv.DictReader(csv_file)))

    def write_json(path, content):
        path.write_text(json.dumps(content, indent=4) + "\n")

    args.workload.write_text(workload_text(classes, services))
    write_json(args.mw_com_config, mw_com_config(services))
    write_json(args.sd_daemon_config, lg_config(services, SD_UNICAST, [], "TX", SD_TX_PORT, SD_RX_PORT))
    write_json(
        args.peer_config, lg_config(services, PEER_UNICAST, [PEER_APPLICATION], "RX", PEER_RX_PORT, PEER_TX_PORT)
    )


if __name__ == "__main__":
    main()
