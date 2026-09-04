//
// Copyright (c) Ammar Ahmed.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//

//
// CLDR likely-subtags on Apple platforms.
//
// WHY THERE IS SWIFT IN THIS MODULE AT ALL
//   `maximize`/`minimize` are the one thing ECMA-402 needs from Apple that
//   Foundation's Objective-C surface does not expose. There is no
//   `addLikelySubtags` on NSLocale; the API is `Locale.Language.maximalIdentifier`
//   and `minimalIdentifier`, which exist only in the Swift overlay (iOS 16 /
//   macOS 13 and later).
//
//   The alternative is shipping the table ourselves, and that table is 181,013
//   bytes — 85% of everything @formatjs/intl-getcanonicallocales contains, and
//   three times the module's entire 60 KB JavaScript budget. So: Swift.
//
// WHY IT IS ISOLATED IN ITS OWN FILE, BEHIND WEAK SYMBOLS
//   Adding Swift to a CocoaPods pod changes the pod's linkage requirements and
//   is the largest build-integration risk in this plan. Keeping the Swift
//   surface to three C functions means the failure mode is bounded and
//   *observable*: ios/IntlPlatform.mm declares these weak and checks for null,
//   so a build without Swift links, runs, and degrades to "no opinion" on
//   likely subtags — which the JavaScript layer already handles, because the
//   stub backend behaves the same way. Nothing else in the module depends on
//   Swift.
//
// MEMORY
//   `@_cdecl` cannot return a Swift String, so each function returns a
//   `strdup`'d C string that the caller frees with `rnqjs_intl_free_swift`.
//   `strdup`/`free` rather than an autoreleased NSString because there is no
//   autorelease pool guarantee on the JS thread.
//
// WHAT THIS DELIBERATELY DOES NOT DO
//   It does not validate the tag. Structural validation is ECMA-402's job and
//   happens in js/intl.js before anything reaches here; `Locale.Language`
//   accepts malformed input silently, so validating here as well would only
//   create a second, differently-wrong opinion.
//

import Foundation

@_cdecl("rnqjs_intl_maximize_swift")
public func rnqjsIntlMaximizeSwift(_ tag: UnsafePointer<CChar>?) -> UnsafePointer<CChar>? {
  guard let tag = tag else { return nil }
  let input = String(cString: tag)
  if #available(iOS 16.0, macOS 13.0, tvOS 16.0, watchOS 9.0, *) {
    let language = Locale.Language(identifier: input)
    let maximal = language.maximalIdentifier
    if maximal.isEmpty { return nil }
    return UnsafePointer(strdup(maximal))
  }
  // Below iOS 16 there is no public API for this on Apple. Returning nil means
  // "no opinion"; the JavaScript layer then uses the tag unchanged, which is
  // deviation D2 degrading rather than an error.
  return nil
}

@_cdecl("rnqjs_intl_minimize_swift")
public func rnqjsIntlMinimizeSwift(_ tag: UnsafePointer<CChar>?) -> UnsafePointer<CChar>? {
  guard let tag = tag else { return nil }
  let input = String(cString: tag)
  if #available(iOS 16.0, macOS 13.0, tvOS 16.0, watchOS 9.0, *) {
    let language = Locale.Language(identifier: input)
    let minimal = language.minimalIdentifier
    if minimal.isEmpty { return nil }
    return UnsafePointer(strdup(minimal))
  }
  return nil
}

@_cdecl("rnqjs_intl_free_swift")
public func rnqjsIntlFreeSwift(_ s: UnsafePointer<CChar>?) {
  guard let s = s else { return }
  free(UnsafeMutableRawPointer(mutating: s))
}

//
// ---------------------------------------------------------------------------
// Stage two: the three things Foundation's Objective-C surface cannot answer
// ---------------------------------------------------------------------------
//
// The file's original header says "the Swift surface is three C functions" and
// frames Swift as a bounded risk. That framing was retired by measurement:
// docs/intl-completeness-map.md establishes that compact notation, the
// list-format type/width matrix and the collation enumeration exist *only* in
// the Swift overlay, so on Apple, Swift is part of the ECMA-402 surface rather
// than a corner of it.
//
// What has NOT changed is the failure mode. Every function here is reached
// through a weak symbol and every caller checks for null, so a build without
// Swift still links and still works — it loses compact notation (falling back
// to standard rendering), the disjunction and short/narrow list widths
// (falling back to conjunction/long), and reports no collations. Each of those
// degradations is enumerated as deviation D20 rather than being silent.
//
// bench/spikes/intl/apple-numberformat-probe.m is the measurement that decided
// which of NumberFormat's options needed to be here: roundingMode,
// roundingIncrement, significant digits and signDisplay all turned out to be
// reachable from NSNumberFormatter, so they are NOT here.
//

