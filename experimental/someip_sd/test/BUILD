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

load("@rules_cc//cc:defs.bzl", "cc_binary")
load("@rules_pkg//pkg:mappings.bzl", "pkg_filegroup")
load("@rules_python//python:defs.bzl", "py_test")
load("//quality/integration_testing:integration_testing.bzl", "dual_qemu_integration_test", "integration_test")
load("//score/mw:common_features.bzl", "COMPILER_WARNING_FEATURES")
load("//score/mw/com/test:pkg_application.bzl", "pkg_application")

# Integration tests: the SD side (10.0.3.1, event module + SD daemon + test client / test provider) and the peer side
# (10.0.3.2, lg-someip someipd + provider / consumer) run the same scenario (someip_sd_scenario.py) on two QNX QEMU VMs
# or two Linux Docker containers.

_LINUX_OR_QNX = select({
    "@platforms//os:linux": [],
    "@platforms//os:qnx": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

exports_files(
    ["config/mw_com_config.json"],
    visibility = ["//experimental/someip_sd/iosock_event_module:__pkg__"],
)

exports_files(
    ["dual_qemu_config.json"],
    visibility = ["//experimental/someip_sd/benchmark:__pkg__"],
)

# Remote provider on the upstream lg-someip stack; needs @lg_someip//:someipd running alongside.
cc_binary(
    name = "someip_sd_peer",
    srcs = ["peer/peer_provider.cpp"],
    features = COMPILER_WARNING_FEATURES,
    target_compatible_with = _LINUX_OR_QNX,
    deps = ["@lg_someip//:lgsomeip"],
)

# Remote consumer of someip_sd_test_provider on the upstream lg-someip stack; needs @lg_someip//:someipd alongside.
cc_binary(
    name = "someip_sd_peer_consumer",
    srcs = ["peer/peer_consumer.cpp"],
    features = COMPILER_WARNING_FEATURES,
    target_compatible_with = _LINUX_OR_QNX,
    deps = ["@lg_someip//:lgsomeip"],
)

pkg_application(
    name = "sd_apps",
    app_name = "someip_sd",
    bin = [
        "//experimental/someip_sd:someip_sd_daemon",
        "//experimental/someip_sd:someip_sd_test_client",
        "//experimental/someip_sd:someip_sd_test_provider",
    ] + select({
        "@platforms//os:qnx": ["//experimental/someip_sd/iosock_event_module:mods_someip_event"],
        "//conditions:default": ["//experimental/someip_sd:someip_event_module"],
    }),
    etc = [
        "config/logging.json",
        "config/mw_com_config.json",
        "config/sd_daemon.json",
    ],
)

pkg_application(
    name = "peer_apps",
    app_name = "someip_sd_peer",
    bin = [
        ":someip_sd_peer",
        ":someip_sd_peer_consumer",
        "@lg_someip//:someipd",
    ],
    etc = ["config/lg_peer.json"],
)

pkg_filegroup(
    name = "all_apps",
    srcs = [
        ":peer_apps",
        ":sd_apps",
    ],
)

dual_qemu_integration_test(
    name = "someip_sd_dual_qemu_test",
    srcs = [
        "someip_sd_scenario.py",
        "test_someip_sd_qemu.py",
    ],
    dual_config = "dual_qemu_config.json",
    filesystem_a = ":sd_apps",
    filesystem_b = ":peer_apps",
)

# Both containers use the same image; the test starts the peer container itself.
integration_test(
    name = "someip_sd_docker_test",
    timeout = "moderate",
    srcs = [
        "someip_sd_scenario.py",
        "test_someip_sd_docker.py",
    ],
    filesystem = ":all_apps",
    target_compatible_with = ["@platforms//os:linux"],
)

py_test(
    name = "scenario_lifecycle_test",
    srcs = [
        "someip_sd_scenario.py",
        "test_scenario_lifecycle.py",
    ],
    imports = ["."],
    main = "test_scenario_lifecycle.py",
)
