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
"""SOME/IP-SD prototype on Linux: two Docker containers on a bridge network with the dual_qemu addresses."""

import os

import docker
import pytest

import someip_sd_scenario as scenario

SUBNET = "10.0.3.0/24"
# Docker would take .1 as bridge gateway, which is the SD side address.
GATEWAY = "10.0.3.254"
SD_ADDRESS = "10.0.3.1"
PEER_ADDRESS = "10.0.3.2"


@pytest.fixture
def peer_network(request, target):
    client = docker.from_env()
    ipam = docker.types.IPAMConfig(pool_configs=[docker.types.IPAMPool(subnet=SUBNET, gateway=GATEWAY)])
    network = client.networks.create(f"someip_sd_{os.urandom(4).hex()}", driver="bridge", ipam=ipam)
    peer = None
    try:
        network.connect(target.container, ipv4_address=SD_ADDRESS)
        peer = client.containers.run(
            request.config.getoption("docker_image"), "sleep infinity", detach=True, init=True
        )
        network.connect(peer, ipv4_address=PEER_ADDRESS)
        yield scenario.ContainerSide(target.container), scenario.ContainerSide(peer)
    finally:
        if peer is not None:
            peer.remove(force=True)
        try:
            network.disconnect(target.container, force=True)
        finally:
            network.remove()


def test_service_discovery_and_events(peer_network):
    sd_side, peer_side = peer_network
    scenario.run(sd_side, peer_side)
