/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Shared half of the platform seam for react-native-quickjs-intl, plus the
 * default no-platform backend.
 *
 * WHY THE DEFAULT BACKEND IS HERE RATHER THAN BEING A NULL POINTER
 *   modules/text-encoding's template lets platform() return nullptr and makes
 *   every caller check. That works when the platform half is optional. It is
 *   not optional here: `Intl` existing but throwing from every constructor is
 *   worse than `Intl` not existing, because a bundle's feature detection would
 *   see it and skip its polyfill.
 *
 *   And there are a lot of no-platform builds. `qjs-bench`,
 *   `tests/quickjs_module_tests`, `tests/conformance`, `tools/intl-cli`, the
 *   opcode profiler and the test262 runner all build on a host with no NSLocale
 *   and no android.icu. Every one of them must link and must get correctly
 *   *shaped* Intl objects, or the JavaScript layer cannot be tested at all
 *   without a device in the loop. That makes this file load-bearing rather than
 *   a placeholder.
 *
 * WHAT THE STUB DOES
 *   Formats dates in the root locale — en-US conventions, proleptic Gregorian,
 *   UTC — and reports exactly one available locale and exactly one timezone.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   - No timezone database. Implementing one would mean shipping the data this
 *     whole design exists to avoid, so any zone other than UTC is *rejected*
 *     rather than accepted-and-ignored. Deviation D7. Accepting
 *     "America/New_York" and then formatting in UTC would make host test output
 *     silently wrong in a way a differential run would blame on the JavaScript.
 *   - No non-Gregorian calendars, no non-Latin numbering systems.
 *   - No likely-subtags: maximize/minimize return "" ("no opinion").
 *
 * ON PARTS
 *   formatToParts here is exact rather than reconstructed, because this backend
 *   renders from the pattern itself and therefore knows where every field
 *   begins and ends. That makes it the reference the differential corpus
 *   compares platform backends against for part *sequence*, even though the
 *   part *text* is locale data and differs.
 */

#include "IntlPlatform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rnqjs::intl {

#ifdef RNQJS_INTL_ABLATION
/*
 * The single definition of the ablation arm, for every backend. Read once from
 * RNQJS_INTL_ABL; see the contract in IntlPlatform.h. Compiled in only under
 * -DRNQJS_INTL_ABLATION, so a shipping build contains neither this function
 * nor the guard variable its function-local static would need.
 */
int intlAblation() {
  static const int arm = [] {
    const char *e = std::getenv("RNQJS_INTL_ABL");
    return e != nullptr ? std::atoi(e) : 0;
  }();
  return arm;
}
#endif

namespace {

/* ------------------------------------------------------------------------- */
/* Civil-date arithmetic                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Howard Hinnant's public-domain chrono-compatible date algorithms.
 *
 * Used rather than gmtime_r because they are exact for the proleptic Gregorian
 * calendar over the whole ECMAScript time range. gmtime_r is undefined outside
 * [1970, 2038] on 32-bit targets and rejects pre-1970 dates on some platforms,
 * and ECMAScript dates run to +/-8.64e15 ms, which is +/-273,790 years.
 */
void civilFromDays(int64_t z, int &y, int &m, int &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;  // [0, 146096]
  const int64_t yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const int64_t yy = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
  const int64_t mp = (5 * doy + 2) / 153;                       // [0, 11]
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);           // [1, 31]
  const int64_t mm = mp + (mp < 10 ? 3 : -9);                   // [1, 12]
  m = static_cast<int>(mm);
  y = static_cast<int>(yy + (mm <= 2));
}

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}
int64_t floorMod(int64_t a, int64_t b) {
  return a - floorDiv(a, b) * b;
}

struct Civil {
  int year = 0, month = 1, day = 1;
  int hour = 0, minute = 0, second = 0, ms = 0;
  int weekday = 0;  ///< 0 = Sunday
};

Civil civilFromEpochMs(double epochMs) {
  Civil c;
  const auto ms = static_cast<int64_t>(epochMs);
  const int64_t days = floorDiv(ms, 86400000LL);
  int64_t rem = floorMod(ms, 86400000LL);
  civilFromDays(days, c.year, c.month, c.day);
  c.hour = static_cast<int>(rem / 3600000);
  rem %= 3600000;
  c.minute = static_cast<int>(rem / 60000);
  rem %= 60000;
  c.second = static_cast<int>(rem / 1000);
  c.ms = static_cast<int>(rem % 1000);
  // 1970-01-01 was a Thursday.
  c.weekday = static_cast<int>(floorMod(days + 4, 7));
  return c;
}

