# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_archive")

TAG_NAME = "0.11.12"
URL = "https://github.com/astral-sh/ruff/releases/download/0.11.12/ruff-aarch64-apple-darwin.tar.gz"
STRIP_PREFIX = "ruff-aarch64-apple-darwin"
SHA256 = "6e8bce88be5063d3378b6fa51430655884794f6cd04a059721839dc012b7ee7d"
TYPE = "tgz"

def dep_ruff_darwin_arm64():
    http_archive(
        name = "ruff-darwin-arm64",
        url = URL,
        strip_prefix = STRIP_PREFIX,
        type = TYPE,
        sha256 = SHA256,
        build_file_content = "filegroup(name='file', srcs=['ruff'], visibility=['//visibility:public'])",
    )
