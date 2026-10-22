# Into The Breach (Switch) on iOS — port report

- profile: `dm/games/010057D00B612000/` (base title ID `010057D00B612000`)
- game: Into The Breach, update 1.2.75 (v458752), SDK 13.3.5, NVN
- build artifact: an unsigned IPA (see "Artifacts" below); the user sideloads it

## 1. What the game is

Subset Games' own engine (a D3D11-style abstraction mapped onto NVN), SDK 13.3.5. The game's files are all in the RomFS; the code is two modules.

| | |
|---|---|
| `main` | the game (`0c5ea0cb96033317fa4a7f37276f1de9`) |
| `sdk` | nnSdk (`3644a6760469451cabc99f00c540acc2`) |
| NPDM program ID | `010057D00B612000` (the base game, so the update runs as the base title) |
| NPDM system resource | 0 → no virtual address memory; the game sizes its heap from the pool minus the used memory |

The RomFS holds 626 files (0.59 GB):

| folder | what |
|---|---|
| `maps/` | 376 `.map` files: Lua tables as text (the mission and tile layouts) |
| `scripts/` | 151 `.lua` files: the game's own source (missions, localization, the pilots' personalities) |
| `resources/resource.dat` | 354.9 MB: a custom pack (2,841 blocks) of 2,690 PNG textures and 151 FONT files |
| `resources/*.bank` | 5 Wwise (FMOD) banks: `Master Bank`, `music`, `sfx`, `ambience` (207 MB of music) |
| `resources/thbrk.tri` | the triangulated title screen mesh |
| `shaders/` | the five GLSLC compiler outputs (2 vertex, 3 pixel) |

There is **no runtime shader compilation**: the game loads `shaders/<name>.vs` and `shaders/<name>.ps` by name and hands them to NVN as they are. No `glslcCompile` import, so the profile needs no `shader_glslc_capture`.

## 2. The shaders

Five files in `RomFS/shaders/`, each one GLSLC compiler output holding a single stage:

| file | stage | key (`dm_ctl_sha16`) |
|---|---|---|
| `color_movie.ps` | pixel | `eeea35b43c2e762c` |
| `color_tex0.ps` | pixel | `7b333184cfccc3ca` |
| `color_tex0_alpha_test.ps` | pixel | `04602ebdc702c28a` |
| `mvppos2_col_tex2.vs` | vertex | `ac357d34b92b19d3` |
| `screenpos2_col_tex2.vs` | vertex | `f902d7971342eb0f` |

A GLSLC output starts with the magic `0x19866891`; its total length is at +0x44; the control section (magic `0x98761234`) is at +0x3f0, its length at control +0x20; the code area follows the control section, with the graphics code magic `0x12345678` and the stage in SPH word 0 (bits 10..13: 1 = vertex, 5 = pixel). The key hashes `sha256(control[:0x880] + code)[:16]`, the same rule the host uses (`hle_nvn.c` `keys_hash`).

`dm/gfx/tools/shader_split.py` learned to recognize a whole file that is one GLSLC output (the `GLSLC_MAGIC` branch); the profile's `shader_groups` maps `/shaders/` to the `shaders` package.

**Bindings.** The pixel shaders bind one texture at slot 0 with one sampler (a fixed slot: the handle is an immediate in the control section, not a bindless handle). The vertex shaders take a constant buffer (`mvppos2_col_tex2.vs`) or nothing (`screenpos2_col_tex2.vs`). No shader reads a driver segment (c0 / c2) at run time, so the profile's `nvn_*` handle-table bases are never filled for this game.

**c1.** All five shaders' constant-section indices are proven bounded, so `c1_full.tsv` is empty (no shader needs its own 64 KiB bank).

## 3. The profile

`dm/games/010057D00B612000/game.tsv`. The values that were guessed and then **checked against the game binary**:

| key | value | how it was checked |
|---|---|---|
| `app_pool` | `0xCD500000` | the constant appears in `main` at +0x71b0c6, in a table of (heap size, used memory) pairs; the neighbours are `0xCD480000`, `0xCD580000`, `0xCD600000` (the same series) and the matching used-memory value is `0x04030000` (64.19 MB) — the host's default `mem_used` |
| `mem_reserve` / `mem_alias` / `mem_stack` | 4 GB / 256 MB / 256 MB | the heap region is then 3.5 GB against the 3.22 GB the game asks for (`pool − 64 MB`), so the request fits |
| `mem_used` | (default 64 MB) | the `0x04030000` in the same table confirms it |
| `nvn_tex_base` etc. | the common values | not exercised: no shader reads a driver segment |

