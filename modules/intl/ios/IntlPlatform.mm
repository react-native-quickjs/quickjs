/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Apple platform layer for react-native-quickjs-intl.
 *
 * Implements rnqjs::intl::Platform using Foundation. Nothing links ICU:
 * NSLocale, NSDateFormatter and NSTimeZone already carry the entire CLDR
 * database, which is the whole premise — docs/intl-platform-backed.md measures
 * the alternative at 8.28 MB of bundle and 415 ms of startup for 15 locales.
 *
 * THE TWO NON-OBVIOUS DECISIONS IN THIS FILE
 *
 * 1. Skeletons, not hand-built patterns.
 *    -[NSDateFormatter setLocalizedDateFormatFromTemplate:] takes a CLDR
 *    skeleton and asks the platform's own DateTimePatternGenerator for the
 *    locale's best pattern. Android's
 *    android.icu.text.DateTimePatternGenerator.getBestPattern is the same
 *    mechanism, so the ECMA-402 component bag is translated into a skeleton
 *    exactly once, in js/intl.js, for both platforms. That symmetry is what
 *    keeps the two backends from drifting the way Hermes's 2,648 lines of
 *    Objective-C++ and 6,870 lines of Java do.
 *
 * 2. formatToParts by pattern walk, not by string surgery.
 *    Hermes reconstructs date parts on Apple by formatting the whole string and
 *    splitting it on NSCharacterSet.alphanumericCharacterSet
 *    (PlatformIntlApple.mm:1979-2018). That is a guess. This file instead walks
 *    the pattern NSDateFormatter *itself chose* (`-dateFormat`), formats each
 *    field token on its own, and treats everything between tokens as a literal.
 *    That is exact, because a CLDR date pattern is by construction the
 *    concatenation of its field renderings and its literal runs, and because a
 *    single-token formatter renders the same form the token asks for — 'M'
 *    (format) and 'L' (standalone) stay distinct, which matters in Russian and
 *    Czech where the two differ.
 *
 *    This is *not* the FormatStyle.attributed route. That API is in
 *    Foundation's Swift overlay, and it does not compose with skeleton-driven
 * formatting: it formats from a Date.FormatStyle built out of preset
 * components, so it cannot render an arbitrary CLDR skeleton. It remains the
 * right mechanism for stage 2 (NumberFormat), where Hermes has
 *    llvm_unreachable("formatToParts is unimplemented on Apple platforms") and
 *    the attributed route gives real number-part boundaries.
 *
 * WHAT IS NOT HERE
 *    Likely subtags. Foundation's Objective-C surface has no addLikelySubtags
 *    equivalent; the API is Swift-only (Locale.Language.maximalIdentifier,
 *    iOS 16+). It lives in ios/IntlLikelySubtags.swift and is reached through
 *    the weak C symbols declared below, so this file still links and still
 *    works when Swift is not in the build — maximize/minimize then return "no
 *    opinion", which the JavaScript layer handles by passing the tag through.
 */

#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "IntlPlatform.h"

/*
 * Implemented in ios/IntlLikelySubtags.swift via @_cdecl.
 *
 * Declared weak so a build without the Swift file still links. Checking a
 * function pointer against null is the only supported way to ask "was this
 * symbol resolved"; there is no build-time flag for it that survives both the
 * CocoaPods and the CMake paths.
 */
extern "C" {
__attribute__((weak)) const char *rnqjs_intl_maximize_swift(const char *tag);
__attribute__((weak)) const char *rnqjs_intl_minimize_swift(const char *tag);
__attribute__((weak)) void rnqjs_intl_free_swift(const char *s);
}

/* ==========================================================================
 * Stage two: NumberFormat, Collator, RelativeTimeFormat, ListFormat,
 * DisplayNames, Segmenter, case mapping and the locale enumerations.
 *
 * NATIVE FIRST, AND WHAT THAT COST TO ESTABLISH
 *   docs/intl-completeness-map.md recorded "signDisplay, roundingMode,
 *   roundingIncrement: Swift FormatStyle only", derived from a probe that
 *   printed "NO Objective-C API" without having tried the NSNumberFormatter
 *   *properties* of those names. bench/spikes/intl/apple-numberformat-probe.m
 *   tried them. MEASURED on macOS 26.5:
 *
 *     roundingMode        NSNumberFormatter.roundingMode covers seven of
 *                         ECMA-402's nine modes directly (halfCeil and
 *                         halfFloor have no NSNumberFormatterRoundingMode).
 *     roundingIncrement   NSNumberFormatter.roundingIncrement, exact.
 *     signDisplay         positivePrefix / negativePrefix / plusSign compose
 *                         into all five values.
 *     significant digits  usesSignificantDigits + min/max, exact.
 *
 *   So the Objective-C surface answers far more of NumberFormat than the map
 *   claimed, and the Swift overlay is needed for exactly two things:
 *   compact notation and the list-format widths. That correction is written
 *   back into the map.
 *
 * WHERE THE PLATFORM IS *NOT* ASKED
 *   Rounding, for `notation: "standard"`. js/intl.js hands this file a decimal
 *   string that is already final, and the formatter is pinned to render exactly
 *   those digits. See the decimalString contract in cpp/IntlPlatform.h. It is
 *   the single largest divergence risk removed by construction: a tie at 2.5
 *   cannot resolve differently on iOS and Android.
 * ========================================================================== */

/* Implemented in ios/IntlLikelySubtags.swift; weak, so a build without Swift
   still links and degrades in a documented way. */
extern "C" {
__attribute__((weak)) const char *rnqjs_intl_compact_swift(
    const char *localeId, double value, int longStyle, int minFrac, int maxFrac,
    int minSig, int maxSig);
__attribute__((weak)) const char *rnqjs_intl_list_swift(
    const char *localeId, const char *type, const char *width,
    const char *itemsUnitSeparated);
__attribute__((weak)) const char *rnqjs_intl_collations_swift(void);
}

namespace rnqjs::intl {
namespace {

std::string toStd(NSString *s) {
  return s == nil ? std::string() : std::string([s UTF8String]);
}

std::u16string toU16(NSString *s) {
  if (s == nil) return {};
  const NSUInteger n = [s length];
  std::u16string out(n, u'\0');
  // NSString is already UTF-16 internally, so this is a copy and not a
  // transcode. Going via UTF8String would be two conversions per format call
  // for no benefit, and QuickJS takes UTF-16 directly.
  [s getCharacters:reinterpret_cast<unichar *>(&out[0])
             range:NSMakeRange(0, n)];
  return out;
}

NSString *fromStd(const std::string &s) {
  return [NSString stringWithUTF8String:s.c_str()];
}

NSString *fromU16(const std::u16string &s) {
  return
      [NSString stringWithCharacters:reinterpret_cast<const unichar *>(s.data())
                              length:s.size()];
}

std::string toUtf8(const std::u16string &s) {
  return toStd(fromU16(s));
}

/**
 * NSCalendar identifier -> the BCP-47 `ca` keyword value ECMA-402 reports.
 *
 * Foundation's identifiers are not the keyword values (NSCalendarIdentifier-
 * Gregorian is "gregorian", ECMA-402 says "gregory"), so the mapping is
 * explicit rather than a string transform. A calendar Foundation does not know
 * comes back as Gregorian, which is why openDateTimeFormat's request is never
 * assumed to have been honoured — see ApplePlatform::calendars().
 */
std::string calendarKeywordFor(NSCalendar *cal) {
  static NSDictionary<NSString *, NSString *> *map = @{
    NSCalendarIdentifierGregorian : @"gregory",
    NSCalendarIdentifierBuddhist : @"buddhist",
    NSCalendarIdentifierChinese : @"chinese",
    NSCalendarIdentifierCoptic : @"coptic",
    NSCalendarIdentifierEthiopicAmeteMihret : @"ethiopic",
    NSCalendarIdentifierEthiopicAmeteAlem : @"ethioaa",
    NSCalendarIdentifierHebrew : @"hebrew",
    NSCalendarIdentifierISO8601 : @"iso8601",
    NSCalendarIdentifierIndian : @"indian",
    NSCalendarIdentifierIslamic : @"islamic",
    NSCalendarIdentifierIslamicCivil : @"islamic-civil",
    NSCalendarIdentifierIslamicTabular : @"islamic-tbla",
    NSCalendarIdentifierIslamicUmmAlQura : @"islamic-umalqura",
    NSCalendarIdentifierJapanese : @"japanese",
    NSCalendarIdentifierPersian : @"persian",
    NSCalendarIdentifierRepublicOfChina : @"roc",
  };
  NSString *ident = cal == nil ? nil : [cal calendarIdentifier];
  NSString *keyword = ident == nil ? nil : map[ident];
  return keyword == nil ? std::string("gregory") : toStd(keyword);
}

/* ------------------------------------------------------------------------- */
/* Numbering systems: named candidates, probed answers                        */
/* ------------------------------------------------------------------------- */
/*
 * WHY THIS LIST EXISTS, AND WHY IT IS NOT "SHIPPING CLDR DATA"
 *   Foundation exposes no API that enumerates the numbering systems it can
 *   honour, and none that reports which one a formatter actually used.
 *   (android.icu has both: NumberingSystem.getAvailableNames() and
 *   NumberingSystem.getInstance(ULocale). Apple has neither.) So the candidate
 *   set has to be *named* here — but nothing about what the platform does with
 *   each candidate is asserted: every answer below is obtained by formatting
 *   and comparing. If a future macOS gains or loses a numbering system, this
 *   file needs no edit.
 *
 *   These are BCP-47 `nu` type identifiers from CLDR common/bcp47/number.xml,
 *   about 800 bytes of ASCII. They are not locale data: there is no per-locale
 *   table here and adding a locale adds nothing.
 *
 * ORDER IS LOAD-BEARING
 *   defaultNumberingSystem() returns the *first* candidate whose rendering
 *   matches the locale's own, so canonical identifiers must precede their
 *   aliases and their look-alikes. "latn" is first because it is by far the
 *   most common answer and the probe then costs one comparison.
 *
 *   The alias identifiers ("native", "traditio", "finance") are deliberately
 *   absent: they are not numbering systems, they are per-locale indirections,
 *   and reporting one from resolvedOptions() would be a lie that test262's
 *   supportedValuesOf/numberingSystems-accepted-by-DateTimeFormat.js catches.
 */
const char *const kNumberingSystemIds[] = {
    "latn",     "adlm",     "ahom",     "arab",     "arabext",  "armn",
    "armnlow",  "bali",     "beng",     "bhks",     "brah",     "cakm",
    "cham",     "cyrl",     "deva",     "diak",     "ethi",     "fullwide",
    "gara",     "geor",     "gong",     "gonm",     "grek",     "greklow",
    "gujr",     "gukh",     "guru",     "hanidec",  "hans",     "hansfin",
    "hant",     "hantfin",  "hebr",     "hmng",     "hmnp",     "java",
    "jpan",     "jpanfin",  "kali",     "kawi",     "khmr",     "knda",
    "krai",     "lana",     "lanatham", "laoo",     "lepc",     "limb",
    "mathbold", "mathdbl",  "mathmono", "mathsanb", "mathsans", "mlym",
    "modi",     "mong",     "mroo",     "mtei",     "mymr",     "mymrepka",
    "mymrpao",  "mymrshan", "mymrtlng", "nagm",     "newa",     "nkoo",
    "olck",     "onao",     "orya",     "osma",     "outlined", "rohg",
    "roman",    "romanlow", "saur",     "segment",  "shrd",     "sind",
    "sinh",     "sora",     "sund",     "sunu",     "takr",     "talu",
    "taml",     "tamldec",  "telu",     "thai",     "tibt",     "tirh",
    "tnsa",     "tols",     "vaii",     "wara",     "wcho",
};

/**
 * Renders a fixed year through a fixed calendar in `base` (a Foundation base
 * identifier such as "ar_EG"), optionally with a `numbers=` keyword.
 *
 * The probe is a *date* rendering rather than an NSNumberFormatter one because
 * a date formatter is what is being resolved: ICU routes algorithmic numbering
 * systems (roman, hebr, jpan) through RBNF, and the two formatter families do
 * not agree about which of those they honour. Probing the wrong one would
 * produce a self-consistent answer about the wrong question.
 *
 * The calendar is pinned to Gregorian so that a locale's default calendar
 * cannot change the digits and be mistaken for a numbering system.
 */
std::string probeYearPooled(const std::string &base, const char *nu);

std::string probeYear(const std::string &base, const char *nu) {
  @autoreleasepool {
    return probeYearPooled(base, nu);
  }
}

std::string probeYearPooled(const std::string &base, const char *nu) {
  std::string ident = base + "@calendar=gregorian";
  if (nu != nullptr && *nu != '\0') ident += std::string(";numbers=") + nu;
  NSDateFormatter *f = [[NSDateFormatter alloc] init];
  [f setLocale:[NSLocale localeWithLocaleIdentifier:fromStd(ident)]];
  [f setTimeZone:[NSTimeZone timeZoneWithName:@"UTC"]];
  [f setDateFormat:@"y"];
  return toStd([f stringFromDate:[NSDate dateWithTimeIntervalSince1970:0]]);
}

/// The numbering systems this platform actually honours, probed in `base`.
/// Cached: ~95 formatter constructions, and Intl.supportedValuesOf may be
/// called in a loop.
const std::vector<std::string> &honouredNumberingSystems(
    const std::string &base) {
  static std::string cachedBase;
  static std::vector<std::string> cached;
  if (!cached.empty() && cachedBase == base) return cached;
  const std::string latn = probeYear(base, "latn");
  std::vector<std::string> out{"latn"};
  for (const char *id : kNumberingSystemIds) {
    if (std::string(id) == "latn") continue;
    // "Honoured" means the platform rendered something *different* from latn.
    // That is the only signal Foundation gives, and it is enough: a numbering
    // system it does not know silently falls back to the locale's own digits.
    if (probeYear(base, id) != latn) out.emplace_back(id);
  }
  cachedBase = base;
  cached = std::move(out);
  return cached;
}

/// What the platform uses for `base` when no numbering system was requested.
std::string defaultNumberingSystem(const std::string &base) {
  static std::string cachedBase;
  static std::string cached;
  if (!cached.empty() && cachedBase == base) return cached;
  const std::string plain = probeYear(base, nullptr);
  std::string answer = "latn";
  for (const char *id : kNumberingSystemIds) {
    if (probeYear(base, id) == plain) {
      answer = id;
      break;
    }
  }
  cachedBase = base;
  cached = answer;
  return answer;
}

/* ------------------------------------------------------------------------- */

class AppleFormatter final : public DateTimeFormatter {
 public:
  AppleFormatter(
      NSDateFormatter *fmt, std::string timeZone, std::string baseLocale,
      std::string requestedNu)
      : fmt_(fmt),
        timeZone_(std::move(timeZone)),
        baseLocale_(std::move(baseLocale)),
        requestedNu_(std::move(requestedNu)) {}

