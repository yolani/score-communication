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
# usage: run_host_reflector.sh <host_ip> <interface> [<sd_multicast> <sd_port> [<rx_port>]]
#   host_ip:      address of the host in the target's subnet
#   interface:    interface carrying that address; the SD multicast group is routed through it
#   sd_multicast: SD multicast group of the stack under test, default 224.244.224.245 (score SD daemon)
#   sd_port:      SD port of the stack under test, default 30491 (score SD daemon)
#   rx_port:      UDP port the reflector receives the target's events on, default 30721
# Work files go to $SOMEIP_BENCH_PEER_WORK, default /tmp/someip_bench_peer. lg-someip always uses /tmp/someip; if
# another user owns it, the script runs in a private mount namespace with $SOMEIP_BENCH_PEER_WORK/someip instead.
# Stops with Ctrl+C.

set -euo pipefail

if [ $# -ne 2 ] && [ $# -ne 4 ] && [ $# -ne 5 ]; then
    echo "usage: $0 <host_ip> <interface> [<sd_multicast> <sd_port> [<rx_port>]]"
    exit 1
fi
HOST_IP=$1
INTERFACE=$2
SD_MULTICAST=${3:-224.244.224.245}
SD_PORT=${4:-30491}
RX_PORT=${5:-30721}
PREFIX=$(cd "$(dirname "$0")/.." && pwd)
WORK=${SOMEIP_BENCH_PEER_WORK:-/tmp/someip_bench_peer}
mkdir -p "$WORK"
if [ ! -w "$WORK" ]; then
    echo "$WORK is not writable, set SOMEIP_BENCH_PEER_WORK"
    exit 1
fi

if [ -d /tmp/someip ] && [ ! -w /tmp/someip ] && [ -z "${SOMEIP_BENCH_PEER_NS:-}" ]; then
    echo "/tmp/someip belongs to another user, using $WORK/someip in a private mount namespace"
    mkdir -p "$WORK/someip"
    export SOMEIP_BENCH_PEER_WORK=$WORK SOMEIP_BENCH_PEER_NS=1
    exec unshare --map-root-user --mount sh -c 'mount --bind "$1" /tmp/someip && shift && exec "$@"' \
        sh "$WORK/someip" "$0" "$@"
fi

sed -e "s/10\.0\.3\.2/$HOST_IP/" \
    -e "s/224\.244\.224\.245/$SD_MULTICAST/" \
    -e "s/\"port\" : \"30491\"/\"port\" : \"$SD_PORT\"/" \
    -e "s/\"unreliable\" : \"30721\"/\"unreliable\" : \"$RX_PORT\"/" \
    "$PREFIX/etc/lg_reflector.json" > "$WORK/lg_reflector.json"

case "$(ip route get "$SD_MULTICAST")" in
    *" dev $INTERFACE "*) ;;
    *)
        echo "routing SOME/IP-SD multicast via $INTERFACE (needs root)"
        sudo ip route replace "$SD_MULTICAST/32" dev "$INTERFACE"
        ;;
esac

"$PREFIX/bin/someipd" "$WORK/lg_reflector.json" > "$WORK/someipd.log" 2>&1 &
SOMEIPD_PID=$!
trap 'kill $SOMEIPD_PID 2>/dev/null' EXIT
sleep 1
echo "reflector (SD $SD_MULTICAST:$SD_PORT) running on $HOST_IP ($INTERFACE), logs in $WORK"
"$PREFIX/bin/someip_bench_reflector" "$WORK/lg_reflector.json" 2>&1 | tee "$WORK/reflector.log"
