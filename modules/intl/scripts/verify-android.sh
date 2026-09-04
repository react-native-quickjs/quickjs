#!/usr/bin/env bash
#
# Compile the Android half of react-native-quickjs-intl without Gradle.
#
# WHY THIS EXISTS
#   The Android backend (JNI C++, the JNI helper, and the Kotlin that does the
#   android.icu work) was written, reviewed and documented for a full cycle
#   *without ever being compiled*. docs/intl-platform-backed.md recorded every
#   Android claim as CITED rather than MEASURED for that reason. The first run
#   of this script found four real defects in ten minutes:
#
#     1. cpp/IntlModuleJSI.cpp: `fatal error: 'jsi/jsi.h' file not found`
#        (the module never put JSI on its own include path)
#     2. `undefined symbol: rnqjs::intl::setPlatform` — RNQJS_MODULE_NAME was
#        set in a subdirectory scope, so android/CMakeLists.txt linked the JNI
#        library against nothing
#     3. `DateFormat.Field.RELATED_YEAR` / `.YEAR_NAME` do not exist in
#        android.icu (they are ICU4J-only)
#     4. an AGP/Gradle build would have caught all three, but a full Gradle run
#        needs network, an emulator image and ~10 minutes
#
#   So this is the cheap, offline, hermetic version: NDK CMake for the native
#   half, the Kotlin compiler out of the Gradle cache for the Kotlin half. It
#   does NOT run anything on a device — see "WHAT THIS DOES NOT DO".
#
# CONTRACT
#   Exit 0 iff every ABI compiles, both Kotlin files compile, and every static
#   method the C++ resolves by name and signature exists on the compiled class
#   with that exact signature. Exit non-zero with the failing step named.
#
#   Requires: ANDROID_HOME (or ~/Library/Android/sdk) with an NDK and a
#   platform; a Gradle cache containing kotlin-compiler-embeddable and a
#   react-android AAR. All of those are already present on any machine that has
#   built the example app once.
#
# WHAT THIS DOES NOT DO
#   It does not run a single line of android.icu. It compiles. Behavioural
#   verification of the Android backend needs a device and is out of scope of
#   this hermetic, offline check (which is the one that belongs in CI).
#
# Usage:  bash modules/intl/scripts/verify-android.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
MODULE="$REPO/modules/intl"
SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
OUT="${TMPDIR:-/tmp}/rnqjs-intl-android-verify"
rm -rf "$OUT"; mkdir -p "$OUT"

step() { printf '\n=== %s ===\n' "$1"; }
die() { printf 'verify-android: FAIL: %s\n' "$1" >&2; exit 1; }

[ -d "$SDK" ] || die "no Android SDK at $SDK (set ANDROID_HOME)"
NDK="$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1)" || true
[ -n "${NDK:-}" ] || die "no NDK under $SDK/ndk"
ANDJAR="$(ls -d "$SDK"/platforms/android-3*/android.jar 2>/dev/null | sort -V | tail -1)"
[ -n "$ANDJAR" ] || die "no android.jar under $SDK/platforms"

# The host qjsc for the bytecode blob is found by the module CMakeLists itself
# (bin/qjsc/<host>/qjsc), so nothing is passed here.

step "native (NDK $(basename "$NDK"))"
for ABI in arm64-v8a armeabi-v7a x86_64; do
  B="$OUT/native-$ABI"
  ARGS=(-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
        -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-24
        -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release
        -DREACT_NATIVE_QUICKJS_DIR="$REPO")
  cmake -B "$B" -S "$MODULE/android" "${ARGS[@]}" >"$B.log" 2>&1 \
    || { cat "$B.log"; die "cmake configure $ABI"; }
  cmake --build "$B" -j8 >>"$B.log" 2>&1 || { cat "$B.log"; die "cmake build $ABI"; }
  echo "  $ABI  ok  ($(wc -c <"$B/libintl_platform.so" | tr -d ' ') bytes)"
done

