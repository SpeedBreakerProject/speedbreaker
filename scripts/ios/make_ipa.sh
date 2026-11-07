#!/bin/bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
# scripts/ios/make_ipa.sh [--app <SpeedBreaker.app>] [--out <folder>] [--entitlements <plist>]:
# an IPA for sideloading (Sideloadly, AltStore, SideStore and the like, which
# sign it with the player's own Apple Account):
# <folder>/SpeedBreaker-<version>-ios.ipa (build/ios/ipa by default), its
# .sha256, and the entitlements it asks for beside it (.entitlements).
# EXPERIMENTAL: see README.md, "Sideloading the IPA".
#   - The app: built here without signing (IOS_UNSIGNED=1 build.sh, which
#     needs IOS_DEPS: README.md, step 2), or --app, a Release build for
#     devices, signed or not, of this version.
#   - Nobody's signature: a developer signature and embedded.mobileprovision
#     are removed. The executable gets an ad-hoc signature (no certificate,
#     no team) that holds only the entitlements in SpeedBreaker.entitlements
#     (Extended Virtual Addressing, Increased Memory Limit), or --entitlements'
#     (a test with fewer): an app's own signature is where the sideloading
#     tools read what it asks for.
#   - No debug map: the executable's links to the object files it was built
#     from (with this Mac's paths) are stripped; the full symbols go to
#     <folder>/symbols/SpeedBreaker-<version>-<commit>-ios.dSYM, for us (never
#     published: they map a player's crash back to the build), as the Mac
#     app's do (scripts/macos/make_app.py).
#   - A generic bundle identifier and display name: the tools replace the
#     identifier with one of the player's own.
#   - The license notices in SpeedBreaker.app/licenses (COPYING, NOTICE,
#     THIRD_PARTY_NOTICES.md, LICENSES/), as the other downloads carry them.
#   - The version is CMakeLists.txt's project(): --app must have it too, and
#     the build (build/ios, or IOS_BUILD) its release tag, v<version>
#     (SB_RELEASE_TAG=v<version> build.sh; SB_UNTAGGED=1 for a test IPA).
#   - Checked before it's written: no team ID, provisioning profile, device ID
#     or home folder path anywhere in the payload. IPA_SCAN_EXTRA="a|b|c"
#     adds strings of your own to look for (your team ID, name, devices).
# Reproducible: the same app gives the same IPA (sorted, fixed times).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
APP=
OUT=$ROOT/build/ios/ipa
ENTITLEMENTS=$ROOT/scripts/ios/SpeedBreaker/SpeedBreaker.entitlements
while [ $# -gt 0 ]; do
    case $1 in
        --app) APP=${2:?--app needs a path}; shift 2 ;;
        --out) OUT=${2:?--out needs a folder}; shift 2 ;;
        --entitlements) ENTITLEMENTS=${2:?--entitlements needs a plist}; shift 2 ;;
        -h|--help) sed -n '3,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "make_ipa.sh: unknown argument $1 (--help)" >&2; exit 2 ;;
    esac
done
fail() { echo "make_ipa.sh: $*" >&2; exit 1; }
# grep -q on a long pipe stops reading early, and with pipefail the writer's
# SIGPIPE then fails the pipe: count instead, reading everything.
has() { [ "$(grep -c -E -e "$1")" != 0 ]; }

VERSION=$(sed -n 's/^project(SpeedBreaker VERSION \([0-9][0-9.]*\) .*/\1/p' "$ROOT/CMakeLists.txt")
[ -n "$VERSION" ] || fail "no version in CMakeLists.txt's project()"
BUNDLE_ID=io.github.speedbreakerproject.SpeedBreaker
NAME=SpeedBreaker-$VERSION-ios
plutil -lint -s "$ENTITLEMENTS" || fail "$ENTITLEMENTS isn't a property list"

if [ -z "$APP" ]; then
    IOS_UNSIGNED=1 IOS_BUNDLE_ID=$BUNDLE_ID "$ROOT/scripts/ios/build.sh"
    APP=$ROOT/build/ios/xcode-unsigned/Build/Products/Release-iphoneos/SpeedBreaker.app
fi
[ -f "$APP/SpeedBreaker" ] && [ -f "$APP/Info.plist" ] || fail "no SpeedBreaker.app at $APP"
[ "$(plutil -extract DTPlatformName raw "$APP/Info.plist" 2>/dev/null)" = iphoneos ] \
    || fail "$APP isn't built for iPhone and iPad devices (DTPlatformName iphoneos)"
