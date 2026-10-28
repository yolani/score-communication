#!/bin/bash
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
# Runs lg-someip someipd and the benchmark reflector on the Linux peer of the target.
#
# usage: run_host_reflector.sh <host_ip> <interface> [<sd_multicast> <sd_port>]
#   host_ip:      address of the host in the target's subnet
#   interface:    interface carrying that address; the SD multicast group is routed through it
#   sd_multicast: SD multicast group of the stack under test, default 224.244.224.245 (score SD daemon)
#   sd_port:      SD port of the stack under test, default 30491 (score SD daemon)
# Stops with Ctrl+C.

set -euo pipefail

if [ $# -ne 2 ] && [ $# -ne 4 ]; then
    echo "usage: $0 <host_ip> <interface> [<sd_multicast> <sd_port>]"
    exit 1
fi
HOST_IP=$1
INTERFACE=$2
SD_MULTICAST=${3:-224.244.224.245}
SD_PORT=${4:-30491}
PREFIX=$(cd "$(dirname "$0")/.." && pwd)
WORK=/tmp/someip_bench_peer
mkdir -p "$WORK"

sed -e "s/10\.0\.3\.2/$HOST_IP/" \
    -e "s/224\.244\.224\.245/$SD_MULTICAST/" \
    -e "s/\"port\" : \"30491\"/\"port\" : \"$SD_PORT\"/" \
    "$PREFIX/etc/lg_reflector.json" > "$WORK/lg_reflector.json"

if ! ip route show table all | grep -q "$SD_MULTICAST"; then
    echo "adding multicast route for SOME/IP-SD via $INTERFACE (needs root)"
    sudo ip route add "$SD_MULTICAST/32" dev "$INTERFACE"
fi

"$PREFIX/bin/someipd" "$WORK/lg_reflector.json" > "$WORK/someipd.log" 2>&1 &
SOMEIPD_PID=$!
trap 'kill $SOMEIPD_PID 2>/dev/null' EXIT
sleep 1
echo "reflector (SD $SD_MULTICAST:$SD_PORT) running on $HOST_IP ($INTERFACE), logs in $WORK"
"$PREFIX/bin/someip_bench_reflector" "$WORK/lg_reflector.json" 2>&1 | tee "$WORK/reflector.log"
