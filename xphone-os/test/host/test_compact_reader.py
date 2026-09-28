#!/usr/bin/env python3
"""Build the actual firmware reader/renderer with file/display facades.
Usage: test_compact_reader.py old.fbp compact.fbp profiles output-directory [x3|x4]
The test checks a seek permutation, all words and anchors, and every pixel.
"""
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[4]
OS=ROOT/'firmware/xphone-os'
def run(old,new,profiles,out,device='x4'):
    out.mkdir(parents=True,exist_ok=True)
    flags=['-O2','-g','-fsanitize=address,undefined']
    if device == 'x3': flags += ['-DFLOWE_TEST_DISPLAY_X3=1']
    subprocess.run(['clang',*flags,'-c',str(OS/'lib/uzlib/src/tinflate.c'),'-I'+str(OS/'lib/uzlib/src'),'-o',str(out/'tinflate.o')],check=True)
    subprocess.run(['c++','-std=c++17',*flags,'-DFLOWE_BENCH_COMPACT=1',
        *['-I'+str(OS/p) for p in ('test/host/compact_stubs','test/host/sync_stubs','lib/uzlib/src','lib/EpdFontCore','lib/Utf8','lib/FbpCompact')],
        str(OS/'src/reader/FbpBook.cpp'),str(OS/'src/Gfx.cpp'),str(OS/'lib/Utf8/Utf8.cpp'),str(OS/'test/host/compact_reader_test.cpp'),str(out/'tinflate.o'),'-o',str(out/'firmware-reader-test')],check=True)
    subprocess.run([str(out/'firmware-reader-test'),str(old),str(new),str(profiles)],check=True)
if __name__=='__main__':
    if len(sys.argv) not in (5,6) or (len(sys.argv)==6 and sys.argv[5] not in ('x3','x4')):raise SystemExit(__doc__)
    run(Path(sys.argv[1]),Path(sys.argv[2]),int(sys.argv[3]),Path(sys.argv[4]),sys.argv[5] if len(sys.argv)==6 else 'x4')
