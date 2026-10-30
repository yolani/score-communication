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
# Runs lg-someip someipd and the mixed load peer on the Linux peer of the target: publishes the RX event classes of the
# workload and receives the TX event classes published by someip_load_lola on the target.
#
# usage: run_host_load_peer.sh <host_ip> <interface> [<sd_multicast> <sd_port>] [peer options]
#   host_ip:      address of the host in the target's subnet
#   interface:    interface carrying that address; the SD multicast group is routed through it
#   sd_multicast: SD multicast group of the stack under test, default 224.244.224.245 (score SD daemon)
#   sd_port:      SD port of the stack under test, default 30491 (score SD daemon)
#   peer options: --scale <factor> and --cpu-scales <f,f>, the same values as for run_target.sh; --label <name>;
#                 --rx-port <port>: one UDP port for all events the peer receives, instead of 34000 + service number
# Start it before run_target.sh --app someip_load_lola and stop it with Ctrl+C after run_target.sh has finished: the
# peer then prints its results, also stored in $SOMEIP_BENCH_PEER_WORK/results.txt. Use one peer run per target run.
# Work files go to $SOMEIP_BENCH_PEER_WORK, default /tmp/someip_load_peer. lg-someip always uses /tmp/someip; if
# another user owns it, the script runs in a private mount namespace with $SOMEIP_BENCH_PEER_WORK/someip instead.

set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <host_ip> <interface> [<sd_multicast> <sd_port>] [--scale 1.0] [--cpu-scales 0.25,0.5]"
    exit 1
fi
HOST_IP=$1
INTERFACE=$2
shift 2
SD_MULTICAST=224.244.224.245
SD_PORT=30491
if [ $# -ge 2 ] && [ "${1#--}" = "$1" ]; then
    SD_MULTICAST=$1
    SD_PORT=$2
    shift 2
fi
RX_PORT=""
PEER_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --rx-port) RX_PORT=$2; shift 2 ;;
        *) PEER_ARGS+=("$1"); shift ;;
    esac
done
set -- ${PEER_ARGS[@]+"${PEER_ARGS[@]}"}
PREFIX=$(cd "$(dirname "$0")/.." && pwd)
WORK=${SOMEIP_BENCH_PEER_WORK:-/tmp/someip_load_peer}
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
        sh "$WORK/someip" "$0" "$HOST_IP" "$INTERFACE" "$SD_MULTICAST" "$SD_PORT" ${RX_PORT:+--rx-port "$RX_PORT"} "$@"
fi

# The event ports of the workload (33000/34000 + service number) are kept unless --rx-port replaces the RX ones.
LOAD_RX_PORTS='"unreliable": "340[0-9][0-9]"'
if [ -z "$RX_PORT" ]; then
    LOAD_RX_PORTS=no-match
fi
sed -e "s/10\.0\.3\.2/$HOST_IP/" \
    -e "s/224\.244\.224\.245/$SD_MULTICAST/" \
    -e "s/\"port\" *: *\"30491\"/\"port\" : \"$SD_PORT\"/" \
    -e "s/$LOAD_RX_PORTS/\"unreliable\": \"$RX_PORT\"/" \
    "$PREFIX/etc/lg_load_peer.json" > "$WORK/lg_load_peer.json"

case "$(ip route get "$SD_MULTICAST")" in
    *" dev $INTERFACE "*) ;;
    *)
        echo "routing SOME/IP-SD multicast via $INTERFACE (needs root)"
        sudo ip route replace "$SD_MULTICAST/32" dev "$INTERFACE"
        ;;
esac

"$PREFIX/bin/someipd" "$WORK/lg_load_peer.json" > "$WORK/someipd.log" 2>&1 &
SOMEIPD_PID=$!
PEER_PID=""
TAIL_PID=""
cleanup() {
    kill $TAIL_PID "$SOMEIPD_PID" 2>/dev/null || true
}
trap cleanup EXIT
sleep 1

# Background jobs of a script ignore SIGINT, so Ctrl+C only reaches this script, which stops the peer with SIGTERM.
"$PREFIX/bin/someip_load_peer" "$WORK/lg_load_peer.json" --workload "$PREFIX/etc/workload.txt" "$@" \
    > "$WORK/load_peer.log" 2>&1 &
PEER_PID=$!
stop_peer() {
    trap - INT TERM
    echo "stopping the peer"
    kill -TERM "$PEER_PID" 2>/dev/null || true
}
trap stop_peer INT TERM
echo "load peer (SD $SD_MULTICAST:$SD_PORT) running on $HOST_IP ($INTERFACE), logs in $WORK; Ctrl+C after the target run"
tail -n +1 -f --pid="$PEER_PID" "$WORK/load_peer.log" | grep -v "^LOAD_RESULT" &
TAIL_PID=$!
# wait returns early when a trapped signal arrives; wait again until the peer has written its results.
while kill -0 "$PEER_PID" 2>/dev/null; do
    wait "$PEER_PID" || true
done

grep "^LOAD_RESULT" "$WORK/load_peer.log" > "$WORK/results.txt" || true
echo "peer results ($(wc -l < "$WORK/results.txt") classes): $WORK/results.txt"
echo "report: python3 $PREFIX/scripts/load_report.py <target results.txt> $WORK/results.txt"
