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

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_shared_library")
load("//experimental/someip_sd/iosock_poc:iosock_module.bzl", "iosock_copts", "iosock_sdp_inputs")
load("//quality/integration_testing:integration_testing.bzl", "integration_test")
load("//score/mw:common_features.bzl", "COMPILER_WARNING_FEATURES")
load("//score/mw/com/test:pkg_application.bzl", "pkg_application")

_QNX_ONLY = select({
    "@platforms//os:qnx": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

config_setting(
    name = "qnx_x86_64",
    constraint_values = [
        "@platforms//os:qnx",
        "@platforms//cpu:x86_64",
    ],
)

config_setting(
    name = "qnx_arm64",
    constraint_values = [
        "@platforms//os:qnx",
        "@platforms//cpu:arm64",
    ],
)

cc_library(
    name = "log",
    srcs = ["module/log.cpp"],
    hdrs = ["module/log.h"],
    features = COMPILER_WARNING_FEATURES,
    target_compatible_with = _QNX_ONLY,
    alwayslink = True,
)

cc_library(
    name = "kernel_transport",
    srcs = [
        "module/module.c",
    ],
    hdrs = [
        "module/event_plugin.h",
        "module/module.h",
    ],
    copts = iosock_copts(":qnx_x86_64", ":qnx_arm64"),
    data = iosock_sdp_inputs(":qnx_x86_64", ":qnx_arm64"),
    target_compatible_with = _QNX_ONLY,
    deps = [":log"],
    alwayslink = True,
)

cc_library(
    name = "event_plugin",
    srcs = [
        "module/control_channel.cpp",
        "module/control_channel.h",
        "module/event_plugin.cpp",
        "module/kernel_sockets.cpp",
        "module/kernel_sockets.h",
        "module/rx_router.cpp",
        "module/rx_router.h",
        "module/tx_forwarder.cpp",
        "module/tx_forwarder.h",
        "module/types.h",
    ],
    features = COMPILER_WARNING_FEATURES,
    target_compatible_with = _QNX_ONLY,
    deps = [
        ":kernel_transport",
        ":log",
        "//experimental/someip_sd:sd_protocol",
        "//score/message_passing",
        "//score/mw/com",
    ],
    alwayslink = True,
)

cc_shared_library(
    name = "mods_someip_event",
    shared_lib_name = "mods-someip-event.so",
    target_compatible_with = _QNX_ONLY,
    user_link_flags = ["-lfsnotify"],
    visibility = [
        "//experimental/someip_sd/benchmark:__pkg__",
        "//experimental/someip_sd/test:__pkg__",
    ],
    deps = [":event_plugin"],
)

cc_binary(
    name = "transport_probe",
    srcs = ["test/transport_probe.cpp"],
    features = COMPILER_WARNING_FEATURES,
    linkopts = ["-lsocket"],
    target_compatible_with = _QNX_ONLY,
    deps = [
        "//experimental/someip_sd:sd_protocol",
        "//score/message_passing",
        "//score/mw/com",
    ],
)

pkg_application(
    name = "transport_test_app",
    app_name = "someip_sd",
    bin = [
        ":mods_someip_event",
        ":transport_probe",
    ],
    etc = ["//experimental/someip_sd/test:config/mw_com_config.json"],
)

integration_test(
    name = "kernel_transport_test",
    srcs = ["test/test_transport.py"],
    filesystem = ":transport_test_app",
    target_compatible_with = ["@platforms//os:qnx"],
)
