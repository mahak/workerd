# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_file")

TAG_NAME = "0.56.0"
URL = "https://github.com/bazelbuild/rules_rust/releases/download/0.56.0/cargo-bazel-x86_64-unknown-linux-gnu"
SHA256 = "53418ae5457040e84009f7c69b070e1f12f10a9a3a3ef4f5d0829c66e5438ba1"

def dep_cargo_bazel_linux_x64():
    http_file(
        name = "cargo_bazel_linux_x64",
        url = URL,
        executable = True,
        sha256 = SHA256,
    )
