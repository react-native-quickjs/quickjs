/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Android platform layer for react-native-quickjs-intl.
 *
 * Hand-written JNI between the shared C++ in cpp/ and the Kotlin in
 * src/main/java/com/intl/IntlPlatform.kt, using the small helper in
 * RnqjsJni.h. No fbjni: what this needs from JNI is an env, a cached class
 * reference, static method calls and string conversion, and that is 200 lines
 * rather than a dependency on React Native's C++ artefacts.
 *
 * WHERE THE ICU WORK HAPPENS, AND WHY NOT HERE
 *   `android.icu` is a Java API. Calling it from C++ would mean one JNI
 *   crossing per ICU call plus a resolved method id per ICU method — dozens of
 *   them — for a result the Kotlin side can compute in one call. So this file
 *   is pure marshalling and the ICU logic is in Kotlin. Every method below is
 *   one crossing.
 *
 * THE COST MODEL THIS IS BUILT AROUND
 *   Construction crosses once, carrying eight strings. `format()` crosses once,
 *   carrying a double, and returns a java.lang.String whose UTF-16 data is
 *   copied straight into a QuickJS string with no UTF-8 round trip.
 *   `formatToParts()` crosses once and returns a flat [type, value, ...] array.
 *
 *   docs/intl-platform-backed.md records the per-call JNI cost as **ASSUMED**
 *   1-5 us and names the measurement that would settle it: one
 *   Intl.DateTimeFormat, 100,000 format() calls, on a physical mid-range
 *   device, with a counter on the native entry point so that "JNI is slow"
 *   can be told apart from "the fast path is not being taken". That
 *   measurement has NOT been made. Nothing in this file assumes an answer:
 *   there is no per-call caching to be invalidated by it, and if the cost
 *   turns out to matter the mitigation is a formatted-string cache on the
 *   Kotlin side keyed by (id, epochMs), which needs no change here.
 *
 * IDS, NOT POINTERS
 *   A formatter is identified by a jlong index into a map on the Kotlin side. A
 *   JNI global reference to the Java formatter would work and is what leaks:
 *   global references are invisible to both garbage collectors and there is no
 *   diagnostic that attributes one to its creator. An integer id cannot leak
 *   silently: a leaked id is a live map entry, visible in a heap dump.
 */

#include "IntlPlatform.h"

#include <jni.h>

#include <memory>
#include <string>
#include <vector>

#include "RnqjsJni.h"

