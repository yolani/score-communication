#!/bin/sh
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
# Runs the SOME/IP round-trip benchmark of the io-sock event module on a QNX target.
# Install the bundle into a writable and executable location.
# Work files go to the directory $SOMEIP_BENCH_WORK, default /tmp/someip_bench_* (/tmp is /dev/shmem: no directories).
#
# usage: run_target.sh <target_ip> [--disable-firewall] [--stop <process>]... [--sd-unicast <ip>] [--sd-multicast <ip>]
#                      [--sd-port <port>] [--rx-port <port>] [--run <payload>:<rate_hz>:<count>]... [bench options]
# The reflector (run_host_reflector.sh) must run on the peer and reach <target_ip>.
# --stop:    stops a process before the measurement, e.g. another SD daemon owning the SD endpoint.
# --sd-*:    SD endpoint, default <target_ip>, 224.244.224.245, 30491.
# --rx-port: UDP port the module receives the reflector's events on, default 30710.
# Networks that only forward configured flows need an SD endpoint and ports from the network configuration.

set -u

if [ $# -lt 1 ]; then
    echo "usage: $0 <target_ip> [--disable-firewall] [--stop <process>]... [--sd-unicast <ip>] [--sd-multicast <ip>]" \
        "[--sd-port <port>] [--rx-port <port>] [--run 16:100:1000]... [--warmup 50] [--setup-timeout 30]"
    exit 1
fi
TARGET_IP=$1
shift
DISABLE_FIREWALL=0
STOP_PROCESSES=""
SD_UNICAST=$TARGET_IP
SD_MULTICAST=224.244.224.245
SD_PORT=30491
RX_PORT=30710
while [ $# -gt 0 ]; do
    case "$1" in
        --disable-firewall) DISABLE_FIREWALL=1; shift ;;
        --stop) STOP_PROCESSES="$STOP_PROCESSES $2"; shift 2 ;;
        --sd-unicast) SD_UNICAST=$2; shift 2 ;;
        --sd-multicast) SD_MULTICAST=$2; shift 2 ;;
        --sd-port) SD_PORT=$2; shift 2 ;;
        --rx-port) RX_PORT=$2; shift 2 ;;
        *) break ;;
    esac
done
if [ $# -eq 0 ]; then
    set -- --run 16:100:1000 --run 1400:100:1000
fi

# Minimal QNX images may lack dirname and tail.
case "$0" in
    */*) SCRIPT_DIR=${0%/*} ;;
    *) SCRIPT_DIR=. ;;
esac
PREFIX=$(cd "$SCRIPT_DIR/.." && pwd)
if [ -n "${SOMEIP_BENCH_WORK:-}" ]; then
    mkdir -p "$SOMEIP_BENCH_WORK" || exit 1
    WORK=$SOMEIP_BENCH_WORK/
else
    WORK=/tmp/someip_bench_
fi
MODULE_LOG='[someip_event]'
# ssh may forward the peer's locale (LANG, LC_*, e.g. de_DE.UTF-8), which QNX libc++ rejects ("can not determine
# global locale"); LC_ALL overrides all of them.
LANG=C
LC_ALL=C
export LANG LC_ALL

last() {
    if command -v tail > /dev/null 2>&1; then
        tail -n "$1"
    else
        cat
    fi
}

log() {
    echo "run_target: $*"
}

# Same services as in the QEMU test; only the SD endpoint and the RX port differ.
sed -e "s/10\.0\.3\.1/$SD_UNICAST/" \
    -e "s/224\.244\.224\.245/$SD_MULTICAST/" \
    -e "s/\"port\" : \"30491\"/\"port\" : \"$SD_PORT\"/" \
    -e "s/\"unreliable\" : \"30710\"/\"unreliable\" : \"$RX_PORT\"/" \
    "$PREFIX/etc/sd_daemon.json" > "${WORK}sd_daemon.json"

for process in $STOP_PROCESSES; do
    log "stopping $process (until it is restarted, e.g. by a power cycle)"
    slay -f "$process" || log "$process was not running"
done

if [ "$DISABLE_FIREWALL" -eq 1 ]; then
    log "disabling pf (the reference measurement must use the same setting)"
    pfctl -d
fi

if [ -e /dev/io-sock/mods-someip-event.so ]; then
    log "module already loaded (generic io-sock modules cannot be unloaded)"
else
    log "loading $PREFIX/bin/mods-someip-event.so into io-sock"
    mount -T io-sock "$PREFIX/bin/mods-someip-event.so" || exit 1
fi
attempt=0
until slog2info | grep -F "$MODULE_LOG" | grep -q "event task started"; do
    attempt=$((attempt + 1))
    if [ $attempt -gt 10 ]; then
        log "module did not become ready"
        slog2info | grep -F io_sock | last 40
        exit 1
    fi
    sleep 1
done

log "starting SD daemon on $SD_UNICAST (SD $SD_MULTICAST:$SD_PORT)"
"$PREFIX/bin/someip_sd_daemon" "${WORK}sd_daemon.json" > "${WORK}sd_daemon.log" 2>&1 &
DAEMON_PID=$!
sleep 1

# CPU load per process during the measurement, sampled every second.
HOGS_PID=""
if command -v hogs > /dev/null 2>&1; then
    hogs -s 1 > "${WORK}hogs.txt" 2>&1 &
    HOGS_PID=$!
fi

"$PREFIX/bin/someip_bench_lola" --label iosock --mw-com-config "$PREFIX/etc/mw_com_config.json" "$@" \
    > "${WORK}bench.txt" 2>&1
BENCH_RC=$?
grep -E "^(BENCH_RESULT|bench:)" "${WORK}bench.txt"

kill $HOGS_PID "$DAEMON_PID" 2>/dev/null
wait "$DAEMON_PID" 2>/dev/null

grep "^BENCH_RESULT" "${WORK}bench.txt" > "${WORK}results.txt"
log "results: ${WORK}results.txt, CPU samples: ${WORK}hogs.txt, logs: ${WORK}*.log"
log "module log:"
slog2info | grep -F "$MODULE_LOG" | last 20
# Secpol logs violations in monitoring and permissive mode; they show the rules missing for io-sock hosting LoLa.
log "secpol violations of io-sock (empty if none were logged):"
slog2info | grep -i secpol | grep -i -E "io_net_stack|io_sock|io-sock" | last 40
exit $BENCH_RC