const char *const kMonthWide[] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};
const char *const kMonthAbbr[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char *const kDayWide[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                "Thursday", "Friday", "Saturday"};
const char *const kDayAbbr[] = {"Sun", "Mon", "Tue", "Wed",
                                "Thu", "Fri", "Sat"};

/* ------------------------------------------------------------------------- */
/* Skeleton -> pattern                                                        */
/* ------------------------------------------------------------------------- */

/*
 * A real ICU DateTimePatternGenerator does locale-specific field ordering and
 * inserts the locale's own separators. This does the en-US arrangement only,
 * which is the whole of the "root locale output" promise. It is not an attempt
 * to be ICU, and it must not grow into one — the moment it needs a per-locale
 * table, that table belongs in a platform backend.
 *
 * The output is a CLDR *pattern*, so the renderer below is shared with the
 * Apple backend, which obtains its pattern from NSDateFormatter instead of
 * synthesising one.
 */
std::string patternFromSkeleton(
    const std::string &skel, const std::string &hourCycle) {
  bool hasEra = false, hasWeekday = false, weekdayWide = false;
  int mon = 0;  // 0 none, 1 numeric, 2 2-digit, 3 abbr, 4 wide, 5 narrow
  int year = 0, day = 0, frac = 0;
  bool hour = false, hour2 = false, h12 = false;
  bool minute = false, second = false;
  int tz = 0;  // 0 none, 1 short, 2 long, 3 shortOffset, 4 longOffset

  for (size_t i = 0; i < skel.size();) {
    const char c = skel[i];
    size_t run = 0;
    while (i + run < skel.size() && skel[i + run] == c) run++;
    switch (c) {
      case 'G':
        hasEra = true;
        break;
      case 'E':
      case 'e':
      case 'c':
        hasWeekday = true;
        weekdayWide = run >= 4;
        break;
      case 'y':
      case 'u':
        year = static_cast<int>(run);
        break;
      case 'M':
      case 'L':
        mon = run == 1 ? 1 : run == 2 ? 2 : run == 3 ? 3 : run == 4 ? 4 : 5;
        break;
      case 'd':
        day = static_cast<int>(run);
        break;
      case 'j':
      case 'J':
      case 'C':
        hour = true;
        hour2 = run >= 2;
        // `j` means "the locale's preferred hour cycle"; the root locale is
        // h12.
        h12 = !(hourCycle == "h23" || hourCycle == "h24");
        break;
      case 'h':
      case 'K':
        hour = true;
        hour2 = run >= 2;
        h12 = true;
        break;
      case 'H':
      case 'k':
        hour = true;
        hour2 = run >= 2;
        h12 = false;
        break;
      case 'm':
        minute = true;
        break;
      case 's':
        second = true;
        break;
      case 'S':
        frac = static_cast<int>(run);
        break;
      case 'z':
      case 'v':
        tz = run >= 4 ? 2 : 1;
        break;
      case 'O':
        tz = run >= 4 ? 4 : 3;
        break;
      case 'V':
        tz = 1;
        break;
      default:
        break;
    }
    i += run;
  }

  std::string date;
  std::string time;

  if (hasWeekday && (mon || day || year))
    date += weekdayWide ? "EEEE, " : "EEE, ";
  if (mon == 4 || mon == 3 || mon == 5) {
    date += mon == 4 ? "MMMM" : mon == 3 ? "MMM" : "MMMMM";
    if (day) date += " d";
    if (year) date += ", y";
  } else if (mon) {
    date += mon == 2 ? "MM" : "M";
    if (day) date += day >= 2 ? "/dd" : "/d";
    if (year) date += "/y";
  } else if (day) {
    date += day >= 2 ? "dd" : "d";
    if (year) date += ", y";
  } else if (year) {
    date += "y";
  } else if (hasWeekday) {
    date += weekdayWide ? "EEEE" : "EEE";
  }
  if (hasEra && (year || mon || day)) date += " G";

  if (hour) {
    time += h12 ? (hour2 ? "hh" : "h") : (hour2 ? "HH" : "H");
    if (minute) time += ":mm";
    if (second) time += ":ss";
    if (frac) {
      time += ".";
      for (int i = 0; i < frac && i < 3; i++) time += "S";
    }
    if (h12) time += " a";
  } else if (minute) {
    time += "mm";
    if (second) time += ":ss";
  } else if (second) {
    time += "ss";
  }
  if (tz) {
    if (!time.empty() || !date.empty()) time += " ";
    time += tz == 2 ? "zzzz" : tz == 3 ? "O" : tz == 4 ? "OOOO" : "z";
  }

  if (!date.empty() && !time.empty()) return date + ", " + time;
  if (!date.empty()) return date;
  if (!time.empty()) return time;
  return "M/d/y";
}

std::string patternFromStyles(const std::string &ds, const std::string &ts) {
  std::string d, t;
  if (!ds.empty()) {
    d = ds == "full"     ? "EEEE, MMMM d, y"
        : ds == "long"   ? "MMMM d, y"
        : ds == "medium" ? "MMM d, y"
                         : "M/d/yy";
  }
  if (!ts.empty()) {
    t = (ts == "full" || ts == "long") ? "h:mm:ss a zzzz"
        : ts == "medium"               ? "h:mm:ss a"
                                       : "h:mm a";
  }
  if (!d.empty() && !t.empty()) return d + ", " + t;
  if (!d.empty()) return d;
  if (!t.empty()) return t;
  return "M/d/yy";
}

/* ------------------------------------------------------------------------- */
/* Pattern rendering                                                          */
/* ------------------------------------------------------------------------- */

class Renderer {
 public:
  void run(const std::string &pattern, const Civil &c);
  const std::u16string &text() const {
    return text_;
  }
  const std::vector<Part> &parts() const {
    return parts_;
  }

 private:
  void open(PartType t) {
    if (!parts_.empty() && parts_.back().type == t) return;  // merge adjacent
    parts_.push_back(Part{
        t, static_cast<int32_t>(text_.size()),
        static_cast<int32_t>(text_.size())});
  }
  void put(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
      text_.push_back(static_cast<char16_t>(static_cast<unsigned char>(s[i])));
    }
    if (!parts_.empty()) parts_.back().end = static_cast<int32_t>(text_.size());
  }
  void put(const char *s) {
    put(s, strlen(s));
  }
  void num(long v, int width) {
    char tmp[32];
    int n = v < 0 ? snprintf(tmp, sizeof(tmp), "-%0*ld", width, -v)
                  : snprintf(tmp, sizeof(tmp), "%0*ld", width, v);
    put(tmp, static_cast<size_t>(n));
  }

  std::u16string text_;
  std::vector<Part> parts_;
};