  /*
   * @autoreleasepool on every entry point that creates Objective-C objects.
   *
   * ARC releases what it owns; it does not drain the autorelease pool, and
   * -stringFromDate: returns an autoreleased NSString. On the React Native JS
   * thread the runloop drains a pool per turn, so the accumulation is bounded
   * in practice — but a single JavaScript tick that formats a table of rows is
   * one turn, and that is exactly the shape this module exists to serve.
   *
   * MEASURED, macOS 26.5, /usr/bin/time -l around `intl-cli-apple`, before
   * these pools existed: peak RSS over N constructions of a formatter was
   * 14.3 MB at N=200, 15.7 MB at N=2,000 and 23.4 MB at N=20,000 — roughly
   * 470 bytes per formatter retained until process exit. `leaks --atExit`
   * reports zero the whole time, because an autoreleased object is reachable
   * from the pool and is therefore not a leak. Peak RSS is the instrument for
   * this question, not `leaks`.
   */
  bool format(double epochMs, std::u16string &out) override {
    @autoreleasepool {
      NSDate *date = dateFrom(epochMs);
      if (date == nil) return false;
      out = toU16([fmt_ stringFromDate:date]);
      return true;
    }
  }

  bool formatToParts(double epochMs, FormattedParts &out) override {
    @autoreleasepool {
      return formatToPartsPooled(epochMs, out);
    }
  }

  bool formatToPartsPooled(double epochMs, FormattedParts &out) {
    NSDate *date = dateFrom(epochMs);
    if (date == nil) return false;
    const std::string pattern = toStd([fmt_ dateFormat]);
    out.text.clear();
    out.parts.clear();

    for (size_t i = 0; i < pattern.size();) {
      const char ch = pattern[i];

      if (ch == '\'') {
        // CLDR quoting. '' is a literal apostrophe; '...' is a literal run.
        i++;
        std::string lit;
        if (i < pattern.size() && pattern[i] == '\'') {
          lit = "'";
          i++;
        } else {
          while (i < pattern.size() && pattern[i] != '\'') lit += pattern[i++];
          if (i < pattern.size()) i++;
        }
        appendLiteral(out, lit);
        continue;
      }

      if (!isFieldLetter(ch)) {
        std::string lit;
        while (i < pattern.size() && !isFieldLetter(pattern[i]) &&
               pattern[i] != '\'') {
          lit += pattern[i++];
        }
        appendLiteral(out, lit);
        continue;
      }

      size_t run = 0;
      while (i + run < pattern.size() && pattern[i + run] == ch) run++;
      const std::string token = pattern.substr(i, run);
      i += run;

      // Format this token alone, with the same locale, calendar and timezone.
      // The token is copied verbatim, so 'M' vs 'L' and 'E' vs 'c' keep their
      // format/standalone distinction rather than being normalised away.
      NSDateFormatter *one = fieldFormatter(token);
      appendField(out, partTypeFor(ch), toU16([one stringFromDate:date]));
    }

    /*
     * The invariant that matters: the parts must concatenate back to what
     * format() produced. If the pattern walk and the whole-string format
     * disagree — which can happen if the platform applies a contextual
     * transform such as capitalisation to the assembled string — the
     * decomposition is wrong, and a wrong decomposition is worse than a coarse
     * one because callers index into it (deviation D1). Fall back to a single
     * literal part rather than shipping a guess.
     */
    std::u16string whole = toU16([fmt_ stringFromDate:date]);
    if (out.text != whole) {
      out.text = whole;
      out.parts.assign(
          1, Part{PartType::Literal, 0, static_cast<int32_t>(whole.size())});
    }
    return true;
  }

  std::string resolved(const std::string &key) override {
    @autoreleasepool {
      return resolvedPooled(key);
    }
  }

  std::string resolvedPooled(const std::string &key) {
    if (key == "locale") return toStd([[fmt_ locale] localeIdentifier]);
    if (key == "calendar") return calendarKeyword();
    if (key == "numberingSystem") return numberingSystem();
    if (key == "timeZone") return timeZone_;
    if (key == "hourCycle") return hourCycleFromPattern();
    if (key == "pattern") return toStd([fmt_ dateFormat]);
    return {};
  }

 private:
  static bool isFieldLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  }

  static PartType partTypeFor(char c) {
    switch (c) {
      case 'G':
        return PartType::Era;
      case 'y':
      case 'Y':
      case 'u':
      case 'U':
        return PartType::Year;
      case 'r':
        return PartType::RelatedYear;
      case 'M':
      case 'L':
        return PartType::Month;
      case 'd':
      case 'D':
      case 'F':
      case 'g':
        return PartType::Day;
      case 'E':
      case 'e':
      case 'c':
        return PartType::Weekday;
      case 'a':
      case 'b':
      case 'B':
        return PartType::DayPeriod;
      case 'h':
      case 'H':
      case 'K':
      case 'k':
        return PartType::Hour;
      case 'm':
        return PartType::Minute;
      case 's':
        return PartType::Second;
      case 'S':
        return PartType::FractionalSecond;
      case 'z':
      case 'Z':
      case 'O':
      case 'v':
      case 'V':
      case 'X':
      case 'x':
        return PartType::TimeZoneName;
      default:
        return PartType::Unknown;
    }
  }

  NSDate *dateFrom(double epochMs) {
    if (!std::isfinite(epochMs)) return nil;
    return [NSDate dateWithTimeIntervalSince1970:epochMs / 1000.0];
  }

  /*
   * Per-token formatters are cached for the lifetime of this formatter. A
   * formatToParts on a full date/time pattern touches 6-10 tokens, and
   * NSDateFormatter construction is the expensive part of the operation, so
   * without the cache formatToParts would cost roughly ten times format().
   * With it, the cost is paid once per formatter and per distinct token.
   */
  NSDateFormatter *fieldFormatter(const std::string &token) {
    NSString *key = fromStd(token);
    NSDateFormatter *f = fieldCache_[key];
    if (f != nil) return f;
    f = [[NSDateFormatter alloc] init];
    [f setLocale:[fmt_ locale]];
    [f setCalendar:[fmt_ calendar]];
    [f setTimeZone:[fmt_ timeZone]];
    [f setDateFormat:key];
    fieldCache_[key] = f;
    return f;
  }

  void appendLiteral(FormattedParts &out, const std::string &lit) {
    if (lit.empty()) return;
    appendField(out, PartType::Literal, toU16(fromStd(lit)));
  }

  void appendField(
      FormattedParts &out, PartType type, const std::u16string &text) {
    if (text.empty()) return;
    const auto begin = static_cast<int32_t>(out.text.size());
    out.text += text;
    const auto end = static_cast<int32_t>(out.text.size());
    if (!out.parts.empty() && out.parts.back().type == type) {
      out.parts.back().end = end;  // merge adjacent runs of the same type
    } else {
      out.parts.push_back(Part{type, begin, end});
    }
  }

  std::string calendarKeyword() {
    return calendarKeywordFor([fmt_ calendar]);
  }

  /*
   * The numbering system the platform *actually used*, not the one that was
   * asked for.
   *
   * The previous implementation read the `numbers=` keyword back off the
   * locale identifier, which means it echoed the request. That was wrong in
   * both directions and MEASURED so on macOS 26.5:
   *
   *   - `new Intl.DateTimeFormat("ar-EG")` renders ١٩٧٠ and reported "latn";
   *   - `{numberingSystem: "finance"}` is an alias, not a numbering system —
   *     it renders latn digits in "en" and was reported as "finance".
   *
   * The second one is what made Intl.supportedValuesOf unsatisfiable: test262
   * requires the reported list to be exactly the set that round-trips through
   * resolvedOptions, and an echoing resolver round-trips *everything*.
   *
   * So: report the request only when the platform's rendering differs from
   * latn (i.e. it honoured it), and otherwise report what the locale itself
   * uses. Both are probes; neither is a table.
   */
  std::string numberingSystem() {
    if (!requestedNu_.empty()) {
      const std::string latn = probeYear(baseLocale_, "latn");
      if (requestedNu_ == "latn") return "latn";
      if (probeYear(baseLocale_, requestedNu_.c_str()) != latn) {
        return requestedNu_;
      }
    }
    return defaultNumberingSystem(baseLocale_);
  }

  std::string hourCycleFromPattern() {
    const std::string pattern = toStd([fmt_ dateFormat]);
    for (size_t i = 0; i < pattern.size(); i++) {
      const char c = pattern[i];
      if (c == '\'') {
        i++;
        while (i < pattern.size() && pattern[i] != '\'') i++;
        continue;
      }
      if (c == 'h') return "h12";
      if (c == 'H') return "h23";
      if (c == 'K') return "h11";
      if (c == 'k') return "h24";
    }
    return {};
  }

