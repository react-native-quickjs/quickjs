/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The shared ECMA-402 decomposition of a formatted number.
 *
 * PURPOSE
 *   Intl.NumberFormat.prototype.formatToParts, implemented once for every
 *   backend. Hermes does not have this on Apple at all — PlatformIntlApple.mm
 *   reaches `llvm_unreachable("formatToParts is unimplemented on Apple
 *   platforms")` — and the Objective-C surface offers nothing that produces
 *   field boundaries for a number.
 *
 * WHY THIS IS A PARSE AND NOT A GUESS
 *   Deviation D1 in docs/intl-platform-backed.md says a backend that cannot
 *   supply real boundaries must return one coarse literal rather than invent a
 *   split, because callers index into the result. This file is not an exception
 *   to that rule; it is a case where the boundaries *are* known.
 *
 *   A formatted number is, by construction, a sequence of runs drawn from a
 *   closed set: the ten digits of the resolved numbering system, and the
 *   locale's own group / decimal / minus / plus / percent / exponent / NaN /
 *   infinity symbols, all of which the backend hands over in NumberSymbols
 *   *after* the platform has chosen them. Everything else is currency text, unit
 *   text, a compact suffix, or a literal — and which of those it is follows from
 *   the option bag, not from the string.
 *
 *   The one thing this cannot do is invent a boundary the platform did not
 *   render. When a run matches nothing it becomes `literal`, which is coarse in
 *   the D1 sense rather than mislabelled in the dangerous sense.
 *
 * INVARIANTS
 *   - The emitted parts cover `text` in order, with no gaps and no overlaps.
 *     modules/intl/test/parts-coverage.js asserts exactly this by concatenating
 *     formatToParts() and comparing it with format().
 *   - No part is empty.
 *   - Adjacent runs of the same type are merged, so `1,234,567` is
 *     integer/group/integer/group/integer and never two adjacent integers.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   - No arithmetic and no rounding. It never looks at the value, only at the
 *     text the platform produced.
 *   - No locale knowledge. Every symbol it matches came from the backend.
 *   - No range formatting. formatRangeToParts needs a `source` field per part,
 *     which is a different output shape; see deviation D12's successor in the
 *     module README.
 */

#include <algorithm>

#include "IntlPlatform.h"

namespace rnqjs::intl {

namespace {

bool isSpace(char16_t c) {
  return c == 0x0020 ||  // SPACE
         c == 0x00A0 ||  // NO-BREAK SPACE
         c == 0x202F ||  // NARROW NO-BREAK SPACE
         c == 0x2009 ||  // THIN SPACE
         c == 0x2007 ||  // FIGURE SPACE
         c == 0x0009 || c == 0x000A;
}

/// Matches `needle` at `pos`. Empty needles never match, which is what keeps an
/// unset symbol from matching everywhere.
bool matchAt(const std::u16string &text, size_t pos, const std::u16string &n) {
  return !n.empty() && text.compare(pos, n.size(), n) == 0;
}

class Builder {
 public:
  explicit Builder(FormattedParts &out) : out_(out) {}

  void add(PartType type, size_t begin, size_t end) {
    if (end <= begin) return;
    if (!out_.parts.empty() && out_.parts.back().type == type &&
        out_.parts.back().end == static_cast<int32_t>(begin)) {
      out_.parts.back().end = static_cast<int32_t>(end);
      return;
    }
    out_.parts.push_back(
        Part{type, static_cast<int32_t>(begin), static_cast<int32_t>(end)});
  }

