#!/usr/bin/env python3
"""Build and run the minPageBufferBytes host test.

Self-contained: the test synthesizes its own fmt_ver 9 compact and v3 packages,
so no bookc-built fixture is required. Mirrors the reader's radio-yield
decision — the smallest contiguous block a package needs to open at all.
"""
from pathlib import Path
import subprocess

OS = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent / "build"
OUT.mkdir(exist_ok=True)

flags = ["-O1", "-g", "-fsanitize=address,undefined"]
includes = ["-I" + str(OS / p) for p in (
    "test/host/compact_stubs", "test/host/sync_stubs",
    "lib/uzlib/src", "lib/EpdFontCore", "lib/FbpCompact")]
common = ["c++", "-std=c++17", *flags, "-DFLOWE_TEST_FBP_IO", *includes]

subprocess.run(["cc", "-c", str(OS / "lib/uzlib/src/tinflate.c"),
                "-I" + str(OS / "lib/uzlib/src"),
                "-o", str(OUT / "tinflate.o")], check=True)
subprocess.run([*common, "-c", str(OS / "src/reader/FbpBook.cpp"),
                "-o", str(OUT / "FbpBook.o")], check=True)
subprocess.run([*common, str(OS / "test/host/min_page_buffer_test.cpp"),
                str(OUT / "FbpBook.o"), str(OUT / "tinflate.o"),
                "-o", str(OUT / "min-page-buffer-test")], check=True)
subprocess.run([str(OUT / "min-page-buffer-test")], check=True)
