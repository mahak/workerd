# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_archive")

TAG_NAME = "v6.4.0"
URL = "https://github.com/simdutf/simdutf/releases/download/v6.4.0/singleheader.zip"
STRIP_PREFIX = ""
SHA256 = "0ce83666a033ea941d0fed6c36b837ecce5c09a7d86b8a076434fdf5e5a823c2"
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