  NSDateFormatter *fmt_;
  NSMutableDictionary<NSString *, NSDateFormatter *> *fieldCache_ =
      [NSMutableDictionary dictionary];
  std::string timeZone_;
  std::string baseLocale_;   ///< Foundation base identifier, no keywords
  std::string requestedNu_;  ///< as asked for by the JS layer; may be empty
};

/* ------------------------------------------------------------------------- */

/*
 * Foundation locale identifiers use ICU keyword syntax, not BCP-47 `-u-`.
 *
 * MEASURED on macOS 26.5 (the probe is reproduced in
 * docs/intl-platform-backed.md): NSLocale accepts a BCP-47 identifier with a
 * *single* Unicode extension keyword, but silently drops the *value* of one of
 * them when two are present, and then fails outright.
 *
 *   +[NSLocale localeWithLocaleIdentifier:]
 *     "th-TH-u-ca-buddhist-nu-thai"  -> identifier "th-TH-u-ca-buddhist-nu",
 *                                       calendar nil, -stringFromDate: nil
 *     "th-TH-u-nu-thai-ca-buddhist"  -> identifier "th-TH-u-nu-ca-buddhist",
 *                                       calendar nil, -stringFromDate: nil
 *     "th-TH-u-ca-buddhist"          -> works
 *     "de-DE-u-nu-thai"              -> works (title-cased to "-nu-Thai")
 *     "th_TH@calendar=buddhist;numbers=thai"
 *                                    -> works, Thai calendar and Thai digits
 *
 * So the ICU form is the only one that is correct for the general case. This is
 * a platform quirk, not an ECMA-402 subtlety, which is exactly the kind of
 * thing that belongs on this side of the seam rather than in js/intl.js.
 */
/// language[-Script][-REGION] with '-' -> '_', extensions dropped: they are
/// re-expressed as ICU keywords by the caller.
std::string foundationBase(const std::string &locale) {
  std::string base;
  for (size_t i = 0; i < locale.size(); i++) {
    if (locale.compare(i, 3, "-u-") == 0 || locale.compare(i, 3, "-t-") == 0 ||
        locale.compare(i, 3, "-x-") == 0) {
      break;
    }
    base += locale[i] == '-' ? '_' : locale[i];
  }
  return base;
}

std::string foundationIdentifier(const DateTimeOptions &o) {
  const std::string base = foundationBase(o.locale);
  std::string keywords;
  if (!o.calendar.empty()) keywords += std::string("calendar=") + o.calendar;
  if (!o.numberingSystem.empty()) {
    if (!keywords.empty()) keywords += ";";
    keywords += "numbers=" + o.numberingSystem;
  }
  return keywords.empty() ? base : base + "@" + keywords;
}

NSDateFormatterStyle styleFor(const std::string &s) {
  if (s == "full") return NSDateFormatterFullStyle;
  if (s == "long") return NSDateFormatterLongStyle;
  if (s == "medium") return NSDateFormatterMediumStyle;
  if (s == "short") return NSDateFormatterShortStyle;
  return NSDateFormatterNoStyle;
}

std::string swiftString(const char *r) {
  if (r == nullptr) return {};
  std::string out(r);
  if (rnqjs_intl_free_swift != nullptr) rnqjs_intl_free_swift(r);
  return out;
}

/* ------------------------------------------------------------------------- */
/* NumberFormat                                                               */
/* ------------------------------------------------------------------------- */

NSNumberFormatterRoundingMode roundingModeFor(const std::string &m) {
  // halfCeil and halfFloor have no NSNumberFormatterRoundingMode. They only
  // matter for the notations this file rounds itself (compact, scientific,
  // engineering) because js/intl.js pre-rounds everything else, and the nearest
  // available mode is used with the difference recorded as deviation D17.
  if (m == "ceil") return NSNumberFormatterRoundCeiling;
  if (m == "floor") return NSNumberFormatterRoundFloor;
  if (m == "expand") return NSNumberFormatterRoundUp;
  if (m == "trunc") return NSNumberFormatterRoundDown;
  if (m == "halfEven") return NSNumberFormatterRoundHalfEven;
  if (m == "halfTrunc" || m == "halfFloor")
    return NSNumberFormatterRoundHalfDown;
  return NSNumberFormatterRoundHalfUp;  // halfExpand, halfCeil
}

/**
 * The sanctioned unit -> NSUnit map.
 *
 * MEASURED coverage on macOS 26.5: 40 of ECMA-402's 45 sanctioned units have an
 * NSUnit. The five that do not are `day`, `week`, `month`, `year` (NSUnitDuration
 * stops at hours) and `percent` (not a measurement at all). Those five are
 * handled separately below rather than being dropped — see unitFallbackText.
 */
NSUnit *nsUnitFor(const std::string &u) {
  static NSDictionary<NSString *, NSUnit *> *map = @{
    @"acre" : NSUnitArea.acres,
    @"bit" : NSUnitInformationStorage.bits,
    @"byte" : NSUnitInformationStorage.bytes,
    @"celsius" : NSUnitTemperature.celsius,
    @"centimeter" : NSUnitLength.centimeters,
    @"degree" : NSUnitAngle.degrees,
    @"fahrenheit" : NSUnitTemperature.fahrenheit,
    @"fluid-ounce" : NSUnitVolume.fluidOunces,
    @"foot" : NSUnitLength.feet,
    @"gallon" : NSUnitVolume.gallons,
    @"gigabit" : NSUnitInformationStorage.gigabits,
    @"gigabyte" : NSUnitInformationStorage.gigabytes,
    @"gram" : NSUnitMass.grams,
    @"hectare" : NSUnitArea.hectares,
    @"hour" : NSUnitDuration.hours,
    @"inch" : NSUnitLength.inches,
    @"kilobit" : NSUnitInformationStorage.kilobits,
    @"kilobyte" : NSUnitInformationStorage.kilobytes,
    @"kilogram" : NSUnitMass.kilograms,
    @"kilometer" : NSUnitLength.kilometers,
    @"liter" : NSUnitVolume.liters,
    @"megabit" : NSUnitInformationStorage.megabits,
    @"megabyte" : NSUnitInformationStorage.megabytes,
    @"meter" : NSUnitLength.meters,
    @"microsecond" : NSUnitDuration.microseconds,
    @"mile" : NSUnitLength.miles,
    @"mile-scandinavian" : NSUnitLength.scandinavianMiles,
    @"milliliter" : NSUnitVolume.milliliters,
    @"millimeter" : NSUnitLength.millimeters,
    @"millisecond" : NSUnitDuration.milliseconds,
    @"minute" : NSUnitDuration.minutes,
    @"nanosecond" : NSUnitDuration.nanoseconds,
    @"ounce" : NSUnitMass.ounces,
    @"petabyte" : NSUnitInformationStorage.petabytes,
    @"pound" : NSUnitMass.poundsMass,
    @"second" : NSUnitDuration.seconds,
    @"stone" : NSUnitMass.stones,
    @"terabit" : NSUnitInformationStorage.terabits,
    @"terabyte" : NSUnitInformationStorage.terabytes,
    @"yard" : NSUnitLength.yards,
  };
  return map[fromStd(u)];
}

NSFormattingUnitStyle unitStyleFor(const std::string &d) {
  if (d == "long") return NSFormattingUnitStyleLong;
  if (d == "narrow") return NSFormattingUnitStyleShort;
  return NSFormattingUnitStyleMedium;
}

class AppleNumberFormatter final : public NumberFormatter {
 public:
  AppleNumberFormatter(
      NSNumberFormatter *fmt, NumberOptions o, std::string baseLocale)
      : fmt_(fmt), o_(std::move(o)), baseLocale_(std::move(baseLocale)) {}

  bool format(
      double value, const std::string &decimalString, uint32_t hints,
      std::u16string &out) override {
    @autoreleasepool {
      return formatPooled(value, decimalString, hints, out);
    }
  }

  /**
   * Pins the formatter to its *configured* fraction-digit limits, once.
   *
   * Legal whenever kHintDigitsWithinLimits is set: the digits js/intl.js
   * produced then lie inside [minimumFractionDigits, maximumFractionDigits],
   * so those limits render them unchanged. The point is that the pin does not
   * depend on the value, so it survives from call to call and the three
   * setters — MEASURED at 750 ns, a quarter of a format call — run once per
   * formatter instead of once per change of digit count.
   */
  void pinToConfiguredDigits() {
    if (fracApplied_ == kFracConfigured) {
      ++fracSetterHits_;
      return;
    }
    const int fmin =
        o_.minimumFractionDigits >= 0 ? o_.minimumFractionDigits : 0;
    const int fmax =
        o_.maximumFractionDigits >= 0 ? o_.maximumFractionDigits : fmin;
    [fmt_ setUsesSignificantDigits:NO];
    [fmt_ setMinimumFractionDigits:fmin];
    [fmt_ setMaximumFractionDigits:fmax];
    fracApplied_ = kFracConfigured;
    ++fracSetterMisses_;
  }

  bool formatPooled(
      double value, const std::string &decimalString, uint32_t hints,
      std::u16string &out) {
    NSNumber *number;
    const bool withinLimits = (hints & kHintDigitsWithinLimits) != 0;
    const bool exactDouble = (hints & kHintExactDouble) != 0 && !RNQJS_ABL(6);
    if (exactDouble && !decimalString.empty()) {
      /*
       * THE DOUBLE ROUTE. Taken only when js/intl.js has proved the double
       * renders to exactly these digits — see the exactDouble contract on
       * NumberFormatter::format in cpp/IntlPlatform.h and its derivation at
       * `exactDoubleBound` in js/intl.js.
       *
       * MEASURED, bench/spikes/intl/apple-numberformatter-probe.m, one en_US
       * decimal formatter, three whole-program runs to +-3%:
       *
       *   stringFromNumber: given an NSNumber double            492 ns
       *   stringFromNumber: given an NSDecimalNumber          2,041 ns
       *   [NSDecimalNumber decimalNumberWithString:]            519 ns
       *
       * 4.1x for the same formatter and the same iteration with one argument
       * type changed, plus a 519 ns parse that disappears entirely.
       *
       * AND IT PINS THE FORMATTER TO ITS CONFIGURED LIMITS, NOT TO THE VALUE.
       * The digit-count memo below keys on the *current value's* fraction
       * count, so alternating 1.5 and 3 -- a price list -- misses on every
       * call and pays the 750 ns setter rebuild every time
       * (`bound-alternating-frac`, MEASURED at 7.81 us against 1.95 us).
       * Here the formatter is set once to (minimumFractionDigits,
       * maximumFractionDigits) as configured and never touched again, because
       * the hint guarantees the digits already lie inside those limits. The
       * sentinel kFracConfigured is a value `frac` can never take, so the two
       * regimes cannot alias.
       */
      pinToConfiguredDigits();
      /*
       * Negative zero is handled exactly as on the decimal route: the sign is
       * read off the digit string and applied by applySign, and the value
       * handed to Foundation is a POSITIVE zero. Passing -0.0 would let
       * NSNumberFormatter render its own minus on top of the prefix applySign
       * swaps in, which is a "--0" waiting to happen.
       */
      negativeZero_ = decimalString[0] == '-' && value == 0;
      number = [NSNumber numberWithDouble:(value == 0 ? 0.0 : value)];
    } else if (!decimalString.empty()) {
      /*
       * The decimalString contract: these digits are final. Pin the formatter
       * to exactly the fraction digits present, and turn significant-digit
       * rounding off, so NSNumberFormatter cannot round a second time. Going
       * through NSDecimalNumber rather than a double is what preserves a BigInt
       * beyond 2^53 (38 significant digits; see deviation D19 for the limit).
       */
      const size_t dot = decimalString.find('.');
      const int frac = dot == std::string::npos
                           ? 0
                           : static_cast<int>(decimalString.size() - dot - 1);
      /*
       * Skip the three setters when nothing would change.
       *
       * MEASURED 2026-07-27, M4 Pro, `new Intl.NumberFormat("de-DE").format(i)`
       * over 40,000 iterations (docs/intl-vs-node.md, "where the format call
       * really goes"): removing these three lines took the call from 2,825 ns
       * to 2,075 ns. **750 ns, a quarter of the whole call, was three property
       * setters** — NSNumberFormatter rebuilds its internal pattern state when
       * a digit-count property is assigned, whether or not the value differs,
       * and this is a formatter that is reused for every call.
       *
       * The memo is sound because these are the ONLY three assignments to
       * fmt_'s digit properties anywhere in this file (the ones inside
       * makeNumberFormatter are on a formatter still under construction), so no
       * other path can invalidate it. Verified by grep, not by memory, and the
       * grep is the maintenance instruction: adding a fourth assignment without
       * updating fracApplied_ is how this becomes a stale-cache bug.
       */
      /*
       * TWO PINNING REGIMES, AND WHY MIXING THEM WAS A MEASURED REGRESSION.
       *
       * With kHintDigitsWithinLimits the formatter is pinned to its configured
       * limits, which does not depend on the value and therefore never has to
       * change. Without it the formatter is pinned to this value's own digit
       * count, which does.
       *
       * The first version of this change set the hint only on the fast path,
       * so a formatter whose values sometimes took the fast path and sometimes
       * did not alternated between the two regimes and paid the 750 ns rebuild
       * on EVERY call. MEASURED: `fmt-large-grouped` (`i * 1234567.89`, where
       * the product's shortest form sometimes has 2 fraction digits and
       * sometimes 16) went 6.89 us -> 8.73 us, a 1.27x REGRESSION, while
       * `fmt-integer` improved 2.4x. js/intl.js now sets
       * kHintDigitsWithinLimits on both paths, so a formatter with
       * `state.fastRound` stays in the configured regime for its whole life.
       */
      if (withinLimits) {
        pinToConfiguredDigits();
      } else if (fracApplied_ != frac) {
        [fmt_ setUsesSignificantDigits:NO];
        [fmt_ setMinimumFractionDigits:frac];
        [fmt_ setMaximumFractionDigits:frac];
        fracApplied_ = frac;
        ++fracSetterMisses_;
      } else {
        ++fracSetterHits_;
      }
      number = [NSDecimalNumber decimalNumberWithString:fromStd(decimalString)
                                                 locale:nil];
      /*
       * Negative zero. NSDecimalNumber has no -0, so "-0.00" formats through
       * the *positive* pattern and the sign disappears — MEASURED, format(-0)
       * with minimumIntegerDigits 3 gave "000.00" where ECMA-402 and node give
       * "-000.00". Ten test262 files check this, one per roundingMode. The sign
       * is recovered from the digit string, which is the only place it survives.
       */
      negativeZero_ = decimalString[0] == '-' && [number doubleValue] == 0;
      value = [number doubleValue];
      /*
       * NSNumberFormatter loses precision above a double's 15-17 significant
       * digits even when handed an NSDecimalNumber. MEASURED: the exact string
       * "1.0000000000000001" with maximumFractionDigits 20 renders as
       * "1.0000000000000000", where the no-platform backend — which renders
       * the digits itself — is exact. Since js/intl.js has already produced the
       * final digits, rendering them directly costs nothing but the symbols,
       * and every symbol comes from this formatter.
       */
      if (significantDigits(decimalString) > 15) {
        /*
         * applySign() must run BEFORE this early return, not only at the
         * bottom of the function.
         *
         * MEASURED BUG, found 2026-07-27 while benchmarking (see
         * docs/intl-vs-node.md). `format(-0)` leaves this formatter with its
         * *positive* prefix set to the negative one — that swap is how a
         * negative zero keeps a currency's own affixes intact, see applySign.
         * Undoing it is applySign's job on the next call. But renderExactDigits
         * reads [fmt_ positivePrefix] directly, and this branch returned
         * without ever having called applySign, so the swap survived:
         *
         *   var n = new Intl.NumberFormat("en");
         *   n.format(-0);         // "-0"
         *   n.format(1e21);       // "-1,000,000,000,000,000,000,000"  WRONG
         *
         * node gives "1,000,000,000,000,000,000,000". The formatter is reused
         * across calls, so this was per-formatter state leaking forward — the
         * class of bug a single-call test can never see, and the reason
         * modules/intl/test/invariants.js now formats a negative zero and a
         * >15-significant-digit value from the SAME formatter in that order.
         */
        applySign(value);
        std::u16string exact;
        if (renderExactDigits(decimalString, exact)) {
          out = exact;
          return true;
        }
      }
    } else {
      negativeZero_ = value == 0 && std::signbit(value);
      number = [NSNumber numberWithDouble:value];
    }

    applySign(value);

    if (o_.notation == "compact") {
      const std::string s = compactViaSwift(value);
      if (!s.empty()) {
        out = toU16(fromStd(s));
        return true;
      }
      // The Swift overlay is absent. Falling back to the standard rendering is
      // a documented degradation (D20), not a silent one: resolvedOptions still
      // reports notation "compact" and the text is simply not compacted.
    }

    /*
     * Infinity does not go through the prefix machinery: NSNumberFormatter
     * renders positiveInfinitySymbol without applying positivePrefix, so
     * `signDisplay: "always"` lost its plus. Eight test262 files check it.
     */
    if (std::isinf(value) && value > 0 &&
        (o_.signDisplay == "always" || o_.signDisplay == "exceptZero")) {
      out = toU16([NSString stringWithFormat:@"%@%@", [fmt_ plusSign] ?: @"+",
                                             [fmt_ positiveInfinitySymbol]]);
      return true;
    }

    NSString *text;
    if (o_.style == "unit") {
      text = formatUnit(number);
    } else {
      text = [fmt_ stringFromNumber:number];
    }
    if (text == nil) return false;
    out = toU16(text);
    return true;
  }