 private:
  FormattedParts &out_;
};

}  // namespace

void numberFormatToParts(
    const std::u16string &text, const NumberSymbols &s, const NumberOptions &o,
    FormattedParts &out) {
  out.text = text;
  out.parts.clear();
  Builder b(out);

  const bool unitStyle = o.style == "unit";
  const bool currencyStyle = o.style == "currency";
  const bool compact = o.notation == "compact";

  // Digit matching. A numbering system whose digits the backend could not name
  // (the algorithmic ones — roman, hebrew, jpanfin) leaves `digits` empty; ASCII
  // is then the only thing recognised as a digit and the rest of the numeral
  // falls through to `literal`, which is the honest coarse answer.
  auto digitLen = [&](size_t pos) -> size_t {
    if (pos >= text.size()) return 0;
    const char16_t c = text[pos];
    if (c >= u'0' && c <= u'9') return 1;
    for (const std::u16string &d : s.digits) {
      if (matchAt(text, pos, d)) return d.size();
    }
    return 0;
  };

  bool seenDecimal = false;   // digits after this are `fraction`
  bool seenExponent = false;  // digits after this are `exponentInteger`
  bool seenDigit = false;  // a leftover run before any digit is not `compact`

  size_t i = 0;
  size_t literalBegin = 0;  // start of the pending unclassified run
  auto flushLiteral = [&](size_t end) {
    // An unclassified run is a literal, *except* where the option bag says what
    // it must be. The split on whitespace matches what a full ICU
    // implementation emits: node reports [{integer:"5"},{literal:" "},
    // {unit:"m"}] rather than one combined run.
    size_t p = literalBegin;
    while (p < end) {
      const bool space = isSpace(text[p]);
      size_t q = p;
      while (q < end && isSpace(text[q]) == space) q++;
      PartType t = PartType::Literal;
      if (!space) {
        if (currencyStyle) {
          t = PartType::Currency;
        } else if (unitStyle) {
          t = PartType::Unit;
        } else if (compact && seenDigit) {
          t = PartType::Compact;
        }
      }
      b.add(t, p, q);
      p = q;
    }
    literalBegin = end;
  };

  while (i < text.size()) {
    // Currency text first: it can contain characters that would otherwise look
    // like symbols ("US$", "kr.", and in some locales a digit-bearing name).
    if (currencyStyle && matchAt(text, i, s.currency)) {
      flushLiteral(i);
      b.add(PartType::Currency, i, i + s.currency.size());
      i += s.currency.size();
      literalBegin = i;
      continue;
    }

    if (size_t n = digitLen(i)) {
      flushLiteral(i);
      size_t begin = i;
      while (size_t m = digitLen(i)) i += m;
      (void)n;
      b.add(
          seenExponent  ? PartType::ExponentInteger
          : seenDecimal ? PartType::Fraction
                        : PartType::Integer,
          begin, i);
      seenDigit = true;
      literalBegin = i;
      continue;
    }

    // Longest-match over the symbol set. `group` and `decimal` can be the same
    // character class in different locales and are never ambiguous *within* one
    // locale, but NaN/Infinity must be tried before minus so "-∞" splits.
    struct Cand {
      const std::u16string *text;
      PartType type;
      bool enabled;
    };
    const Cand cands[] = {
        {&s.nan, PartType::Nan, true},
        {&s.infinity, PartType::Infinity, true},
        {&s.exponential, PartType::ExponentSeparator,
         o.notation == "scientific" || o.notation == "engineering"},
        {&s.decimal, PartType::Decimal, true},
        {&s.group, PartType::Group, !seenDecimal && seenDigit},
        {&s.minusSign,
         seenExponent ? PartType::ExponentMinusSign : PartType::MinusSign,
         true},
        {&s.plusSign, PartType::PlusSign, true},
        {&s.percent, PartType::PercentSign, !unitStyle},
    };
    const Cand *best = nullptr;
    for (const Cand &c : cands) {
      if (!c.enabled || !matchAt(text, i, *c.text)) continue;
      if (best == nullptr || c.text->size() > best->text->size()) best = &c;
    }
    if (best != nullptr) {
      flushLiteral(i);
      b.add(best->type, i, i + best->text->size());
      if (best->type == PartType::Decimal) seenDecimal = true;
      if (best->type == PartType::ExponentSeparator) seenExponent = true;
      if (best->type == PartType::Nan || best->type == PartType::Infinity) {
        seenDigit = true;
      }
      i += best->text->size();
      literalBegin = i;
      continue;
    }

    i++;  // unclassified; flushLiteral will name the run
  }
  flushLiteral(text.size());
}

}  // namespace rnqjs::intl
