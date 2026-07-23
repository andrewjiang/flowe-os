# M2.1c flash diet — PlatformIO extra_script (post:).
#
# PlatformIO routes bare `-flto` from build_flags into CCFLAGS only (SCons
# ParseFlags has no LINKFLAGS mapping for -f* except a small allowlist), so
# the link step never sees it: ld then hits the slim LTO objects without the
# linker plugin ("plugin needed to handle lto object" ... undefined reference
# to `app_main`). Appending -flto to LINKFLAGS makes the gcc driver load
# liblto_plugin at link time, which both reads the LTO objects and runs the
# cross-TU optimization.
Import("env")
env.Append(LINKFLAGS=["-flto"])