namespace rnqjs::intl {
namespace {

namespace jni = rnqjs::jni;

/*
 * The cached class reference and every method id, resolved once at attach.
 *
 * Resolution happens from a Java-initiated call (IntlPlatform.attach ->
 * nativeAttach) rather than from the JS thread, because FindClass uses the
 * calling thread's classloader and the JS thread's is the system one — an
 * application class simply does not resolve there. That is the classic "works
 * in a unit test, NoClassDefFoundError on device" JNI bug and this is how it is
 * avoided.
 */
struct Methods {
  jni::ClassRef cls;
  jmethodID availableLocales = nullptr;
  jmethodID defaultLocale = nullptr;
  jmethodID defaultTimeZone = nullptr;
  jmethodID maximize = nullptr;
  jmethodID minimize = nullptr;
  jmethodID canonicalize = nullptr;
  jmethodID normalizeTimeZone = nullptr;
  jmethodID timeZones = nullptr;
  jmethodID calendars = nullptr;
  jmethodID numberingSystems = nullptr;
  jmethodID dtfOpen = nullptr;
  jmethodID dtfClose = nullptr;
  jmethodID dtfFormat = nullptr;
  jmethodID dtfFormatToParts = nullptr;
  jmethodID dtfResolved = nullptr;
  /* stage two */
  jmethodID nfOpen = nullptr;
  jmethodID nfClose = nullptr;
  jmethodID nfFormat = nullptr;
  jmethodID nfSymbols = nullptr;
  jmethodID nfResolved = nullptr;
  jmethodID colOpen = nullptr;
  jmethodID colClose = nullptr;
  jmethodID colCompare = nullptr;
  jmethodID rtfOpen = nullptr;
  jmethodID rtfClose = nullptr;
  jmethodID rtfFormat = nullptr;
  jmethodID lfOpen = nullptr;
  jmethodID lfClose = nullptr;
  jmethodID lfFormat = nullptr;
  jmethodID displayName = nullptr;
  jmethodID segment = nullptr;
  jmethodID caseMap = nullptr;
  jmethodID collations = nullptr;
  jmethodID currencies = nullptr;
  jmethodID currencyDigits = nullptr;
  jmethodID localeList = nullptr;
  jmethodID localeString = nullptr;
  jmethodID weekInfo = nullptr;
  bool ready = false;
};

Methods g_m;

constexpr const char *kStringArraySig = "()[Ljava/lang/String;";
constexpr const char *kStringSig = "()Ljava/lang/String;";
constexpr const char *kStringStringSig =
    "(Ljava/lang/String;)Ljava/lang/String;";

void resolveMethods(const jni::Env &env) {
  g_m.availableLocales =
      g_m.cls.staticMethod(env, "availableLocales", kStringArraySig);
  g_m.defaultLocale = g_m.cls.staticMethod(env, "defaultLocale", kStringSig);
  g_m.defaultTimeZone =
      g_m.cls.staticMethod(env, "defaultTimeZone", kStringSig);
  g_m.maximize = g_m.cls.staticMethod(env, "maximize", kStringStringSig);
  g_m.minimize = g_m.cls.staticMethod(env, "minimize", kStringStringSig);
  g_m.canonicalize =
      g_m.cls.staticMethod(env, "canonicalize", kStringStringSig);
  g_m.normalizeTimeZone =
      g_m.cls.staticMethod(env, "normalizeTimeZone", kStringStringSig);
  g_m.timeZones = g_m.cls.staticMethod(env, "timeZones", kStringArraySig);
  g_m.calendars = g_m.cls.staticMethod(env, "calendars", kStringArraySig);
  g_m.numberingSystems =
      g_m.cls.staticMethod(env, "numberingSystems", kStringArraySig);
  g_m.dtfOpen = g_m.cls.staticMethod(
      env, "dtfOpen",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/"
      "String;"
      "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/"
      "String;)J");
  g_m.dtfClose = g_m.cls.staticMethod(env, "dtfClose", "(J)V");
  g_m.dtfFormat =
      g_m.cls.staticMethod(env, "dtfFormat", "(JD)Ljava/lang/String;");
  g_m.dtfFormatToParts =
      g_m.cls.staticMethod(env, "dtfFormatToParts", "(JD)[Ljava/lang/String;");
  g_m.dtfResolved = g_m.cls.staticMethod(
      env, "dtfResolved", "(JLjava/lang/String;)Ljava/lang/String;");

  constexpr const char *kStr = "Ljava/lang/String;";
  (void)kStr;
  g_m.nfOpen = g_m.cls.staticMethod(env, "nfOpen", "([Ljava/lang/String;)J");
  g_m.nfClose = g_m.cls.staticMethod(env, "nfClose", "(J)V");
  g_m.nfFormat = g_m.cls.staticMethod(
      env, "nfFormat", "(JDLjava/lang/String;)Ljava/lang/String;");
  g_m.nfSymbols =
      g_m.cls.staticMethod(env, "nfSymbols", "(J)[Ljava/lang/String;");
  g_m.nfResolved = g_m.cls.staticMethod(
      env, "nfResolved", "(JLjava/lang/String;)Ljava/lang/String;");
  g_m.colOpen = g_m.cls.staticMethod(env, "colOpen", "([Ljava/lang/String;)J");
  g_m.colClose = g_m.cls.staticMethod(env, "colClose", "(J)V");
  g_m.colCompare = g_m.cls.staticMethod(
      env, "colCompare", "(JLjava/lang/String;Ljava/lang/String;)I");
  g_m.rtfOpen = g_m.cls.staticMethod(env, "rtfOpen", "([Ljava/lang/String;)J");
  g_m.rtfClose = g_m.cls.staticMethod(env, "rtfClose", "(J)V");
  g_m.rtfFormat = g_m.cls.staticMethod(
      env, "rtfFormat",
      "(JDLjava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
  g_m.lfOpen = g_m.cls.staticMethod(env, "lfOpen", "([Ljava/lang/String;)J");
  g_m.lfClose = g_m.cls.staticMethod(env, "lfClose", "(J)V");
  g_m.lfFormat = g_m.cls.staticMethod(
      env, "lfFormat", "(J[Ljava/lang/String;)Ljava/lang/String;");
  g_m.displayName = g_m.cls.staticMethod(
      env, "displayName",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/"
      "String;)Ljava/lang/String;");
  g_m.segment = g_m.cls.staticMethod(
      env, "segment",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[I");
  g_m.caseMap = g_m.cls.staticMethod(
      env, "caseMap",
      "(Ljava/lang/String;ZLjava/lang/String;)Ljava/lang/String;");
  g_m.collations = g_m.cls.staticMethod(env, "collations", kStringArraySig);
  g_m.currencies = g_m.cls.staticMethod(env, "currencies", kStringArraySig);
  g_m.currencyDigits =
      g_m.cls.staticMethod(env, "currencyDigits", "(Ljava/lang/String;)I");
  g_m.localeList = g_m.cls.staticMethod(
      env, "localeList",
      "(Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;");
  g_m.localeString = g_m.cls.staticMethod(
      env, "localeString",
      "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
  g_m.weekInfo =
      g_m.cls.staticMethod(env, "weekInfo", "(Ljava/lang/String;)[I");

  // `ready` gates every call below. A partially-resolved method table would
  // otherwise call through a null jmethodID, which is undefined behaviour
  // rather than an exception.
  g_m.ready = g_m.cls && g_m.availableLocales != nullptr &&
              g_m.defaultLocale != nullptr && g_m.defaultTimeZone != nullptr &&
              g_m.maximize != nullptr && g_m.minimize != nullptr &&
              g_m.canonicalize != nullptr && g_m.normalizeTimeZone != nullptr &&
              g_m.timeZones != nullptr && g_m.calendars != nullptr &&
              g_m.numberingSystems != nullptr && g_m.dtfOpen != nullptr &&
              g_m.dtfClose != nullptr && g_m.dtfFormat != nullptr &&
              g_m.dtfFormatToParts != nullptr && g_m.dtfResolved != nullptr &&
              /*
               * Stage two is included in `ready` on purpose. A build whose
               * Kotlin half is older than its C++ half would otherwise install
               * a platform that answers DateTimeFormat and returns null for
               * every other service — an Intl where NumberFormat throws, which
               * is worse than the root-locale stub. All or nothing.
               */
              g_m.nfOpen != nullptr && g_m.nfClose != nullptr &&
              g_m.nfFormat != nullptr && g_m.nfSymbols != nullptr &&
              g_m.nfResolved != nullptr && g_m.colOpen != nullptr &&
              g_m.colClose != nullptr && g_m.colCompare != nullptr &&
              g_m.rtfOpen != nullptr && g_m.rtfClose != nullptr &&
              g_m.rtfFormat != nullptr && g_m.lfOpen != nullptr &&
              g_m.lfClose != nullptr && g_m.lfFormat != nullptr &&
              g_m.displayName != nullptr && g_m.segment != nullptr &&
              g_m.caseMap != nullptr && g_m.collations != nullptr &&
              g_m.currencies != nullptr && g_m.currencyDigits != nullptr &&
              g_m.localeList != nullptr && g_m.localeString != nullptr &&
              g_m.weekInfo != nullptr;
}

/* ------------------------------------------------------------------------- */

/// Maps the Kotlin side's part-type name back onto the enum. One string compare
/// per part; the alternative is a second parallel enum in Kotlin, which is a
/// second place to get out of sync.
PartType partTypeFromName(const std::string &name) {
  if (name == "literal") return PartType::Literal;
  if (name == "era") return PartType::Era;
  if (name == "year") return PartType::Year;
  if (name == "relatedYear") return PartType::RelatedYear;
  if (name == "yearName") return PartType::YearName;
  if (name == "month") return PartType::Month;
  if (name == "day") return PartType::Day;
  if (name == "weekday") return PartType::Weekday;
  if (name == "dayPeriod") return PartType::DayPeriod;
  if (name == "hour") return PartType::Hour;
  if (name == "minute") return PartType::Minute;
  if (name == "second") return PartType::Second;
  if (name == "fractionalSecond") return PartType::FractionalSecond;
  if (name == "timeZoneName") return PartType::TimeZoneName;
  return PartType::Unknown;
}

class AndroidFormatter final : public DateTimeFormatter {
 public:
  explicit AndroidFormatter(jlong id) : id_(id) {}

  ~AndroidFormatter() override {
    // Deterministic: this destructor runs from the QuickJS finalizer of the
    // JavaScript formatter object, so the android.icu.text.DateFormat is
    // released at the moment its owner is, not whenever the Java collector
    // gets to it.
    jni::Env env;
    if (env.valid() && g_m.ready) {
      env->CallStaticVoidMethod(g_m.cls.get(), g_m.dtfClose, id_);
      env.check();
    }
  }

  bool format(double epochMs, std::u16string &out) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return false;
    jni::LocalFrame frame(env, 4);
    auto s = static_cast<jstring>(env->CallStaticObjectMethod(
        g_m.cls.get(), g_m.dtfFormat, id_, epochMs));
    if (env.check() || s == nullptr) return false;
    // UTF-16 straight out of the java.lang.String; no UTF-8 round trip.
    out = jni::toU16(env, s);
    return true;
  }

  bool formatToParts(double epochMs, FormattedParts &out) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return false;
    jni::LocalFrame frame(env, 8);
    auto array = static_cast<jobjectArray>(env->CallStaticObjectMethod(
        g_m.cls.get(), g_m.dtfFormatToParts, id_, epochMs));
    if (env.check() || array == nullptr) return false;

    const jsize n = env->GetArrayLength(array);
    out.text.clear();
    out.parts.clear();
    out.parts.reserve(static_cast<size_t>(n / 2));
    /*
     * The array is flat [type, value, type, value, ...], so it is 2x the part
     * count in local references. A full date-time pattern is 10-16 parts, i.e.
     * up to 32 references, which is well inside the table — but each is deleted
     * as it is consumed anyway, because the same code path serves patterns we
     * have not seen.
     */
    for (jsize i = 0; i + 1 < n; i += 2) {
      auto typeStr = static_cast<jstring>(env->GetObjectArrayElement(array, i));
      if (env.check()) return false;
      jni::Local<jstring> ownedType(env.get(), typeStr);
      auto valueStr =
          static_cast<jstring>(env->GetObjectArrayElement(array, i + 1));
      if (env.check()) return false;
      jni::Local<jstring> ownedValue(env.get(), valueStr);

      const PartType type = partTypeFromName(jni::toUtf8(env, ownedType.get()));
      const std::u16string value = jni::toU16(env, ownedValue.get());
      const auto begin = static_cast<int32_t>(out.text.size());
      out.text += value;
      const auto end = static_cast<int32_t>(out.text.size());
      if (!out.parts.empty() && out.parts.back().type == type) {
        out.parts.back().end = end;  // merge adjacent runs of one type
      } else {
        out.parts.push_back(Part{type, begin, end});
      }
    }
    return true;
  }

  std::string resolved(const std::string &key) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return {};
    jni::LocalFrame frame(env, 4);
    jni::Local<jstring> jkey = jni::fromUtf8(env, key);
    if (!jkey) return {};
    auto s = static_cast<jstring>(env->CallStaticObjectMethod(
        g_m.cls.get(), g_m.dtfResolved, id_, jkey.get()));
    if (env.check() || s == nullptr) return {};
    jni::Local<jstring> owned(env.get(), s);
    return jni::toUtf8(env, owned.get());
  }

 private:
  jlong id_;
};

/* ==========================================================================
 * Stage two: NumberFormat, Collator, RelativeTimeFormat, ListFormat,
 * DisplayNames, Segmenter, case mapping and the enumerations.
 *
 * THE OPTION BAG CROSSES AS A String[], NOT AS N PARAMETERS
 *   NumberOptions has nineteen fields. A nineteen-parameter JNI signature is
 *   one where inserting a field in the middle silently shifts every later one
 *   and JNI reports nothing at all — the same failure shape as registering a
 *   QuickJS setter under the wrong JSCFunctionEnum, which this module has
 *   already shipped once. The order is written down once, in
 *   numberOptionBag() below, and mirrored by the NF_* index constants in
 *   IntlPlatform.kt. Both sides carry the field count so a mismatch is a
 *   rejected call rather than a wrong answer.
 *
 * WHAT IS DELIBERATELY NOT ASKED OF ANDROID
 *   Rounding for `notation: "standard"`, and formatToParts. The first is
 *   pre-computed by js/intl.js (the decimalString contract in
 *   cpp/IntlPlatform.h) and the second by the shared decomposition in
 *   cpp/IntlNumberParts.cpp. Both are deliberate, and for the same reason:
 *   they are the two places where using each platform's own implementation
 *   would give an app's iOS and Android builds different answers for identical
 *   input.
 * ========================================================================== */

/// Builds a java.lang.String[] for an option bag. Returns nullptr on failure.
jobjectArray newStringArray(
    const jni::Env &env, const std::vector<std::string> &items) {
  jclass stringClass = env->FindClass("java/lang/String");
  if (stringClass == nullptr) {
    env->ExceptionClear();
    return nullptr;
  }
  jobjectArray arr = env->NewObjectArray(
      static_cast<jsize>(items.size()), stringClass, nullptr);
  env->DeleteLocalRef(stringClass);
  if (arr == nullptr) {
    env->ExceptionClear();
    return nullptr;
  }
  for (size_t i = 0; i < items.size(); i++) {
    jni::Local<jstring> s = jni::fromUtf8(env, items[i]);
    env->SetObjectArrayElement(arr, static_cast<jsize>(i), s.get());
    if (env.check()) return nullptr;
  }
  return arr;
}

jobjectArray newStringArrayU16(
    const jni::Env &env, const std::vector<std::u16string> &items) {
  std::vector<std::string> utf8;
  utf8.reserve(items.size());
  for (const std::u16string &s : items) {
    // Round-tripping list elements through UTF-8 is safe: they are JavaScript
    // strings and NewStringUTF is the only jstring constructor the helper
    // exposes. Lone surrogates would not survive, and a list element containing
    // one is not something CLDR list patterns can render meaningfully anyway.
    std::string out;
    for (char16_t c : s) {
      if (c < 0x80) {
        out += static_cast<char>(c);
      } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
      } else {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
      }
    }
    utf8.push_back(out);
  }
  return newStringArray(env, utf8);
}

std::vector<std::string> numberOptionBag(const NumberOptions &o) {
  auto i2s = [](int32_t v) { return std::to_string(v); };
  return {
      o.locale,
      o.numberingSystem,
      o.style,
      o.currency,
      o.currencyDisplay,
      o.currencySign,
      o.unit,
      o.unitDisplay,
      o.notation,
      o.compactDisplay,
      o.signDisplay,
      o.roundingMode,
      o.useGrouping,
      i2s(o.minimumIntegerDigits),
      i2s(o.minimumFractionDigits),
      i2s(o.maximumFractionDigits),
      i2s(o.minimumSignificantDigits),
      i2s(o.maximumSignificantDigits),
      i2s(o.roundingIncrement),
  };
}
static_assert(
    true, "numberOptionBag mirrors NF_FIELD_COUNT = 19 in IntlPlatform.kt");

/// Calls a `static void name(long)` close method, tolerating a dead VM.
void callClose(jmethodID m, jlong id) {
  if (m == nullptr || !g_m.ready) return;
  jni::Env env;
  if (!env.valid()) return;
  env->CallStaticVoidMethod(g_m.cls.get(), m, id);
  env.check();
}

class AndroidNumberFormatter final : public NumberFormatter {
 public:
  AndroidNumberFormatter(jlong id, NumberOptions o)
      : id_(id), o_(std::move(o)) {}
  ~AndroidNumberFormatter() override {
    callClose(g_m.nfClose, id_);
  }

