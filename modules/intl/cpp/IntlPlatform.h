/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The platform seam for react-native-quickjs-intl.
 *
 * The shared C++ in this directory is platform-independent. Anything that needs
 * NSLocale on Apple or android.icu on Android goes behind this interface, with
 * one implementation per platform:
 *
 *   cpp/IntlPlatform.cpp                     the default, all-stub backend
 *   ios/IntlPlatform.mm                      Objective-C++, Foundation
 *   android/src/main/cpp/IntlPlatform.cpp    hand-written JNI -> android.icu
 *
 * WHY THIS SHAPE
 *   ECMA-402 splits cleanly into *algorithm* and *data*. The algorithm — BCP-47
 *   parsing, option resolution, locale negotiation, `resolvedOptions`
 *   bookkeeping, the order option getters are read in — is written once, in
 *   JavaScript (js/intl.js), and is byte-identical on every platform. The data
 *   is the CLDR database, which both mobile operating systems already carry,
 *   and is reached through here.
 *
 *   docs/intl-platform-backed.md has the motivating measurement: a formatjs
 *   stack covering 15 locales costs 8.28 MB of bundle and 415 ms of startup,
 *   and 92% of that is `DateTimeFormat` plus its timezone table.
 *
 *   Writing the algorithm once also removes most of the platform-divergence
 *   risk by construction. Hermes maintains 2,648 lines of Objective-C++ and
 *   6,870 lines of Java implementing the same algorithms twice, and documents
 *   20+ behavioural differences between them.
 *
 * GRANULARITY, AND WHY IT MATTERS HERE MORE THAN USUALLY
 *   Platform calls are expensive relative to the C++ around them, a JNI
 *   crossing especially, so this interface is coarse on purpose. In particular
 *   an options bag crosses **once**, when a formatter is opened, and a
 *   `format()` is one crossing carrying a double. Hermes's Android bridge
 *   marshals a java.util.HashMap of options and an ArrayList of locales for
 *   every construction and calls into Java for every format
 *   (PlatformIntlAndroid.cpp:47-90); that shape is what this interface exists
 *   to avoid.
 *
 *   The `std::vector<std::string>` returns look expensive and are not: each is
 *   called at most once per runtime (availableLocales, defaultLocale) or once
 *   per `Intl.supportedValuesOf` call, never on a format path.
 *
 * INVARIANTS
 *   - No method here throws, and none of them knows QuickJS exists. That keeps
 *     the JNI and Objective-C code out of the engine's refcounting rules and
 *     makes every backend testable without an engine.
 *   - An empty string means "no opinion" (for the resolution and
 *     canonicalization queries) or "unknown" (for normalizeTimeZone). It never
 *     means "the empty answer".
 *   - Calls arrive on the JS thread only.
 *   - A DateTimeFormatter returned by openDateTimeFormat is owned by exactly
 *     one JavaScript object and destroyed by its finalizer, so an
 *     NSDateFormatter or android.icu.text.DateFormat is released
 *     deterministically when its owner is.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO
 *   - No ECMA-402 semantics. `skeleton` is a CLDR skeleton, not a component
 *     bag; the ECMA-402 to CLDR translation is in js/intl.js and happens once
 *     for both platforms.
 *   - No error reporting beyond success/failure. A locale the OS does not know
 *     is not a program error, so a backend failure becomes a fallback in
 *     JavaScript rather than a thrown exception.
 *   - No formatter caching. That belongs in a backend, keyed by the resolved
 *     skeleton, where the key is a string.
 *
 * FUTURE EXTENSIONS THIS ADMITS
 *   NumberFormat (stage 2), DisplayNames (stage 3), Collator (stage 4) and
 *   ListFormat/RelativeTimeFormat (stage 5) each add one `openX` factory and
 *   one small interface alongside DateTimeFormatter. Nothing already here
 *   changes, and the default backend gains a stub for each.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rnqjs::intl {

/* ==========================================================================
 * Ablation instrumentation
 * ========================================================================== */

/**
 * Runtime-switched ablation arms, compiled in only under
 * `-DRNQJS_INTL_ABLATION`.
 *
 * PURPOSE
 *   Replace a subtraction with a measurement. `docs/intl-native-placement.md`
 *   attributed ~303 ns of a 688 ns `localeCompare` to "the C seam" by
 *   subtracting three other figures, and flagged that as the weakest number on
 *   the page. The fix it named is an ablation build with runtime-switched arms
 *   measured through one binary, which is the same one-binary discipline the
 *   engine work uses; this is that facility.
 *
 *   Its first use immediately corrected the estimate. See
 *   `docs/intl-string-seam.md` for the arms, the numbers and the correction.
 *
 * CONTRACT
 *   - `RNQJS_INTL_ABLATION` is **off by default**, and when it is off
 *     `intlAblation()` does not exist and no call site is compiled. There is
 *     no branch, no function-local static and no guard variable on any hot
 *     path in a shipping build. This matters: the first version of this
 *     facility was an always-on `static const char *e = getenv(...)` inside
 *     `AppleCollator::compare`, which puts a thread-safe-initialization guard
 *     check on the single hottest instruction sequence in the module.
 *   - The arm is read from `RNQJS_INTL_ABL` once, on first call.
 *   - Arm 0 (and an unset variable) is the unmodified engine. Every other arm
 *     produces DELIBERATELY WRONG ANSWERS and exists only to be timed. An
 *     ablation build must never be scored for correctness and must never be
 *     shipped, which is why it is not a default and not a CMake option that
 *     a package consumer can trip over.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   It does not name its arms. An arm number means something only in the file
 *   that reads it, and the meanings are written next to the `if`s and repeated
 *   in the document that quotes the numbers. A central registry of arm names
 *   would be one more thing to keep in sync with the measurement.
 *
 * USAGE
 *   cmake -B build-intl-abl -DCMAKE_BUILD_TYPE=Release -DRNQJS_BUILD_INTL=ON \
 *         -DRNQJS_INTL_BUILD_CLI=ON -DRNQJS_INTL_QJSC=$PWD/build-rel/qjsc-ng \
 *         -DCMAKE_CXX_FLAGS=-DRNQJS_INTL_ABLATION
 *   RNQJS_INTL_ABL=2 build-intl-abl/modules/intl/intl-cli-apple probe.js
 */
#ifdef RNQJS_INTL_ABLATION
int intlAblation();
#define RNQJS_ABL(n) (::rnqjs::intl::intlAblation() == (n))
#else
#define RNQJS_ABL(n) (false)
#endif

/**
 * ECMA-402 part types, as returned by formatToParts.
 *
 * The names live in IntlModule.cpp's kPartTypeNames, indexed by this enum; the
 * static_assert there fails if the two drift.
 */
enum class PartType : int32_t {
  Literal = 0,
  /* DateTimeFormat */
  Era,
  Year,
  RelatedYear,
  YearName,
  Month,
  Day,
  Weekday,
  DayPeriod,
  Hour,
  Minute,
  Second,
  FractionalSecond,
  TimeZoneName,
  Unknown,
  /* NumberFormat. Every one of these is an ECMA-402 part type name; see
     kPartTypeNames in IntlModule.cpp, which the static_assert there pins to
     this enum. */
  Integer,
  Group,
  Decimal,
  Fraction,
  Currency,
  PercentSign,
  PlusSign,
  MinusSign,
  Nan,
  Infinity,
  Compact,
  ExponentSeparator,
  ExponentMinusSign,
  ExponentInteger,
  Unit,
  /* ListFormat */
  Element,
  Count
};

/**
 * A run of the formatted string with a known type.
 *
 * Offsets are into FormattedParts::text, in UTF-16 code units. Offsets rather
 * than copies, so one formatted result is one allocation rather than one per
 * part.
 */
struct Part {
  PartType type = PartType::Literal;
  int32_t begin = 0;  ///< inclusive
  int32_t end = 0;    ///< exclusive
};

/**
 * A formatted result together with its decomposition.
 *
 * `parts` must cover `text` in order, without gaps or overlaps.
 *
 * A backend that cannot supply real boundaries must return exactly one
 * Literal part covering the whole string — deviation D1 in
 * docs/intl-platform-backed.md. It must **not** guess a decomposition: callers
 * index into the result, so a wrong split is worse than an honest coarse one,
 * and there is no way for a caller to tell that a guess was made.
 */
struct FormattedParts {
  std::u16string text;
  std::vector<Part> parts;
};

/**
 * Everything needed to open a date formatter, already resolved by the
 * JavaScript layer. An empty string means "not requested".
 */
struct DateTimeOptions {
  std::string locale;           ///< canonical BCP-47; never empty
  std::string calendar;         ///< "gregory", "islamic", ...
  std::string numberingSystem;  ///< "latn", "arab", ...
  std::string timeZone;         ///< IANA id; empty means system default
  std::string hourCycle;        ///< "h11" | "h12" | "h23" | "h24"

  /**
   * A CLDR date-time skeleton such as "yMMMdjmm".
   *
   * Both platforms take skeletons directly — Apple through
   * -[NSDateFormatter setLocalizedDateFormatFromTemplate:], Android through
   * android.icu.text.DateTimePatternGenerator.getBestPattern — which is why
   * the ECMA-402 component bag is translated into one exactly once, in
   * JavaScript. That symmetry is the main reason the two backends cannot drift
   * the way two independent native implementations do.
   *
   * Empty when dateStyle/timeStyle are used instead.
   */
  std::string skeleton;
  std::string dateStyle;  ///< "full" | "long" | "medium" | "short"
  std::string timeStyle;
};

/**
 * One open date formatter.
 *
 * Owned by one JavaScript object; destroyed by its finalizer.
 */
class DateTimeFormatter {
 public:
  virtual ~DateTimeFormatter() = default;

  /// `epochMs` is milliseconds since the epoch, already clipped and integral.
  /// Returns false if the platform could not format, which the JS layer turns
  /// into a RangeError.
  virtual bool format(double epochMs, std::u16string &out) = 0;

  virtual bool formatToParts(double epochMs, FormattedParts &out) = 0;

  /**
   * What the backend actually chose, which is not always what was asked for.
   *
   * `resolvedOptions()` must report the truth and only the backend knows it: a
   * locale may not support the requested numbering system, and the pattern the
   * platform picked for a skeleton is what determines the real hourCycle.
   *
   * Keys: "locale", "calendar", "numberingSystem", "timeZone", "hourCycle",
   * and "pattern". The last is diagnostics only — no ECMA-402 surface exposes
   * it — and exists because when two platforms disagree about output the first
   * question is always "did they pick the same pattern", and answering it
   * without this means guessing.
   *
   * An empty return means "no opinion"; the JS layer then reports what was
   * requested.
   */
  virtual std::string resolved(const std::string &key) = 0;
};

/* ==========================================================================
 * NumberFormat
 * ========================================================================== */

/**
 * Everything needed to open a number formatter, already resolved by the
 * JavaScript layer.
 *
 * All ECMA-402 defaulting, validation and the *order* option getters are read
 * in happen in js/intl.js. A backend receives a fully-resolved bag and never
 * has an opinion about what an absent field means: -1 spells "not requested"
 * for every integer field, and the empty string does for every string field.
 */
struct NumberOptions {
  std::string locale;           ///< canonical BCP-47; never empty
  std::string numberingSystem;  ///< "" means the locale's own
  std::string style;            ///< "decimal"|"percent"|"currency"|"unit"
  std::string currency;         ///< ISO 4217, upper case
  std::string currencyDisplay;  ///< "code"|"symbol"|"narrowSymbol"|"name"
  std::string currencySign;     ///< "standard"|"accounting"
  std::string unit;             ///< sanctioned unit id, or "x-per-y"
  std::string unitDisplay;      ///< "short"|"narrow"|"long"
  std::string notation;  ///< "standard"|"scientific"|"engineering"|"compact"
  std::string compactDisplay;  ///< "short"|"long"
  std::string signDisplay;  ///< "auto"|"always"|"never"|"exceptZero"|"negative"
  std::string roundingMode;  ///< ECMA-402 spelling: "halfExpand", "trunc", ...
  std::string trailingZeroDisplay;  ///< "auto"|"stripIfInteger"
  std::string useGrouping;          ///< "auto"|"always"|"min2"|"" (false)
  /**
   * "fractionDigits" | "significantDigits" | "morePrecision" | "lessPrecision".
   *
   * Only the notations js/intl.js does not pre-round need this: for standard
   * notation the digits are already final. It exists because ECMA-402's
   * morePrecision/lessPrecision rule compares *two* renderings and picks one,
   * which is a decision a backend cannot make without being told it is the
   * rule in force. MEASURED consequence of omitting it: formatjs's own
   * `supportsES2023()` probe — compact with both digit limits and
   * roundingPriority "morePrecision" — rendered "100M" instead of "100.00M",
   * and @formatjs/intl-numberformat therefore asked to be polyfilled.
   */
  std::string roundingType;

  int32_t minimumIntegerDigits = 1;
  int32_t minimumFractionDigits = -1;
  int32_t maximumFractionDigits = -1;
  int32_t minimumSignificantDigits = -1;
  int32_t maximumSignificantDigits = -1;
  int32_t roundingIncrement = 1;
};

/**
 * The locale's number symbols, as the backend renders them.
 *
 * WHY THE SEAM CARRIES SYMBOLS RATHER THAN A formatToParts()
 *   ECMA-402's number part types are a decomposition of a string whose every
 *   non-digit run is one of the symbols below. Given the symbols and the digit
 *   set, that decomposition is a *parse of our own output* — not a guess — and
 *   it can be written once, in shared C++ (numberFormatToParts), instead of
 *   once per backend.
 *
 *   The alternative is each backend supplying its own boundaries: Android has
 *   android.icu.text.DecimalFormat.formatToCharacterIterator, Apple has no
 *   Objective-C equivalent at all and Hermes gives up with
 *   `llvm_unreachable("formatToParts is unimplemented on Apple platforms")`.
 *   Taking ICU's answer on Android and deriving one on Apple would put a
 *   *different derivation* on each platform for the same input, which is the
 *   exact failure class docs/intl-completeness-map.md exists to prevent. The
 *   text stays native; only the boundaries are shared.
 *
 *   What this costs is stated in docs/intl-platform-backed.md as deviation D18,
 *   together with the measurement that would settle whether ICU's own
 *   decomposition differs from this one.
 */
struct NumberSymbols {
  std::u16string decimal;
  std::u16string group;
  std::u16string minusSign;
  std::u16string plusSign;
  std::u16string percent;
  std::u16string exponential;  ///< "E"
  std::u16string nan;
  std::u16string infinity;
  /// The exact text the currency renders as, when style == "currency". Empty
  /// otherwise. Used to tell a currency run from a literal run.
  std::u16string currency;
  /// The ten digits of the resolved numbering system, in order, as UTF-16.
  /// Non-decimal (algorithmic) systems leave this empty and the whole numeric
  /// run is then reported as a single `integer` part.
  std::vector<std::u16string> digits;
};

/// One open number formatter. Owned by one JavaScript object.
class NumberFormatter {
 public:
  virtual ~NumberFormatter() = default;

  /**
   * Formats one value.
   *
   * THE decimalString CONTRACT — read this before writing a backend.
   *   When `decimalString` is non-empty it is the **final** value to render:
   *   already rounded to the requested digits by js/intl.js, already scaled by
   *   100 for `style: "percent"`, and carrying exactly the fraction digits that
   *   must appear. A backend must render precisely those digits and must not
   *   round again, must not re-scale, and must not apply its own minimum or
   *   maximum fraction digits.
   *
   *   Two things follow, and both are the point:
   *     - rounding is identical on every platform, because it happened once in
   *       JavaScript. A tie such as 2.5 at zero fraction digits cannot resolve
   *       differently in an app's iOS and Android builds.
   *     - precision beyond a double survives, because the digits never go
   *       through one. ECMA-402 formats a *mathematical value* and
   *       `9007199254740993n` must not become `...992`.
   *
   *   `decimalString` is empty exactly when the platform must do the work
   *   itself: NaN, +/-Infinity, and `notation` of "compact", "scientific" or
   *   "engineering", where the scale and the suffix are locale data. `value` is
   *   then the input.
   *
   * THE HINTS — read this second.
   *   `hints` is a bitmask of `kHint*` below. It is ADVICE about what the
   *   JavaScript layer has already proved, never an instruction; a backend
   *   that ignores it entirely renders `decimalString` and produces the same
   *   text, and both the Android and the no-platform backend do exactly that.
   *
   *   `kHintDigitsWithinLimits` says: the digits in `decimalString` lie inside
   *   the formatter's configured [minimumFractionDigits,
   *   maximumFractionDigits], so a backend may configure itself **once** from
   *   those limits instead of re-pinning to each value's own digit count.
   *   js/intl.js sets it exactly when `state.fastRound` holds, which is the
   *   same set of conjuncts that makes the digit string well-behaved:
   *   standard notation, fractionDigits rounding, roundingIncrement 1,
   *   minimumIntegerDigits 1, trailingZeroDisplay "auto" and a style that does
   *   not rescale.
   *
   *   This bit is what removes the `bound-alternating-frac` pathology, and it
   *   has to be set on the general path as well as the fast one. It was not,
   *   in the first version of this change, and the consequence was measured
   *   immediately: `fmt-large-grouped` regressed 6.89 -> 8.73 us, because
   *   `i * 1234567.89` alternates between values whose shortest form fits
   *   maximumFractionDigits and values whose does not, so the formatter
   *   alternated between "pinned to the configured limits" and "pinned to this
   *   value's 2 digits" and paid the 750 ns rebuild on every single call —
   *   a WORSE thrash than the one the bit was introduced to fix.
   *
   *   `kHintExactDouble` additionally says: `value` may be rendered instead of
   *   `decimalString`. It implies `kHintDigitsWithinLimits`. It is set only
   *   when js/intl.js has proved all of:
   *     - `decimalString` is non-empty and is `String(value)`, padded with
   *       trailing zeros up to the formatter's configured minimumFractionDigits
   *       and no further;
   *     - the digit count lies inside the formatter's configured
   *       [minimumFractionDigits, maximumFractionDigits], so the formatter's
   *       own limits render it unchanged;
   *     - `|value| < 10^(15 - maximumFractionDigits)`, which is what makes
   *       rendering the double's exact binary value to that many fraction
   *       digits reproduce `decimalString` rather than its trailing garbage.
   *       The derivation is at `exactDoubleBound` in js/intl.js.
   *
   *   MEASURED motivation (bench/spikes/intl/apple-numberformatter-probe.m):
   *   `-[NSNumberFormatter stringFromNumber:]` costs 2,041 ns given an
   *   NSDecimalNumber and 492 ns given an NSNumber double — 4.1x — and the
   *   NSDecimalNumber has to be built from a string first, at another 519 ns.
   *
   *   `tools/exact-double-differential.mjs` is the check that the two routes
   *   agree: 5,406 cases through one binary with the route switched by an
   *   ablation arm, requiring byte-identical output. It has been
   *   mutation-tested — removing the magnitude bound produces 72 failures.
   */
  static constexpr uint32_t kHintNone = 0;
  static constexpr uint32_t kHintDigitsWithinLimits = 1u << 0;
  static constexpr uint32_t kHintExactDouble = 1u << 1;

  virtual bool format(
      double value, const std::string &decimalString, uint32_t hints,
      std::u16string &out) = 0;

  /// Keys: "locale", "numberingSystem", "minimumFractionDigits",
  /// "maximumFractionDigits", "currency". Empty means "no opinion".
  virtual std::string resolved(const std::string &key) = 0;

  /// The symbols behind the decomposition. Called once per formatter, lazily.
  virtual void symbols(NumberSymbols &out) = 0;
};

/**
 * The shared ECMA-402 part decomposition for a formatted number.
 *
 * INPUT   the formatted text, the symbols that produced it, and enough of the
 *         option bag to name the leftover runs (currency / unit / compact).
 * OUTPUT  parts covering `text` in order with no gaps, exactly as
 *         FormattedParts requires.
 * FAILS   never. A run it cannot classify becomes `literal`, which is wrong in
 *         the D1 sense (coarse) rather than wrong in the dangerous sense
 *         (mislabelled).
 */
void numberFormatToParts(
    const std::u16string &text, const NumberSymbols &symbols,
    const NumberOptions &options, FormattedParts &out);

/* ==========================================================================
 * Collator
 * ========================================================================== */

struct CollatorOptions {
  std::string locale;
  std::string usage;        ///< "sort" | "search"
  std::string sensitivity;  ///< "base"|"accent"|"case"|"variant"
  std::string caseFirst;    ///< "upper"|"lower"|"false"
  std::string collation;    ///< "" means the locale's default
  bool numeric = false;
  bool ignorePunctuation = false;
};

class Collator {
 public:
  virtual ~Collator() = default;
  /**
   * Strictly -1 / 0 / 1, as ECMA-402 requires (not an arbitrary sign).
   *
   * THE VIEW CONTRACT. `a` and `b` are **borrowed** from the engine's own
   * string storage and are valid only for the duration of the call. A backend
   * must not retain them, and must not assume they are NUL-terminated.
   *
   * WHY A VIEW AND NOT A `std::u16string`. This is the hot path of
   * `String.prototype.localeCompare`, so it is also the hot path of every
   * `Array.prototype.sort` comparator an app writes. Taking a `const
   * std::u16string &` forced the caller to heap-allocate and copy both
   * arguments on every comparison; MEASURED decomposition in
   * docs/intl-native-placement.md put that, together with the `NSString`
   * construction below it, at two thirds of a 688 ns call.
   */
  virtual int32_t compare(std::u16string_view a, std::u16string_view b) = 0;
  /// Keys: "locale", "collation", "sensitivity", "caseFirst", "numeric",
  /// "ignorePunctuation". Empty means the JS layer reports what it requested.
  virtual std::string resolved(const std::string &key) = 0;
};

/* ==========================================================================
 * RelativeTimeFormat
 * ========================================================================== */

struct RelativeTimeOptions {
  std::string locale;
  std::string numberingSystem;
  std::string numeric;  ///< "always" | "auto"
  std::string style;    ///< "long" | "short" | "narrow"
};

class RelativeTimeFormatter {
 public:
  virtual ~RelativeTimeFormatter() = default;
  /**
   * `unit` is the singular ECMA-402 unit: "year", "quarter", "month", "week",
   * "day", "hour", "minute", "second".
   *
   * Returns false when the platform cannot express this unit — MEASURED on
   * macOS 26.5, -[NSRelativeDateTimeFormatter localizedStringFromDateComponents:]
   * returns nil for `quarter` in every locale tried. The JS layer turns a false
   * into its own fallback rather than into a throw, because a missing unit is a
   * degradation and not a program error.
   */
  virtual bool format(
      double value, const std::string &unit, std::u16string &out) = 0;
  virtual std::string resolved(const std::string &key) = 0;
};

/* ==========================================================================
 * ListFormat
 * ========================================================================== */

struct ListFormatOptions {
  std::string locale;
  std::string type;   ///< "conjunction" | "disjunction" | "unit"
  std::string style;  ///< "long" | "short" | "narrow"
};

class ListFormatter {
 public:
  virtual ~ListFormatter() = default;
  virtual bool format(
      const std::vector<std::u16string> &items, std::u16string &out) = 0;
  virtual std::string resolved(const std::string &key) = 0;
};

/* ==========================================================================
 * Segmenter
 * ========================================================================== */

/**
 * One segment of a segmented string.
 *
 * `isWordLike` is only meaningful for granularity == "word", and it is the one
 * field whose *derivation* differs between backends: Android reads
 * BreakIterator.getRuleStatus(), which is ICU's own answer, while Apple has no
 * equivalent and derives it structurally from which runs
 * -enumerateSubstringsInRange:options:NSStringEnumerationByWords yielded.
 * docs/intl-completeness-map.md records this as a divergence to measure, and
 * tests/differential/intl/ is where it gets measured.
 */
struct Segment {
  int32_t begin = 0;  ///< UTF-16 code unit offset, inclusive
  int32_t end = 0;    ///< exclusive
  bool isWordLike = false;
};

/* ==========================================================================
 * Locale information (Intl.Locale's info getters)
 * ========================================================================== */

/**
 * What Intl.Locale's getWeekInfo()/getTextInfo() need, per locale.
 *
 * ECMA-402 numbers weekdays 1 = Monday .. 7 = Sunday. Both platforms number
 * them 1 = Sunday .. 7 = Saturday, so the conversion is on the platform side of
 * the seam and this struct is already in spec units.
 */
struct WeekInfo {
  int32_t firstDay = 1;          ///< 1..7, Monday-based
  int32_t minimalDays = 1;       ///< 1..7
  std::vector<int32_t> weekend;  ///< Monday-based weekday numbers, ascending
};

/**
 * The platform's CLDR database, projected through what ECMA-402 needs.
 */
class Platform {
 public:
  virtual ~Platform() = default;

  /// For diagnostics and for tests to assert on. "stub", "apple", "android".
  virtual const char *name() = 0;

  /// Every locale the platform can format in, as BCP-47 tags.
  virtual std::vector<std::string> availableLocales() = 0;

  /// The user's locale. Never empty; falls back to "en-US".
  virtual std::string defaultLocale() = 0;

  /// The system timezone as an IANA id, e.g. "Europe/Berlin".
  virtual std::string defaultTimeZone() = 0;

  /**
   * CLDR likely-subtags, from the platform's own database.
   *
   * This method is why `likelySubtags` is not in the JavaScript layer. That
   * table is 181,013 bytes — 85% of what @formatjs/intl-getcanonicallocales
   * ships — and putting it in JS would blow the module's whole 60 KB budget on
   * its own. Apple exposes it as Locale.Language.maximalIdentifier /
   * minimalIdentifier, Android as android.icu.util.ULocale.addLikelySubtags /
   * minimizeSubtags.
   *
   * Empty means "no opinion"; the JS layer then uses the tag unchanged.
   */
  virtual std::string maximize(const std::string &tag) = 0;
  virtual std::string minimize(const std::string &tag) = 0;

  /**
   * Platform canonicalization of an already structurally-valid BCP-47 tag.
   *
   * Structural validation happens in JavaScript and is deliberately *not*
   * delegated: NSLocale and ULocale both accept malformed tags silently, and
   * ECMA-402 requires a RangeError. This method exists only for the legacy
   * mappings a platform may know about beyond the alias tables in js/intl.js.
   *
   * Empty means "no opinion".
   */
  virtual std::string canonicalize(const std::string &tag) = 0;

  /**
   * Validates and canonicalizes an IANA timezone id.
   *
   * ECMA-402 requires RangeError for an unknown zone and canonicalization of
   * case ("america/new_york" -> "America/New_York") and of link names. Only the
   * platform knows the zone list. Empty means unknown, which the JS layer turns
   * into RangeError.
   */
  virtual std::string normalizeTimeZone(const std::string &tz) = 0;

  /**
   * For Intl.supportedValuesOf. Called at most once per call, never on a
   * format path.
   *
   * ECMA-402 requires these lists to be exactly the set of values that
   * round-trip through the corresponding resolvedOptions(): anything a
   * formatter accepts must be listed, and anything listed must be accepted.
   * A backend must therefore *probe* rather than assert. Reporting a
   * hand-written set cost the Apple backend two test262 tests and put it below
   * the do-nothing default backend on the `Intl` area; both backends now
   * derive these by asking the platform.
   */
  virtual std::vector<std::string> timeZones() = 0;
  virtual std::vector<std::string> calendars() = 0;
  virtual std::vector<std::string> numberingSystems() = 0;

  /// Returns nullptr if no formatter could be created.
  virtual std::unique_ptr<DateTimeFormatter> openDateTimeFormat(
      const DateTimeOptions &options) = 0;

  /* ---- stage two: the rest of ECMA-402 ------------------------------------
   *
   * Every one of these returns nullptr / the empty answer when the platform
   * cannot serve it. That is a *degradation*, never a throw: the JavaScript
   * layer decides what a missing backend means for each service, and for most
   * of them the answer is a documented deviation rather than an exception.
   */

  virtual std::unique_ptr<NumberFormatter> openNumberFormat(
      const NumberOptions &options) = 0;

  virtual std::unique_ptr<Collator> openCollator(
      const CollatorOptions &options) = 0;

  virtual std::unique_ptr<RelativeTimeFormatter> openRelativeTimeFormat(
      const RelativeTimeOptions &options) = 0;

  virtual std::unique_ptr<ListFormatter> openListFormat(
      const ListFormatOptions &options) = 0;

  /**
   * Intl.DisplayNames.
   *
   * One call rather than a handle, because a DisplayNames instance holds no
   * platform state worth keeping: on Apple it is
   * -[NSLocale localizedStringForLanguageCode:] and friends, which take the
   * locale each time anyway.
   *
   * `type` is the ECMA-402 type ("language", "region", "script", "currency",
   * "calendar", "dateTimeField"), `style` is "long"|"short"|"narrow", and
   * `code` is the already-validated code. Empty return means "the platform has
   * no name for this", which the JS layer turns into the requested `fallback`.
   */
  virtual std::string displayName(
      const std::string &locale, const std::string &type,
      const std::string &style, const std::string &code) = 0;

  /**
   * Intl.Segmenter.
   *
   * The whole string crosses once and the whole segmentation comes back, which
   * is the right granularity for a JNI boundary: the alternative is one
   * crossing per segment, and a sentence-segmented paragraph is hundreds.
   *
   * An empty result for a non-empty string means the platform declined; the JS
   * layer then falls back to one segment covering the string.
   */
  virtual std::vector<Segment> segment(
      const std::string &locale, const std::string &granularity,
      const std::u16string &text) = 0;

  /**
   * String.prototype.toLocaleUpperCase / toLocaleLowerCase.
   *
   * Locale-sensitive case mapping is data (Turkish dotted/dotless i, Lithuanian
   * accents, Greek final sigma) and both platforms have it:
   * -[NSString uppercaseStringWithLocale:] and
   * android.icu.lang.UCharacter.toUpperCase(ULocale, String).
   */
  virtual std::u16string caseMap(
      const std::string &locale, bool upper, const std::u16string &text) = 0;

  /* ---- enumerations, for Intl.supportedValuesOf and Intl.Locale ---------- */

  virtual std::vector<std::string> collations() = 0;
  virtual std::vector<std::string> currencies() = 0;

  /**
   * The ISO 4217 minor-unit count for `code`, or -1 when the platform has no
   * opinion (the JavaScript layer then uses 2).
   *
   * This is data — JPY is 0, KWD is 3 — and it is asked for rather than tabled
   * so that the module never carries a currency table. It is read once per
   * NumberFormat construction with style "currency", and never on a format
   * path.
   */
  virtual int32_t currencyDigits(const std::string &code) = 0;

  /// Per-locale answers behind Intl.Locale's getters. An empty vector means
  /// "no opinion" and the JS layer omits the property.
  virtual std::vector<std::string> localeCalendars(
      const std::string &locale) = 0;
  virtual std::vector<std::string> localeNumberingSystems(
      const std::string &locale) = 0;
  virtual std::vector<std::string> localeTimeZones(
      const std::string &locale) = 0;
  virtual std::vector<std::string> localeCollations(
      const std::string &locale) = 0;
  virtual std::string localeHourCycle(const std::string &locale) = 0;
  /// "ltr" | "rtl" | "" (unknown).
  virtual std::string localeTextDirection(const std::string &locale) = 0;
  /// Returns false when the platform has no week information for this locale.
  virtual bool localeWeekInfo(const std::string &locale, WeekInfo &out) = 0;
};

/**
 * A Platform with every stage-two method answering "cannot".
 *
 * Backends inherit this rather than Platform directly, so adding a service to
 * the seam does not break the build of a backend that has not implemented it
 * yet — and, more importantly, so that "not implemented" is spelled once,
 * consistently, instead of being copied into three files.
 *
 * The DateTimeFormat half is *not* defaulted: it is the stage-one contract and
 * every backend implements it.
 */
class PlatformDefaults : public Platform {
 public:
  std::unique_ptr<NumberFormatter> openNumberFormat(
      const NumberOptions &) override {
    return nullptr;
  }
  std::unique_ptr<Collator> openCollator(const CollatorOptions &) override {
    return nullptr;
  }
  std::unique_ptr<RelativeTimeFormatter> openRelativeTimeFormat(
      const RelativeTimeOptions &) override {
    return nullptr;
  }
  std::unique_ptr<ListFormatter> openListFormat(
      const ListFormatOptions &) override {
    return nullptr;
  }
  std::string displayName(
      const std::string &, const std::string &, const std::string &,
      const std::string &) override {
    return {};
  }
  std::vector<Segment> segment(
      const std::string &, const std::string &,
      const std::u16string &) override {
    return {};
  }
  std::u16string caseMap(
      const std::string &, bool, const std::u16string &text) override {
    return text;
  }
  std::vector<std::string> collations() override {
    return {};
  }
  std::vector<std::string> currencies() override {
    return {};
  }
  int32_t currencyDigits(const std::string &) override {
    return -1;
  }
  std::vector<std::string> localeCalendars(const std::string &) override {
    return {};
  }
  std::vector<std::string> localeNumberingSystems(
      const std::string &) override {
    return {};
  }
  std::vector<std::string> localeTimeZones(const std::string &) override {
    return {};
  }
  std::vector<std::string> localeCollations(const std::string &) override {
    return {};
  }
  std::string localeHourCycle(const std::string &) override {
    return {};
  }
  std::string localeTextDirection(const std::string &) override {
    return {};
  }
  bool localeWeekInfo(const std::string &, WeekInfo &) override {
    return false;
  }
};

/// Installed by the platform layer during startup, before any JavaScript runs.
void setPlatform(Platform *platform);

/**
 * The active platform. **Never null.**
 *
 * This differs from the modules/text-encoding template, where platform() may
 * return nullptr and every caller must check. Here a null would mean `Intl`
 * exists but every constructor throws, and that is a worse outcome than
 * root-locale output: `qjs-bench`, `tests/conformance`, the CLI, the opcode
 * profiler, the test262 runner and every benchmark run on a host with no
 * NSLocale and no android.icu, and all of them must still get an `Intl` whose
 * shapes are correct. So when no platform layer has been linked, this returns
 * the stub in IntlPlatform.cpp, which formats en-US in UTC and says so through
 * resolvedOptions.
 */
Platform *platform();

}  // namespace rnqjs::intl