  std::string resolved(const std::string &key) override {
    @autoreleasepool {
      if (key == "locale") return toStd([[fmt_ locale] localeIdentifier]);
      if (key == "numberingSystem") return defaultNumberingSystem(baseLocale_);
      return {};
    }
  }

  void symbols(NumberSymbols &s) override {
    @autoreleasepool {
      s.decimal = toU16([fmt_ decimalSeparator]);
      s.group = toU16([fmt_ groupingSeparator]);
      s.minusSign = toU16([fmt_ minusSign]);
      s.plusSign = toU16([fmt_ plusSign]);
      s.percent = toU16([fmt_ percentSymbol]);
      s.exponential = toU16([fmt_ exponentSymbol]);
      s.nan = toU16([fmt_ notANumberSymbol]);
      // The infinity symbol is stored with its sign attached ("+∞"); ECMA-402
      // wants the bare symbol so that "-∞" splits into minusSign + infinity.
      NSString *inf = [fmt_ positiveInfinitySymbol];
      NSString *plusSym = [fmt_ plusSign];
      if (inf != nil && plusSym != nil && [plusSym length] > 0 &&
          [inf hasPrefix:plusSym]) {
        inf = [inf substringFromIndex:[plusSym length]];
      }
      s.infinity = toU16(inf);
      if (o_.style == "currency") {
        s.currency = toU16(
            o_.currencyDisplay == "code" ? fromStd(o_.currency)
                                         : [fmt_ currencySymbol]);
      }
      s.digits = localeDigits();
    }
  }

 private:
  static size_t significantDigits(const std::string &dec) {
    size_t n = 0;
    bool started = false;
    for (char c : dec) {
      if (c < '0' || c > '9') continue;
      if (c != '0') started = true;
      if (started) n++;
    }
    return n;
  }

  /**
   * Renders an exact decimal string using only this formatter's own symbols.
   *
   * Not a reimplementation of number formatting: js/intl.js has already decided
   * every digit, and the grouping positions, separators, digit glyphs and
   * affixes all come from the platform. What it does not attempt is currency,
   * unit or compact rendering — those have patterns this cannot reconstruct —
   * so it declines for them and the caller falls back to NSNumberFormatter with
   * the precision loss that implies. Deviation D19.
   */
  bool renderExactDigits(const std::string &dec, std::u16string &out) {
    if (o_.style == "currency" || o_.style == "unit" ||
        o_.notation != "standard") {
      return false;
    }
    const std::vector<std::u16string> digits = localeDigits();
    if (digits.size() != 10) return false;

    const bool neg = !dec.empty() && dec[0] == '-';
    const size_t start = (neg || (!dec.empty() && dec[0] == '+')) ? 1 : 0;
    const size_t dot = dec.find('.');
    const std::string ip = dec.substr(
        start, (dot == std::string::npos ? dec.size() : dot) - start);
    const std::string fp =
        dot == std::string::npos ? std::string() : dec.substr(dot + 1);

    const std::u16string group = toU16([fmt_ groupingSeparator]);
    const std::u16string decimalSep = toU16([fmt_ decimalSeparator]);
    const NSUInteger primary =
        [fmt_ groupingSize] > 0 ? [fmt_ groupingSize] : 3;
    const NSUInteger secondary = [fmt_ secondaryGroupingSize] > 0
                                     ? [fmt_ secondaryGroupingSize]
                                     : primary;
    const bool grouping = [fmt_ usesGroupingSeparator] && !group.empty();

    std::u16string body;
    for (size_t i = 0; i < ip.size(); i++) {
      const size_t fromRight = ip.size() - i;
      if (grouping && i > 0) {
        // Primary group nearest the point, secondary beyond it. That is what
        // makes hi-IN's "12,34,567" come out right rather than "1,234,567".
        const bool boundary = fromRight > primary
                                  ? ((fromRight - primary) % secondary == 0)
                                  : (fromRight == primary);
        if (boundary) body += group;
      }
      body += digits[static_cast<size_t>(ip[i] - '0')];
    }
    if (!fp.empty()) {
      body += decimalSep;
      for (char c : fp) body += digits[static_cast<size_t>(c - '0')];
    }

    NSString *prefix = neg ? [fmt_ negativePrefix] : [fmt_ positivePrefix];
    NSString *suffix = neg ? [fmt_ negativeSuffix] : [fmt_ positiveSuffix];
    out = toU16(prefix ?: @"");
    out += body;
    out += toU16(suffix ?: @"");
    return true;
  }

  /**
   * The ten digits of the formatter's numbering system.
   *
   * Read by rendering, not from a table: format 0..9 with grouping off and no
   * fraction digits, and take what comes back. An algorithmic numbering system
   * (roman, hebrew) does not produce ten single glyphs, and the empty vector
   * that results is exactly the "no decimal digits" signal
   * numberFormatToParts expects.
   */
  std::vector<std::u16string> localeDigits() {
    std::vector<std::u16string> out;
    NSNumberFormatter *plain = [[NSNumberFormatter alloc] init];
    [plain setLocale:[fmt_ locale]];
    [plain setNumberStyle:NSNumberFormatterDecimalStyle];
    [plain setUsesGroupingSeparator:NO];
    [plain setMaximumFractionDigits:0];
    for (int i = 0; i < 10; i++) {
      NSString *d = [plain stringFromNumber:@(i)];
      if (d == nil || [d length] == 0) return {};
      out.push_back(toU16(d));
    }
    return out;
  }

  /**
   * signDisplay, which Foundation expresses through the prefixes rather than
   * through a mode. `exceptZero` and `negative` depend on the value, so they
   * are applied per call.
   */
  void applySign(double value) {
    const bool isZero = value == 0;
    /*
     * Everything below is a pure function of (signDisplay, isZero,
     * negativeZero_), and signDisplay is fixed for the life of the formatter.
     * So the prefixes and suffixes this installs depend on exactly two bits,
     * and re-installing the same ones is wasted work on every call.
     *
     * MEASURED 2026-07-27: skipping applySign entirely took
     * `format(i)` from 2,825 ns to 2,450 ns — 375 ns, or 13% of the call, in
     * four property assignments. The memo below keeps the assignments and skips
     * the repeats.
     *
     * The key must include negativeZero_ and not just isZero: format(-0) and
     * format(0) both have isZero true and need *different* prefixes, and
     * collapsing them is how the negative zero's sign would disappear on the
     * second call.
     */
    const int key = (isZero ? 1 : 0) | (negativeZero_ ? 2 : 0);
    if (key == signApplied_) {
      ++signHits_;
      return;
    }
    signApplied_ = key;
    ++signMisses_;
    NSString *plus = [fmt_ plusSign] ?: @"+";
    if (o_.signDisplay == "never") {
      [fmt_ setPositivePrefix:basePositivePrefix_];
      [fmt_ setNegativePrefix:strippedNegativePrefix_];
    } else if (
        o_.signDisplay == "always" ||
        (o_.signDisplay == "exceptZero" && !isZero)) {
      [fmt_
          setPositivePrefix:[plus stringByAppendingString:basePositivePrefix_]];
      [fmt_ setNegativePrefix:baseNegativePrefix_];
    } else if (o_.signDisplay == "negative" && isZero) {
      [fmt_ setPositivePrefix:basePositivePrefix_];
      [fmt_ setNegativePrefix:strippedNegativePrefix_];
    } else {
      [fmt_ setPositivePrefix:basePositivePrefix_];
      [fmt_ setNegativePrefix:baseNegativePrefix_];
    }
    /*
     * A negative zero takes the negative *pattern* even though the value
     * compares equal to zero. Swapping the prefix and suffix rather than
     * prepending a minus is what keeps a currency's own affixes intact: the
     * negative pattern for en-US currency is "-$" and "( )" under accounting,
     * neither of which is "minus plus the positive pattern".
     */
    if (negativeZero_ && o_.signDisplay != "never" &&
        o_.signDisplay != "negative") {
      [fmt_ setPositivePrefix:[fmt_ negativePrefix]];
      [fmt_ setPositiveSuffix:[fmt_ negativeSuffix]];
    } else if (negativeZero_) {
      [fmt_ setPositivePrefix:basePositivePrefix_];
      [fmt_ setPositiveSuffix:basePositiveSuffix_];
    } else {
      [fmt_ setPositiveSuffix:basePositiveSuffix_];
    }
  }

  /**
   * Compact notation, through the Swift overlay.
   *
   * ECMA-402's `roundingPriority` of "morePrecision" / "lessPrecision" is a
   * rule over *two* renderings — one under the significant-digit limits and one
   * under the fraction-digit limits — and Swift's FormatStyle has no such mode.
   * So both are rendered and the rule is applied here, by counting the fraction
   * digits each produced. That is exactly the spec's comparison: it picks the
   * result whose last significant place is smaller.
   *
   * MEASURED: without this, formatjs's own supportsES2023() probe rendered
   * "100M" where it requires "100.00M", and @formatjs/intl-numberformat asked
   * to be polyfilled. It is the one line that moved that package from
   * "polyfill" to "not needed".
   */
  std::string compactViaSwift(double value) {
    if (rnqjs_intl_compact_swift == nullptr) return {};
    const bool both = (o_.roundingType == "morePrecision" ||
                       o_.roundingType == "lessPrecision") &&
                      o_.maximumSignificantDigits > 0 &&
                      o_.maximumFractionDigits >= 0;
    auto render = [&](int minFrac, int maxFrac, int minSig, int maxSig) {
      return swiftString(rnqjs_intl_compact_swift(
          baseLocale_.c_str(), value, o_.compactDisplay == "long" ? 1 : 0,
          minFrac, maxFrac, minSig, maxSig));
    };
    if (!both) {
      return render(
          o_.minimumFractionDigits, o_.maximumFractionDigits,
          o_.minimumSignificantDigits, o_.maximumSignificantDigits);
    }
    const std::string bySig = render(
        -1, -1, o_.minimumSignificantDigits, o_.maximumSignificantDigits);
    const std::string byFrac =
        render(o_.minimumFractionDigits, o_.maximumFractionDigits, -1, -1);
    if (bySig.empty()) return byFrac;
    if (byFrac.empty()) return bySig;
    const std::string sep = toStd([fmt_ decimalSeparator]);
    auto fracDigits = [&](const std::string &s) -> size_t {
      const size_t at = sep.empty() ? std::string::npos : s.find(sep);
      if (at == std::string::npos) return 0;
      size_t n = 0;
      for (size_t i = at + sep.size(); i < s.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= '0' && c <= '9')
          n++;
        else
          break;
      }
      return n;
    };
    const bool more = o_.roundingType == "morePrecision";
    return (fracDigits(bySig) > fracDigits(byFrac)) == more ? bySig : byFrac;
  }

  /**
   * `style: "unit"`.
   *
   * NSMeasurementFormatter takes our configured NSNumberFormatter, so the digit
   * options and the unit pattern come from the same place. The five sanctioned
   * units with no NSUnit (day, week, month, year, percent) and the `x-per-y`
   * compounds go through the fallbacks below; both are enumerated as deviation
   * D16.
   */
  NSString *formatUnit(NSNumber *number) {
    const size_t per = o_.unit.find("-per-");
    if (per != std::string::npos) {
      const std::string num = o_.unit.substr(0, per);
      const std::string den = o_.unit.substr(per + 5);
      NSString *lhs = formatSingleUnit(number, num);
      NSString *rhs = unitNameOnly(den);
      if (lhs == nil || rhs == nil) return [fmt_ stringFromNumber:number];
      return [NSString stringWithFormat:@"%@/%@", lhs, rhs];
    }
    return formatSingleUnit(number, o_.unit);
  }