APP_VERSION=$(plutil -extract CFBundleShortVersionString raw "$APP/Info.plist")
[ "$APP_VERSION" = "$VERSION" ] || fail "$APP is v$APP_VERSION, CMakeLists.txt says $VERSION: build it again"
# The release tag (NFSMW_BUILD_TAG, -DSB_RELEASE_TAG: the update check's
# identity for it) of the build the app was linked from (build/ios, or
# IOS_BUILD): v<version>. SB_UNTAGGED=1 makes an IPA of an untagged build
# anyway, to try one (never a release).
INFO=${IOS_BUILD:-$ROOT/build/ios}/runtime/generated/build_info.inc
TAG=$(sed -n 's/^#define NFSMW_BUILD_TAG "\([^"]*\)"$/\1/p' "$INFO" 2>/dev/null || true)
[ -z "$TAG" ] || [ "$TAG" = "v$VERSION" ] || fail "the build ($INFO) is tagged $TAG, not v$VERSION: SB_RELEASE_TAG=v$VERSION build.sh"
if [ -z "$TAG" ]; then
    [ "${SB_UNTAGGED:-0}" = 1 ] || fail "the build ($INFO) has no release tag: SB_RELEASE_TAG=v$VERSION build.sh (SB_UNTAGGED=1: an IPA to try, never a release)"
    echo "make_ipa.sh: warning: an untagged build (SB_UNTAGGED=1): not for release" >&2
fi
[ "$(lipo -archs "$APP/SpeedBreaker")" = arm64 ] || fail "the executable isn't arm64 alone ($(lipo -archs "$APP/SpeedBreaker"))"

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/make_ipa.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/Payload"
P=$STAGE/Payload/SpeedBreaker.app
ditto --norsrc --noextattr --noqtn "$APP" "$P"

# Nobody's signature or profile, and nothing of the Mac's.
rm -rf "$P/_CodeSignature" "$P/embedded.mobileprovision"
codesign --remove-signature "$P/SpeedBreaker" 2>/dev/null || true
find "$P" \( -name .DS_Store -o -name '._*' \) -delete
[ -z "$(find "$P" -name '*.mobileprovision' -o -name '*.xcent')" ] || fail "a profile is left in the app"

# The debug map out (it names this Mac's folders), the full symbols kept
# for us. A stripped --app has none: nothing to keep.
if nm -ap "$P/SpeedBreaker" 2>/dev/null | has ' OSO '; then
    COMMIT=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
    [ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null)" ] || COMMIT=$COMMIT-dirty
    DSYM=$OUT/symbols/SpeedBreaker-$VERSION-$COMMIT-ios.dSYM
    mkdir -p "$OUT/symbols"
    rm -rf "$DSYM"
    dsymutil "$APP/SpeedBreaker" -o "$DSYM" 2>/dev/null || fail "dsymutil failed"
    strip -S "$P/SpeedBreaker"
fi
nm -gU "$P/SpeedBreaker" | has ' _vkGetInstanceProcAddr$' || fail "vkGetInstanceProcAddr is gone (SDL looks it up)"

# Rewritten with its keys sorted (plutil -replace orders them differently
# from one run to the next, and the signature seals the file).
python3 -I - "$P/Info.plist" "$BUNDLE_ID" <<'PY'
import plistlib, sys
path, bundle_id = sys.argv[1:]
with open(path, 'rb') as f:
    info = plistlib.load(f)
info['CFBundleIdentifier'] = bundle_id
info['CFBundleDisplayName'] = 'SpeedBreaker'
with open(path, 'wb') as f:
    plistlib.dump(info, f, fmt=plistlib.FMT_BINARY, sort_keys=True)
PY
plutil -lint -s "$P/Info.plist"

# The license notices, as the Mac app and the SteamOS downloads carry them:
# an IPA is a download, and the libraries built into it ask for theirs.
# SpeedBreaker.app/licenses: COPYING, NOTICE, THIRD_PARTY_NOTICES.md and
# LICENSES/, which cover SpeedBreaker and everything in the app.
L=$P/licenses
rm -rf "$L"
mkdir -p "$L"
cp "$ROOT/COPYING" "$ROOT/NOTICE" "$ROOT/THIRD_PARTY_NOTICES.md" "$L/"
ditto --norsrc --noextattr --noqtn "$ROOT/LICENSES" "$L/LICENSES"
cat > "$L/README.txt" <<EOF
SpeedBreaker v$VERSION for iPhone and iPad is free software under the GPL-3.0-or-later
(COPYING, NOTICE). What it is built from and with, and each license, is in
THIRD_PARTY_NOTICES.md and LICENSES/.

Built into this app, besides SpeedBreaker's own code and what THIRD_PARTY_NOTICES.md
lists as compiled into every platform: SDL3 (zlib; with HIDAPI and yuv2rgb,
BSD-3-Clause), glslang (its own list, LICENSES/glslang.txt), MoltenVK (Apache-2.0;
with SPIRV-Cross and SPIRV-Tools, Apache-2.0, and cereal, BSD-3-Clause), the
Vulkan and SPIRV headers, and FFmpeg's XMA decoders (LGPL-2.1-or-later), all
statically linked (scripts/ios/build_deps.sh says which versions).

