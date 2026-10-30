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
load("//quality/integration_testing:integration_testing.bzl", "integration_test")
load("//score/mw:common_features.bzl", "COMPILER_WARNING_FEATURES")
load("//score/mw/com/test:pkg_application.bzl", "pkg_application")
load(":iosock_module.bzl", "iosock_copts", "iosock_sdp_inputs")

# Feasibility check: do mw::com (LoLa) and score::message_passing work inside an io-sock module, also together with a
# FreeBSD kernel socket served by a kthread? The module (mods-iosock-poc.so) offers a LoLa service and a
# message_passing server from within io-sock and forwards datagrams of its kernel UDP socket to LoLa (and echoes them);
# the consumer is a regular process. QNX only.

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

# FreeBSD module glue, compiled against the SDP's kernel headers.
cc_library(
    name = "io_sock_module",
    srcs = ["module/io_sock_module.c"],
    hdrs = ["module/poc_plugin.h"],
    copts = iosock_copts(":qnx_x86_64", ":qnx_arm64"),
    # Only for the location expansion in copts; the toolchain already provides all SDP headers to the compiler.
    data = iosock_sdp_inputs(":qnx_x86_64", ":qnx_arm64"),
    target_compatible_with = _QNX_ONLY,
    # Only consists of linker sets that io-sock looks up when loading the module.
    alwayslink = True,
)

cc_library(
    name = "poc_protocol",
    hdrs = ["poc_protocol.h"],
    features = COMPILER_WARNING_FEATURES,
)

# C++ part running in io-sock; uses QNX / POSIX APIs only.
cc_library(
    name = "poc_plugin",
    srcs = ["module/poc_plugin.cpp"],
    hdrs = ["module/poc_plugin.h"],
    features = COMPILER_WARNING_FEATURES,
    local_defines = ['IOSOCK_POC_MW_COM_CONFIG=\\"/opt/iosock_poc/etc/mw_com_config.json\\"'],
    target_compatible_with = _QNX_ONLY,
    deps = [
        ":poc_protocol",
        "//score/message_passing",
        "//score/mw/com",
    ],
    alwayslink = True,
)

cc_shared_library(
    name = "mods_iosock_poc",
    shared_lib_name = "mods-iosock-poc.so",
    target_compatible_with = _QNX_ONLY,
    # cc_shared_library drops the linkopts of baselibs' //third_party/qnx:libfsnotify (inotify for LoLa).
    user_link_flags = ["-lfsnotify"],
    deps = [
        ":io_sock_module",
        ":poc_plugin",
    ],
)

cc_binary(
    name = "iosock_poc_consumer",
    srcs = ["consumer/consumer.cpp"],
    features = COMPILER_WARNING_FEATURES,
    linkopts = ["-lsocket"],
    target_compatible_with = _QNX_ONLY,
    deps = [
        ":poc_protocol",
        "//score/message_passing",
        "//score/mw/com",
    ],
)

pkg_application(
    name = "iosock_poc_app",
    app_name = "iosock_poc",
    bin = [
        ":iosock_poc_consumer",
        ":mods_iosock_poc",
    ],
    etc = ["config/mw_com_config.json"],
)

integration_test(
    name = "iosock_poc_test",
    srcs = ["test_iosock_poc.py"],
    filesystem = ":iosock_poc_app",
    target_compatible_with = ["@platforms//os:qnx"],
)