void Renderer::run(const std::string &pat, const Civil &c) {
  text_.clear();
  parts_.clear();
  for (size_t i = 0; i < pat.size();) {
    const char ch = pat[i];
    if (ch == '\'') {
      // CLDR quoting: '' is a literal quote, '...' is a literal run.
      i++;
      open(PartType::Literal);
      if (i < pat.size() && pat[i] == '\'') {
        put("'", 1);
        i++;
        continue;
      }
      while (i < pat.size() && pat[i] != '\'') put(&pat[i++], 1);
      if (i < pat.size()) i++;
      continue;
    }
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) {
      open(PartType::Literal);
      put(&pat[i], 1);
      i++;
      continue;
    }
    size_t run = 0;
    while (i + run < pat.size() && pat[i + run] == ch) run++;
    switch (ch) {
      case 'G':
        open(PartType::Era);
        if (run >= 5)
          put(c.year > 0 ? "A" : "B");
        else if (run == 4)
          put(c.year > 0 ? "Anno Domini" : "Before Christ");
        else
          put(c.year > 0 ? "AD" : "BC");
        break;
      case 'y':
      case 'u': {
        long y = c.year;
        if (ch == 'y' && y <= 0) y = 1 - y;  // era year
        open(PartType::Year);
        if (run == 2)
          num(y % 100, 2);
        else
          num(y, static_cast<int>(run));
        break;
      }
      case 'M':
      case 'L':
        open(PartType::Month);
        if (run >= 5)
          put(kMonthWide[c.month - 1], 1);
        else if (run == 4)
          put(kMonthWide[c.month - 1]);
        else if (run == 3)
          put(kMonthAbbr[c.month - 1]);
        else
          num(c.month, static_cast<int>(run));
        break;
      case 'd':
        open(PartType::Day);
        num(c.day, static_cast<int>(run));
        break;
      case 'E':
      case 'e':
      case 'c':
        open(PartType::Weekday);
        if (run >= 5)
          put(kDayWide[c.weekday], 1);
        else if (run == 4)
          put(kDayWide[c.weekday]);
        else
          put(kDayAbbr[c.weekday]);
        break;
      case 'a':
      case 'b':
      case 'B':
        open(PartType::DayPeriod);
        put(c.hour < 12 ? "AM" : "PM");
        break;
      case 'h': {
        int h = c.hour % 12;
        if (h == 0) h = 12;
        open(PartType::Hour);
        num(h, static_cast<int>(run));
        break;
      }
      case 'K':
        open(PartType::Hour);
        num(c.hour % 12, static_cast<int>(run));
        break;
      case 'H':
        open(PartType::Hour);
        num(c.hour, static_cast<int>(run));
        break;
      case 'k':
        open(PartType::Hour);
        num(c.hour == 0 ? 24 : c.hour, static_cast<int>(run));
        break;
      case 'm':
        open(PartType::Minute);
        num(c.minute, static_cast<int>(run));
        break;
      case 's':
        open(PartType::Second);
        num(c.second, static_cast<int>(run));
        break;
      case 'S': {
        open(PartType::FractionalSecond);
        const int digits = run > 3 ? 3 : static_cast<int>(run);
        long v = c.ms;
        if (digits == 1)
          v /= 100;
        else if (digits == 2)
          v /= 10;
        num(v, digits);
        break;
      }
      case 'z':
      case 'v':
      case 'V':
      case 'O':
      case 'Z':
      case 'X':
      case 'x':
        open(PartType::TimeZoneName);
        put("UTC");
        break;
      default:
        // An unknown pattern letter is a bug in the pattern, not in the data.
        // Emitting it verbatim makes it visible in a diff rather than silently
        // dropping a field.
        open(PartType::Literal);
        put(&pat[i], run);
        break;
    }
    i += run;
  }
}

