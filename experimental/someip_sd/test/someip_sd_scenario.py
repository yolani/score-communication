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
"""SOME/IP-SD prototype scenario shared by the QNX (dual QEMU) and Linux (Docker) integration tests.

The SD side (10.0.3.1) runs the event module (io-sock on QNX, a process on Linux), someip_sd_daemon and the test
client; the peer side (10.0.3.2) runs the
upstream lg-someip someipd with a provider offering 0x1234.0x5678 and notifying event 0x8778 every second. The test
client receives the notifications as LoLa events from the event module.
"""

import logging
import time

logger = logging.getLogger(__name__)

SD_DIR = "/opt/someip_sd"
PEER_DIR = "/opt/someip_sd_peer"
CLIENT_RUNTIME_SECONDS = 15
# The client unsubscribes after its runtime and checks for late events (grace 0.5 s + check 3 s).
CLIENT_TIMEOUT_SECONDS = CLIENT_RUNTIME_SECONDS + 15
# The provider must outlive the RX check and the peer consumer's subscribe / unsubscribe phase; its StopOffer is
# checked by the peer consumer at the end.
PROVIDER_RUNTIME_SECONDS = 45
PEER_CONSUMER_TIMEOUT_SECONDS = 90
EVENT_MODULE_LOG = "/tmp/someip_sd_event_module.log"
# The io-sock module logs to slog2; its messages carry this prefix.
IOSOCK_MODULE_LOG_COMMAND = 'slog2info | grep -F "[someip_event]"'


class ConsoleSide:
    """QNX VM driven through its ITF serial console."""

    def __init__(self, console):
        self._console = console
        self._pid_files = []

    def sh(self, command, timeout=30):
        return self._console.run_sh_cmd_output(command, timeout=timeout)

    def start(self, command):
        # Subshell, since the console wrapper appends further commands and a trailing '&;' is a syntax error.
        pid_file = f"/tmp/someip_sd_process_{len(self._pid_files)}.pid"
        self._pid_files.append(pid_file)
        _sh(self, f"( {command} & echo $! > {pid_file} )")

    def cleanup(self):
        errors = []
        for pid_file in reversed(self._pid_files):
            rc, out = self.sh(
                f"( if [ -r {pid_file} ]; then read pid < {pid_file};"
                ' if kill -0 "$pid" 2>/dev/null; then kill -TERM "$pid" || exit $?; fi;'
                f" rm {pid_file}; fi )"
            )
            if rc != 0:
                errors.append(f"{pid_file}: {out}")
        self._pid_files.clear()
        if errors:
            raise RuntimeError("QNX application cleanup failed:\n" + "\n".join(errors))

    def start_event_module(self):
        _sh(self, f"mount -T io-sock {SD_DIR}/bin/mods-someip-event.so")
        deadline = time.monotonic() + 10
        out = ""
        while time.monotonic() < deadline:
            rc, out = self.sh(IOSOCK_MODULE_LOG_COMMAND)
            if rc == 0 and "event task started" in out:
                return
            time.sleep(0.1)
        raise RuntimeError(f"io-sock event module did not become ready:\n{out}")

    def event_module_log(self):
        return _sh(self, IOSOCK_MODULE_LOG_COMMAND)

    def check_event_module(self, log):
        assert "event task started" in log, "QNX did not use the io-sock module"
        assert "RX direct mbuf-to-LoLa:" in log, "kernel RX did not write directly into LoLa"
        assert "TX MEXTADD: first notification" in log, "kernel TX did not use MEXTADD"
        assert "TX MEXTADD: first LoLa sample reference released" in log, "mbuf sample references were not released"
        _sh(self, "ls -d /dev/socket /dev/io-sock/mods-someip-event.so")
        # This SDK cannot detach generic io-sock modules. The module stays resident until VM teardown.


class ContainerSide:
    """Docker container driven through docker exec."""

    def __init__(self, container):
        self._container = container

    def sh(self, command, timeout=30):
        del timeout  # docker exec has no timeout; all commands of the scenario bound themselves.
        rc, out = self._container.exec_run(["/bin/sh", "-c", command])
        return rc, out.decode(errors="replace")

    def start(self, command):
        self._container.exec_run(["/bin/sh", "-c", f"{command}"], detach=True)

    def start_event_module(self):
        self.start(f"{SD_DIR}/bin/someip_event_module {SD_DIR}/etc/mw_com_config.json > {EVENT_MODULE_LOG} 2>&1")

    def event_module_log(self):
        return _sh(self, f"cat {EVENT_MODULE_LOG}")

    def check_event_module(self, log):
        del log

    def cleanup(self):
        pass  # Container lifetime and process cleanup are managed by the Docker fixtures.


