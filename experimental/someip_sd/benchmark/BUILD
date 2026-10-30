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
load("@rules_pkg//pkg:mappings.bzl", "pkg_attributes", "pkg_files")
load("@rules_pkg//pkg:tar.bzl", "pkg_tar")
load("//quality/integration_testing:integration_testing.bzl", "dual_qemu_integration_test")
load("//score/mw:common_features.bzl", "COMPILER_WARNING_FEATURES")
load("//score/mw/com/test:pkg_application.bzl", "pkg_application")

# SOME/IP round-trip benchmark: the benchmarked side publishes "ping" and receives "pong"; a remote reflector on
# lg-someip echoes every ping. The wire contract (bench_protocol.h) is shared with the benchmark applications of
# other stacks (e.g. an AUTOSAR ara::com stack), so only the benchmarked side differs between measurements.

_LINUX_OR_QNX = select({
    "@platforms//os:linux": [],
    "@platforms//os:qnx": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

cc_library(
    name = "bench_protocol",
    hdrs = ["bench_protocol.h"],
    features = COMPILER_WARNING_FEATURES,
)

cc_binary(
    name = "someip_bench_lola",
    srcs = ["bench_lola.cpp"],
    features = COMPILER_WARNING_FEATURES,
    target_compatible_with = _LINUX_OR_QNX,
    deps = [
        ":bench_protocol",
        "//experimental/someip_sd:sd_client",
        "//experimental/someip_sd:sd_protocol",
        "//score/mw/com",
    ],
)

# Needs @lg_someip//:someipd running alongside.
cc_binary(
    name = "someip_bench_reflector",
    srcs = ["bench_reflector.cpp"],
    features = COMPILER_WARNING_FEATURES,
    # Runs on Linux peers with an older libstdc++ than the toolchain's; -static-libstdc++ is not enough, the
    # toolchain adds -lstdc++ itself. Static glibc < 2.34 needs all of libpthread, else std::condition_variable crashes.
    linkopts = select({
        "@platforms//os:linux": [
            "-static",
            "-Wl,--whole-archive",
            "-lpthread",
            "-Wl,--no-whole-archive",
        ],
        "//conditions:default": [],
    }),
    target_compatible_with = _LINUX_OR_QNX,
    deps = [
        ":bench_protocol",
        "@lg_someip//:lgsomeip",
    ],
)

# The io-sock module reads <prefix>/etc/mw_com_config.json next to its <prefix>/bin directory.
pkg_application(
    name = "bench_sd_apps",
    app_name = "someip_sd",
    bin = [
        ":someip_bench_lola",
        "target/run_target.sh",
        "//experimental/someip_sd:someip_sd_daemon",
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
    name = "bench_peer_apps",
    app_name = "someip_bench_peer",
    bin = [
        ":someip_bench_reflector",
        "@lg_someip//:someipd",
    ],
    etc = ["config/lg_reflector.json"],
)

dual_qemu_integration_test(
    name = "someip_bench_dual_qemu_test",
    srcs = ["test_bench_qemu.py"],
    dual_config = "//experimental/someip_sd/test:dual_qemu_config.json",
    filesystem_a = ":bench_sd_apps",
    filesystem_b = ":bench_peer_apps",
)

# Deployment bundles for measurements on a real QNX target and its Linux peer, see target/*.sh.
# Target: bazel build --config=qnx_arm64 :target_bundle; host peer: build :host_bundle for the host's Linux platform.

_EXECUTABLE = pkg_attributes(mode = "0755")

pkg_files(
    name = "target_bin",
    srcs = [
        ":someip_bench_lola",
        "//experimental/someip_sd:someip_sd_daemon",
        "//experimental/someip_sd/iosock_event_module:mods_someip_event",
    ],
    attributes = _EXECUTABLE,
    prefix = "someip_sd/bin",
)

pkg_files(
    name = "target_etc",
    srcs = [
        "config/logging.json",
        "config/mw_com_config.json",
        "config/sd_daemon.json",
    ],
    prefix = "someip_sd/etc",
)

pkg_files(
    name = "target_scripts",
    srcs = ["target/run_target.sh"],
    attributes = _EXECUTABLE,
    prefix = "someip_sd/scripts",
)

pkg_tar(
    name = "target_bundle",
    srcs = [
        ":target_bin",
        ":target_etc",
        ":target_scripts",
    ],
    extension = "tar.gz",
    target_compatible_with = ["@platforms//os:qnx"],
)

pkg_files(
    name = "host_bin",
    srcs = [
        ":someip_bench_reflector",
        "@lg_someip//:someipd",
    ],
    attributes = _EXECUTABLE,
    prefix = "someip_bench_peer/bin",
)

pkg_files(
    name = "host_etc",
    srcs = ["config/lg_reflector.json"],
    prefix = "someip_bench_peer/etc",
)

pkg_files(
    name = "host_scripts",
    srcs = [
        "target/run_host_reflector.sh",
        "target_time.py",
    ],
    attributes = _EXECUTABLE,
    prefix = "someip_bench_peer/scripts",
)

pkg_tar(
    name = "host_bundle",
    srcs = [
        ":host_bin",
        ":host_etc",
        ":host_scripts",
    ],
    extension = "tar.gz",
    target_compatible_with = ["@platforms//os:linux"],
)