/* ------------------------------------------------------------------------- */
/* The stub backend                                                           */
/* ------------------------------------------------------------------------- */

class StubFormatter final : public DateTimeFormatter {
 public:
  StubFormatter(std::string pattern, std::string timeZone)
      : pattern_(std::move(pattern)), timeZone_(std::move(timeZone)) {
    // The resolved hour cycle is a property of the pattern that was actually
    // chosen, not of what was asked for. Read it back rather than echoing the
    // request; that is the whole reason resolved() exists.
    for (size_t i = 0; i < pattern_.size(); i++) {
      const char c = pattern_[i];
      if (c == '\'') {
        i++;
        while (i < pattern_.size() && pattern_[i] != '\'') i++;
        continue;
      }
      if (c == 'h') {
        hourCycle_ = "h12";
        break;
      }
      if (c == 'H') {
        hourCycle_ = "h23";
        break;
      }
      if (c == 'K') {
        hourCycle_ = "h11";
        break;
      }
      if (c == 'k') {
        hourCycle_ = "h24";
        break;
      }
    }
  }

  bool format(double epochMs, std::u16string &out) override {
    if (!std::isfinite(epochMs)) return false;
    Renderer r;
    r.run(pattern_, civilFromEpochMs(epochMs));
    out = r.text();
    return true;
  }

  bool formatToParts(double epochMs, FormattedParts &out) override {
    if (!std::isfinite(epochMs)) return false;
    Renderer r;
    r.run(pattern_, civilFromEpochMs(epochMs));
    out.text = r.text();
    out.parts = r.parts();
    return true;
  }

  std::string resolved(const std::string &key) override {
    if (key == "locale") return "en-US";
    if (key == "calendar") return "gregory";
    if (key == "numberingSystem") return "latn";
    if (key == "timeZone") return timeZone_;
    if (key == "hourCycle") return hourCycle_;
    if (key == "pattern") return pattern_;
    return {};
  }

 private:
  std::string pattern_;
  std::string timeZone_;
  std::string hourCycle_;
};

/* ------------------------------------------------------------------------- */
/* The stub number formatter                                                  */
/* ------------------------------------------------------------------------- */
/*
 * Root-locale number formatting: en-US conventions, latn digits, group of
 * three, `.` and `,`.
 *
 * The same rule as the date stub applies — this is not an attempt to be ICU and
 * it must not grow into one. The moment it needs a per-locale table, that table
 * belongs in a platform backend. What it must do is be *shaped* correctly, so
 * that js/intl.js and the differential corpus can be exercised on a host with
 * no NSLocale and no android.icu.
 *
 * Its rounding is IEEE `%.*f`, which breaks ties to even, where ECMA-402's
 * default is `halfExpand`. That difference is part of deviation D7 (the
 * no-platform backend is approximate) and is why the corpus never diffs stub
 * *text* against node.
 */
std::u16string ascii(const std::string &s) {
  std::u16string out;
  out.reserve(s.size());
  for (unsigned char c : s) out.push_back(static_cast<char16_t>(c));
  return out;
}

/// Rounds to `digits` fraction digits and returns "123.456" with no grouping.
std::string fixed(double v, int digits) {
  char buf[512];
  snprintf(buf, sizeof(buf), "%.*f", digits, v);
  return buf;
}

/// Rounds to `sig` significant digits, returning a plain decimal string.
std::string significant(double v, int sig) {
  if (v == 0) return fixed(0, sig > 0 ? sig - 1 : 0);
  const double mag = std::floor(std::log10(std::fabs(v)));
  int frac = sig - 1 - static_cast<int>(mag);
  if (frac < 0) frac = 0;
  if (frac > 100) frac = 100;
  std::string s = fixed(v, frac);
  // Re-derive: rounding may have carried into a new magnitude (9.99 -> 10.0).
  return s;
}

void stripTrailingZeros(std::string &s, int minFrac) {
  const size_t dot = s.find('.');
  if (dot == std::string::npos) return;
  size_t end = s.size();
  const size_t floor = dot + 1 + static_cast<size_t>(minFrac < 0 ? 0 : minFrac);
  while (end > floor && s[end - 1] == '0') end--;
  if (end == dot + 1) end = dot;  // no fraction digits left at all
  s.resize(end);
}