def _sh(side, command, timeout=30):
    rc, out = side.sh(command, timeout=timeout)
    if (rc != 0):
        raise RuntimeError(f"Command '{command}' failed with rc={rc}: {out}")
    return out


def run(sd_side, peer_side):
    succeeded = False
    try:
        _run(sd_side, peer_side)
        succeeded = True
    finally:
        errors = []
        for side in (sd_side, peer_side):
            try:
                side.cleanup()
            except RuntimeError as error:
                errors.append(str(error))
                logger.error("%s", error)
        if succeeded and errors:
            raise RuntimeError("\n".join(errors))


def _run(sd_side, peer_side):
    # Background output must not reach the QNX console, it would interleave with the command parsing of the ITF.
    peer_side.start(f"{PEER_DIR}/bin/someipd {PEER_DIR}/etc/lg_peer.json > /tmp/someip_sd_someipd.log 2>&1")
    _sh(peer_side, "sleep 1")
    peer_side.start(f"{PEER_DIR}/bin/someip_sd_peer {PEER_DIR}/etc/lg_peer.json > /tmp/someip_sd_peer.log 2>&1")
    sd_side.start_event_module()
    _sh(sd_side, "sleep 1")
    sd_side.start(f"{SD_DIR}/bin/someip_sd_daemon {SD_DIR}/etc/sd_daemon.json > /tmp/someip_sd_daemon.log 2>&1")
    _sh(sd_side, "sleep 1")
    sd_side.start(
        f"{SD_DIR}/bin/someip_sd_test_provider --runtime {PROVIDER_RUNTIME_SECONDS}"
        f" --mw-com-config {SD_DIR}/etc/mw_com_config.json > /tmp/someip_sd_provider.log 2>&1"
    )

    out = _sh(
        sd_side,
        f"timeout -s TERM {CLIENT_TIMEOUT_SECONDS} {SD_DIR}/bin/someip_sd_test_client"
        " --service 0x1234 --instance 0x5678 --eventgroup 0x4465 --event 0x8778"
        f" --runtime {CLIENT_RUNTIME_SECONDS} --mw-com-config {SD_DIR}/etc/mw_com_config.json",
        timeout=CLIENT_TIMEOUT_SECONDS + 30,
    )

    # expected log outputs from dummy client application
    assert "registered at SD daemon" in out, "test client could not register at the SD daemon"
    assert "0x1234.0x5678 available" in out, "service was not reported available"
    assert "eventgroup 0x4465 acknowledged" in out, "subscription was not acknowledged"
    assert "LoLa: found someip/1234/5678" in out, "event module did not offer the LoLa service"
    assert "event: service 0x1234 event 0x8778" in out, "no event received from the peer via LoLa"
    assert "unsubscribed eventgroup 0x4465" in out, "test client did not unsubscribe"
    assert "result: OK, no events after unsubscribe" in out, "events still received after unsubscribe"

    # TX: the peer consumer subscribes the service offered by the test provider via the SD daemon; the event module
    # sends the provider's LoLa samples as SOME/IP notifications.
    peer_out = _sh(
        peer_side,
        f"timeout -s TERM {PEER_CONSUMER_TIMEOUT_SECONDS} {PEER_DIR}/bin/someip_sd_peer_consumer"
        f" {PEER_DIR}/etc/lg_peer.json",
        timeout=PEER_CONSUMER_TIMEOUT_SECONDS + 30,
    )
    _sh(sd_side, "sleep 1")
    module_log = sd_side.event_module_log()
    print(f"\n[peer consumer]\n{peer_out}\n[event module]\n{module_log}")
    sd_side.check_event_module(module_log)

    assert "service 0x2345.0x0001 available" in peer_out, "provided service not discovered by the peer"
    assert "received 5 event(s)" in peer_out, "no notifications from the event module"
    assert "no events after unsubscribe" in peer_out, "notifications still sent after unsubscribe"
    assert "service not available after stop offer" in peer_out, "StopOffer of the provider not seen"
    assert "result: OK" in peer_out, "peer consumer failed"
    assert "sent initial field value of event 0x8001" in module_log, "no initial field value for the new subscriber"
    if "stopped providing 0x2345.0x0001" not in module_log:
        daemon_log = _sh(sd_side, "cat /tmp/someip_sd_daemon.log")
        raise AssertionError(
            f"event module still provides the service after the StopOffer\n[event module]\n{module_log}\n"
            f"[SD daemon]\n{daemon_log}"
        )