  /*
   * The `exactDouble` hint is accepted and DELIBERATELY IGNORED, so Android's
   * output and its cost are both exactly what they were before the hint
   * existed.
   *
   * The hint's payoff is specific to Foundation: -[NSNumberFormatter
   * stringFromNumber:] takes a 4.1x slower internal path for an
   * NSDecimalNumber than for a double (MEASURED, see the contract in
   * cpp/IntlPlatform.h). Whether android.icu.text.DecimalFormat has the same
   * asymmetry is **unmeasured**, and this module has no way to run android.icu
   * from a macOS host. Taking the hint here on the strength of an analogy is
   * how the two backends would start answering differently in a way no test on
   * this machine could see, so it is not taken.
   *
   * The measurement that would settle it: one Intl.NumberFormat and 100,000
   * format() calls on a physical mid-range device, timed with
   * `nfFormat(handle, v, decimalString)` against a variant that passes the
   * double, with Intl.__rnqjsPerf.stats() read at the end to confirm
   * `exactDoubleHits` is non-zero there too.
   */
  bool format(
      double value, const std::string &decimalString, uint32_t /*hints*/,
      std::u16string &out) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return false;
    jni::LocalFrame frame(env, 4);
    jni::Local<jstring> dec = jni::fromUtf8(env, decimalString);
    auto s = static_cast<jstring>(env->CallStaticObjectMethod(
        g_m.cls.get(), g_m.nfFormat, id_, value, dec.get()));
    if (env.check() || s == nullptr) return false;
    out = jni::toU16(env, s);
    return true;
  }