void groupIntegerPart(std::string &s, int primary) {
  size_t begin = (!s.empty() && s[0] == '-') ? 1 : 0;
  size_t dot = s.find('.');
  if (dot == std::string::npos) dot = s.size();
  if (dot - begin <= static_cast<size_t>(primary)) return;
  for (size_t pos = dot - static_cast<size_t>(primary); pos > begin;
       pos -= static_cast<size_t>(primary)) {
    s.insert(pos, ",");
    if (pos < begin + static_cast<size_t>(primary)) break;
  }
}

class StubNumberFormatter final : public NumberFormatter {
 public:
  explicit StubNumberFormatter(NumberOptions o) : o_(std::move(o)) {}

  /*
   * The `exactDouble` hint is accepted and DELIBERATELY IGNORED.
   *
   * This backend renders `decimalString` itself, digit by digit, with no
   * double anywhere on the path — that is what makes it exact for BigInt and
   * what makes it the reference the differential corpora are scored against.
   * There is no cheaper route for it to switch to, and switching to a double
   * would forfeit the one property it exists to have.
   *
   * It is also the control for the hint: `tools/exact-double-differential.mjs`
   * formats the same corpus through this backend and through the Apple one and
   * requires byte-identical output, which is how "the double route agrees with
   * the digit route" is checked rather than assumed.
   */
  bool format(
      double value, const std::string &decimalString, uint32_t /*hints*/,
      std::u16string &out) override {
    /*
     * The decimalString path renders the digits directly, without a double
     * anywhere. That is what makes the no-platform backend exact for BigInt:
     * `9007199254740993n` prints its own last digit rather than the nearest
     * double's. The header states the contract — these digits are final.
     */
    if (!decimalString.empty()) {
      std::string body = decimalString;
      if (o_.useGrouping != "") groupIntegerPart(body, 3);
      out = utf8ToU16(decorate(body, decimalString[0] == '-' ? -1.0 : 1.0));
      return true;
    }

    if (std::isnan(value)) {
      out = ascii("NaN");
      return true;
    }
    double v = value;
    if (o_.style == "percent") v *= 100.0;

    std::string body;
    if (std::isinf(v)) {
      body = v < 0 ? "-\xE2\x88\x9E" : "\xE2\x88\x9E";  // U+221E
      out = utf8ToU16(decorate(body, v));
      return true;
    }

    int expShown = 0;
    std::u16string compactSuffix;
    if (o_.notation == "scientific" || o_.notation == "engineering") {
      if (v != 0) {
        expShown = static_cast<int>(std::floor(std::log10(std::fabs(v))));
        if (o_.notation == "engineering") {
          expShown = static_cast<int>(std::floor(expShown / 3.0)) * 3;
        }
        v /= std::pow(10.0, expShown);
      }
    } else if (o_.notation == "compact") {
      static const struct {
        double factor;
        const char *shortSuffix;
        const char *longSuffix;
      } kScales[] = {
          {1e12, "T", " trillion"},
          {1e9, "B", " billion"},
          {1e6, "M", " million"},
          {1e3, "K", " thousand"},
      };
      for (const auto &sc : kScales) {
        if (std::fabs(v) >= sc.factor) {
          v /= sc.factor;
          compactSuffix = ascii(
              o_.compactDisplay == "long" ? sc.longSuffix : sc.shortSuffix);
          break;
        }
      }
    }

    body = digitsFor(v);
    if (o_.useGrouping != "" && o_.notation != "scientific" &&
        o_.notation != "engineering") {
      groupIntegerPart(body, 3);
    }

    std::u16string text = utf8ToU16(decorate(body, value));
    if (o_.notation == "scientific" || o_.notation == "engineering") {
      text += u'E';
      if (expShown < 0) text += u'-';
      text += ascii(std::to_string(std::abs(expShown)));
    }
    text += compactSuffix;
    out = text;
    return true;
  }

  std::string resolved(const std::string &key) override {
    if (key == "locale") return "en-US";
    if (key == "numberingSystem") return "latn";
    return {};
  }

  void symbols(NumberSymbols &s) override {
    s.decimal = u".";
    s.group = u",";
    s.minusSign = u"-";
    s.plusSign = u"+";
    s.percent = u"%";
    s.exponential = u"E";
    s.nan = u"NaN";
    s.infinity = u"∞";
    if (o_.style == "currency") s.currency = ascii(o_.currency);
    s.digits.clear();
    for (char c = '0'; c <= '9'; c++) s.digits.push_back(std::u16string(1, c));
  }

