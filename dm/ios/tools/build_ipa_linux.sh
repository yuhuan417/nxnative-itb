#!/bin/bash
# Build one unsigned IPA on Linux (no Xcode): the rewritten modules compiled into the executable, the iOS shader libraries at DmIOS.app/msl, and the game files as one packed file
# (romfs.dmpak in the bundle, or beside the IPA with romfs_pack separate: dm/tools/romfs_pack.py, read by dm/host/hle_pak.c).
# This is the Linux counterpart of dm/ios/tools/build_ipa.sh: the same Gen/ inputs (host sources, embedded modules, tables, fonts), the same app bundle and the same packaging (tools/pack_ipa.py), but the app is
# compiled and linked with clang and ld64.lld against the Darwin SDK (the Swift toolchain's darwin.artifactbundle) instead of xcodebuild. The result is the same unsigned IPA the user sideloads.
#   The app icon needs Apple's actool, which is not here, so the icon is written by hand instead: DM_ICON_FILE names a PNG (any size; it is resized to 1024x1024 and the icon sizes iOS asks for),
#   and the app gets an Assets.car-free icon set (the loose .png files and the Info.plist keys). Without DM_ICON_FILE the app has no icon (the home-screen icon stays blank).
# Prerequisites: the rewriter (patched directory) and the iOS shader build (IOSOUT) have been run as in the README; a Darwin SDK and ld64.lld are available.
# Usage: dm/ios/tools/build_ipa_linux.sh [output.ipa]; environment: GAME=game profile (dm/games/<base game title ID>, or one in DM_GAMES_DIR; required), W=the game's work folder (required: <NSP folder>/<title ID>-work, never the home folder), PATCHED / ROMFS / MSL_IOS can each be overridden
#   AOC=add-on content directory (default $W/data/aoc; when it exists it goes to DmIOS.app/aoc/<index>/; an empty string = none)
#   SDK=the iPhoneOS SDK to build against (default: the swift toolchain's darwin.artifactbundle); IOS_MIN=the deployment target (default 26.0, the Metal 4 requirement of the graphics replay)
#   The app name is the game name in the base NSP's control data, the app version the display version in the packed game's control data, the bundle ID org.nxnative.<base game title ID in lowercase>
#   (DM_DISPLAY_NAME / DM_APP_VERSION / DM_BUNDLE_ID override them); the IPA name, code modules, RomFS marker and shader tables come from the game profile
set -u
cd "$(dirname "$0")/.." || exit 1
IOS=$PWD; DM=$(cd .. && pwd)
t() { echo "[$(date +%T)] $*"; }; die() { t "FAIL: $*"; exit 1; }
[ -n "${GAME:-}" ] || die "GAME is not set: GAME=<base game title ID> $0; profiles: $(python3 "$DM/games/game.py" list | awk -F'\t' '{printf "%s (%s)  ", $1, $3}')"
export GAME; GD=$(python3 "$DM/games/game.py" dir "$GAME") || die "no game profile $GAME"   # dm/games/<id>, or a profile in DM_GAMES_DIR (the NSP script's provisional one)
gp() { python3 "$DM/games/game.py" "$GAME" "$1" || die "game profile $GD: $1"; }   # read a key of the game profile
[ -f "$GD/game.tsv" ] || die "no game profile $GD/game.tsv"
[ -n "${W:-}" ] || die "W is not set: W=<NSP folder>/$GAME-work (the game's work folder) $0"
PKG=$(gp package)
PATCHED=${PATCHED:-$W/build/patched_$PKG}; ROMFS=${ROMFS:-$W/data/$PKG/RomFS}; MSL_IOS=${MSL_IOS:-$W/build/ios/dmsh_b}; AOC=${AOC-$W/data/aoc}
OUT_IPA=${1:-$(dirname "$W")/$(gp ipa)}; O=$W/build/ios/pkg; DD=$O/dd
MODS=$(gp modules); MARKER=$(gp romfs_marker)
# The game data as one packed file instead of the RomFS tree (dm/tools/romfs_pack.py, dm/host/hle_pak.c): romfs_pack=ipa puts romfs.dmpak in the bundle, separate writes it beside the
#   IPA and leaves it out, so a program-only rebuild is a small IPA and the data goes to the app's Documents once per game version. dir keeps the old tree (a game with no profile key)
ROMPACK=$(python3 "$DM/games/game.py" "$GAME" romfs_pack 2> /dev/null) || ROMPACK=ipa
case "$ROMPACK" in ipa|separate|dir) ;; *) die "romfs_pack must be ipa, separate or dir: $ROMPACK" ;; esac
NACP=$W/data/v0/control.nacp; [ -f "$NACP" ] || NACP=$(dirname "$ROMFS")/control.nacp
[ -n "${DM_DISPLAY_NAME:-}" ] || DM_DISPLAY_NAME=$(python3 "$DM/tools/nacp.py" "$NACP") || die "no game name in $NACP (re-extract with nx_extract, or set DM_DISPLAY_NAME)"
VNACP=$(dirname "$ROMFS")/control.nacp; [ -f "$VNACP" ] || VNACP=$W/data/v0/control.nacp
[ -n "${DM_APP_VERSION:-}" ] || DM_APP_VERSION=$(python3 "$DM/tools/nacp.py" --version "$VNACP") || { t "no display version in $VNACP: the app version is 1.0"; DM_APP_VERSION=1.0; }
export LC_ALL=C DM_BUNDLE_ID=${DM_BUNDLE_ID:-org.nxnative.$(echo "$GAME" | tr 'A-Z' 'a-z')} DM_DISPLAY_NAME DM_APP_VERSION DM_APP_CATEGORY=$(gp app_category) DM_TEAM_ID=${DM_TEAM_ID:-0000000000}   # placeholder identity: unsigned, the install tool replaces it
for f in $MODS; do [ -f "$PATCHED/$f.patched.bin" ] && [ -f "$PATCHED/$f.fixups.tsv" ] || die "no rewritten module $PATCHED/$f (run dm/tools/patch_nso.py first)"; done
[ -e "$ROMFS/$MARKER" ] || die "no RomFS at $ROMFS (missing $MARKER)"
V=$(sed -n 's/^#define DMSH_CHAIN "\(.*\)"/\1/p' "$GD/dmsh_chain.h")
# graphics vulkan (Hades): the shaders are the game's SPIR-V, translated by dm/gfx/tools/vk_shaders.py into $MSL_IOS/vk; there are no NVN chain libraries
GFXAPI=$(python3 "$DM/games/game.py" "$GAME" graphics 2> /dev/null) || GFXAPI=nvn
if [ "$GFXAPI" = vulkan ]; then [ -f "$MSL_IOS/vk/vk_shaders.metallib" ] || die "no iOS Vulkan shader library $MSL_IOS/vk (dm/gfx/tools/vk_shaders.py \"$ROMFS\" <Mac dir> --ios \"$MSL_IOS\")"
else [ -n "$V" ] && ls "$MSL_IOS/b-$V" 2> /dev/null | grep -q '_b.metallib$' || die "no iOS shader libraries $MSL_IOS/b-$V (IOSOUT=$MSL_IOS dmsh_build.py ...)"; fi