  std::string resolved(const std::string &key) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return {};
    jni::LocalFrame frame(env, 4);
    jni::Local<jstring> k = jni::fromUtf8(env, key);
    auto s = static_cast<jstring>(env->CallStaticObjectMethod(
        g_m.cls.get(), g_m.nfResolved, id_, k.get()));
    if (env.check() || s == nullptr) return {};
    return jni::toUtf8(env, s);
  }

  /**
   * The symbol block, in the order IntlPlatform.kt's nfSymbols documents:
   * decimal, group, minus, plus, percent, exponent, NaN, infinity, currency,
   * then the ten digits. A short array means the platform declined and the
   * decomposition falls back to ASCII digits with no symbols, which is coarse
   * (deviation D1) rather than wrong.
   */
  void symbols(NumberSymbols &s) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return;
    jni::LocalFrame frame(env, 24);
    auto arr = static_cast<jobjectArray>(
        env->CallStaticObjectMethod(g_m.cls.get(), g_m.nfSymbols, id_));
    if (env.check() || arr == nullptr) return;
    const jsize n = env->GetArrayLength(arr);
    if (n < 9) return;
    auto at = [&](jsize i) -> std::u16string {
      auto js = static_cast<jstring>(env->GetObjectArrayElement(arr, i));
      if (env.check() || js == nullptr) return {};
      jni::Local<jstring> owned(env.get(), js);
      return jni::toU16(env, owned.get());
    };
    s.decimal = at(0);
    s.group = at(1);
    s.minusSign = at(2);
    s.plusSign = at(3);
    s.percent = at(4);
    s.exponential = at(5);
    s.nan = at(6);
    s.infinity = at(7);
    s.currency = at(8);
    s.digits.clear();
    if (n >= 19) {
      for (jsize i = 9; i < 19; i++) s.digits.push_back(at(i));
    }
  }

 private:
  jlong id_;
  NumberOptions o_;
};