/// Compact notation. `.number.notation(.compactName)`, which has no
/// NSNumberFormatter equivalent — NSNumberFormatterStyle has no compact member.
@_cdecl("rnqjs_intl_compact_swift")
public func rnqjsIntlCompactSwift(
  _ localeId: UnsafePointer<CChar>?,
  _ value: Double,
  _ longStyle: Int32,
  _ minFrac: Int32,
  _ maxFrac: Int32,
  _ minSig: Int32,
  _ maxSig: Int32
) -> UnsafePointer<CChar>? {
  guard let localeId = localeId else { return nil }
  if #available(iOS 15.0, macOS 12.0, tvOS 15.0, watchOS 8.0, *) {
    let locale = Locale(identifier: String(cString: localeId))
    // `.compactName` is the only compact notation the overlay exposes, so
    // `compactDisplay: "long"` and `"short"` render identically on Apple —
    // deviation D20. Android's CompactDecimalFormat has both
    // CompactStyle.SHORT and CompactStyle.LONG, so this is a real cross-backend
    // difference and is recorded as one rather than papered over.
    _ = longStyle
    var style = FloatingPointFormatStyle<Double>(locale: locale)
      .notation(.compactName)
    // Significant digits win over fraction digits when both are present, which
    // is the same precedence js/intl.js applies when it resolves the digit
    // options; ECMA-402's roundingPriority "auto" is exactly that rule.
    if minSig > 0 || maxSig > 0 {
      let lo = minSig > 0 ? Int(minSig) : 1
      let hi = maxSig > 0 ? Int(maxSig) : 21
      style = style.precision(NumberFormatStyleConfiguration.Precision.significantDigits(lo...hi))
    } else if minFrac >= 0 || maxFrac >= 0 {
      let lo = minFrac >= 0 ? Int(minFrac) : 0
      let hi = maxFrac >= 0 ? Int(maxFrac) : 3
      style = style.precision(NumberFormatStyleConfiguration.Precision.fractionLength(lo...hi))
    }
    let text = style.format(value)
    if text.isEmpty { return nil }
    return UnsafePointer(strdup(text))
  }
  return nil
}

/// ListFormat's type/width matrix. NSListFormatter has neither: it is
/// conjunction, long, and nothing else.
///
/// Items arrive joined by U+001F UNIT SEPARATOR rather than as an array,
/// because `@_cdecl` cannot take a Swift array and marshalling an
/// NSArray across the boundary would mean the Objective-C side building one
/// only for this call.
@_cdecl("rnqjs_intl_list_swift")
public func rnqjsIntlListSwift(
  _ localeId: UnsafePointer<CChar>?,
  _ type: UnsafePointer<CChar>?,
  _ width: UnsafePointer<CChar>?,
  _ items: UnsafePointer<CChar>?
) -> UnsafePointer<CChar>? {
  guard let localeId = localeId, let type = type, let width = width,
        let items = items else { return nil }
  if #available(iOS 15.0, macOS 12.0, tvOS 15.0, watchOS 8.0, *) {
    let locale = Locale(identifier: String(cString: localeId))
    let list = String(cString: items).components(separatedBy: "\u{1f}")
    let t = String(cString: type)
    let w = String(cString: width)
    let listType: ListFormatStyle<StringStyle, [String]>.ListType =
      // ListFormatStyle.ListType has `.and` and `.or` and nothing else, so
      // ECMA-402's `type: "unit"` ("1 ft, 2 in") has no Swift expression and is
      // approximated by the narrow *width* of `.and` below. Deviation D20.
      t == "disjunction" ? .or : .and
    let listWidth: ListFormatStyle<StringStyle, [String]>.Width =
      w == "short" ? .short
        : (w == "narrow" || t == "unit" ? .narrow : .standard)
    let text = list.formatted(
      .list(type: listType, width: listWidth).locale(locale))
    if text.isEmpty { return nil }
    return UnsafePointer(strdup(text))
  }
  return nil
}

/// The collation enumeration, for Intl.supportedValuesOf("collation") and
/// Intl.Locale.prototype.getCollations. Foundation has no Objective-C
/// equivalent of `Locale.Collation.availableCollations`.
///
/// Returned U+001F-joined for the same reason as above.
@_cdecl("rnqjs_intl_collations_swift")
public func rnqjsIntlCollationsSwift() -> UnsafePointer<CChar>? {
  if #available(iOS 16.0, macOS 13.0, tvOS 16.0, watchOS 9.0, *) {
    let ids = Locale.Collation.availableCollations.map { $0.identifier }
    if ids.isEmpty { return nil }
    return UnsafePointer(strdup(ids.joined(separator: "\u{1f}")))
  }
  return nil
}