# The JNI entry point must be exported under the exact name the Kotlin `external
# fun nativeAttach()` will look for. A typo here is a runtime UnsatisfiedLink,
# not a build error, which is why it is asserted rather than eyeballed.
nm -D "$OUT/native-arm64-v8a/libintl_platform.so" \
  | grep -q "T Java_com_intl_IntlPlatform_nativeAttach" \
  || die "libintl_platform.so does not export Java_com_intl_IntlPlatform_nativeAttach"
echo "  exports Java_com_intl_IntlPlatform_nativeAttach"

step "kotlin"
find_jar() { find "$HOME/.gradle/caches" -name "$1" 2>/dev/null | sort -V | tail -1; }
# The compiler, the stdlib and kotlin-reflect must all be the *same version*.
# Mixing them fails with `NoClassDefFoundError: kotlin/reflect/jvm/
# ReflectJvmMapping` before a single source file is read, which reads like a
# broken installation and is not one. So pick the newest version for which the
# Gradle cache holds a complete set, rather than the newest of each part.
KC=""; STD=""; REFLECT=""
for KV in $(find "$HOME/.gradle/caches" -name 'kotlin-compiler-embeddable-2.*.jar' \
              2>/dev/null | sed 's|.*/kotlin-compiler-embeddable-||;s|\.jar$||' \
              | grep -v -- '-' | sort -Vr); do
  c="$(find_jar "kotlin-compiler-embeddable-$KV.jar")"
  s="$(find_jar "kotlin-stdlib-$KV.jar")"
  r="$(find_jar "kotlin-reflect-$KV.jar")"
  if [ -n "$c" ] && [ -n "$s" ] && [ -n "$r" ]; then
    KC="$c"; STD="$s"; REFLECT="$r"; echo "  kotlin $KV"; break
  fi
done
CORO="$(find_jar 'kotlinx-coroutines-core-jvm-*.jar')"
TROVE="$(find_jar 'trove4j-*.jar')"
# org.jetbrains.annotations: the compiler *backend* needs it on its own
# classpath to emit @NotNull. Without it the failure is a
# `BackendException: Exception during IR lowering`, which looks like a source
# bug and is not one. Recorded because it cost time.
ANNOT="$(find_jar 'annotations-13.0.jar')"
RNAAR="$(find "$HOME/.gradle/caches" -name 'react-android-*-debug.aar' 2>/dev/null | sort -V | tail -1)"
for v in KC STD REFLECT CORO TROVE ANNOT; do
  [ -n "${!v}" ] || die "no $v in the Gradle cache; build the example app once first"