class AndroidCollator final : public Collator {
 public:
  AndroidCollator(jlong id, CollatorOptions o) : id_(id), o_(std::move(o)) {}
  ~AndroidCollator() override {
    callClose(g_m.colClose, id_);
  }

  /*
   * The views are copied into `std::u16string`s here and nowhere else.
   *
   * The seam changed to `std::u16string_view` for the Apple backend, where a
   * no-copy CFString wrapper is available (see ios/IntlPlatform.mm). JNI has
   * no equivalent: `NewString` copies into the JVM heap unconditionally, so
   * there is nothing to win and the copy is made explicitly rather than
   * hidden. Android's per-comparison cost and its answers are both unchanged
   * by that seam change, deliberately: the module has no way to run
   * android.icu from a macOS host, and divergence between the two backends is
   * worse than slowness.
   */
  int32_t compare(std::u16string_view a, std::u16string_view b) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return a < b ? -1 : a > b ? 1 : 0;
    jni::LocalFrame frame(env, 4);
    std::vector<std::u16string> pair{std::u16string(a), std::u16string(b)};
    jobjectArray arr = newStringArrayU16(env, pair);
    if (arr == nullptr) return a < b ? -1 : a > b ? 1 : 0;
    jni::Local<jobjectArray> owned(env.get(), arr);
    auto x = static_cast<jstring>(env->GetObjectArrayElement(arr, 0));
    jni::Local<jstring> ox(env.get(), x);
    auto y = static_cast<jstring>(env->GetObjectArrayElement(arr, 1));
    jni::Local<jstring> oy(env.get(), y);
    const jint r = env->CallStaticIntMethod(
        g_m.cls.get(), g_m.colCompare, id_, ox.get(), oy.get());
    if (env.check()) return a < b ? -1 : a > b ? 1 : 0;
    return static_cast<int32_t>(r);
  }

  std::string resolved(const std::string &key) override {
    if (key == "collation")
      return o_.collation.empty() ? "default" : o_.collation;
    return {};
  }

 private:
  jlong id_;
  CollatorOptions o_;
};

class AndroidRelativeTimeFormatter final : public RelativeTimeFormatter {
 public:
  AndroidRelativeTimeFormatter(jlong id, RelativeTimeOptions o)
      : id_(id), o_(std::move(o)) {}
  ~AndroidRelativeTimeFormatter() override {
    callClose(g_m.rtfClose, id_);
  }

  bool format(
      double value, const std::string &unit, std::u16string &out) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return false;
    jni::LocalFrame frame(env, 6);
    jni::Local<jstring> u = jni::fromUtf8(env, unit);
    jni::Local<jstring> n = jni::fromUtf8(env, o_.numeric);
    auto s = static_cast<jstring>(env->CallStaticObjectMethod(
        g_m.cls.get(), g_m.rtfFormat, id_, value, u.get(), n.get()));
    if (env.check() || s == nullptr) return false;
    out = jni::toU16(env, s);
    return true;
  }

  std::string resolved(const std::string &key) override {
    (void)key;
    return {};
  }

 private:
  jlong id_;
  RelativeTimeOptions o_;
};

class AndroidListFormatter final : public ListFormatter {
 public:
  AndroidListFormatter(jlong id, ListFormatOptions o)
      : id_(id), o_(std::move(o)) {}
  ~AndroidListFormatter() override {
    callClose(g_m.lfClose, id_);
  }

