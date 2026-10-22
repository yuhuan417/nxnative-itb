#!/usr/bin/env python3
# The platform part of the shader translation chain: a build machine without Apple's toolchain (a Linux build) has no Metal compiler, so the shader libraries it produces are the MSL source
# and the device compiles them when it loads them (dm/gfx/nvn/nvn_mtl.m lib_from_file: a file that does not start with the metallib byte order mark is MSL source).
#   This module is deliberately NOT part of the chain version's file list (dm/gfx/tools/dmsh_chain.py FILES): it decides how the libraries are compiled, not what they contain. The chain version
#   still moves for a build on a different toolchain, because the tool fingerprints are part of it (the lock file), but the other profiles' versions do not move when this module changes.
#   The tool fingerprints of a platform without Apple's tools live in their own lock file (dmsh_chain.lock.<platform>), so the shared lock file stays as it is and a Linux build does not pretend
#   to have a Metal compiler. A profile whose libraries were built on another platform needs its own rebuild; the chain version names which build a host accepts.
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PLATFORMS = ('linux',)
PLATFORM = os.environ.get('DMSH_PLATFORM') or ('linux' if sys.platform.startswith('linux') else 'mac')
# The record written into the reflection json's chain_metal: the compile convention the libraries were made with
SOURCE_RECORD = 'msl source (the device compiles it)'
METAL_RECORD = None   # the Metal compiler's own record, filled in by the caller (the mac and iOS conventions differ)


def has_xcrun():
    return subprocess.run(['which', 'xcrun'], capture_output=True, text=True).stdout.strip() != ''


def mode():
    """The library build mode: 'metal' (Apple's shader compiler makes the libraries) or 'source' (the MSL source is shipped and the device compiles it)"""
    m = os.environ.get('DMSH_SHADER_MODE', 'auto')
    if m == 'auto': m = 'metal' if has_xcrun() else 'source'
    if m not in ('source', 'metal'):
        sys.exit(f'[RED] DMSH_SHADER_MODE must be source or metal, not {m!r}')
    return m


def lock_file(shared):
    """The lock file of this platform: the platform's own one when it has it (or is about to write it), else the shared one"""
    p = os.path.join(HERE, f'dmsh_chain.lock.{PLATFORM}') if PLATFORM in PLATFORMS else None
    return p if p and (os.path.exists(p) or '--update-lock' in sys.argv) else shared
