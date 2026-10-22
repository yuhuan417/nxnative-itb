#!/usr/bin/env python3
# Run the B shader library build on a machine without Apple's toolchain (a Linux build): the same translation as dm/gfx/tools/dmsh_build.py (the same translator, the same spirv-cross, the same MSL),
# but the libraries are the MSL source instead of a compiled metallib, and the device compiles them when it loads them (dm/gfx/nvn/nvn_mtl.m lib_from_file: a file that does not start with the metallib byte order mark is MSL source).
# Why a separate script: dm/gfx/tools/dmsh_build.py and dmsh_chain.py are part of the translation chain version (their contents are hashed into it), so changing them would move the chain version of every game profile
#   and need every profile's shader libraries rebuilt. This script leaves them alone: it runs them and replaces only the parts that need Apple's shader compiler.
#   The chain version stays the shared one (dmsh_chain.py computes it from the shared lock file): the translation, the MSL, is the same on every platform; only the way the library is compiled differs.
#   The tool fingerprints of this machine live in dmsh_chain.lock.linux (dmsh_platform.py), so the shared lock file is not touched and the other profiles' chain versions do not move.
# Usage: dmsh_build_linux.py <Switch package dir> <output cache dir> [key...] (the arguments of dmsh_build.py); environment as dmsh_build.py's, plus IOSOUT=dir for the iOS libraries.
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dmsh_chain, dmsh_platform, dmsh_mtlcache   # the chain version, the platform part, the compiled-shader cache

MODE = dmsh_platform.mode()
if MODE != 'source':
    sys.exit('[RED] dmsh_build_linux.py is the build without Apple\'s shader compiler; with one, use dm/gfx/tools/dmsh_build.py')

# The tool fingerprints: this machine has no Metal compiler, so the shared lock does not match it. The platform's own lock file records what this machine has.
dmsh_chain.LOCK = dmsh_platform.lock_file(dmsh_chain.LOCK)

def _tools_now():
    t = {}
    for dll in ('dmsh_xlate.dll', 'Ryujinx.Graphics.Shader.dll', 'Ryujinx.Common.dll'):
        p = os.path.join(dmsh_chain.XLATE_DIR, dll)
        t[dll] = dmsh_chain.sha16(p) if os.path.exists(p) else 'missing'
    t['xlate_source'] = subprocess.run([os.path.join(dmsh_chain.XLATE_DIR, 'dmsh_xlate'), '--source-sha'], capture_output=True, text=True).stdout.strip()
    sc = subprocess.run(['which', 'spirv-cross'], capture_output=True, text=True).stdout.strip()
    t['spirv-cross'] = dmsh_chain.sha16(sc) if sc else 'missing'
    t['metal_macosx'] = t['metal_iphoneos'] = 'missing'   # no Apple shader compiler here
    return t
dmsh_chain.tools_now = _tools_now

# The compiled-shader cache: instead of compiling, the library file gets the MSL source. The cache is keyed by the MSL text, so it still skips a repeated build.
def _compile(self, target, msl, lib):
    shutil.copyfile(msl, lib); self.count((target, 'source'))
dmsh_mtlcache.Cache.compile = _compile
# The cache's compiler identity (it runs xcrun to read the Metal compiler's version): there is none here
dmsh_mtlcache.compiler_id = lambda sdk: 'no metal compiler (the device compiles the MSL)'
# Filling the cache from an earlier build checks that a fresh compile reproduces the earlier library byte for byte; without a compiler there is nothing to check, so the earlier build is not taken
dmsh_mtlcache.Cache.fill = lambda self, mac_dir, ios_dir=None: []

# The compile itself: the library file is the MSL source (the device compiles it when it loads the library)
def source_compile(msl, air, lib):
    shutil.copyfile(msl, lib)

# The record written into the reflection json's chain_metal names the compile convention: the source, not a compiler
class _Globals(dict):
    """The globals dmsh_build.py runs in: the compile convention becomes the source convention, and the compile functions write the MSL source"""
    def __setitem__(self, k, v):
        if k in ('MAC_RECORD', 'RECORD') and isinstance(v, str) and 'metal' in v: v = dmsh_platform.SOURCE_RECORD
        dict.__setitem__(self, k, v)

dmsh_build_path = os.path.join(HERE, 'dmsh_build.py')
sys.argv = [dmsh_build_path] + sys.argv[1:]
# The two compile functions call Apple's compiler (xcrun metal / metallib); here the library file gets the MSL source instead. The replacements are made in the source before it runs,
# so that every call site (the cache, the readable-tile variants) uses them.
src = open(dmsh_build_path, 'rb').read().decode()
old_mac = """def mac_compile(msl, air, lib):   # same interface as ios_metal.compile
    subprocess.run([*MAC_METAL, msl, '-o', air], check=True)
    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', air, '-o', lib], check=True)"""
new_mac = """def mac_compile(msl, air, lib):   # same interface as ios_metal.compile; no Apple shader compiler here: the library file gets the MSL source
    shutil.copyfile(msl, lib)"""
assert old_mac in src, 'dmsh_build.py: the mac_compile definition changed'
src = src.replace(old_mac, new_mac)
old_ios = """    try: MTL.compile('mac', msl, lib)   # MSL identical to one compiled before takes that library (dmsh_mtlcache.py)"""
new_ios = """    try: shutil.copyfile(msl, lib)   # no Apple shader compiler here: the library file gets the MSL source (the device compiles it)"""
assert old_ios in src, 'dmsh_build.py: the mac compile call changed'
src = src.replace(old_ios, new_ios)
# The iOS libraries: the same source convention
old_ios2 = """        try: MTL.compile('ios', msl, os.path.join(IOSOUT, key + '_b.metallib'))"""
new_ios2 = """        try: shutil.copyfile(msl, os.path.join(IOSOUT, key + '_b.metallib'))   # no Apple shader compiler here: the device compiles it"""
assert old_ios2 in src, 'dmsh_build.py: the iOS compile call changed'
src = src.replace(old_ios2, new_ios2)
old_ios3 = """                    ios_metal.compile(fmsl, fmsl[:-6] + '.ios.air', ilib + '.tmp')"""
new_ios3 = """                    shutil.copyfile(fmsl, ilib + '.tmp')   # no Apple shader compiler here: the device compiles it"""
assert old_ios3 in src, 'dmsh_build.py: the iOS variant compile call changed'
src = src.replace(old_ios3, new_ios3)
# The record written into the reflection json's chain_metal names the compile convention
src = src.replace("MAC_RECORD = ' '.join(MAC_METAL) + '; xcrun -sdk macosx metallib'",
                  "MAC_RECORD = 'msl source (the device compiles it)'")
src = src.replace("""    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../ios/tools')); import ios_metal""",
                  """    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../ios/tools')); import ios_metal; ios_metal.RECORD = 'msl source (the device compiles it)'""")
g = _Globals({'__name__': '__main__', '__file__': dmsh_build_path, '__builtins__': __builtins__})
exec(compile(src, dmsh_build_path, 'exec'), g)