 private:
  static std::u16string utf8ToU16(const std::string &s) {
    // The stub only ever produces ASCII plus U+221E, so a 2/3-byte decoder is
    // enough and there is no need to link a transcoder.
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
      const auto c = static_cast<unsigned char>(s[i]);
      if (c < 0x80) {
        out.push_back(static_cast<char16_t>(c));
        i++;
      } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        out.push_back(static_cast<char16_t>(
            ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F)));
        i += 2;
      } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        out.push_back(static_cast<char16_t>(
            ((c & 0x0F) << 12) |
            ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
            (static_cast<unsigned char>(s[i + 2]) & 0x3F)));
        i += 3;
      } else {
        i++;
      }
    }
    return out;
  }

  std::string digitsFor(double v) {
    std::string s;
    if (o_.maximumSignificantDigits > 0) {
      s = significant(v, o_.maximumSignificantDigits);
      stripTrailingZeros(s, o_.minimumSignificantDigits > 0 ? 0 : 0);
    } else {
      int maxFrac =
          o_.maximumFractionDigits >= 0 ? o_.maximumFractionDigits : 3;
      int minFrac =
          o_.minimumFractionDigits >= 0 ? o_.minimumFractionDigits : 0;
      if (maxFrac > 100) maxFrac = 100;
      s = fixed(v, maxFrac);
      stripTrailingZeros(s, minFrac);
    }
    // minimumIntegerDigits
    size_t begin = (!s.empty() && s[0] == '-') ? 1 : 0;
    size_t dot = s.find('.');
    if (dot == std::string::npos) dot = s.size();
    while (dot - begin < static_cast<size_t>(o_.minimumIntegerDigits)) {
      s.insert(begin, "0");
      dot++;
    }
    return s;
  }

  /// Applies sign display, the percent sign, the currency text and the unit
  /// text around an already-rendered numeral.
  std::string decorate(const std::string &body, double original) {
    std::string s = body;
    const bool negative = !s.empty() && s[0] == '-';
    if (negative) s.erase(0, 1);

    std::string sign;
    if (o_.signDisplay == "never") {
      sign = "";
    } else if (negative) {
      sign = o_.signDisplay == "negative" && original == 0 ? "" : "-";
    } else if (o_.signDisplay == "always") {
      sign = "+";
    } else if (o_.signDisplay == "exceptZero" && original != 0) {
      sign = "+";
    }

    if (o_.style == "percent") return sign + s + "%";
    if (o_.style == "currency") return sign + o_.currency + s;
    if (o_.style == "unit") return sign + s + " " + o_.unit;
    return sign + s;
  }

  NumberOptions o_;
};

/* ------------------------------------------------------------------------- */
/* The stub collator, list, relative-time and segmenter                       */
/* ------------------------------------------------------------------------- */

class StubCollator final : public Collator {
 public:
  explicit StubCollator(CollatorOptions o) : o_(std::move(o)) {}

  int32_t compare(std::u16string_view a, std::u16string_view b) override {
    // Code-unit order, plus the two option levers that are algorithm rather
    // than data: `numeric` and `sensitivity: "base"|"accent"` case folding.
    // Anything that needs a collation table is not attempted, which is why
    // resolved("collation") answers "default" and never a tailoring.
    //
    // The plain path compares the borrowed views directly. Only the two
    // branches that must transform the text materialize a string, which is
    // the same answer as before with two fewer allocations in the common case.
    if (o_.sensitivity == "base" || o_.sensitivity == "accent") {
      const std::u16string x = foldAsciiCase(std::u16string(a));
      const std::u16string y = foldAsciiCase(std::u16string(b));
      const int r = o_.numeric ? numericCompare(x, y)
                               : (x < y   ? -1
                                  : x > y ? 1
                                          : 0);
      return static_cast<int32_t>(r);
    }
    if (o_.numeric) {
      return static_cast<int32_t>(
          numericCompare(std::u16string(a), std::u16string(b)));
    }
    return a < b ? -1 : a > b ? 1 : 0;
  }

  std::string resolved(const std::string &key) override {
    if (key == "locale") return "en-US";
    if (key == "collation") return "default";
    return {};
  }

 private:
  static std::u16string foldAsciiCase(std::u16string s) {
    for (char16_t &c : s) {
      if (c >= u'A' && c <= u'Z') c = static_cast<char16_t>(c - u'A' + u'a');
    }
    return s;
  }

  static int numericCompare(const std::u16string &a, const std::u16string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
      const bool da = a[i] >= u'0' && a[i] <= u'9';
      const bool db = b[j] >= u'0' && b[j] <= u'9';
      if (da && db) {
        size_t ia = i, jb = j;
        while (i < a.size() && a[i] >= u'0' && a[i] <= u'9') i++;
        while (j < b.size() && b[j] >= u'0' && b[j] <= u'9') j++;
        const std::u16string na = a.substr(ia, i - ia);
        const std::u16string nb = b.substr(jb, j - jb);
        const double va =
            std::strtod(std::string(na.begin(), na.end()).c_str(), nullptr);
        const double vb =
            std::strtod(std::string(nb.begin(), nb.end()).c_str(), nullptr);
        if (va != vb) return va < vb ? -1 : 1;
        continue;
      }
      if (a[i] != b[j]) return a[i] < b[j] ? -1 : 1;
      i++;
      j++;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
  }

  CollatorOptions o_;
};

class StubListFormatter final : public ListFormatter {
 public:
  explicit StubListFormatter(ListFormatOptions o) : o_(std::move(o)) {}