  bool format(
      const std::vector<std::u16string> &items, std::u16string &out) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return false;
    jni::LocalFrame frame(env, static_cast<int>(items.size()) + 8);
    jobjectArray arr = newStringArrayU16(env, items);
    if (arr == nullptr) return false;
    jni::Local<jobjectArray> owned(env.get(), arr);
    auto s = static_cast<jstring>(env->CallStaticObjectMethod(
        g_m.cls.get(), g_m.lfFormat, id_, owned.get()));
    if (env.check() || s == nullptr) return false;
    out = jni::toU16(env, s);
    return true;
  }

  std::string resolved(const std::string &key) override {
    (void)key;
    return {};
  }

 private:
  jlong id_;
  ListFormatOptions o_;
};

class AndroidPlatform final : public PlatformDefaults {
 public:
  const char *name() override {
    return "android";
  }

  std::vector<std::string> availableLocales() override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return {"en-US"};
    auto out = jni::callStaticStringArray(env, g_m.cls, g_m.availableLocales);
    return out.empty() ? std::vector<std::string>{"en-US"} : out;
  }

  std::string defaultLocale() override {
    std::string s = callNoArgString(g_m.defaultLocale);
    return s.empty() ? std::string("en-US") : s;
  }

  std::string defaultTimeZone() override {
    std::string s = callNoArgString(g_m.defaultTimeZone);
    return s.empty() ? std::string("UTC") : s;
  }

  std::string maximize(const std::string &tag) override {
    return callStringString(g_m.maximize, tag);
  }
  std::string minimize(const std::string &tag) override {
    return callStringString(g_m.minimize, tag);
  }
  std::string canonicalize(const std::string &tag) override {
    return callStringString(g_m.canonicalize, tag);
  }
  std::string normalizeTimeZone(const std::string &tz) override {
    return callStringString(g_m.normalizeTimeZone, tz);
  }

  std::vector<std::string> timeZones() override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return {"UTC"};
    return jni::callStaticStringArray(env, g_m.cls, g_m.timeZones);
  }

  std::vector<std::string> numberingSystems() override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return {"latn"};
    return jni::callStaticStringArray(env, g_m.cls, g_m.numberingSystems);
  }

  std::vector<std::string> calendars() override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return {"gregory"};
    return jni::callStaticStringArray(env, g_m.cls, g_m.calendars);
  }

  std::unique_ptr<DateTimeFormatter> openDateTimeFormat(
      const DateTimeOptions &o) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return nullptr;
    // Eight strings in one frame. The frame is what keeps a construction-heavy
    // workload — a table of rows each building its own formatter — from walking
    // the local reference table up to its 512-entry limit.
    jni::LocalFrame frame(env, 16);
    jni::Local<jstring> a0 = jni::fromUtf8(env, o.locale);
    jni::Local<jstring> a1 = jni::fromUtf8(env, o.calendar);
    jni::Local<jstring> a2 = jni::fromUtf8(env, o.numberingSystem);
    jni::Local<jstring> a3 = jni::fromUtf8(env, o.timeZone);
    jni::Local<jstring> a4 = jni::fromUtf8(env, o.hourCycle);
    jni::Local<jstring> a5 = jni::fromUtf8(env, o.skeleton);
    jni::Local<jstring> a6 = jni::fromUtf8(env, o.dateStyle);
    jni::Local<jstring> a7 = jni::fromUtf8(env, o.timeStyle);
    if (!a0) return nullptr;
    const jlong id = env->CallStaticLongMethod(
        g_m.cls.get(), g_m.dtfOpen, a0.get(), a1.get(), a2.get(), a3.get(),
        a4.get(), a5.get(), a6.get(), a7.get());
    if (env.check() || id == 0) return nullptr;
    return std::make_unique<AndroidFormatter>(id);
  }

 private:
  static std::string callNoArgString(jmethodID m) {
    jni::Env env;
    if (!env.valid() || !g_m.ready || m == nullptr) return {};
    jni::LocalFrame frame(env, 4);
    auto s =
        static_cast<jstring>(env->CallStaticObjectMethod(g_m.cls.get(), m));
    if (env.check() || s == nullptr) return {};
    jni::Local<jstring> owned(env.get(), s);
    return jni::toUtf8(env, owned.get());
  }

  static std::string callStringString(jmethodID m, const std::string &arg) {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return {};
    return jni::callStaticStringString(env, g_m.cls, m, arg);
  }

  /* ---- stage two ------------------------------------------------------- */

  std::unique_ptr<NumberFormatter> openNumberFormat(
      const NumberOptions &o) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return nullptr;
    jni::LocalFrame frame(env, 32);
    jobjectArray bag = newStringArray(env, numberOptionBag(o));
    if (bag == nullptr) return nullptr;
    jni::Local<jobjectArray> owned(env.get(), bag);
    const jlong id =
        env->CallStaticLongMethod(g_m.cls.get(), g_m.nfOpen, owned.get());
    if (env.check() || id == 0) return nullptr;
    return std::make_unique<AndroidNumberFormatter>(id, o);
  }

  std::unique_ptr<Collator> openCollator(const CollatorOptions &o) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return nullptr;
    jni::LocalFrame frame(env, 16);
    const std::vector<std::string> bagValues{
        o.locale,
        o.usage,
        o.sensitivity,
        o.caseFirst,
        o.collation,
        o.numeric ? "1" : "",
        o.ignorePunctuation ? "1" : ""};
    jobjectArray bag = newStringArray(env, bagValues);
    if (bag == nullptr) return nullptr;
    jni::Local<jobjectArray> owned(env.get(), bag);
    const jlong id =
        env->CallStaticLongMethod(g_m.cls.get(), g_m.colOpen, owned.get());
    if (env.check() || id == 0) return nullptr;
    CollatorOptions resolved = o;
    if (o.usage == "search") resolved.collation.clear();
    return std::make_unique<AndroidCollator>(id, resolved);
  }

  std::unique_ptr<RelativeTimeFormatter> openRelativeTimeFormat(
      const RelativeTimeOptions &o) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return nullptr;
    jni::LocalFrame frame(env, 16);
    jobjectArray bag =
        newStringArray(env, {o.locale, o.numberingSystem, o.numeric, o.style});
    if (bag == nullptr) return nullptr;
    jni::Local<jobjectArray> owned(env.get(), bag);
    const jlong id =
        env->CallStaticLongMethod(g_m.cls.get(), g_m.rtfOpen, owned.get());
    if (env.check() || id == 0) return nullptr;
    return std::make_unique<AndroidRelativeTimeFormatter>(id, o);
  }

  std::unique_ptr<ListFormatter> openListFormat(
      const ListFormatOptions &o) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return nullptr;
    jni::LocalFrame frame(env, 16);
    jobjectArray bag = newStringArray(env, {o.locale, o.type, o.style});
    if (bag == nullptr) return nullptr;
    jni::Local<jobjectArray> owned(env.get(), bag);
    const jlong id =
        env->CallStaticLongMethod(g_m.cls.get(), g_m.lfOpen, owned.get());
    /*
     * id == 0 means android.icu.text.ListFormatter did not exist: it is API 26
     * and React Native's minSdk is 24. Returning nullptr makes js/intl.js fall
     * back to a comma join rather than throw, which is deviation D23 — an
     * Intl.ListFormat that works badly on two old API levels beats one that
     * throws.
     */
    if (env.check() || id == 0) return nullptr;
    return std::make_unique<AndroidListFormatter>(id, o);
  }

  std::string displayName(
      const std::string &locale, const std::string &type,
      const std::string &style, const std::string &code) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return {};
    jni::LocalFrame frame(env, 8);
    jni::Local<jstring> l = jni::fromUtf8(env, locale);
    jni::Local<jstring> t = jni::fromUtf8(env, type);
    jni::Local<jstring> st = jni::fromUtf8(env, style);
    jni::Local<jstring> c = jni::fromUtf8(env, code);
    auto s = static_cast<jstring>(env->CallStaticObjectMethod(
        g_m.cls.get(), g_m.displayName, l.get(), t.get(), st.get(), c.get()));
    if (env.check() || s == nullptr) return {};
    return jni::toUtf8(env, s);
  }

  std::vector<Segment> segment(
      const std::string &locale, const std::string &granularity,
      const std::u16string &text) override {
    jni::Env env;
    std::vector<Segment> out;
    if (!env.valid() || !g_m.ready) return out;
    jni::LocalFrame frame(env, 8);
    std::vector<std::u16string> one{text};
    jobjectArray holder = newStringArrayU16(env, one);
    if (holder == nullptr) return out;
    jni::Local<jobjectArray> ownedHolder(env.get(), holder);
    auto js = static_cast<jstring>(env->GetObjectArrayElement(holder, 0));
    jni::Local<jstring> ownedText(env.get(), js);
    jni::Local<jstring> l = jni::fromUtf8(env, locale);
    jni::Local<jstring> g = jni::fromUtf8(env, granularity);
    auto arr = static_cast<jintArray>(env->CallStaticObjectMethod(
        g_m.cls.get(), g_m.segment, l.get(), g.get(), ownedText.get()));
    if (env.check() || arr == nullptr) return out;
    jni::Local<jintArray> ownedArr(env.get(), arr);
    const jsize n = env->GetArrayLength(arr);
    std::vector<jint> buf(static_cast<size_t>(n));
    if (n > 0) env->GetIntArrayRegion(arr, 0, n, buf.data());
    if (env.check()) return {};
    for (jsize i = 0; i + 2 < n; i += 3) {
      out.push_back(Segment{buf[i], buf[i + 1], buf[i + 2] != 0});
    }
    return out;
  }

  std::u16string caseMap(
      const std::string &locale, bool upper,
      const std::u16string &text) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return text;
    jni::LocalFrame frame(env, 8);
    std::vector<std::u16string> one{text};
    jobjectArray holder = newStringArrayU16(env, one);
    if (holder == nullptr) return text;
    jni::Local<jobjectArray> ownedHolder(env.get(), holder);
    auto js = static_cast<jstring>(env->GetObjectArrayElement(holder, 0));
    jni::Local<jstring> ownedText(env.get(), js);
    jni::Local<jstring> l = jni::fromUtf8(env, locale);
    auto s = static_cast<jstring>(env->CallStaticObjectMethod(
        g_m.cls.get(), g_m.caseMap, l.get(),
        static_cast<jboolean>(upper ? JNI_TRUE : JNI_FALSE), ownedText.get()));
    if (env.check() || s == nullptr) return text;
    return jni::toU16(env, s);
  }

  std::vector<std::string> collations() override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return {};
    return jni::callStaticStringArray(env, g_m.cls, g_m.collations);
  }

  std::vector<std::string> currencies() override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return {};
    return jni::callStaticStringArray(env, g_m.cls, g_m.currencies);
  }

  int32_t currencyDigits(const std::string &code) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return -1;
    jni::LocalFrame frame(env, 4);
    jni::Local<jstring> c = jni::fromUtf8(env, code);
    const jint n =
        env->CallStaticIntMethod(g_m.cls.get(), g_m.currencyDigits, c.get());
    if (env.check()) return -1;
    return static_cast<int32_t>(n);
  }

  std::vector<std::string> localeCalendars(const std::string &l) override {
    return localeStrings(l, "calendars");
  }
  std::vector<std::string> localeNumberingSystems(
      const std::string &l) override {
    return localeStrings(l, "numberingSystems");
  }
  std::vector<std::string> localeTimeZones(const std::string &l) override {
    return localeStrings(l, "timeZones");
  }
  std::vector<std::string> localeCollations(const std::string &l) override {
    return localeStrings(l, "collations");
  }

  std::string localeHourCycle(const std::string &l) override {
    return localeStringOf(l, "hourCycle");
  }
  std::string localeTextDirection(const std::string &l) override {
    return localeStringOf(l, "textDirection");
  }

  bool localeWeekInfo(const std::string &locale, WeekInfo &out) override {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return false;
    jni::LocalFrame frame(env, 4);
    jni::Local<jstring> l = jni::fromUtf8(env, locale);
    auto arr = static_cast<jintArray>(
        env->CallStaticObjectMethod(g_m.cls.get(), g_m.weekInfo, l.get()));
    if (env.check() || arr == nullptr) return false;
    jni::Local<jintArray> owned(env.get(), arr);
    const jsize n = env->GetArrayLength(arr);
    if (n < 2) return false;
    std::vector<jint> buf(static_cast<size_t>(n));
    env->GetIntArrayRegion(arr, 0, n, buf.data());
    if (env.check()) return false;
    out.firstDay = buf[0];
    out.minimalDays = buf[1];
    out.weekend.clear();
    for (jsize i = 2; i < n; i++) out.weekend.push_back(buf[i]);
    return true;
  }

 private:
  std::vector<std::string> localeStrings(
      const std::string &locale, const char *key) {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return {};
    jni::LocalFrame frame(env, 16);
    jni::Local<jstring> l = jni::fromUtf8(env, locale);
    jni::Local<jstring> k = jni::fromUtf8(env, key);
    auto arr = static_cast<jobjectArray>(env->CallStaticObjectMethod(
        g_m.cls.get(), g_m.localeList, l.get(), k.get()));
    if (env.check() || arr == nullptr) return {};
    jni::Local<jobjectArray> owned(env.get(), arr);
    const jsize n = env->GetArrayLength(arr);
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(n));
    for (jsize i = 0; i < n; i++) {
      auto js = static_cast<jstring>(env->GetObjectArrayElement(arr, i));
      if (env.check() || js == nullptr) continue;
      jni::Local<jstring> ownedStr(env.get(), js);
      out.push_back(jni::toUtf8(env, ownedStr.get()));
    }
    return out;
  }

  std::string localeStringOf(const std::string &locale, const char *key) {
    jni::Env env;
    if (!env.valid() || !g_m.ready) return {};
    jni::LocalFrame frame(env, 8);
    jni::Local<jstring> l = jni::fromUtf8(env, locale);
    jni::Local<jstring> k = jni::fromUtf8(env, key);
    auto s = static_cast<jstring>(env->CallStaticObjectMethod(
        g_m.cls.get(), g_m.localeString, l.get(), k.get()));
    if (env.check() || s == nullptr) return {};
    return jni::toUtf8(env, s);
  }

 public:
};