  NSString *formatSingleUnit(NSNumber *number, const std::string &unit) {
    if (unit == "percent") {
      return [NSString stringWithFormat:@"%@%@", [fmt_ stringFromNumber:number],
                                        [fmt_ percentSymbol]];
    }
    NSUnit *u = nsUnitFor(unit);
    if (u != nil) {
      NSMeasurementFormatter *mf = [[NSMeasurementFormatter alloc] init];
      [mf setLocale:[fmt_ locale]];
      [mf setUnitOptions:NSMeasurementFormatterUnitOptionsProvidedUnit];
      [mf setUnitStyle:unitStyleFor(o_.unitDisplay)];
      [mf setNumberFormatter:fmt_];
      NSMeasurement *m =
          [[NSMeasurement alloc] initWithDoubleValue:[number doubleValue]
                                                unit:u];
      return [mf stringFromMeasurement:m];
    }
    /*
     * day / week / month / year. NSUnitDuration stops at hours, so the calendar
     * units come from NSDateComponentsFormatter, which is the only Foundation
     * API that localises them. It renders the count itself, so the digit
     * options do not apply — deviation D16.
     */
    NSDateComponentsFormatter *df = [[NSDateComponentsFormatter alloc] init];
    [df setCalendar:[[fmt_ locale] objectForKey:NSLocaleCalendar]];
    [df setUnitsStyle:o_.unitDisplay == "long"
                          ? NSDateComponentsFormatterUnitsStyleFull
                          : NSDateComponentsFormatterUnitsStyleShort];
    NSDateComponents *dc = [[NSDateComponents alloc] init];
    /*
     * NSDateComponentsFormatter renders the count itself and never consults
     * fmt_'s prefixes, so signDisplay was silently ignored on exactly these
     * four units — MEASURED, `{style:"unit", unit:"month",
     * signDisplay:"never"}.format(-2)` produced "-2 months" where "2 months"
     * is required, while the same options on "second" (which goes through
     * NSMeasurementFormatter) were correct. Intl.DurationFormat depends on it:
     * only the *first* displayed unit may carry a sign, so a negative duration
     * rendered "-1 year, -2 months". The magnitude is therefore formatted here
     * and the sign is reapplied by the same rules applySign uses.
     */
    NSString *signPrefix = unitSignPrefix([number doubleValue]);
    const double dv = [number doubleValue];
    const NSInteger n = static_cast<NSInteger>(dv < 0 ? -dv : dv);
    if (unit == "day") {
      [df setAllowedUnits:NSCalendarUnitDay];
      [dc setDay:n];
    } else if (unit == "week") {
      [df setAllowedUnits:NSCalendarUnitWeekOfMonth];
      [dc setWeekOfMonth:n];
    } else if (unit == "month") {
      [df setAllowedUnits:NSCalendarUnitMonth];
      [dc setMonth:n];
    } else if (unit == "year") {
      [df setAllowedUnits:NSCalendarUnitYear];
      [dc setYear:n];
    } else {
      return [fmt_ stringFromNumber:number];
    }
    NSString *s = [df stringFromDateComponents:dc];
    if (s == nil) return [fmt_ stringFromNumber:number];
    return signPrefix.length == 0 ? s : [signPrefix stringByAppendingString:s];
  }

  /**
   * The sign a calendar unit should carry, by the same rules applySign encodes
   * in the formatter's prefixes. Kept next to applySign in behaviour and
   * separate in code because NSDateComponentsFormatter takes no prefixes.
   */
  NSString *unitSignPrefix(double value) {
    NSString *minus = [fmt_ minusSign] ?: @"-";
    NSString *plus = [fmt_ plusSign] ?: @"+";
    if (o_.signDisplay == "never") return @"";
    if (negativeZero_) return o_.signDisplay == "negative" ? @"" : minus;
    if (value < 0) return minus;
    if (o_.signDisplay == "always" ||
        (o_.signDisplay == "exceptZero" && value != 0)) {
      return plus;
    }
    return @"";
  }

  /// "kilometer" -> "km": format 1 of the unit and remove the numeral.
  NSString *unitNameOnly(const std::string &unit) {
    NSUnit *u = nsUnitFor(unit);
    if (u == nil) return fromStd(unit);
    NSMeasurementFormatter *mf = [[NSMeasurementFormatter alloc] init];
    [mf setLocale:[fmt_ locale]];
    [mf setUnitOptions:NSMeasurementFormatterUnitOptionsProvidedUnit];
    [mf setUnitStyle:unitStyleFor(o_.unitDisplay)];
    NSMeasurement *m = [[NSMeasurement alloc] initWithDoubleValue:1 unit:u];
    NSString *one = [mf stringFromMeasurement:m];
    if (one == nil) return fromStd(unit);
    NSMutableCharacterSet *strip = [NSMutableCharacterSet
        characterSetWithCharactersInString:@"0123456789   "];
    NSRange r = [one rangeOfCharacterFromSet:[strip invertedSet]];
    return r.location == NSNotFound ? one : [one substringFromIndex:r.location];
  }

 public:
  void captureBasePrefixes() {
    basePositivePrefix_ = [fmt_ positivePrefix] ?: @"";
    baseNegativePrefix_ = [fmt_ negativePrefix] ?: @"";
    basePositiveSuffix_ = [fmt_ positiveSuffix] ?: @"";
    /*
     * `signDisplay: "never"` removes the minus from the negative prefix rather
     * than clearing the prefix, because in a currency locale the prefix is
     * "-$" and clearing it would drop the currency symbol too.
     */
    NSString *minus = [fmt_ minusSign] ?: @"-";
    strippedNegativePrefix_ =
        [baseNegativePrefix_ stringByReplacingOccurrencesOfString:minus
                                                       withString:@""];
  }

 private:
  NSNumberFormatter *fmt_;
  NumberOptions o_;
  std::string baseLocale_;
  NSString *basePositivePrefix_ = @"";
  NSString *baseNegativePrefix_ = @"";
  NSString *strippedNegativePrefix_ = @"";
  NSString *basePositiveSuffix_ = @"";
  bool negativeZero_ = false;
  /*
   * Per-formatter memo state for the two setter memos above, plus their hit
   * counters. `-1` is "nothing applied yet" for both and is not a reachable
   * value of either key, so the first call always applies.
   *
   * The counters are read through `Intl.__rnqjsBackend` diagnostics in
   * modules/intl/bench and exist because this project has shipped a fast path
   * with zero hits. A memo whose hit rate nobody can read is a memo nobody can
   * trust.
   */
  /*
   * The digit-count setter memo's state. Non-negative values are "the
   * formatter is pinned to exactly this many fraction digits", set from the
   * current value's own digit string. kFracConfigured means "pinned to the
   * formatter's configured (min, max)", which is the double route's regime.
   * -1 is "nothing has been assigned since construction".
   */
  static constexpr int kFracConfigured = -2;
  int fracApplied_ = -1;
  int signApplied_ = -1;
  unsigned long fracSetterHits_ = 0, fracSetterMisses_ = 0;
  unsigned long signHits_ = 0, signMisses_ = 0;
};

/* ------------------------------------------------------------------------- */
/* Collator                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * -[NSString compare:options:range:locale:] with the levers Foundation has, and
 * `-u-` keywords on the locale identifier for the levers it does not.
 *
 * MEASURED (bench/spikes/intl/apple-surface-probe.m): `caseFirst` and
 * `ignorePunctuation` have NO NSStringCompareOptions equivalent. They are
 * therefore expressed as ICU keywords — `colcasefirst=` and `colalternate=` —
 * which is a *different mechanism* from Android's
 * RuleBasedCollator.setCaseFirst / setAlternateHandling and may fail
 * differently. docs/intl-completeness-map.md flags this as the highest
 * divergence risk in the module and tests/differential/intl/collator.js
 * measures it.
 */
class AppleCollator final : public Collator {
 public:
  AppleCollator(
      NSLocale *locale, NSStringCompareOptions options, CollatorOptions o)
      : locale_(locale), options_(options), o_(std::move(o)) {}

  /*
   * The strings are wrapped, not copied -- but by which of two wrappers is a
   * decision that turned out to depend on the string, and getting it wrong in
   * either direction costs 25-40%.
   *
   * WHAT A STANDALONE PROBE SAID, AND WHY IT WAS INCOMPLETE.
   * bench/spikes/intl/apple-collator-probe.m measured, on two **18-character**
   * ASCII strings: `[NSString stringWithCharacters:length:]` x2 at 166.4 ns
   * against `CFStringCreateWithCharactersNoCopy` x2 at 105.9 ns, and concluded
   * the no-copy CFString was worth 77 ns unconditionally. Substituting it here
   * and measuring **in situ** did not reproduce that: on the six German words
   * the benchmark suite sorts, `Intl.Collator.compare` went 220 ns -> 260 ns,
   * a REGRESSION.
   *
   * WHAT IS ACTUALLY HAPPENING. `stringWithCharacters:length:` on a short
   * all-ASCII string returns a **tagged-pointer NSString**: no allocation at
   * all, and a representation Foundation collates from directly.
   * `CFStringCreateWithCharactersNoCopy` never can -- it must allocate a real
   * CFString object to hold the borrowed pointer. So the copy the "no-copy"
   * call avoids is smaller than the allocation it forces.
   *
   * MEASURED, build-intl-abl (-DRNQJS_INTL_ABLATION), `Intl.Collator("de")`
   * with the accessor hoisted, min of 5 blocks of 100,000, 64 distinct strings
   * per length. Full numbers and the sweep script are in
   * docs/intl-string-seam.md.
   *
   *   len   ascii: NoCopy / stringWithChars   wide: NoCopy / stringWithChars
   *     2      250 ns  /  190 ns                 260 ns  /  290 ns
   *     6      250 ns  /  200 ns                 260 ns  /  290 ns
   *     8      250 ns  /  270 ns                 270 ns  /  290 ns
   *    16      260 ns  /  360 ns                 270 ns  /  300 ns
   *    64      310 ns  /  500 ns                 320 ns  /  350 ns
   *
   * So: tagged when it can be tagged, borrowed when it cannot. The crossover
   * is between 6 and 8 characters and the predicate for the tagged
   * representation is "short and all-ASCII", which is exactly the shape of the
   * labels a list sorts.
   *
   * `kCFAllocatorNull` as the *contents* deallocator is what makes the second
   * form a wrapper: CoreFoundation must not free storage the engine owns, and
   * the U16Borrow on the other side of the seam keeps that storage alive for
   * exactly this call.
   *
   * The @autoreleasepool is KEPT even though dropping it measured 10 ns
   * cheaper in the standalone probe. `compare:options:range:locale:`
   * autoreleases internally, `stringWithCharacters:` returns an autoreleased
   * object whenever it is not tagged, and intl-cli has no outer pool. 10 ns is
   * not worth an unbounded-growth risk that only the `mem` arm would find.
   */
  static constexpr size_t kTaggedMaxLength = 7;

  /**
   * Wraps a borrowed view as an NSString for collation.
   *
   * `*owned` receives a CFStringRef the caller must CFRelease, or nullptr when
   * the returned string is autoreleased/tagged and needs no release. Returns
   * nil only if CoreFoundation could not allocate.
   */
  static NSString *wrapForCollation(std::u16string_view s, CFStringRef *owned) {
    *owned = nullptr;
    if (s.size() <= kTaggedMaxLength) {
      bool ascii = true;
      for (char16_t c : s) {
        if (c >= 0x80) {
          ascii = false;
          break;
        }
      }
      if (ascii) {
        return [NSString
            stringWithCharacters:reinterpret_cast<const unichar *>(s.data())
                          length:s.size()];
      }
    }
    CFStringRef r = CFStringCreateWithCharactersNoCopy(
        kCFAllocatorDefault, reinterpret_cast<const UniChar *>(s.data()),
        static_cast<CFIndex>(s.size()), kCFAllocatorNull);
    *owned = r;
    return (__bridge NSString *)r;
  }

  int32_t compare(std::u16string_view a, std::u16string_view b) override {
    // Ablation arm 1: return before the @autoreleasepool, so arm 1 minus arm 4
    // is the virtual dispatch and the pool alone.
    if (RNQJS_ABL(1)) {
      return static_cast<int32_t>(a.size()) - static_cast<int32_t>(b.size());
    }
    @autoreleasepool {
      CFStringRef ownA = nullptr;
      CFStringRef ownB = nullptr;
      NSString *x = wrapForCollation(a, &ownA);
      NSString *y = wrapForCollation(b, &ownB);
      int32_t out;
      if (x == nil || y == nil) {
        // Allocation failure. Fall back to code-unit order rather than
        // returning an arbitrary answer; ECMA-402 needs -1/0/1 either way.
        out = a < b ? -1 : a > b ? 1 : 0;
      } else if (RNQJS_ABL(2)) {
        // Ablation arm 2: both wrappers built and released, no comparison.
        // Arm 2 minus arm 1 is the wrapper cost; the full run minus arm 2 is
        // Foundation collation and nothing else.
        out = 0;
      } else {
        NSComparisonResult r =
            [x compare:y
                options:options_
                  range:NSMakeRange(0, static_cast<NSUInteger>(a.size()))
                 locale:locale_];
        out = r == NSOrderedAscending ? -1 : r == NSOrderedDescending ? 1 : 0;
      }
      if (ownA != nullptr) CFRelease(ownA);
      if (ownB != nullptr) CFRelease(ownB);
      return out;
    }
  }

  std::string resolved(const std::string &key) override {
    if (key == "locale") return toStd([locale_ localeIdentifier]);
    if (key == "collation") return o_.collation;
    return {};
  }