Source: https://github.com/SpeedBreakerProject/speedbreaker (tag v$VERSION), and the
LGPL parts' complete source in that release's third-party source archive.
EOF

# The entitlements, where the tools read them: an ad-hoc signature (XML and
# DER), no identity, no time stamp.
codesign --force --sign - --timestamp=none --generate-entitlement-der --entitlements "$ENTITLEMENTS" "$P"
codesign --verify --strict "$P"
SIG=$(codesign -dvv "$P" 2>&1)
grep -q '^Signature=adhoc$' <<<"$SIG" || fail "the signature isn't ad-hoc"
grep -q '^TeamIdentifier=not set$' <<<"$SIG" || fail "the signature names a team"
# Exactly the file's entitlements, nothing more (no team, no get-task-allow).
keys() { plutil -convert json -o - - | python3 -I -c 'import json, sys; print(" ".join(sorted(f"{k}={v}" for k, v in json.load(sys.stdin).items())))'; }
WANTED=$(keys < "$ENTITLEMENTS")
SIGNED=$(codesign -d --entitlements - --xml "$P" 2>/dev/null | keys)
[ "$SIGNED" = "$WANTED" ] || fail "the signature's entitlements ($SIGNED) aren't $(basename "$ENTITLEMENTS")'s ($WANTED)"

# Nothing personal: no home folders, profiles, teams or devices. A device's
# UDID (since 2018: 8 hex digits, a dash, 16) is looked for in any form.
PATTERNS=(/Users/ /home/ /private/var/mobile ProvisionedDevices embedded.mobileprovision com.apple.developer.team-identifier
    application-identifier TeamIdentifier DEVELOPMENT_TEAM)
if [ -n "${IPA_SCAN_EXTRA:-}" ]; then
    IFS='|' read -r -a EXTRA <<<"$IPA_SCAN_EXTRA"
    PATTERNS+=("${EXTRA[@]}")
fi
HITS=0
for pattern in "${PATTERNS[@]}"; do
    [ -n "$pattern" ] || continue
    if found=$(LC_ALL=C grep -r -a -i -l -F -e "$pattern" "$STAGE/Payload"); then
        echo "make_ipa.sh: \"$pattern\" found in: ${found//$STAGE\//}" >&2
        HITS=$((HITS + 1))
    fi
done
if found=$(LC_ALL=C grep -r -a -l -E '[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}([^0-9A-Fa-f]|$)' "$STAGE/Payload"); then
    echo "make_ipa.sh: something shaped like a device UDID in: ${found//$STAGE\//}" >&2
    HITS=$((HITS + 1))
fi
[ "$HITS" = 0 ] || fail "$HITS personal string(s) in the payload: not written"

# A sorted zip with fixed times (2000-01-01, or SOURCE_DATE_EPOCH) and no
# extra fields: the same app, the same IPA, whatever the checkout's state.
EPOCH=${SOURCE_DATE_EPOCH:-946684800}
STAMP=$(TZ=UTC date -r "$EPOCH" +%Y%m%d%H%M.%S)
find "$STAGE/Payload" -exec env TZ=UTC touch -h -t "$STAMP" {} +
mkdir -p "$OUT"
rm -f "$OUT/$NAME.ipa" "$OUT/$NAME.ipa.sha256"
(cd "$STAGE" && find Payload | LC_ALL=C sort | TZ=UTC zip -q -X -y -9 -@ "$STAGE/$NAME.ipa")
unzip -Z1 "$STAGE/$NAME.ipa" | has '^(Payload/|Payload/SpeedBreaker\.app/.*)$' || fail "the IPA is empty"
unzip -Z1 "$STAGE/$NAME.ipa" | grep -v -E '^(Payload/|Payload/SpeedBreaker\.app/.*)$' | has . && fail "the IPA holds more than Payload/SpeedBreaker.app"
mv "$STAGE/$NAME.ipa" "$OUT/$NAME.ipa"
cp "$ENTITLEMENTS" "$OUT/$NAME.entitlements"
(cd "$OUT" && shasum -a 256 "$NAME.ipa" > "$NAME.ipa.sha256")
echo "$OUT/$NAME.ipa ($(du -h "$OUT/$NAME.ipa" | cut -f1 | tr -d ' '), sha256 $(cut -d' ' -f1 "$OUT/$NAME.ipa.sha256"))"
echo "  $BUNDLE_ID v$VERSION (build $(plutil -extract CFBundleVersion raw "$P/Info.plist")), ad-hoc signed with" \
     "$(sed 's/com\.apple\.developer\.//g; s/=True//g' <<<"$WANTED"); no team, profile or device"
