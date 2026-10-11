load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

licenses(["notice"])

cc_library(
    name = "native_file_logging",
    srcs = ["native_file_logging.cc"],
    hdrs = ["native_file_logging.h"],
    deps = [
        "//sharing/linux:linux_sharing_platform",
        "@com_google_absl//absl/base:log_severity",
        "@com_google_absl//absl/log:globals",
        "@com_google_absl//absl/log:initialize",
        "@com_google_absl//absl/log:log_entry",
        "@com_google_absl//absl/log:log_sink",
        "@com_google_absl//absl/log:log_sink_registry",
        "@com_google_absl//absl/strings:string_view",
    ],
)

cc_test(
    name = "native_file_logging_test",
    size = "small",
    srcs = ["native_file_logging_test.cc"],
    deps = [
        ":native_file_logging",
        "//sharing/linux:linux_sharing_platform",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:globals",
        "@com_google_absl//absl/log:initialize",
        "@com_google_absl//absl/log:log_sink",
        "@com_google_absl//absl/log:log_sink_registry",
        "@com_google_googletest//:gtest_main",
    ],
)

# Qt-free shared library with a plain C API (quickshare.h). The version script
# keeps absl, protobuf and everything else out of the dynamic symbol table.
cc_binary(
    name = "libquickshare.so",
    srcs = [
        "quickshare.cc",
        "quickshare.h",
    ],
    additional_linker_inputs = ["quickshare.map"],
    linkopts = [
        "-Wl,--version-script=$(location quickshare.map)",
        "-Wl,--exclude-libs,ALL",
        "-Wl,-soname,libquickshare.so",
    ],
    linkshared = True,
    visibility = ["//visibility:public"],
    deps = [
        ":native_file_logging",
        "//connections/implementation/flags:connections_flags",
        "//internal/base:file_path",
        "//internal/flags:nearby_flags",
        "//internal/platform/implementation:platform",
        "//sharing:attachments",
        "//sharing:nearby_sharing_service",
        "//sharing:transfer_metadata",
        "//sharing:types",
        "//sharing/flags/generated:generated_flags",
        "//sharing/internal/api:platform",
        "//sharing/linux:linux_sharing_platform",
        "//sharing/proto:enums_cc_proto",
        "@com_google_absl//absl/time",
    ],
)

alias(
    name = "quickshare",
    actual = ":libquickshare.so",
    visibility = ["//visibility:public"],
)
