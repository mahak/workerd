# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_archive")

TAG_NAME = "v7.3.0"
URL = "https://github.com/simdutf/simdutf/releases/download/v7.3.0/singleheader.zip"
STRIP_PREFIX = ""
SHA256 = "512374f8291d3daf102ccd0ad223b1a8318358f7c1295efd4d9a3abbb8e4b6ff"
TYPE = "zip"

def dep_simdutf():
    http_archive(
        name = "simdutf",
        url = URL,
        strip_prefix = STRIP_PREFIX,
        type = TYPE,
        sha256 = SHA256,
        build_file = "//:build/BUILD.simdutf",
    )