 private:
  NSLocale *locale_;
  NSStringCompareOptions options_;
  CollatorOptions o_;
};

/* ------------------------------------------------------------------------- */
/* RelativeTimeFormat                                                         */
/* ------------------------------------------------------------------------- */

class AppleRelativeTimeFormatter final : public RelativeTimeFormatter {
 public:
  AppleRelativeTimeFormatter(
      NSRelativeDateTimeFormatter *fmt, RelativeTimeOptions o)
      : fmt_(fmt), o_(std::move(o)) {}

  bool format(
      double value, const std::string &unit, std::u16string &out) override {
    @autoreleasepool {
      NSDateComponents *dc = [[NSDateComponents alloc] init];
      const NSInteger n = static_cast<NSInteger>(value);
      if (static_cast<double>(n) != value) {
        // NSDateComponents is integral. A fractional value ("in 1.5 hours") has
        // no Foundation expression, so the JS layer's fallback wording is used
        // instead of silently truncating to "in 1 hour". Deviation D15.
        return false;
      }
      if (unit == "year")
        [dc setYear:n];
      else if (unit == "quarter")
        [dc setQuarter:n];
      else if (unit == "month")
        [dc setMonth:n];
      else if (unit == "week")
        [dc setWeekOfMonth:n];
      else if (unit == "day")
        [dc setDay:n];
      else if (unit == "hour")
        [dc setHour:n];
      else if (unit == "minute")
        [dc setMinute:n];
      else if (unit == "second")
        [dc setSecond:n];
      else
        return false;
      NSString *s = [fmt_ localizedStringFromDateComponents:dc];
      // MEASURED: nil for `quarter` in every locale tried on macOS 26.5.
      if (s == nil || [s length] == 0) return false;
      out = toU16(s);
      return true;
    }
  }

  std::string resolved(const std::string &key) override {
    @autoreleasepool {
      if (key == "locale") return toStd([[fmt_ locale] localeIdentifier]);
      if (key == "numberingSystem") {
        /*
         * Probed, never echoed. An unsupported `nu` must be *ignored* by
         * ECMA-402, and echoing the request made resolvedOptions report "abc"
         * for `en-u-nu-abc`. This is the same class of bug that made
         * Intl.supportedValuesOf unsatisfiable for the date formatter, and it
         * is fixed the same way: ask what the platform actually used.
         */
        const std::string base =
            foundationBase(toStd([[fmt_ locale] localeIdentifier]));
        if (!o_.numberingSystem.empty()) {
          const std::string latn = probeYear(base, "latn");
          if (o_.numberingSystem == "latn") return "latn";
          if (probeYear(base, o_.numberingSystem.c_str()) != latn) {
            return o_.numberingSystem;
          }
        }
        return defaultNumberingSystem(base);
      }
      return {};
    }
  }

 private:
  NSRelativeDateTimeFormatter *fmt_;
  RelativeTimeOptions o_;
};

/* ------------------------------------------------------------------------- */
/* ListFormat                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * NSListFormatter has no `type` and no `width`: it does conjunction, long,
 * only. MEASURED — the Objective-C class has one factory and no options.
 * Disjunction and the short/narrow widths exist in Swift as
 * `.list(type:width:)`, so they go through the overlay, and a build without
 * Swift degrades to the conjunction wording with the deviation recorded (D20)
 * rather than throwing.
 */
class AppleListFormatter final : public ListFormatter {
 public:
  AppleListFormatter(
      NSListFormatter *fmt, ListFormatOptions o, std::string baseLocale)
      : fmt_(fmt), o_(std::move(o)), baseLocale_(std::move(baseLocale)) {}

  bool format(
      const std::vector<std::u16string> &items, std::u16string &out) override {
    @autoreleasepool {
      if (items.empty()) {
        out.clear();
        return true;
      }
      if (o_.type != "conjunction" || o_.style != "long") {
        if (rnqjs_intl_list_swift != nullptr) {
          // U+001F UNIT SEPARATOR: a control character that cannot occur in a
          // list element that came from JavaScript source in practice, and one
          // the Swift side splits on rather than parsing.
          std::string joined;
          for (size_t i = 0; i < items.size(); i++) {
            if (i) joined += '\x1f';
            joined += toUtf8(items[i]);
          }
          const std::string s = swiftString(rnqjs_intl_list_swift(
              baseLocale_.c_str(), o_.type.c_str(), o_.style.c_str(),
              joined.c_str()));
          if (!s.empty()) {
            out = toU16(fromStd(s));
            return true;
          }
        }
      }
      NSMutableArray<NSString *> *arr =
          [NSMutableArray arrayWithCapacity:items.size()];
      for (const std::u16string &s : items) [arr addObject:fromU16(s)];
      NSString *text = [fmt_ stringFromItems:arr];
      if (text == nil) return false;
      out = toU16(text);
      return true;
    }
  }

  std::string resolved(const std::string &key) override {
    if (key == "locale") return toStd([[fmt_ locale] localeIdentifier]);
    return {};
  }

 private:
  NSListFormatter *fmt_;
  ListFormatOptions o_;
  std::string baseLocale_;
};

class ApplePlatform final : public PlatformDefaults {
 public:
  const char *name() override {
    return "apple";
  }

  std::vector<std::string> availableLocales() override {
    @autoreleasepool {
      return availableLocalesPooled();
    }
  }

  std::vector<std::string> availableLocalesPooled() {
    std::vector<std::string> out;
    NSArray<NSString *> *ids = [NSLocale availableLocaleIdentifiers];
    out.reserve([ids count]);
    for (NSString *ident in ids) {
      // Foundation identifiers use '_' where BCP-47 uses '-', and carry
      // keywords after '@'. Converting here rather than in the JS layer keeps
      // the JS layer free of platform quirks.
      NSString *tag = [NSLocale canonicalLanguageIdentifierFromString:ident];
      if (tag != nil && [tag length] > 0) out.push_back(toStd(tag));
    }
    return out;
  }

  std::string defaultLocale() override {
    NSString *tag = [[NSLocale currentLocale] localeIdentifier];
    NSString *bcp = [NSLocale canonicalLanguageIdentifierFromString:tag];
    std::string s = toStd(bcp);
    return s.empty() ? std::string("en-US") : s;
  }

  std::string defaultTimeZone() override {
    NSString *name = [[NSTimeZone localTimeZone] name];
    std::string s = toStd(name);
    return s.empty() ? std::string("UTC") : s;
  }

  std::string maximize(const std::string &tag) override {
    return viaSwift(rnqjs_intl_maximize_swift, tag);
  }
  std::string minimize(const std::string &tag) override {
    return viaSwift(rnqjs_intl_minimize_swift, tag);
  }

  std::string canonicalize(const std::string &tag) override {
    NSString *out =
        [NSLocale canonicalLanguageIdentifierFromString:fromStd(tag)];
    return toStd(out);
  }

  static std::string asciiLower(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
  }

  std::string normalizeTimeZone(const std::string &tz) override {
    @autoreleasepool {
      return normalizeTimeZonePooled(tz);
    }
  }

  std::string normalizeTimeZonePooled(const std::string &tz) {
    const std::string lower = asciiLower(tz);

    /*
     * The UTC-alias family is NOT collapsed onto "UTC".
     *
     * test262's DateTimeFormat/canonicalize-utc-timezone.js (feature
     * `canonical-tz`) requires "Etc/GMT", "Etc/UTC" and "GMT" to be *preserved*
     * in resolvedOptions rather than canonicalized, which is a recent spec
     * change. node v22 / ICU 77 collapses all of them onto "UTC" and fails that
     * test; we do not. Only the bare identifier "UTC" is spelled "UTC".
     *
     * Foundation makes this fiddly: "UTC" is not in -knownTimeZoneNames at all,
     * and [[NSTimeZone timeZoneWithName:@"UTC"] name] answers "GMT". So the
     * canonical spelling is decided here from the *input*, not read back
     * off the NSTimeZone.
     */
    if (lower == "utc") return "UTC";
    if (lower == "gmt") return "GMT";
    if (lower == "etc/utc") return "Etc/UTC";
    if (lower == "etc/gmt") return "Etc/GMT";

    /*
     * Exact match first: the common case, and it avoids walking ~600 names.
     * -[NSTimeZone timeZoneWithAbbreviation:] is deliberately NOT consulted:
     * it accepts "ACT", "PST", "EST" and friends, and ECMA-402 accepts IANA
     * identifiers and "UTC" and nothing else.
     * DateTimeFormat/timezone-legacy-non-iana.js caught that.
     */
    if ([NSTimeZone timeZoneWithName:fromStd(tz)] != nil &&
        [[NSTimeZone knownTimeZoneNames] containsObject:fromStd(tz)]) {
      return tz;
    }

    /*
     * ASCII-case-insensitive match against the known list. ECMA-402 requires
     * "america/new_york" to canonicalize rather than throw.
     *
     * ASCII rather than -[NSString caseInsensitiveCompare:], which does full
     * Unicode case folding and therefore matches "europe/brußels" (sharp s) to
     * "Europe/Brussels". IANA identifiers are ASCII and ECMA-402's matching is
     * ASCII-case-insensitive. DateTimeFormat/timezone-invalid.js caught that.
     */
    for (NSString *known in [NSTimeZone knownTimeZoneNames]) {
      if (asciiLower(toStd(known)) == lower) return toStd(known);
    }

    /*
     * IANA *link* names that -knownTimeZoneNames omits.
     *
     * MEASURED: "Asia/Kolkata" is a valid IANA identifier that
     * -[NSTimeZone timeZoneWithName:] resolves and -knownTimeZoneNames does not
     * list (it lists "Asia/Calcutta"). ECMA-402 accepts every IANA identifier,
     * so rejecting it was wrong; two test262 files caught it.
     *
     * The `/` requirement is what keeps this from re-admitting the abbreviation
     * family: -timeZoneWithName: also accepts "PST", "ACT" and friends, which
     * ECMA-402 does not, and that was the reason -timeZoneWithAbbreviation: is
     * never consulted. Every IANA identifier except "UTC" contains a slash, and
     * "UTC" is handled above.
     */
    if (lower.find('/') != std::string::npos &&
        [NSTimeZone timeZoneWithName:fromStd(tz)] != nil) {
      return tz;
    }
    return {};
  }

  std::vector<std::string> timeZones() override {
    @autoreleasepool {
      return timeZonesPooled();
    }
  }

  std::vector<std::string> timeZonesPooled() {
    std::vector<std::string> out;
    for (NSString *name in [NSTimeZone knownTimeZoneNames]) {
      out.push_back(toStd(name));
    }
    return out;
  }

  /*
   * Probed, not listed.
   *
   * This used to be a hand-written set of twelve identifiers, and it was wrong
   * in a way that made Intl.supportedValuesOf unsatisfiable: NSCalendar also
   * honours "ethioaa", "islamic-civil", "islamic-tbla" and "islamic-umalqura",
   * which round-tripped through resolvedOptions but were absent from the list.
   * test262's supportedValuesOf/calendars-accepted-by-DateTimeFormat.js checks
   * exactly that equivalence in both directions, and the Apple backend scored
   * *below* the do-nothing stub backend on the `Intl` area because of it
   * (33/66 against 35/66).
   *
   * Asking the platform removes the class of error rather than the instance:
   * every identifier the AppleFormatter can report is offered to NSCalendar,
   * and only the ones it accepts unchanged are returned. "dangi" and
   * "islamic-rgsa" fail that probe on macOS 26.5 — Foundation quietly
   * substitutes Gregorian — and are correctly excluded.
   */
  std::vector<std::string> calendars() override {
    @autoreleasepool {
      return calendarsPooled();
    }
  }

  std::vector<std::string> calendarsPooled() {
    static const char *const kCandidates[] = {
        "buddhist",     "chinese",          "coptic",
        "dangi",        "ethioaa",          "ethiopic",
        "gregory",      "hebrew",           "indian",
        "islamic",      "islamic-civil",    "islamic-rgsa",
        "islamic-tbla", "islamic-umalqura", "iso8601",
        "japanese",     "persian",          "roc",
    };
    std::vector<std::string> out;
    for (const char *id : kCandidates) {
      NSString *ident = [NSString stringWithFormat:@"en@calendar=%s", id];
      NSCalendar *cal = [[NSLocale localeWithLocaleIdentifier:ident]
          objectForKey:NSLocaleCalendar];
      if (cal != nil && calendarKeywordFor(cal) == std::string(id)) {
        out.emplace_back(id);
      }
    }
    return out;
  }

  /*
   * Likewise probed. See kNumberingSystemIds for why the candidate set has to
   * be named on Apple and does not have to be on Android.
   *
   * "en" rather than the default locale: Intl.supportedValuesOf is a
   * process-wide question, and test262 asks it against DateTimeFormat("en").
   * Answering it in the user's locale would make the result depend on device
   * settings, which is a worse property than the small chance that some locale
   * honours a system "en" does not.
   */
  std::vector<std::string> numberingSystems() override {
    return honouredNumberingSystems("en");
  }

