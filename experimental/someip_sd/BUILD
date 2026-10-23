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

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load("//score/mw:common_features.bzl", "COMPILER_WARNING_FEATURES")

# Experimental SOME/IP service discovery prototype based on lg-someip's ServiceManager.
# Not part of the product, see notes in the design discussion.

_LINUX_OR_QNX = select({
    "@platforms//os:linux": [],
    "@platforms//os:qnx": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

cc_library(
    name = "sd_protocol",
    hdrs = [
        "protocol/event_module_protocol.h",
        "protocol/sd_protocol.h",
    ],
    features = COMPILER_WARNING_FEATURES,
    visibility = ["//experimental/someip_sd/iosock_event_module:__pkg__"],
)

cc_binary(
    name = "someip_sd_daemon",
    srcs = [
        "daemon/event_module_client.cpp",
        "daemon/event_module_client.h",
        "daemon/main.cpp",
        "daemon/sd_facade.cpp",
        "daemon/sd_facade.h",
        "daemon/sd_only_router.cpp",
        "daemon/sd_only_router.h",
    ],
    features = COMPILER_WARNING_FEATURES,
    target_compatible_with = _LINUX_OR_QNX,
    visibility = ["//experimental/someip_sd/test:__pkg__"],
    deps = [
        ":sd_protocol",
        "//score/message_passing",
        "@lg_someip//:daemon_core",
    ],
)

# User-space stand-in for the io-sock event module: SOME/IP notifications (UDP) -> LoLa events.
cc_binary(
    name = "someip_event_module",
    srcs = [
        "event_module/event_module.cpp",
        "event_module/event_module.h",
        "event_module/main.cpp",
    ],
    features = COMPILER_WARNING_FEATURES,
    linkopts = select({
        "@platforms//os:qnx": ["-lsocket"],
        "//conditions:default": [],
    }),
    target_compatible_with = _LINUX_OR_QNX,
    visibility = ["//experimental/someip_sd/test:__pkg__"],
    deps = [
        ":sd_protocol",
        "//score/message_passing",
        "//score/mw/com",
    ],
)

cc_library(
    name = "sd_client",
    srcs = ["client/sd_client.cpp"],
    hdrs = ["client/sd_client.h"],
    features = COMPILER_WARNING_FEATURES,
    deps = [
        ":sd_protocol",
        "//score/message_passing",
    ],
)

cc_binary(
    name = "someip_sd_test_provider",
    srcs = ["client/test_provider.cpp"],
    features = COMPILER_WARNING_FEATURES,
    target_compatible_with = _LINUX_OR_QNX,
    visibility = ["//experimental/someip_sd/test:__pkg__"],
    deps = [
        ":sd_client",
        ":sd_protocol",
        "//score/mw/com",
    ],
)

cc_binary(
    name = "someip_sd_test_client",
    srcs = ["client/test_client.cpp"],
    features = COMPILER_WARNING_FEATURES,
    target_compatible_with = _LINUX_OR_QNX,
    visibility = ["//experimental/someip_sd/test:__pkg__"],
    deps = [
        ":sd_client",
        ":sd_protocol",
        "//score/mw/com",
    ],
)