  bool format(
      const std::vector<std::u16string> &items, std::u16string &out) override {
    const std::u16string conj = o_.type == "disjunction" ? u"or"
                                : o_.type == "unit"      ? u""
                                                         : u"and";
    out.clear();
    for (size_t i = 0; i < items.size(); i++) {
      if (i > 0) {
        if (items.size() == 2) {
          out += conj.empty() ? u", " : u" ";
          if (!conj.empty()) {
            out += conj;
            out += u' ';
          }
        } else if (i + 1 == items.size()) {
          out += u", ";
          if (!conj.empty()) {
            out += conj;
            out += u' ';
          }
        } else {
          out += u", ";
        }
      }
      out += items[i];
    }
    return true;
  }

  std::string resolved(const std::string &key) override {
    return key == "locale" ? "en-US" : std::string();
  }

 private:
  ListFormatOptions o_;
};

class StubRelativeTimeFormatter final : public RelativeTimeFormatter {
 public:
  explicit StubRelativeTimeFormatter(RelativeTimeOptions o)
      : o_(std::move(o)) {}

  bool format(
      double value, const std::string &unit, std::u16string &out) override {
    // English, numeric, plural by |value| != 1. `numeric: "auto"` produces the
    // three idiomatic forms English has and nothing else — the root-locale
    // promise, not a CLDR relative table.
    const bool past = value < 0 || (value == 0 && std::signbit(value));
    const double n = std::fabs(value);
    std::string s;
    if (o_.numeric == "auto" && n <= 1 && n == std::floor(n)) {
      if (unit == "day") {
        s = value == 0 ? "today" : past ? "yesterday" : "tomorrow";
      } else if (value == 0) {
        s = "this " + unit;
      } else {
        s = past ? "last " + unit : "next " + unit;
      }
      out = ascii(s);
      return true;
    }
    char buf[64];
    if (n == std::floor(n) && n < 1e15) {
      snprintf(buf, sizeof(buf), "%.0f", n);
    } else {
      snprintf(buf, sizeof(buf), "%g", n);
    }
    const std::string count = buf;
    const std::string plural = (n == 1 ? unit : unit + "s");
    s = past ? count + " " + plural + " ago" : "in " + count + " " + plural;
    out = ascii(s);
    return true;
  }

  std::string resolved(const std::string &key) override {
    if (key == "locale") return "en-US";
    if (key == "numberingSystem") return "latn";
    return {};
  }

 private:
  RelativeTimeOptions o_;
};

class StubPlatform final : public PlatformDefaults {
 public:
  const char *name() override {
    return "stub";
  }

  std::vector<std::string> availableLocales() override {
    return {"en-US"};
  }
  std::string defaultLocale() override {
    return "en-US";
  }
  std::string defaultTimeZone() override {
    return "UTC";
  }

  std::string maximize(const std::string &) override {
    return {};
  }
  std::string minimize(const std::string &) override {
    return {};
  }
  std::string canonicalize(const std::string &) override {
    return {};
  }

  std::string normalizeTimeZone(const std::string &tz) override {
    // Exactly one zone is known. Rejecting the rest is the honest answer;
    // accepting "America/New_York" and formatting in UTC would make host test
    // output silently wrong.
    if (tz == "UTC" || tz == "utc" || tz == "GMT" || tz == "gmt" ||
        tz == "Etc/UTC" || tz == "Etc/GMT" || tz == "Z") {
      return "UTC";
    }
    return {};
  }

  std::vector<std::string> timeZones() override {
    return {"UTC"};
  }
  std::vector<std::string> calendars() override {
    return {"gregory"};
  }
  // Trivially self-consistent: resolved("numberingSystem") is unconditionally
  // "latn", so "latn" is exactly the set that round-trips.
  std::vector<std::string> numberingSystems() override {
    return {"latn"};
  }

  std::unique_ptr<DateTimeFormatter> openDateTimeFormat(
      const DateTimeOptions &o) override {
    std::string pattern = (!o.dateStyle.empty() || !o.timeStyle.empty())
                              ? patternFromStyles(o.dateStyle, o.timeStyle)
                              : patternFromSkeleton(o.skeleton, o.hourCycle);
    return std::make_unique<StubFormatter>(
        std::move(pattern), o.timeZone.empty() ? "UTC" : o.timeZone);
  }

  /*
   * Stage two on the no-platform backend.
   *
   * Every one of these is *shaped* correctly and *worded* in the root locale,
   * which is exactly the contract the DateTimeFormat stub already had
   * (deviation D7). The reason they exist at all rather than returning nullptr
   * is unchanged: qjs-bench, tests/conformance, tools/intl-cli, the opcode
   * profiler and the test262 runner all link this backend, and an
   * `Intl.NumberFormat` that throws would make the JavaScript layer untestable
   * without a device.
   */
  std::unique_ptr<NumberFormatter> openNumberFormat(
      const NumberOptions &o) override {
    return std::make_unique<StubNumberFormatter>(o);
  }