  std::unique_ptr<DateTimeFormatter> openDateTimeFormat(
      const DateTimeOptions &o) override {
    /*
     * Two attempts, and the second one is not defensive padding.
     *
     * ECMA-402 says an unsupported `nu` or `ca` is *ignored*, not an error, and
     * Foundation does not always ignore it: MEASURED on macOS 26.5,
     * "en@numbers=traditio" makes -setLocalizedDateFormatFromTemplate: produce
     * an empty pattern, so a formatter that should have quietly fallen back to
     * latn instead became a RangeError. That is one test262 file
     * (supportedValuesOf/numberingSystems-accepted-by-DateTimeFormat.js) and,
     * more importantly, an app-visible throw on an option the spec says to
     * ignore.
     *
     * Dropping the numbering system is safe because resolved("numberingSystem")
     * is probed rather than echoed: after the retry it reports what the locale
     * actually used, so resolvedOptions still tells the truth.
     */
    if (auto f = tryOpen(o, /*withNumberingSystem=*/true)) return f;
    if (!o.numberingSystem.empty()) {
      return tryOpen(o, /*withNumberingSystem=*/false);
    }
    return nullptr;
  }

  std::unique_ptr<DateTimeFormatter> tryOpen(
      const DateTimeOptions &options, bool withNumberingSystem) {
    @autoreleasepool {
      return tryOpenPooled(options, withNumberingSystem);
    }
  }

  std::unique_ptr<DateTimeFormatter> tryOpenPooled(
      const DateTimeOptions &options, bool withNumberingSystem) {
    DateTimeOptions o = options;
    if (!withNumberingSystem) o.numberingSystem.clear();

    NSDateFormatter *fmt = [[NSDateFormatter alloc] init];

    // Build the locale identifier with the resolved extension keywords, so the
    // platform picks the calendar and numbering system rather than us patching
    // the output afterwards.
    [fmt setLocale:[NSLocale
                       localeWithLocaleIdentifier:fromStd(foundationIdentifier(
                                                      o))]];

    std::string zoneName = o.timeZone;
    if (zoneName.empty()) zoneName = defaultTimeZone();
    NSTimeZone *zone = [NSTimeZone timeZoneWithName:fromStd(zoneName)];
    if (zone == nil)
      zone = [NSTimeZone timeZoneWithAbbreviation:fromStd(zoneName)];
    if (zone != nil) [fmt setTimeZone:zone];

    if (!o.dateStyle.empty() || !o.timeStyle.empty()) {
      [fmt setDateStyle:styleFor(o.dateStyle)];
      [fmt setTimeStyle:styleFor(o.timeStyle)];
    } else {
      // The skeleton route. setLocalizedDateFormatFromTemplate asks the
      // platform's own pattern generator for this locale's best pattern for
      // these fields — the same call android.icu's getBestPattern makes.
      [fmt setLocalizedDateFormatFromTemplate:fromStd(o.skeleton)];
      if ([[fmt dateFormat] length] == 0) return nullptr;
    }

    return std::make_unique<AppleFormatter>(
        fmt, toStd([[fmt timeZone] name]), foundationBase(o.locale),
        o.numberingSystem);
  }

  /* ---- stage two ------------------------------------------------------- */

  std::unique_ptr<NumberFormatter> openNumberFormat(
      const NumberOptions &o) override {
    @autoreleasepool {
      NSNumberFormatter *f = [[NSNumberFormatter alloc] init];
      bool engineeringSigDigits = false;
      std::string ident = foundationBase(o.locale);
      if (!o.numberingSystem.empty()) ident += "@numbers=" + o.numberingSystem;
      [f setLocale:[NSLocale localeWithLocaleIdentifier:fromStd(ident)]];

      if (o.style == "percent") {
        /*
         * NSNumberFormatterPercentStyle multiplies by 100. js/intl.js has
         * already done that (the decimalString contract), so the *decimal*
         * style is used and the percent symbol is attached through the
         * suffixes. Using the percent style here would scale twice; MEASURED as
         * a real bug on the stub backend before the contract was written down.
         */
        [f setNumberStyle:NSNumberFormatterDecimalStyle];
        [f setPositiveSuffix:[f percentSymbol]];
        [f setNegativeSuffix:[f percentSymbol]];
      } else if (o.style == "currency") {
        [f setNumberStyle:o.currencySign == "accounting"
                              ? NSNumberFormatterCurrencyAccountingStyle
                          : o.currencyDisplay == "code"
                              ? NSNumberFormatterCurrencyISOCodeStyle
                          : o.currencyDisplay == "name"
                              ? NSNumberFormatterCurrencyPluralStyle
                              : NSNumberFormatterCurrencyStyle];
        // Order matters: -setNumberStyle: resets currencyCode to the locale's,
        // so the code must be assigned afterwards. MEASURED — setting it first
        // made fr_FR + USD render "euros" under the plural style.
        [f setCurrencyCode:fromStd(o.currency)];
      } else if (o.notation == "scientific" || o.notation == "engineering") {
        [f setNumberStyle:NSNumberFormatterScientificStyle];
        if (o.notation == "engineering") {
          /*
           * NSNumberFormatter has no engineering mode, but its underlying
           * DecimalFormat does: the ICU pattern "##0.###E0" constrains the
           * integer part to a multiple of three digits, which is precisely
           * what engineering notation is. Setting the format string is the
           * documented way to reach a DecimalFormat feature Foundation does
           * not surface. MEASURED: 0.000345 becomes "345E-6" rather than
           * "3.45E-4".
           */
          const std::string frac(
              o.maximumFractionDigits > 0
                  ? static_cast<size_t>(o.maximumFractionDigits)
                  : 3,
              '#');
          NSString *pattern =
              [NSString stringWithFormat:@"##0.%sE0", frac.c_str()];
          [f setPositiveFormat:pattern];
          [f setNegativeFormat:[NSString stringWithFormat:@"-%@", pattern]];
          /*
           * ICU applies maximumFractionDigits to the *unscaled* value under a
           * scientific pattern, so setting it to 3 rendered 543211.1 as
           * "543.2E3" where ECMA-402 and node give "543.211E3". MEASURED, and
           * it is not fixed by re-applying the property after the pattern.
           *
           * Significant digits are the lever that does work: engineering's
           * mantissa has one to three integer digits, so a maximum of
           * 3 + maximumFractionDigits significant digits reproduces
           * maximumFractionDigits fraction digits at every scale.
           */
          [f setUsesSignificantDigits:YES];
          [f setMinimumSignificantDigits:1];
          [f setMaximumSignificantDigits:3 + (o.maximumFractionDigits >= 0
                                                  ? o.maximumFractionDigits
                                                  : 3)];
          engineeringSigDigits = true;
        }
      } else {
        [f setNumberStyle:NSNumberFormatterDecimalStyle];
      }

      [f setUsesGroupingSeparator:!o.useGrouping.empty()];
      /*
       * Foundation's positiveInfinitySymbol carries the sign ("+∞"), and
       * ECMA-402 renders a bare "∞" under signDisplay "auto". Twenty test262
       * files in NumberFormat/prototype/{format,formatToParts} check this. The
       * sign is applied by the prefix machinery instead, so stripping it here
       * does not lose it under signDisplay "always".
       */
      // Both can be nil — MEASURED, -plusSign is nil under
      // NSNumberFormatterCurrencyPluralStyle — and -hasPrefix: with a nil
      // argument raises NSInvalidArgumentException and terminates the process.
      // Four test262 files crashed the runner before this guard existed.
      NSString *inf = [f positiveInfinitySymbol];
      NSString *plus = [f plusSign];
      if (inf != nil && plus != nil && [plus length] > 0 &&
          [inf hasPrefix:plus]) {
        [f setPositiveInfinitySymbol:[inf substringFromIndex:[plus length]]];
      }
      [f setMinimumIntegerDigits:o.minimumIntegerDigits];
      if (o.minimumSignificantDigits > 0 || o.maximumSignificantDigits > 0) {
        [f setUsesSignificantDigits:YES];
        if (o.minimumSignificantDigits > 0) {
          [f setMinimumSignificantDigits:o.minimumSignificantDigits];
        }
        if (o.maximumSignificantDigits > 0) {
          [f setMaximumSignificantDigits:o.maximumSignificantDigits];
        }
      } else if (!engineeringSigDigits) {
        if (o.minimumFractionDigits >= 0) {
          [f setMinimumFractionDigits:o.minimumFractionDigits];
        }
        if (o.maximumFractionDigits >= 0) {
          [f setMaximumFractionDigits:o.maximumFractionDigits];
        }
      }
      [f setRoundingMode:roundingModeFor(o.roundingMode)];
      if (o.roundingIncrement != 1 && o.maximumFractionDigits >= 0) {
        [f setRoundingIncrement:@(o.roundingIncrement /
                                  std::pow(10.0, o.maximumFractionDigits))];
      }

      auto fmt = std::make_unique<AppleNumberFormatter>(
          f, o, foundationBase(o.locale));
      fmt->captureBasePrefixes();
      return fmt;
    }
  }

  /*
   * Collator.
   *
   * `sensitivity` maps onto NSStringCompareOptions; `caseFirst`,
   * `ignorePunctuation` and `collation` have no NSString option and go on the
   * locale as ICU keywords. That asymmetry is the divergence risk
   * docs/intl-completeness-map.md names, and it is written here rather than
   * hidden so the differential corpus knows what it is comparing.
   */
  std::unique_ptr<Collator> openCollator(const CollatorOptions &o) override {
    @autoreleasepool {
      std::string ident = foundationBase(o.locale);
      std::string keywords;
      auto addKeyword = [&](const std::string &k, const std::string &v) {
        if (v.empty()) return;
        if (!keywords.empty()) keywords += ";";
        keywords += k + "=" + v;
      };
      if (!o.collation.empty() && o.usage != "search") {
        addKeyword("collation", o.collation);
      }
      if (o.caseFirst == "upper") addKeyword("colcasefirst", "upper");
      if (o.caseFirst == "lower") addKeyword("colcasefirst", "lower");
      if (o.ignorePunctuation) addKeyword("colalternate", "shifted");
      if (o.numeric) addKeyword("colnumeric", "yes");
      if (!keywords.empty()) ident += "@" + keywords;

      NSStringCompareOptions opts = 0;
      if (o.sensitivity == "base") {
        opts |= NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch;
      } else if (o.sensitivity == "accent") {
        opts |= NSCaseInsensitiveSearch;
      } else if (o.sensitivity == "case") {
        opts |= NSDiacriticInsensitiveSearch;
      }
      // NSNumericSearch is a *lexical* numeric comparison and is the only
      // option Foundation has for `kn`; the `colnumeric` keyword above is the
      // collator-level one. Both are set, so the answer does not depend on
      // which of the two Foundation honours for a given locale.
      if (o.numeric) opts |= NSNumericSearch;

      NSLocale *loc = [NSLocale localeWithLocaleIdentifier:fromStd(ident)];
      if (loc == nil) return nullptr;
      CollatorOptions resolved = o;
      resolved.collation = o.usage == "search" || o.collation.empty()
                               ? std::string("default")
                               : o.collation;
      return std::make_unique<AppleCollator>(loc, opts, resolved);
    }
  }

  std::unique_ptr<RelativeTimeFormatter> openRelativeTimeFormat(
      const RelativeTimeOptions &o) override {
    @autoreleasepool {
      NSRelativeDateTimeFormatter *f =
          [[NSRelativeDateTimeFormatter alloc] init];
      std::string ident = foundationBase(o.locale);
      if (!o.numberingSystem.empty()) ident += "@numbers=" + o.numberingSystem;
      [f setLocale:[NSLocale localeWithLocaleIdentifier:fromStd(ident)]];
      [f setDateTimeStyle:o.numeric == "auto"
                              ? NSRelativeDateTimeFormatterStyleNamed
                              : NSRelativeDateTimeFormatterStyleNumeric];
      [f setUnitsStyle:o.style == "short"
                           ? NSRelativeDateTimeFormatterUnitsStyleShort
                       : o.style == "narrow"
                           ? NSRelativeDateTimeFormatterUnitsStyleAbbreviated
                           : NSRelativeDateTimeFormatterUnitsStyleFull];
      return std::make_unique<AppleRelativeTimeFormatter>(f, o);
    }
  }

  std::unique_ptr<ListFormatter> openListFormat(
      const ListFormatOptions &o) override {
    @autoreleasepool {
      NSListFormatter *f = [[NSListFormatter alloc] init];
      [f setLocale:[NSLocale localeWithLocaleIdentifier:fromStd(foundationBase(
                                                            o.locale))]];
      return std::make_unique<AppleListFormatter>(
          f, o, foundationBase(o.locale));
    }
  }

