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
"""Exercise kernel mbuf transport and the unchanged control protocol inside io-sock."""

import logging
import time

logger = logging.getLogger(__name__)

MODULE_LOG_COMMAND = 'slog2info | grep -F "[someip_event]"'


def execute(target, command):
    rc, data = target.execute(command)
    output = data.decode(errors="replace")
    logger.info("$ %s (rc=%s)\n%s", command, rc, output)
    return rc, output


def test_kernel_transport(target):
    rc, output = execute(target, "mount -T io-sock /opt/someip_sd/bin/mods-someip-event.so")
    assert rc == 0, output
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        rc, output = execute(target, MODULE_LOG_COMMAND)
        if rc == 0 and "event task started" in output:
            break
        time.sleep(0.1)
    else:
        raise AssertionError(f"module not ready:\n{output}")

    rc, output = execute(target, "timeout -s TERM 40 /opt/someip_sd/bin/transport_probe 2>&1")
    _, module_log = execute(target, MODULE_LOG_COMMAND)
    assert rc == 0, f"{output}\n[module]\n{module_log}"
    assert "result: OK" in output
    assert "RX direct mbuf-to-LoLa:" in module_log
    assert "TX MEXTADD: first LoLa sample reference released" in module_log
    assert "pending TX references: 0" in module_log
    rc, output = execute(target, "ls -d /dev/socket /dev/io-sock/mods-someip-event.so")
    assert rc == 0, output
    # Generic modules cannot be detached by this SDK; the isolated VM owns the module lifetime.
