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

# Mirrors the upstream CMake build (default options: no TLS, DLT, SOME/IP-TP, packet filtering).
_COPTS = ["-w"] + select({
    # CMake builds QNX with GNU extensions enabled.
    "@platforms//os:qnx": ["-D_QNX_SOURCE"],
    "//conditions:default": [],
})

_DEFINES = select({
    "@platforms//os:qnx": ["QNX"],
    "//conditions:default": ["LINUX"],
}) + [
    "APPLICATION_MGMT_ENTRY=0xF0",
    "SOMEIP_UDP_MAX_PAYLOAD_SIZE=1400",
]

_LOCAL_DEFINES = ['SOMEIP_VERSION=\\"0.9.0\\"']

_LINUX_OR_QNX = select({
    "@platforms//os:linux": [],
    "@platforms//os:qnx": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

_LINKOPTS = select({
    "@platforms//os:qnx": ["-lsocket"],
    "//conditions:default": ["-lpthread"],
})

_INCLUDES = [
    "include",
    "src/osabstraction",
    "src/someip",
]

_MESSAGE_PASSING_SRCS = glob(["src/osabstraction/messagepassing/*.cpp"]) + [
    "src/someip/endpoint/EndpointMessagePassing.cpp",
]

_COMMON_SRCS = glob(
    [
        "src/osabstraction/**/*.cpp",
        "src/someip/**/*.cpp",
    ],
    exclude = _MESSAGE_PASSING_SRCS + [
        "src/osabstraction/multiplex/*.cpp",
        "src/osabstraction/socket/SecureConnector.cpp",
        "src/someip/packetrouter/PacketRouterHost.cpp",
        "src/someip/runtime/ServiceManager.cpp",
    ],
) + select({
    "@platforms//os:qnx": ["src/osabstraction/multiplex/MultiplexerSelect.cpp"],
    "//conditions:default": ["src/osabstraction/multiplex/MultiplexerEpoll.cpp"],
})

_MESSAGE_PASSING_HDRS = glob(["src/osabstraction/messagepassing/*.h"]) + [
    "src/someip/endpoint/EndpointMessagePassing.h",
]

_COMMON_HDRS = glob(
    [
        "include/lgsomeip/*.h",
        "src/osabstraction/**/*.h",
        "src/someip/**/*.h",
    ],
    exclude = _MESSAGE_PASSING_HDRS,
)

cc_library(
    name = "e2e_protection",
    srcs = glob(["third_party/e2e_protection/src/**/*.cpp"]),
    hdrs = glob(["third_party/e2e_protection/include/**/*.hpp"]),
    copts = _COPTS,
    includes = ["third_party/e2e_protection/include"],
    target_compatible_with = _LINUX_OR_QNX,
)

# Application library (lgsomeip::api) including the OS abstraction. Like the CMake default, local IPC uses QNX
# message passing on QNX and Unix domain sockets on Linux.
cc_library(
    name = "lgsomeip",
    srcs = _COMMON_SRCS + select({
        "@platforms//os:qnx": _MESSAGE_PASSING_SRCS,
        "//conditions:default": [],
    }),
    hdrs = _COMMON_HDRS + select({
        "@platforms//os:qnx": _MESSAGE_PASSING_HDRS,
        "//conditions:default": [],
    }),
    copts = _COPTS,
    defines = _DEFINES + select({
        "@platforms//os:qnx": ["ENABLE_QNX_MESSAGE_PASSING"],
        "//conditions:default": [],
    }),
    includes = _INCLUDES,
    linkopts = _LINKOPTS,
    local_defines = _LOCAL_DEFINES,
    target_compatible_with = _LINUX_OR_QNX,
    visibility = ["//visibility:public"],
    deps = [
        ":e2e_protection",
        "@rapidjson",
    ],
)

# lgsomeip-daemon-core: ServiceManager and PacketRouterHost. The SD-only daemon injects its own ServiceRouter, so
# PacketRouterHost is linked but unused there.
cc_library(
    name = "daemon_core",
    srcs = [
        "src/someip/packetrouter/PacketRouterHost.cpp",
        "src/someip/runtime/ServiceManager.cpp",
    ],
    copts = _COPTS,
    local_defines = _LOCAL_DEFINES,
    target_compatible_with = _LINUX_OR_QNX,
    visibility = ["//visibility:public"],
    deps = [":lgsomeip"],
)

# Upstream routing daemon (someip-daemon), only used as test peer representing a remote ECU.
cc_binary(
    name = "someipd",
    srcs = ["daemon/someipd.cpp"],
    copts = _COPTS,
    local_defines = _LOCAL_DEFINES,
    target_compatible_with = _LINUX_OR_QNX,
    visibility = ["@@//experimental/someip_sd/test:__pkg__"],
    deps = [":daemon_core"],
)
