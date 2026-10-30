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
"""SOME/IP-SD prototype on QNX: two QEMU VMs connected by the dual_qemu inter-VM network."""

import someip_sd_scenario as scenario


def test_service_discovery_and_events(console_a, console_b):
    scenario.run(scenario.ConsoleSide(console_a), scenario.ConsoleSide(console_b))
