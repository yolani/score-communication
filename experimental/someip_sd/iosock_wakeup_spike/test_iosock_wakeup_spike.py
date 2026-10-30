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
"""Loads the wakeup spike module into io-sock and measures one primitive / wake source combination.

The combination comes from the test target name: iosock_wakeup_spike_<primitive>_<source>_test.
"""

import logging
import os
import re

logger = logging.getLogger(__name__)

APP_DIR = "/opt/iosock_spike"
MODULE_LOG = "/tmp/iosock_spike.log"
PRIMITIVES = ("semaphore", "taskqueue", "msleep_wakeup")
SOURCES = ("socket_upcall", "thread", "lola_handler")


def _combination():
    name = os.environ["TEST_TARGET"].rsplit(":", 1)[-1]
    for primitive in PRIMITIVES:
        for source in SOURCES:
            if name == f"iosock_wakeup_spike_{primitive}_{source}_test":
                return primitive, source
    raise ValueError(f"unexpected test target {name}")


def _execute(target, command):
    rc, out = target.execute(command)
    out = out.decode(errors="replace")
    logger.info("$ %s (rc=%s)\n%s", command, rc, out)
    return rc, out


def test_wakeup_primitive(target):
    primitive, source = _combination()
    rc, out = _execute(target, f"mount -T io-sock {APP_DIR}/bin/mods-iosock-spike.so 2>&1")
    assert rc == 0, f"loading the module into io-sock failed: {out}"

    _, driver_out = _execute(
        target,
        f"LC_ALL=C {APP_DIR}/bin/iosock_spike_driver {APP_DIR}/etc/mw_com_config.json "
        f"--count 1000 --rate 1000 --rate 0 --primitive {primitive} --source {source} 2>&1",
    )
    _, module_log = _execute(target, f"cat {MODULE_LOG}")
    io_sock_rc, _ = _execute(target, "ls -d /dev/socket")
    print(f"\n[driver]\n{driver_out}\n[module log]\n{module_log}")

    results = re.findall(r"^RESULT .* (OK|FAILED)\s*$", driver_out, re.MULTILINE)
    assert io_sock_rc == 0, "io-sock is gone"
    assert len(results) == 2, "not all runs completed"
    assert all(result == "OK" for result in results), "lost wakeups or a stuck signaling thread"
