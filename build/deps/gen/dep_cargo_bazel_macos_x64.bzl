# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_file")

TAG_NAME = "0.56.0"
URL = "https://github.com/bazelbuild/rules_rust/releases/download/0.56.0/cargo-bazel-x86_64-apple-darwin"
SHA256 = "9e8a92674771982f68031d0cc516ba08b34dd736c4f818d51349547f2136d012"

def dep_cargo_bazel_macos_x64():
    http_file(
        name = "cargo_bazel_macos_x64",
        url = URL,
        executable = True,
        sha256 = SHA256,
    )
