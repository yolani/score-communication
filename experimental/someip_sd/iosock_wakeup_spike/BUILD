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
load("@rules_pkg//pkg:mappings.bzl", "pkg_attributes", "pkg_files")
load("@rules_pkg//pkg:tar.bzl", "pkg_tar")
load("//experimental/someip_sd/iosock_poc:iosock_module.bzl", "iosock_copts", "iosock_sdp_inputs")
load("//quality/integration_testing:integration_testing.bzl", "integration_test")
load("//score/mw:common_features.bzl", "COMPILER_WARNING_FEATURES")
load("//score/mw/com/test:pkg_application.bzl", "pkg_application")

# Spike for the event-driven io-sock event module: which primitive wakes a sleeping worker inside io-sock reliably and
# fast, from a socket upcall, from a std::thread and from a LoLa receive handler? QNX only.

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
    name = "spike_protocol",
    hdrs = ["spike_protocol.h"],
    features = COMPILER_WARNING_FEATURES,
)

# FreeBSD side: worker, primitives, socket upcall.
cc_library(
    name = "spike_module",
    srcs = ["module/spike_module.c"],
    hdrs = ["module/spike_plugin.h"],
    copts = iosock_copts(":qnx_x86_64", ":qnx_arm64"),
    # Only for the location expansion in copts; the toolchain already provides all SDP headers to the compiler.
    data = iosock_sdp_inputs(":qnx_x86_64", ":qnx_arm64"),
    target_compatible_with = _QNX_ONLY,
    alwayslink = True,
)

# POSIX/C++ side: statistics, semaphore, control server, LoLa receive handler.
cc_library(
    name = "spike_plugin",
    srcs = ["module/spike_plugin.cpp"],
    hdrs = ["module/spike_plugin.h"],
    features = COMPILER_WARNING_FEATURES,
    target_compatible_with = _QNX_ONLY,
    deps = [
        ":spike_protocol",
        "//score/message_passing",
        "//score/mw/com",
    ],
    alwayslink = True,
)

cc_shared_library(
    name = "mods_iosock_spike",
    shared_lib_name = "mods-iosock-spike.so",
    target_compatible_with = _QNX_ONLY,
    # cc_shared_library drops the linkopts of baselibs' //third_party/qnx:libfsnotify (inotify for LoLa).
    user_link_flags = ["-lfsnotify"],
    deps = [
        ":spike_module",
        ":spike_plugin",
    ],
)

cc_binary(
    name = "iosock_spike_driver",
    srcs = ["driver/spike_driver.cpp"],
    features = COMPILER_WARNING_FEATURES,
    linkopts = ["-lsocket"],
    target_compatible_with = _QNX_ONLY,
    deps = [
        ":spike_protocol",
        "//score/message_passing",
        "//score/mw/com",
    ],
)

pkg_application(
    name = "iosock_spike_app",
    app_name = "iosock_spike",
    bin = [
        ":iosock_spike_driver",
        ":mods_iosock_spike",
    ],
    etc = ["config/mw_com_config.json"],
)

# For a QNX target: unpack to a writable and executable location, then
# mount -T io-sock <dir>/iosock_spike/bin/mods-iosock-spike.so and run iosock_spike_driver <dir>/iosock_spike/etc/...
pkg_files(
    name = "bundle_bin",
    srcs = [
        ":iosock_spike_driver",
        ":mods_iosock_spike",
    ],
    attributes = pkg_attributes(mode = "0755"),
    prefix = "iosock_spike/bin",
)

pkg_files(
    name = "bundle_etc",
    srcs = ["config/mw_com_config.json"],
    prefix = "iosock_spike/etc",
)

pkg_tar(
    name = "bundle",
    srcs = [
        ":bundle_bin",
        ":bundle_etc",
    ],
    extension = "tar.gz",
    target_compatible_with = _QNX_ONLY,
)

# One QEMU per combination: a primitive that hangs io-sock would hide the results of the following runs.
# Taskqueue and msleep/wakeup hang io-sock when signaled from non-kernel threads, so these combinations are manual.
# The QEMU instances share a host port: run with --local_test_jobs=1.
[
    integration_test(
        name = "iosock_wakeup_spike_{}_{}_test".format(primitive, source),
        srcs = ["test_iosock_wakeup_spike.py"],
        filesystem = ":iosock_spike_app",
        tags = [] if ((primitive == "semaphore") or (source == "socket_upcall")) else ["manual"],
        target_compatible_with = ["@platforms//os:qnx"],
    )
    for primitive in [
        "semaphore",
        "taskqueue",
        "msleep_wakeup",
    ]
    for source in [
        "socket_upcall",
        "thread",
        "lola_handler",
    ]
]