  /*
   * DisplayNames.
   *
   * All five of the types Foundation can answer were MEASURED working in
   * bench/spikes/intl/apple-surface-probe.m. `dateTimeField` has no
   * Objective-C API at all — android.icu has
   * DateTimePatternGenerator.getAppendItemName — so it returns empty and the
   * JavaScript layer applies the requested `fallback`. Deviation D21.
   */
  std::string displayName(
      const std::string &locale, const std::string &type,
      const std::string &style, const std::string &code) override {
    @autoreleasepool {
      NSLocale *loc =
          [NSLocale localeWithLocaleIdentifier:fromStd(foundationBase(locale))];
      if (loc == nil) return {};
      NSString *c = fromStd(code);
      NSString *out = nil;
      if (type == "language") {
        // -localizedStringForLanguageCode: takes a bare language code; a full
        // identifier ("de-AT") needs -localizedStringForLocaleIdentifier:, and
        // ECMA-402 accepts both shapes under `type: "language"`.
        out =
            code.find('-') == std::string::npos
                ? [loc localizedStringForLanguageCode:c]
                : [loc
                      localizedStringForLocaleIdentifier:fromStd(foundationBase(
                                                             code))];
      } else if (type == "region") {
        out = [loc localizedStringForCountryCode:c];
      } else if (type == "script") {
        out = [loc localizedStringForScriptCode:c];
      } else if (type == "currency") {
        out = [loc localizedStringForCurrencyCode:c];
      } else if (type == "calendar") {
        NSString *ident = [NSString
            stringWithFormat:@"%s@calendar=%s", foundationBase(locale).c_str(),
                             code.c_str()];
        NSCalendar *cal = [[NSLocale localeWithLocaleIdentifier:ident]
            objectForKey:NSLocaleCalendar];
        if (cal != nil && calendarKeywordFor(cal) == code) {
          out = [loc
              localizedStringForCalendarIdentifier:[cal calendarIdentifier]];
        }
      }
      return toStd(out);
    }
  }

  /*
   * Segmenter.
   *
   * -enumerateSubstringsInRange:options: yields the *content* runs and skips
   * the gaps between them for NSStringEnumerationByWords. MEASURED: grapheme
   * and sentence cover the string exactly, word covered 14 of 18 units and the
   * four it skipped were ", ", "! " and " ". So the gaps are recovered here and
   * reported as non-word-like segments, which is exactly what ECMA-402 wants —
   * but note the *derivation*: `isWordLike` is "did Foundation enumerate this
   * run", where Android reads ICU's own BreakIterator.getRuleStatus(). The two
   * agree on ordinary text and are not the same question.
   */
  std::vector<Segment> segment(
      const std::string &locale, const std::string &granularity,
      const std::u16string &text) override {
    @autoreleasepool {
      std::vector<Segment> out;
      if (text.empty()) return out;
      NSString *s = fromU16(text);
      NSStringEnumerationOptions opts =
          granularity == "word" ? NSStringEnumerationByWords
          : granularity == "sentence"
              ? NSStringEnumerationBySentences
              : NSStringEnumerationByComposedCharacterSequences;
      (void)locale;
      __block std::vector<Segment> found;
      [s enumerateSubstringsInRange:NSMakeRange(0, [s length])
                            options:opts
                         usingBlock:^(NSString *, NSRange r, NSRange, BOOL *) {
                           found.push_back(Segment{
                               static_cast<int32_t>(r.location),
                               static_cast<int32_t>(r.location + r.length),
                               true});
                         }];
      // Fill the gaps. For word granularity they are the non-word-like
      // segments ECMA-402 also requires; for the other granularities there are
      // none, and this loop is then a no-op that costs one comparison.
      int32_t pos = 0;
      for (const Segment &seg : found) {
        if (seg.begin > pos) {
          out.push_back(Segment{pos, seg.begin, false});
        }
        out.push_back(seg);
        pos = seg.end;
      }
      if (pos < static_cast<int32_t>(text.size())) {
        out.push_back(Segment{pos, static_cast<int32_t>(text.size()), false});
      }
      return out;
    }
  }

  std::u16string caseMap(
      const std::string &locale, bool upper,
      const std::u16string &text) override {
    @autoreleasepool {
      NSLocale *loc =
          [NSLocale localeWithLocaleIdentifier:fromStd(foundationBase(locale))];
      NSString *s = fromU16(text);
      return toU16(
          upper ? [s uppercaseStringWithLocale:loc]
                : [s lowercaseStringWithLocale:loc]);
    }
  }

  /*
   * The enumerations.
   *
   * Foundation has no collation enumeration in Objective-C; Swift's
   * Locale.Collation.availableCollations does. A build without Swift reports
   * none, which is self-consistent — Intl.supportedValuesOf("collation") must
   * be exactly what a Collator accepts, and with no enumeration the honest
   * answer is the empty list rather than a hand-written one. That is the same
   * mistake that cost this backend two test262 files when its calendar list was
   * hand-written.
   */
  /**
   * The ICU keyword name for a collation is not the BCP-47 `co` type.
   *
   * Locale.Collation.availableCollations reports ICU's long names —
   * "dictionary", "phonebook", "traditional" — while the `co` extension uses
   * "dict", "phonebk", "trad". test262's supportedValuesOf/collations.js
   * checks that every entry matches the `type` production, whose subtags are
   * 3-8 alphanumerics; "dictionary" is ten characters and fails it.
   *
   * The mapping is a fixed CLDR bcp47 alias table (common/bcp47/collation.xml),
   * not locale data: it is the same six entries in every locale and adding a
   * locale adds nothing to it.
   */
  static std::string bcp47Collation(const std::string &icuName) {
    static const std::pair<const char *, const char *> kAliases[] = {
        {"dictionary", "dict"},
        {"gb2312han", "gb2312"},
        {"phonebook", "phonebk"},
        {"traditional", "trad"},
    };
    for (const auto &a : kAliases) {
      if (icuName == a.first) return a.second;
    }
    return icuName;
  }

  /// Every subtag 3-8 alphanumerics, which is the BCP-47 `type` production.
  static bool isKeywordType(const std::string &s) {
    size_t run = 0;
    for (size_t i = 0; i <= s.size(); i++) {
      if (i == s.size() || s[i] == '-') {
        if (run < 3 || run > 8) return false;
        run = 0;
        continue;
      }
      const char c = s[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))) {
        return false;
      }
      run++;
    }
    return true;
  }

  std::vector<std::string> collations() override {
    if (rnqjs_intl_collations_swift == nullptr) return {};
    const std::string joined = swiftString(rnqjs_intl_collations_swift());
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
      if (cur.empty()) return;
      const std::string t = bcp47Collation(cur);
      if (t != "standard" && t != "search" && isKeywordType(t)) {
        out.push_back(t);
      }
      cur.clear();
    };
    for (char c : joined) {
      if (c == '\x1f') {
        flush();
      } else {
        cur += c;
      }
    }
    flush();
    return out;
  }

  std::vector<std::string> currencies() override {
    @autoreleasepool {
      /*
       * Only the codes Foundation can also *name*.
       *
       * test262's supportedValuesOf/currencies-accepted-by-DisplayNames.js
       * requires every currency reported here to have a display name, and
       * MEASURED on macOS 26.5 a handful of ISOCurrencyCodes entries ("LSM"
       * among them) have none. Probing rather than asserting is the same
       * correction that was applied to calendars() and numberingSystems().
       */
      NSLocale *en = [NSLocale localeWithLocaleIdentifier:@"en_US"];
      std::vector<std::string> out;
      for (NSString *c in [NSLocale ISOCurrencyCodes]) {
        // ECMA-402 requires exactly three ASCII letters, upper case.
        const std::string s = toStd(c);
        if (s.size() != 3) continue;
        NSString *name = [en localizedStringForCurrencyCode:c];
        if (name == nil || [name length] == 0) continue;
        out.push_back(s);
      }
      return out;
    }
  }

  int32_t currencyDigits(const std::string &code) override {
    @autoreleasepool {
      NSNumberFormatter *f = [[NSNumberFormatter alloc] init];
      [f setNumberStyle:NSNumberFormatterCurrencyStyle];
      [f setLocale:[NSLocale localeWithLocaleIdentifier:@"en_US"]];
      [f setCurrencyCode:fromStd(code)];
      return static_cast<int32_t>([f maximumFractionDigits]);
    }
  }

  std::vector<std::string> localeCalendars(const std::string &locale) override {
    @autoreleasepool {
      // The locale's own calendar first, then everything the platform honours.
      std::vector<std::string> out;
      NSCalendar *own =
          [[NSLocale localeWithLocaleIdentifier:fromStd(foundationBase(locale))]
              objectForKey:NSLocaleCalendar];
      if (own != nil) out.push_back(calendarKeywordFor(own));
      for (const std::string &c : calendars()) {
        if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
      }
      return out;
    }
  }

  std::vector<std::string> localeNumberingSystems(
      const std::string &locale) override {
    std::vector<std::string> out;
    out.push_back(defaultNumberingSystem(foundationBase(locale)));
    return out;
  }

  std::vector<std::string> localeTimeZones(const std::string &locale) override {
    @autoreleasepool {
      /*
       * Foundation has no "zones for region" query. Rather than return the
       * whole 443-entry list — which would be wrong, since ECMA-402 asks for
       * the zones *of the locale's region* — this returns the empty list, and
       * the JavaScript layer reports an empty array. Deviation D22, and the
       * measurement that would close it is whether
       * android.icu.util.TimeZone.getAvailableIDs(String country) has an Apple
       * equivalent; the probe found none.
       */
      (void)locale;
      return {};
    }
  }

  std::vector<std::string> localeCollations(const std::string &) override {
    return collations();
  }

  std::string localeHourCycle(const std::string &locale) override {
    @autoreleasepool {
      // Read from the pattern the platform picks for an hour-only skeleton,
      // which is the only way to learn a locale's preferred cycle: there is no
      // property for it.
      NSDateFormatter *f = [[NSDateFormatter alloc] init];
      [f setLocale:[NSLocale localeWithLocaleIdentifier:fromStd(foundationBase(
                                                            locale))]];
      [f setLocalizedDateFormatFromTemplate:@"j"];
      const std::string pattern = toStd([f dateFormat]);
      for (char c : pattern) {
        if (c == 'h') return "h12";
        if (c == 'H') return "h23";
        if (c == 'K') return "h11";
        if (c == 'k') return "h24";
      }
      return {};
    }
  }

  std::string localeTextDirection(const std::string &locale) override {
    @autoreleasepool {
      std::string lang = foundationBase(locale);
      const size_t us = lang.find('_');
      if (us != std::string::npos) lang = lang.substr(0, us);
      const NSLocaleLanguageDirection d =
          [NSLocale characterDirectionForLanguage:fromStd(lang)];
      if (d == NSLocaleLanguageDirectionRightToLeft) return "rtl";
      if (d == NSLocaleLanguageDirectionLeftToRight) return "ltr";
      return {};
    }
  }

  /*
   * Week information.
   *
   * MEASURED on macOS 26.5: en_US first=1 minDays=1 weekend=[7,1];
   * fr_FR first=2 minDays=4; ar_EG first=7 weekend=[6,7], all in Foundation's
   * Sunday=1 numbering. ECMA-402 numbers Monday=1..Sunday=7, so the conversion
   * happens here — the seam carries spec units, not platform units.
   *
   * The weekend is probed rather than read, because Foundation exposes only
   * -isDateInWeekend: and no list. Seven consecutive days from a known Monday
   * answer it exactly.
   */
  bool localeWeekInfo(const std::string &locale, WeekInfo &out) override {
    @autoreleasepool {
      NSLocale *loc =
          [NSLocale localeWithLocaleIdentifier:fromStd(foundationBase(locale))];
      NSCalendar *cal = [loc objectForKey:NSLocaleCalendar];
      if (cal == nil) return false;
      auto toSpec = [](NSUInteger sundayBased) -> int32_t {
        // Foundation: 1 = Sunday .. 7 = Saturday. ECMA-402: 1 = Monday .. 7 = Sunday.
        return static_cast<int32_t>(sundayBased == 1 ? 7 : sundayBased - 1);
      };
      out.firstDay = toSpec([cal firstWeekday]);
      out.minimalDays = static_cast<int32_t>([cal minimumDaysInFirstWeek]);
      out.weekend.clear();
      // 2024-01-01 was a Monday, so day i of this run is ECMA-402 weekday i+1.
      NSDateComponents *dc = [[NSDateComponents alloc] init];
      [dc setYear:2024];
      [dc setMonth:1];
      [dc setDay:1];
      [dc setHour:12];
      NSCalendar *gregorian = [[NSCalendar alloc]
          initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
      [gregorian setTimeZone:[NSTimeZone timeZoneWithName:@"UTC"]];
      NSDate *monday = [gregorian dateFromComponents:dc];
      if (monday == nil) return true;
      for (int i = 0; i < 7; i++) {
        NSDate *d = [monday dateByAddingTimeInterval:i * 86400.0];
        if ([cal isDateInWeekend:d]) out.weekend.push_back(i + 1);
      }
      return true;
    }
  }

 private:
  static std::string viaSwift(
      const char *(*fn)(const char *), const std::string &tag) {
    // Weak symbol: null when the Swift file is not in the build. Returning
    // empty means "no opinion" and the JS layer passes the tag through, which
    // is a documented degradation rather than a failure.
    if (fn == nullptr) return {};
    const char *r = fn(tag.c_str());
    if (r == nullptr) return {};
    std::string out(r);
    if (rnqjs_intl_free_swift != nullptr) rnqjs_intl_free_swift(r);
    return out;
  }
};

ApplePlatform gPlatform;

}  // namespace
}  // namespace rnqjs::intl

/*
 * Registered at image load, which is earlier than any React Native lifecycle
 * hook and therefore earlier than any JavaScript. The module's own install()
 * only defines an accessor, so the platform must be in place by the time that
 * accessor is first *read*, not by the time it is installed — but +load is
 * earlier than both and needs no ordering argument.
 */
@interface RNQJSIntlPlatformRegistrar : NSObject
@end

@implementation RNQJSIntlPlatformRegistrar
+ (void)load {
  rnqjs::intl::setPlatform(&rnqjs::intl::gPlatform);
}
@end
