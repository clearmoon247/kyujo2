#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
rev=${1:?Pass the revision label}
[[ "$rev" =~ ^[A-Za-z0-9_-]+$ ]]
sdk=${ANDROID_HOME:-$PWD/.work/android-sdk}
ndk=${ANDROID_NDK_HOME:-$sdk/ndk/27.3.13750724}
native_dir=${KYUJO2_ANDROID_BUILD:-build/game-android}
region=$(sed -n 's/^KYUJO2_REGION:STRING=//p' "$native_dir/CMakeCache.txt")
case "$region" in
    us) package=org.kyujo2.game; game='Pokémon Stadium 2 (USA)'; digest=d8343e69a7dc63b869cf6361d87cde64444281d3 ;;
    jp) package=org.kyujo2.game.jp; game='Pokémon Stadium Gold & Silver (Japan)'; digest=05682e60b13479ca1c54656e5a5b1ee6d099c1a4 ;;
    *) echo "Unknown native Android region: $region" >&2; exit 2 ;;
esac
out="$PWD/build/packages/$rev"
mkdir -p "$out"
stage=$(mktemp -d "$out/android.XXXXXX")
bt="$sdk/build-tools/34.0.0"
jar="$sdk/platforms/android-34/android.jar"
mkdir -p "$stage"/{classes,lib/arm64-v8a}
cat > "$stage/Cartridge.java" <<REGION
package org.kyujo2.game;
final class Cartridge {
    static final String GAME = "$game";
    static final String SHA1 = "$digest";
    static final String FILE = "stadium2-$region.z64";
    static final String LEGACY_FILE = "Kyujo2-${region^^}.z64";
}
REGION
javac --release 8 -cp "$jar" -d "$stage/classes" "$stage/Cartridge.java" packaging/android/*.java .work/SDL2-2.32.10/android-project/app/src/main/java/org/libsdl/app/*.java
(cd "$stage/classes" && jar cf "$stage/classes.jar" .)
"$sdk/build-tools/35.0.1/d8" --min-api 24 --lib "$jar" --output "$stage" "$stage/classes.jar"
"$bt/aapt2" compile --dir packaging/android/res -o "$stage/res.zip"
"$bt/aapt2" link --rename-manifest-package "$package" -I "$jar" --manifest packaging/android/AndroidManifest.xml -R "$stage/res.zip" --auto-add-overlay -o "$stage/base.apk"
strip="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
for input in "$native_dir/libKyujo2.so" .work/sdl-android/libSDL2.so; do
    "$strip" --strip-debug -o "$stage/lib/arm64-v8a/${input##*/}" "$input"
done
(cd "$stage" && jar uf base.apk classes.dex lib/arm64-v8a/libKyujo2.so lib/arm64-v8a/libSDL2.so)
"$bt/zipalign" -f 4 "$stage/base.apk" "$stage/aligned.apk"
# Signing. Android installs an update only when it is signed with the same key as the installed app, so keep the key.
# KYUJO2_ANDROID_KEYSTORE (with _KEYSTORE_PASS, _KEY_ALIAS, _KEY_PASS) selects your own key; without it a local test
# key is made once in .work/kyujo2-debug.jks.
key=${KYUJO2_ANDROID_KEYSTORE:-.work/kyujo2-debug.jks}
ks_pass=${KYUJO2_ANDROID_KEYSTORE_PASS:-android}
alias=${KYUJO2_ANDROID_KEY_ALIAS:-androiddebugkey}
key_pass=${KYUJO2_ANDROID_KEY_PASS:-$ks_pass}
if [[ ! -e "$key" ]]; then
    [[ -z "${KYUJO2_ANDROID_KEYSTORE:-}" ]] || { echo "Keystore not found: $key" >&2; exit 1; }
    keytool -genkeypair -keystore "$key" -alias "$alias" -storepass "$ks_pass" -keypass "$key_pass" \
        -keyalg RSA -keysize 2048 -validity 3650 -dname 'CN=Kyujo2 Local Test'
fi
apk="$out/Kyujo2-${region^^}-$rev.apk"
"$bt/apksigner" sign --ks "$key" --ks-pass "pass:$ks_pass" --ks-key-alias "$alias" --key-pass "pass:$key_pass" \
    --out "$apk" "$stage/aligned.apk"
"$bt/apksigner" verify --verbose "$apk"
