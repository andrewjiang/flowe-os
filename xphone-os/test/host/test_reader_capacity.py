#!/usr/bin/env python3
"""Build and run real reader failure tests: legacy.fbp compact.fbp output-directory [x3|x4]."""
from pathlib import Path
import subprocess,sys
ROOT=Path(__file__).resolve().parents[4]
OS=ROOT/'firmware/xphone-os'
def main(old,new,out,device="x4"):
 out.mkdir(parents=True,exist_ok=True)
 flags=['-O1','-g','-fsanitize=address,undefined']
 if device=='x3': flags += ['-DFLOWE_TEST_DISPLAY_X3=1']
 includes=['-I'+str(OS/p) for p in ('test/host/compact_stubs','test/host/sync_stubs','lib/uzlib/src','lib/EpdFontCore','lib/Utf8','lib/FbpCompact')]
 common=['c++','-std=c++17',*flags,'-DFLOWE_TEST_FBP_IO',*includes]
 subprocess.run(['clang',*flags,'-c',str(OS/'lib/uzlib/src/tinflate.c'),'-I'+str(OS/'lib/uzlib/src'),'-o',str(out/'tinflate.o')],check=True)
 subprocess.run([*common,'-include',str(OS/'test/host/compact_stubs/ReaderAllocationFaults.h'),'-c',str(OS/'src/reader/FbpBook.cpp'),'-o',str(out/'FbpBook.o')],check=True)
 subprocess.run([*common,str(OS/'src/Gfx.cpp'),str(OS/'lib/Utf8/Utf8.cpp'),str(OS/'test/host/reader_capacity_test.cpp'),str(out/'FbpBook.o'),str(out/'tinflate.o'),'-o',str(out/'reader-capacity-test')],check=True)
 subprocess.run([str(out/'reader-capacity-test'),str(old),str(new)],check=True)
if __name__=='__main__':
 if len(sys.argv) not in (4,5) or (len(sys.argv)==5 and sys.argv[4] not in ('x3','x4')):raise SystemExit(__doc__)
 main(Path(sys.argv[1]),Path(sys.argv[2]),Path(sys.argv[3]),sys.argv[4] if len(sys.argv)==5 else 'x4')