The profile carries one row per build (`v1`, display 1.2.75) and `modules main sdk`.

## 4. What was built, and how

The build ran on Linux, without Xcode and without Apple's shader compiler. Three new pieces make that possible; none of them changes the translation the other games use.

### 4.1 `dm/gfx/tools/shader_split.py`: a whole file that is one GLSLC output

The scanner looked for GLSLC outputs inside larger files. Into The Breach's shaders are standalone files, so a new branch recognizes the magic at the start of the file (`GLSLC_MAGIC`, one stage each).

### 4.2 `dm/gfx/nvn/nvn_mtl.m`: a library file that holds the MSL source

`lib_from_file()` mapped the file and handed it to `newLibraryWithData:`, which needs a compiled metallib. It now tells the two apart by the metallib's byte order mark: a file that does not start with it is MSL source, and the device compiles it with `newLibraryWithSource:`. A build with Apple's compiler is unaffected (its files are metallibs).

### 4.3 `dm/gfx/tools/dmsh_build_linux.py`: the build without Apple's shader compiler

`dmsh_build.py` and `dmsh_chain.py` are part of the translation chain version (their contents are hashed into it), so changing them would move the chain version of **every** game profile and need every profile's shader libraries rebuilt. The Linux build therefore leaves them alone: `dmsh_build_linux.py` runs `dmsh_build.py` with the four places that call Apple's compiler replaced (the library file gets the MSL source) and the tool fingerprints read without `xcrun`.

- The chain version stays the shared one: the MSL the translation produces is the same on every platform, only the way the library is compiled differs. All 21 profiles' `dmsh_chain.h` still match their computed version.
- The tool fingerprints of a machine without Apple's tools live in `dm/gfx/tools/dmsh_chain.lock.linux` (`dm/gfx/tools/dmsh_platform.py` picks it), so the shared `dmsh_chain.lock` is not touched.
- `dmsh_chain.py`'s `tools_now()` crashed when `xcrun` was absent (it read the version with `subprocess.run` and no `try`); the Linux driver supplies its own fingerprint function instead of changing the shared file.

### 4.4 `dm/ios/tools/build_ipa_linux.sh`: the app without Xcode

The counterpart of `dm/ios/tools/build_ipa.sh`. Same `Gen/` inputs (the host sources, the embedded modules, the tables, the fonts), same app bundle, same packaging (`tools/pack_ipa.py`); the app is compiled with `clang` and linked with `ld64.lld` against the Darwin SDK from the Swift toolchain's `darwin.artifactbundle` instead of `xcodebuild`.

The pieces that needed care:

- `dm/ios/tools/gen_embed.py` gained the `.globl` / `.no_dead_strip` labels the loader's `getsectiondata` lookup needs: the linker cannot see a normal reference to those sections, so without the labels it strips them.
- The linker is the SDK's own `ld64.lld` (21.1.6); the Swift toolchain's older one refuses to link for iOS.
- The framework list comes from `Gen/dm.xcconfig`, with `UIKit`, `Foundation`, `CoreGraphics`, `CoreText`, `GameController` and `AVFoundation` added (the iOS front end needs them), and `-lSystem` for the C library.
- The app icon needs Apple's `actool` (the asset catalog), which is not here: the icon files are written directly instead (`DM_ICON_FILE` names a PNG; the script resizes it to the sizes iOS asks for and writes the `Info.plist` keys).

## 5. Artifacts

| | |
|---|---|
| IPA | `work/nsp/010057D00B612000_build/IntoTheBreach.ipa`, 604 MB, 693 entries |
| sha256 | `sha256sum` of it (it changes with every rebuild) |
| app | `Payload/DmIOS.app/DmIOS`, 33.9 MB, arm64, `__TEXT` + `__DMmainW` + `__DMsdkW` |
| shaders | `Payload/DmIOS.app/msl/b-a712c69b6bb7/` — 5 libraries + 2 readable-tile variants, MSL source |
| RomFS | `Payload/DmIOS.app/romfs/` — 653 entries, 632 MB |

What was checked in the built IPA:

- the zip is intact (`testzip` clean);
- the executable is a Mach-O with exactly one executable segment (`__TEXT`) and the two writable guest images in their own segments (Apple ITMS-90999);
- the embedded module images are byte-exact against the rewritten `main.patched.bin` / `sdk.patched.bin` (the code part, the writable part and the patch area, with the tail padded by zeros), and every section is 16 KB-aligned, which is what the loader checks;
- `z0 + zw == PATCH_OFF` for both modules, the loader's own layout check;
- all five shader libraries carry the chain version `a712c69b6bb7` that the profile's `dmsh_chain.h` names, and each holds MSL source (not a metallib);
- the RomFS marker `resources/resource.dat` is in the IPA, with the same size as the extracted file (354,930,338 bytes);
- the host's tables (`shader_map.tsv`, `c1_full.tsv`, `pso_manifest.tsv`, the rewriter's `fixups` / `gates` / `cuts` / `obs` tables) and the two fonts are in the app's `dm/` folder;
- the app icon (the base game's own icon, resized to the sizes iOS asks for) is in the bundle, with the `Info.plist` keys that point at it.

## 6. On the device

The build was installed on an iPhone (iOS 27.0, iPhone17,4) with `xtool install` (signed with the user's Apple Development certificate) and run.

**It works.** From the app's own `Documents/log.txt`:

```
[dm] [nvn-mtl] Metal device Apple A18 GPU
[dm] [nvn] WindowInitialize swap chain 3 textures, interval 1, first 1920x1080 format 0x25
[dm] [nvn-mtl] shader route B (translated Switch originals): shader map 5 entries, metallib directory .../msl/b-a712c69b6bb7
[dm] [nvn] timing up to frame 600: frame time p50 16.65 / p95 17.07 / max 32.02 ms (300 frames); GPU p50 1.22 / p95 2.85 ms
[dm] [nvn] on-screen up to frame 600: 300 frames, dropped 2, p99 16.65 ms; footprint 1052 MiB
[dm] audio #0 device callbacks 2000: 0 underruns
[dm] [nvn] translation up to frame 600: 10330 instructions per draw, 69 draws per frame
```

The game reaches `nnMain`, the NVN device and the Metal device are up, the swap chain runs at 60 fps (p50 16.65 ms), the audio works with no underruns, the save file is written, the game survives a background / foreground cycle, and the FMOD mixer thread runs.

**One defect, found and fixed:** `nvnCommandBufferBindTextures` (the plural, combined-handle form) had no implementation in `dm/gfx/nvn/hle_nvn.c`, so the game's texture binds went to the red-reporting stub (238 calls in the first run). The engine binds its textures in batches, one call for a run of slots; the plural form is now implemented like the singular one (`OP_BIND_TEX`, the combined handle recorded as is):

```c
NVN(nvnCommandBufferBindTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) {
    bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h);
}
```

### Signing and installing

The IPA is unsigned, as the repository's builds are: the install tool signs it. `xtool` (the AppImage at `/usr/local/bin/xtool`) does the signing and the install:

```
xtool auth status                                   # the Apple ID and the team
xtool install --usb <the .ipa>                      # signs it and installs it
xtool launch --udid <the udid> XTL-<team>.<bundle id>
```

Two things about this environment (WSL2, the iPhone on the Windows host over usbipd):

- `xtool` keeps its state in `~/.config/xtool` and its cache in `~/.cache/xtool`; both are on a read-only volume here, so the Anisette library (`~/.config/xtool/Anisette/adi/adi.pb`) cannot be written and the install fails with "The volume is read only". The workaround is to run it in a user namespace with the uid mapped, which makes the files writable:
  `unshare -rm --map-user=1000 --map-group=1000 xtool install --usb <the .ipa>`
- `pymobiledevice3` looks for the usbmuxd socket at its own default path; the device is on `/var/run/usbmuxd` here, so `USBMUXD_SOCKET_ADDRESS=/var/run/usbmuxd` has to be set. With it, `pymobiledevice3 apps afc <bundle id> --documents` reaches the app's own container, which is where `log.txt` and `pso_user.tsv` are.

The bundle ID the install tool gives the app is `<bundle id>` with the team's prefix (`XTL-6Q5D6LN57B.org.nxnative.010057d00b612000`), and that is the ID `launch` and `apps afc` want.

## 7. Known limits

- **The audio banks** are Wwise (FMOD) files; the game's own FMOD is in `main` and runs as guest code, so the host's audio is the SDK's. The mixer thread runs, but the music was not listened to.
- **`pso_manifest.tsv` is empty** (the warm-up seed was not recorded; the game wrote its own `pso_user.tsv`).
- **The profile's `nvn_*` handle-table bases** are the common values; they are not exercised by this game, but they are not measured either.
- The rewrite (`dm/tools/patch_nso.py`) ran with the profile's defaults; the survey's rewrite-site census shows 8,478 `x18 use` sites in `main` and 76 `svc` sites in `sdk`, so there may be imports that need a hook that the game's own run did not reach.
- The game was run on the title screen only; the campaign was not played through.
