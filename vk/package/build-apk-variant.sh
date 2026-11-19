#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build a MediaDeck APK variant whose app code has mali-open-app-hook.patch applied, plus the matching driver zip "Mali-open-<sha>".
#   ./build-apk-variant.sh [--no-gradle] [variant]      variant default a64-glibc-wl (needs ../out/<variant>/libvulkan_mali.so)
# Env: DD_SRC   DroidDeck 0.7.1 source tree (default ~/src/droiddeck-src, read-only: copied with git archive, never modified)
#      BASE     base MediaDeck APK to splice into (default: droidtest out/MediaDeck-1.0.0-p22-gs2.apk)
#      T        droidtest dir (default ~/Projects/mali-gaming/droidtest: tools/bt, keys, android-env.sh)
#      OUT      output dir (default ./dist/apk)
# Output: $OUT/MediaDeck-open-<sha>.apk and $OUT/mali-open-<variant>-<sha>.zip. NOTHING is installed on any phone.
# The APK does not contain the driver: import the zip in-app (README "Import and select"); with the hook, selecting it
# switches MaliBridge off by itself, no PanVK-* bundle trick. Heavy (Gradle, ~10+ min): run under `mali-slot --`.
set -euo pipefail
P=$(cd "$(dirname "$0")" && pwd); V=$(dirname "$P")
gradle=1; [ "${1:-}" = "--no-gradle" ] && { gradle=0; shift; }
var=${1:-a64-glibc-wl}
sha=$(git -C "$V" rev-parse --short HEAD)
DD_SRC=${DD_SRC:-$HOME/src/droiddeck-src}
T=${T:-$HOME/Projects/mali-gaming/droidtest}
BASE=${BASE:-$T/out/MediaDeck-1.0.0-p22-gs2.apk}
OUT=$(realpath -m "${OUT:-$P/dist/apk}"); mkdir -p "$OUT"
MD=${MD:-$HOME/Documents/scripts/mali-wt2/md-matrix/build}     # splice.py lives here (build/malideck17)
for f in "$BASE" "$MD/malideck17/splice.py" "$V/out/$var/libvulkan_mali.so"; do [ -e "$f" ] || { echo "missing $f" >&2; exit 1; }; done
BT=$(echo "$T"/tools/bt/android-*)

# 1. driver zip named Mali-open-<sha>
DRIVER_NAME="Mali-open-$sha" "$P/build-zip.sh" -o "$OUT/zip" "$var"
cp "$OUT"/zip/mali-open-"$var"-*.zip "$OUT/mali-open-$var-$sha.zip"

# 2. hook applied to a scratch copy of the DroidDeck source
S=${TMPDIR:-/tmp}/mo-variant-src-$sha; rm -rf "$S"; mkdir -p "$S"
git -C "$DD_SRC" archive HEAD | tar -x -C "$S"
(cd "$S" && git init -q . && git apply --check -p1 "$P/mali-open-app-hook.patch" && git apply -p1 "$P/mali-open-app-hook.patch")
grep -q maliOpenSelected "$S/app/src/main/java/com/droiddeck/launcher/core/DeviceSupport.kt"
grep -q isMaliOpen "$S/app/src/main/java/com/droiddeck/launcher/gpu/LinuxVulkanDriverManager.java"
[ -e "$T/android-env.sh" ] && . "$T/android-env.sh"
if [ $gradle = 1 ]; then
  (cd "$S" && ./gradlew --no-daemon -PskipRust=true -x buildVkBridgeServer -x verifyRuntimeInputs :app:mergeDexRelease :app:processReleaseResources)
fi

# 3. splice the new dex/res into the base MediaDeck APK, align, sign (same key as the base: updates in place)
W=${TMPDIR:-/tmp}/mo-variant-pkg-$sha; rm -rf "$W"; mkdir -p "$W"
python3 -I "$MD/malideck17/splice.py" "$BASE" "$S/app/build" "$W/a.apk"
"$BT/zipalign" -f -P 16 4 "$W/a.apk" "$W/b.apk"
apk=$OUT/MediaDeck-open-$sha.apk; rm -f "$apk" "$apk.idsig"
"$BT/apksigner" sign --ks "$T/keys/droidtest.p12" --ks-pass file:"$T/keys/password" --ks-key-alias droidtest \
  --min-sdk-version 26 --v1-signing-enabled false --v2-signing-enabled true --v3-signing-enabled true --out "$apk" "$W/b.apk" 2>&1 | grep -v '^WARNING' || true
rm -f "$apk.idsig"
"$BT/zipalign" -c -P 16 4 "$apk"
"$BT/apksigner" verify --min-sdk-version 26 "$apk"
sha256sum "$apk" "$OUT/mali-open-$var-$sha.zip"
echo "built $apk (NOT installed). Next: README 'MediaDeck variant with the app hook'."