done
# react-android is needed only by IntlPackage.kt (the ReactPackage registration).
# IntlPlatform.kt — every line of android.icu in this module — depends on nothing
# but android.jar, so its compilation must not be blocked by a missing AAR. A
# check that cannot run on a machine without a warm React Native Gradle cache is
# a check nobody runs.
KOTLIN_SOURCES=("$MODULE"/android/src/main/java/com/intl/*.kt)
AARCP=""
if [ -n "$RNAAR" ]; then
  mkdir -p "$OUT/aar" && (cd "$OUT/aar" && unzip -oq "$RNAAR" classes.jar)
  AARCP=":$OUT/aar/classes.jar"
else
  echo "  NOTE: no react-android AAR in the Gradle cache."
  echo "        Compiling IntlPlatform.kt only; IntlPackage.kt is SKIPPED."
  echo "        Build the example app once to check it too."
  KOTLIN_SOURCES=("$MODULE"/android/src/main/java/com/intl/IntlPlatform.kt)
fi

java -cp "$KC:$STD:$REFLECT:$CORO:$TROVE:$ANNOT" \
     org.jetbrains.kotlin.cli.jvm.K2JVMCompiler \
     -nowarn -no-stdlib \
     -cp "$ANDJAR:$STD$AARCP" \
     -d "$OUT/classes" \
     "${KOTLIN_SOURCES[@]}" > "$OUT/kotlinc.log" 2>&1 || true
grep -v '^warning:' "$OUT/kotlinc.log" | grep -v '^$' || true
# `set -e` and `grep -q ... && die` do not mix: a grep that finds nothing
# returns 1, which under `set -e` ends the script *silently and with status 0*
# through the caller's pipeline. This script was doing exactly that and
# reporting PASS by printing nothing. Hence the explicit `if`.
if grep -q 'error:' "$OUT/kotlinc.log"; then die "kotlin compile"; fi
[ -f "$OUT/classes/com/intl/IntlPlatform.class" ] || die "no IntlPlatform.class"
echo "  compiled $(ls "$OUT/classes/com/intl" | tr '\n' ' ')"

step "JNI signature agreement"
# The C++ resolves each of these by (name, signature) at attach time and gates
# every call on `g_m.ready`. A Kotlin rename or a changed parameter type would
# leave `ready` false and the whole backend would silently degrade to the stub
# with no error anywhere. So the agreement is checked here, from the compiled
# class, against the signatures spelled in android/src/main/cpp/IntlPlatform.cpp.
javap -p -cp "$OUT/classes" com.intl.IntlPlatform > "$OUT/javap.txt"
expect() {
  grep -qF "$1" "$OUT/javap.txt" || die "IntlPlatform is missing: $1"
  echo "  ok  $1"
}
expect 'public static final java.lang.String[] availableLocales();'
expect 'public static final java.lang.String defaultLocale();'
expect 'public static final java.lang.String defaultTimeZone();'
expect 'public static final java.lang.String maximize(java.lang.String);'
expect 'public static final java.lang.String minimize(java.lang.String);'
expect 'public static final java.lang.String canonicalize(java.lang.String);'
expect 'public static final java.lang.String normalizeTimeZone(java.lang.String);'
expect 'public static final java.lang.String[] timeZones();'
expect 'public static final java.lang.String[] calendars();'
expect 'public static final java.lang.String[] numberingSystems();'
expect 'public static final long dtfOpen(java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String);'
expect 'public static final void dtfClose(long);'
expect 'public static final java.lang.String dtfFormat(long, double);'
expect 'public static final java.lang.String[] dtfFormatToParts(long, double);'
expect 'public static final java.lang.String dtfResolved(long, java.lang.String);'
# Stage two. Every one of these is resolved by name and signature in
# android/src/main/cpp/IntlPlatform.cpp and is part of `g_m.ready`, so a
# mismatch degrades the *whole* backend to the stub rather than one service.
expect 'public static final long nfOpen(java.lang.String[]);'
expect 'public static final void nfClose(long);'
expect 'public static final java.lang.String nfFormat(long, double, java.lang.String);'
expect 'public static final java.lang.String[] nfSymbols(long);'
expect 'public static final java.lang.String nfResolved(long, java.lang.String);'
expect 'public static final long colOpen(java.lang.String[]);'
expect 'public static final void colClose(long);'
expect 'public static final int colCompare(long, java.lang.String, java.lang.String);'
expect 'public static final long rtfOpen(java.lang.String[]);'
expect 'public static final void rtfClose(long);'
expect 'public static final java.lang.String rtfFormat(long, double, java.lang.String, java.lang.String);'
expect 'public static final long lfOpen(java.lang.String[]);'
expect 'public static final void lfClose(long);'
expect 'public static final java.lang.String lfFormat(long, java.lang.String[]);'
expect 'public static final java.lang.String displayName(java.lang.String, java.lang.String, java.lang.String, java.lang.String);'
expect 'public static final int[] segment(java.lang.String, java.lang.String, java.lang.String);'
expect 'public static final java.lang.String caseMap(java.lang.String, boolean, java.lang.String);'
expect 'public static final java.lang.String[] collations();'
expect 'public static final java.lang.String[] currencies();'
expect 'public static final int currencyDigits(java.lang.String);'
expect 'public static final java.lang.String[] localeList(java.lang.String, java.lang.String);'
expect 'public static final java.lang.String localeString(java.lang.String, java.lang.String);'
expect 'public static final int[] weekInfo(java.lang.String);'
if ! grep -q 'native void nativeAttach();' "$OUT/javap.txt"; then
  die "IntlPlatform.nativeAttach is not a native instance method"
fi
echo "  ok  private final native void nativeAttach();"

printf '\nverify-android: PASS (native 3 ABIs, kotlin %d file(s), 38 JNI signatures)\n' \
       "${#KOTLIN_SOURCES[@]}"