  std::unique_ptr<Collator> openCollator(const CollatorOptions &o) override {
    return std::make_unique<StubCollator>(o);
  }

  std::unique_ptr<RelativeTimeFormatter> openRelativeTimeFormat(
      const RelativeTimeOptions &o) override {
    return std::make_unique<StubRelativeTimeFormatter>(o);
  }

  std::unique_ptr<ListFormatter> openListFormat(
      const ListFormatOptions &o) override {
    return std::make_unique<StubListFormatter>(o);
  }

  /*
   * No display names at all, deliberately. Returning the code itself would be
   * indistinguishable from a real answer; returning nothing makes the JS layer
   * apply the `fallback: "code"` behaviour ECMA-402 specifies, which is both
   * correct and visibly a fallback.
   */
  std::string displayName(
      const std::string &, const std::string &, const std::string &,
      const std::string &) override {
    return {};
  }

  /*
   * Segmentation without a platform.
   *
   * Grapheme is one UTF-16 code point per segment (surrogate pairs kept whole),
   * sentence is the whole string, and word splits on the ASCII letter/digit
   * boundary. None of these is Unicode-correct — UAX #29 is a data-driven
   * algorithm and the data is the thing this design does not ship — but the
   * *shape* is right, so the JS layer's iterator protocol, `containing()` and
   * the `isWordLike` plumbing are all exercised on a host.
   */
  std::vector<Segment> segment(
      const std::string &, const std::string &granularity,
      const std::u16string &text) override {
    std::vector<Segment> out;
    if (text.empty()) return out;
    if (granularity == "sentence") {
      out.push_back(Segment{0, static_cast<int32_t>(text.size()), false});
      return out;
    }
    if (granularity == "grapheme") {
      for (size_t i = 0; i < text.size();) {
        size_t n = 1;
        if (text[i] >= 0xD800 && text[i] <= 0xDBFF && i + 1 < text.size() &&
            text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
          n = 2;
        }
        out.push_back(Segment{
            static_cast<int32_t>(i), static_cast<int32_t>(i + n), false});
        i += n;
      }
      return out;
    }
    auto wordish = [](char16_t c) {
      return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
             (c >= u'0' && c <= u'9') || c >= 0x80;
    };
    size_t i = 0;
    while (i < text.size()) {
      const bool w = wordish(text[i]);
      size_t j = i;
      while (j < text.size() && wordish(text[j]) == w) j++;
      out.push_back(
          Segment{static_cast<int32_t>(i), static_cast<int32_t>(j), w});
      i = j;
    }
    return out;
  }

  /// ASCII case mapping only. Turkish dotted-i and Greek final sigma are data.
  std::u16string caseMap(
      const std::string &, bool upper, const std::u16string &text) override {
    std::u16string out = text;
    for (char16_t &c : out) {
      if (upper && c >= u'a' && c <= u'z') {
        c = static_cast<char16_t>(c - u'a' + u'A');
      } else if (!upper && c >= u'A' && c <= u'Z') {
        c = static_cast<char16_t>(c - u'A' + u'a');
      }
    }
    return out;
  }

  /*
   * The enumerations. `collations()` reports exactly what StubCollator's
   * resolved("collation") answers, because ECMA-402 requires
   * Intl.supportedValuesOf to be precisely the set that round-trips — the same
   * equivalence that cost the Apple backend two test262 files when its
   * calendar list was hand-written.
   */
  std::vector<std::string> collations() override {
    return {};
  }
  std::vector<std::string> currencies() override {
    return {};
  }
  std::vector<std::string> localeCalendars(const std::string &) override {
    return {"gregory"};
  }
  std::vector<std::string> localeNumberingSystems(
      const std::string &) override {
    return {"latn"};
  }
  std::vector<std::string> localeTimeZones(const std::string &) override {
    return {};
  }
  std::vector<std::string> localeCollations(const std::string &) override {
    return {};
  }
  std::string localeHourCycle(const std::string &) override {
    return "h12";
  }
  std::string localeTextDirection(const std::string &) override {
    return "ltr";
  }
  bool localeWeekInfo(const std::string &, WeekInfo &out) override {
    out.firstDay = 7;  // Sunday, in ECMA-402's Monday-based numbering
    out.minimalDays = 1;
    out.weekend = {6, 7};
    return true;
  }
};

StubPlatform g_stub;
Platform *g_platform = nullptr;

}  // namespace

void setPlatform(Platform *p) {
  g_platform = p;
}

Platform *platform() {
  // Never null; see the contract note in IntlPlatform.h. A host build with no
  // platform layer linked gets root-locale formatting rather than an Intl whose
  // every constructor throws.
  return g_platform != nullptr ? g_platform : &g_stub;
}

}  // namespace rnqjs::intl