# ---- The toolchain: clang for arm64-apple-ios, ld64.lld for the link, the Darwin SDK for the headers and the system libraries ----
SDK=${SDK:-$(ls -d "$HOME"/.swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS*.sdk 2>/dev/null | sort -V | tail -1)}
[ -n "$SDK" ] && [ -d "$SDK" ] || die "no iPhoneOS SDK (set SDK=<path to iPhoneOS.sdk>)"
LLD=${LLD:-$(ls -d "$HOME"/.swiftpm/swift-sdks/darwin.artifactbundle/toolset/bin/orig/ld64.lld 2>/dev/null | head -1)}
[ -n "$LLD" ] && [ -x "$LLD" ] || LLD=$(command -v ld64.lld || ls ~/.local/share/swiftly/bin/ld64.lld 2>/dev/null || echo /usr/local/swift/usr/bin/ld64.lld)
[ -x "$LLD" ] || die "no ld64.lld (the link step needs it; the Darwin SDK's toolset has one: $HOME/.swiftpm/swift-sdks/darwin.artifactbundle/toolset/bin/orig/ld64.lld)"
IOS_MIN=${IOS_MIN:-26.0}   # the graphics replay uses Metal 4 (MTL4CommandQueue etc.), which needs iOS 26
CC=${CC:-clang}
TRIPLE=arm64-apple-ios$IOS_MIN
COMMON="-target $TRIPLE -isysroot $SDK -miphoneos-version-min=$IOS_MIN -fobjc-arc -fno-omit-frame-pointer -Wno-unused-function -Wno-deprecated-declarations -Wno-format -Wno-unknown-warning-option -mcpu=apple-a18"
t "app: $DM_DISPLAY_NAME $DM_APP_VERSION ($DM_BUNDLE_ID)"
t "toolchain: $CC ($TRIPLE), $SDK"
t "generate Gen/ (host sources, embedded modules, tables)"
rm -rf Gen && mkdir -p Gen/dm || die "mkdir Gen"
python3 tools/gen_build.py ../host Gen > Gen/dm.xcconfig || die "gen_build"
F=$(python3 tools/gen_embed.py "$PATCHED" Gen/embed.S $MODS) || die "gen_embed"
echo "OTHER_LDFLAGS = \$(inherited) \$(DM_FRAMEWORKS) $F" >> Gen/dm.xcconfig
cp "$PATCHED"/*.fixups.tsv "$PATCHED"/*.gates.tsv "$PATCHED"/*.cuts.tsv "$PATCHED"/*.obs.tsv Gen/dm/ || die "copy rewriter tables"
if [ "$GFXAPI" = vulkan ]; then cp "$GD/shader_map.tsv" "$GD/pso_manifest.tsv" Gen/dm/ || die "copy shader tables"   # empty NVN tables: the NVN layer is never used
else cp "$GD/shader_map.tsv" "$GD/pso_manifest.tsv" "$GD/c1_full.tsv" Gen/dm/ || die "copy shader tables"; fi
# Diagnostic build: DM_ENV_FILE=file → dm/env.txt in the bundle (main.m sets one environment variable per line at boot)
if [ -n "${DM_ENV_FILE:-}" ]; then cp "$DM_ENV_FILE" Gen/dm/env.txt || die "copy DM_ENV_FILE"; t "env: $(grep -v '^#' "$DM_ENV_FILE" | tr '\n' ' ')"; fi
# The shader libraries go into the archive as DmIOS.app/msl/<folder> (main.m sets DM_MSL_DIR to it)
if [ "$GFXAPI" = vulkan ]; then MSLDIR=vk; else MSLDIR=b-$V; fi
# Shared-font stand-ins: standard = a TTF with the game's glyphs (macOS's Arial Unicode is absent on iOS); the Nintendo extension = button icons generated by make_ext_font.py (needs fontTools; FONT_PY names a python that has it)
mkdir -p Gen/dm/font
if [ -n "${STD_FONT:-}" ] && [ -f "${STD_FONT}" ]; then cp "$STD_FONT" Gen/dm/font/standard.ttf || die "copy standard font"
elif [ -f "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" ]; then cp "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" Gen/dm/font/standard.ttf
else die "no standard font (set STD_FONT=<path to a TTF>)"; fi
"${FONT_PY:-python3}" "$DM/tools/make_ext_font.py" Gen/dm/font/nintendo_ext.ttf "${LETTER_FONT:-/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf}" > /dev/null || die "make_ext_font.py (pip install fonttools, or FONT_PY=python with fontTools; LETTER_FONT=<path to a TTF>)"

# ---- Compile the app: the host sources (Gen/src), the iOS front end and the app entry (Sources), the embedded modules (Gen/embed.S) ----
mkdir -p "$O/obj" "$DD"
: > "$O/build.log"
OBJS=()
# The frameworks the host's compile command names (Gen/dm.xcconfig's DM_FRAMEWORKS), plus the ones the iOS front end needs
FW=$(sed -n 's/^DM_FRAMEWORKS = //p' Gen/dm.xcconfig | tr -d '"')
LDFLAGS=()
for f in $FW; do [ "$f" = "-framework" ] && continue; LDFLAGS+=("-framework" "$f"); done
for f in UIKit Foundation CoreGraphics CoreText GameController AVFoundation; do case " $FW " in *" $f "*) ;; *) LDFLAGS+=("-framework" "$f");; esac; done
# The SDK's frameworks and libraries: the linker searches the sysroot and the SDK's own directories
LDFLAGS+=(-syslibroot "$SDK" -F "$SDK/System/Library/Frameworks" -L "$SDK/usr/lib" -L "$SDK/System/Library/Frameworks" -L "$SDK/usr/lib/swift")
LDFLAGS+=(-lSystem -lc++ -lz)
# The embedded modules' writable segments: the linker must not make them read-only (the loader remaps them copy-on-write)
for m in $MODS; do LDFLAGS+=(-segprot "__DM${m}W" rw rw); done
INCS=$(sed -n 's/^HEADER_SEARCH_PATHS = \$(inherited) //p' Gen/dm.xcconfig | tr -d '"' | tr ' ' '\n' | grep -v '^$' | sed 's|^|-I|' | tr '\n' ' ')   # the xcconfig lists the paths; clang wants them as -I flags
CFL=$(sed -n 's/^OTHER_CFLAGS = \$(inherited) //p' Gen/dm.xcconfig | tr -d '"')
compile() {   # <source> <object> [extra flags]
  local src=$1 obj=$2; shift 2
  $CC $COMMON $INCS $CFL "$@" -c "$src" -o "$obj" >> "$O/build.log" 2>&1 || { grep -E "error:" "$O/build.log" | head -20; die "compile $src"; }
}
t "compile the app"
for s in Gen/src/*; do [ -e "$s" ] || continue; o="$O/obj/$(basename "$s").o"; compile "$s" "$o"; OBJS+=("$o"); done
for s in Sources/*.m; do o="$O/obj/$(basename "$s").o"; compile "$s" "$o"; OBJS+=("$o"); done
# The host's main.c is the app's main: -Dmain=dm_main turns it into the guest entry the app calls
compile Gen/main/main.c "$O/obj/main.o" -Dmain=dm_main; OBJS+=("$O/obj/main.o")
# The embedded module images
compile Gen/embed.S "$O/obj/embed.o"; OBJS+=("$O/obj/embed.o")
t "link"
"$LLD" -arch arm64 -platform_version ios "$IOS_MIN" "$IOS_MIN" -o "$DD/DmIOS" "${OBJS[@]}" "${LDFLAGS[@]}" >> "$O/build.log" 2>&1 || { grep -E "error|undefined" "$O/build.log" | head -30; die "link"; }
APP=$DD/DmIOS.app
rm -rf "$APP"; mkdir -p "$APP"
cp "$DD/DmIOS" "$APP/DmIOS" || die "copy the executable"
# Info.plist: the same properties as the Xcode project's (project.yml)
python3 - "$APP/Info.plist" <<'PY' || die "Info.plist"
import os, plistlib, sys
out = sys.argv[1]
plistlib.dump({
    'CFBundleIdentifier': os.environ['DM_BUNDLE_ID'],
    'CFBundleExecutable': 'DmIOS',
    'CFBundleName': os.environ['DM_DISPLAY_NAME'],
    'CFBundleDisplayName': os.environ['DM_DISPLAY_NAME'],
    'CFBundleIconFiles': ['AppIcon60x60'],
    'CFBundleIcons': {'CFBundlePrimaryIcon': {'CFBundleIconFiles': ['AppIcon60x60'], 'CFBundleIconName': 'AppIcon'}},
    'CFBundleVersion': os.environ['DM_APP_VERSION'],
    'CFBundleShortVersionString': os.environ['DM_APP_VERSION'],
    'CFBundlePackageType': 'APPL',
    'CFBundleInfoDictionaryVersion': '6.0',
    'MinimumOSVersion': os.environ.get('IOS_MIN', '26.0'),
    'UIDeviceFamily': [1, 2],
    'UIFileSharingEnabled': True,
    'LSSupportsOpeningDocumentsInPlace': True,
    'UILaunchScreen': {},
    'UIRequiresFullScreen': True,
    'UIStatusBarHidden': True,
    'UIViewControllerBasedStatusBarAppearance': False,
    'LSApplicationCategoryType': os.environ.get('DM_APP_CATEGORY', 'public.app-category.games'),
    'LSSupportsGameMode': True,
    'GCSupportsControllerUserInteraction': True,
    'GCSupportedGameControllers': [{'ProfileName': 'ExtendedGamepad'}],
    'CADisableMinimumFrameDurationOnPhone': True,
    'UISupportedInterfaceOrientations': ['UIInterfaceOrientationLandscapeRight', 'UIInterfaceOrientationLandscapeLeft'],
    'UISupportedInterfaceOrientations~ipad': ['UIInterfaceOrientationLandscapeRight', 'UIInterfaceOrientationLandscapeLeft'],
    'UIApplicationSceneManifest': {
        'UIApplicationSupportsMultipleScenes': False,
        'UISceneConfigurations': {'UIWindowSceneSessionRoleApplication': [{'UISceneConfigurationName': 'Default', 'UISceneDelegateClassName': 'SceneDelegate'}]},
    },
}, open(out, 'wb'))
PY
# The host's tables and the fonts go into the app bundle (main.m reads them from the bundle's dm/ folder)
mkdir -p "$APP/dm" && cp -R Gen/dm/. "$APP/dm/" || die "copy the host's tables"

# The app icon: Apple's actool (the asset catalog) is not here, so the icon files are written directly. iOS takes the icon from the bundle's loose .png files
#   (CFBundleIconFiles) or from the asset catalog; the loose files work for an app that is not built by Xcode.
if [ -n "${DM_ICON_FILE:-}" ] && [ -f "$DM_ICON_FILE" ]; then
    ICON_PY=${ICON_PY:-${FONT_PY:-python3}}   # FONT_PY may be unset (line 73 defaults it), and set -u would abort here
    "$ICON_PY" - "$DM_ICON_FILE" "$APP" <<'PY' || die "app icon (DM_ICON_FILE=<png>; needs Pillow: pip install pillow, or ICON_PY=<python with it>)"
import os, sys
from PIL import Image
src, app = sys.argv[1], sys.argv[2]
im = Image.open(src).convert('RGB')
# the sizes iOS asks for (the @2x and @3x variants of the 60-point icon, plus the 1024-point one for the settings and the app store)
for name, size in (('AppIcon60x60', 60), ('AppIcon60x60@2x', 120), ('AppIcon60x60@3x', 180), ('AppIcon76x76', 76), ('AppIcon76x76@2x', 152),
                   ('AppIcon83.5x83.5@2x', 167), ('AppIcon1024x1024', 1024)):
    im.resize((size, size), Image.LANCZOS).save(os.path.join(app, name + '.png'))
PY
    t "app icon: $DM_ICON_FILE"
fi

# Straight from the sources: the app without its signature, then the shader libraries, the add-on content (the RomFS extracted from the player's DLC NSPs, one folder per index:
# the app sets DM_AOC_DIR at launch, the host reports them as owned and mounts them from it) and the RomFS
rm -rf "$APP/msl" "$O/Payload" "$O/out.ipa"
P=Payload/DmIOS.app; MAPS=("$P=$APP" "$P/msl/$MSLDIR=$MSL_IOS/$MSLDIR")
if [ -n "$AOC" ] && [ -d "$AOC" ]; then MAPS+=("$P/aoc=$AOC"); t "add-on content: $(ls "$AOC" | tr '\n' ' ')"; fi
PAK=${PAK:-$(dirname "$OUT_IPA")/romfs.dmpak}
case "$ROMPACK" in
    dir) MAPS+=("$P/romfs=$ROMFS"); t "game data: the RomFS tree (romfs_pack dir)" ;;
    ipa|separate)
        python3 "$DM/tools/romfs_pack.py" "$ROMFS" "$PAK" || die "romfs_pack (dm/tools/romfs_pack.py)"
        t "game data: $PAK ($(du -h "$PAK" | cut -f1))"
        if [ "$ROMPACK" = ipa ]; then MAPS+=("$P/romfs.dmpak=$PAK"); else t "romfs_pack separate: not in the IPA; copy it to the app's Documents once (pymobiledevice3 apps push <bundle id> romfs.dmpak --documents)"; fi ;;
esac
t "pack IPA (already-compressed game files stored, everything else deflated)"
python3 tools/pack_ipa.py "$OUT_IPA" --store "$(gp zip_store):.dmpak" --skip $P/_CodeSignature --skip $P/embedded.mobileprovision --skip $P/msl --skip $P/aoc --skip $P/romfs "${MAPS[@]}" || die "pack_ipa"
t "done: $OUT_IPA ($(du -h "$OUT_IPA" | cut -f1))"
