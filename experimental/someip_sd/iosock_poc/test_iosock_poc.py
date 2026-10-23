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
"""Loads the PoC module into io-sock and checks LoLa and message_passing from a regular process."""

import logging
import time

logger = logging.getLogger(__name__)

APP_DIR = "/opt/iosock_poc"
MODULE_LOG = "/tmp/iosock_poc.log"


def _execute(target, command):
    rc, out = target.execute(command)
    out = out.decode(errors="replace")
    logger.info("$ %s (rc=%s)\n%s", command, rc, out)
    return rc, out


def test_lola_and_message_passing_in_io_sock(target):
    rc, out = _execute(target, f"mount -T io-sock {APP_DIR}/bin/mods-iosock-poc.so 2>&1")
    if rc != 0:
        _execute(target, f"ls -l {APP_DIR}/bin /proc/boot | grep -E 'mods|slog|io-sock|ldd'")
        _execute(target, "slog2info 2>&1 | tail -40")
    assert rc == 0, f"loading the module into io-sock failed: {out}"
    time.sleep(2)

    rc, consumer_out = _execute(target, f"{APP_DIR}/bin/iosock_poc_consumer {APP_DIR}/etc/mw_com_config.json")
    _, module_log = _execute(target, f"cat {MODULE_LOG}")
    io_sock_rc, _ = _execute(target, "ls -d /dev/socket")
    print(f"\n[module log]\n{module_log}\n[consumer] (rc={rc})\n{consumer_out}")

    assert io_sock_rc == 0, "io-sock is gone"
    assert "module loaded" in module_log, "module was not started by io-sock"
    assert "message_passing: server 'iosock_poc' listening" in module_log, "message_passing server failed in io-sock"
    assert "LoLa: service 'iosock/poc' offered" in module_log, "LoLa offer failed in io-sock"
    assert "message_passing: reply 42 OK" in consumer_out, "message_passing request to the module failed"
    assert "LoLa: received 5 sample(s)" in consumer_out, "LoLa samples from the module not received"
    assert "kernel socket: 5 echo(s), 5 datagram(s) via LoLa" in consumer_out, "kernel socket path failed"
    assert rc == 0, "consumer failed"