AndroidPlatform g_platform;

}  // namespace
}  // namespace rnqjs::intl

/*
 * Folded into the engine .so (RNQJS_QUICKJS_FOLDED) a module cannot define
 * JNI_OnLoad -- only one may exist per library -- and the JavaVM is captured
 * here instead, from the JNIEnv the attach call arrives with. The exported
 * Java_com_intl_IntlPlatform_nativeAttach entry point is what makes the engine
 * .so answer IntlPlatform.attach().
 */
extern "C" JNIEXPORT void JNICALL
Java_com_intl_IntlPlatform_nativeAttach(JNIEnv *e, jobject) {
  JavaVM *vm = nullptr;
  if (e->GetJavaVM(&vm) == JNI_OK) {
    rnqjs::jni::setVM(vm);
  }
  rnqjs::jni::Env env;
  if (!env.valid()) return;
  /*
   * FindClass from *this* call resolves against the app's classloader, because
   * the call arrived from Java. The same FindClass on the JS thread would use
   * the system classloader and fail to find an application class. The local
   * reference dies with this frame, so ClassRef promotes it to a global.
   */
  jclass local = e->FindClass("com/intl/IntlPlatform");
  if (local == nullptr) {
    e->ExceptionClear();
    return;
  }
  rnqjs::intl::g_m.cls.adopt(env, local);
  e->DeleteLocalRef(local);
  rnqjs::intl::resolveMethods(env);
  if (rnqjs::intl::g_m.ready) {
    rnqjs::intl::setPlatform(&rnqjs::intl::g_platform);
  }
  /*
   * If resolution failed, the platform is deliberately NOT installed and the
   * stub in cpp/IntlPlatform.cpp stays active. `Intl` then works in the root
   * locale rather than throwing from every constructor, and the reason is in
   * logcat from Env::check(). A half-installed platform would be worse than
   * either.
   */
}
