#!/usr/bin/env python3
"""Report the minimum Android API level of every android.icu API this module
uses or is considering using.

WHY
    React Native's minSdk is 24. `android.icu` as a whole has been public since
    24, which is the sentence everyone repeats — and it is not enough: several
    of the classes and methods ECMA-402 needs arrived much later.
    `Intl.ListFormat`'s type/width selection, for instance, is API **33**, and
    the class itself is API **26**, not 24. Getting that wrong is a
    NoSuchMethodError on a real device, months after the code was written.

SOURCE
    <sdk>/platforms/android-NN/data/api-versions.xml, which ships with the
    Android SDK platform and carries a `since` attribute per class, per method
    and per field. This is the SDK's own record, not documentation scraping.

USAGE
    python3 modules/intl/scripts/android-api-levels.py [path/to/api-versions.xml]

    Exit 1 if any API listed under REQUIRED is above MIN_SDK, so this can be a
    build gate once the corresponding backend lands.
"""
import os
import sys
import glob
import xml.etree.ElementTree as ET

MIN_SDK = 24

# (class, method-or-None, what it is for). None means "the class itself".
REQUIRED = [
    ("android/icu/util/ULocale", "addLikelySubtags(Landroid/icu/util/ULocale;)Landroid/icu/util/ULocale;", "Intl.Locale.maximize"),
    ("android/icu/util/ULocale", "minimizeSubtags(Landroid/icu/util/ULocale;)Landroid/icu/util/ULocale;", "Intl.Locale.minimize"),
    ("android/icu/text/DateTimePatternGenerator", "getBestPattern(Ljava/lang/String;)Ljava/lang/String;", "DateTimeFormat skeletons"),
    ("android/icu/text/SimpleDateFormat", None, "DateTimeFormat"),
    ("android/icu/text/NumberingSystem", "getAvailableNames()[Ljava/lang/String;", "supportedValuesOf(numberingSystem)"),
    ("android/icu/util/Calendar", "getKeywordValuesForLocale(Ljava/lang/String;Landroid/icu/util/ULocale;Z)[Ljava/lang/String;", "supportedValuesOf(calendar)"),
    ("android/icu/util/Currency", "getAvailableCurrencies()Ljava/util/Set;", "supportedValuesOf(currency)"),
    ("android/icu/text/Collator", "getKeywordValues(Ljava/lang/String;)[Ljava/lang/String;", "supportedValuesOf(collation)"),
]

# Candidates for the surface that is not built yet. Not gated; reported so the
# design can choose a mechanism with its API level in view.
CANDIDATE = [
    ("android/icu/text/DecimalFormat", None, "NumberFormat"),
    ("android/icu/text/CompactDecimalFormat", None, "NumberFormat notation:compact"),
    ("android/icu/text/MeasureFormat", None, "NumberFormat style:unit"),
    ("android/icu/number/NumberFormatter", None, "NumberFormat, the modern API"),
    ("android/icu/text/RuleBasedCollator", None, "Intl.Collator"),
    ("android/icu/text/PluralRules", "select(D)Ljava/lang/String;", "Intl.PluralRules"),
    ("android/icu/text/RelativeDateTimeFormatter", "format(DLandroid/icu/text/RelativeDateTimeFormatter$Direction;Landroid/icu/text/RelativeDateTimeFormatter$RelativeUnit;)Ljava/lang/String;", "Intl.RelativeTimeFormat"),
    ("android/icu/text/RelativeDateTimeFormatter", "formatNumeric(DLandroid/icu/text/RelativeDateTimeFormatter$RelativeDateTimeUnit;)Ljava/lang/String;", "RelativeTimeFormat numeric:always"),
    ("android/icu/text/RelativeDateTimeFormatter", "formatToValue(DLandroid/icu/text/RelativeDateTimeFormatter$Direction;Landroid/icu/text/RelativeDateTimeFormatter$RelativeUnit;)Landroid/icu/text/RelativeDateTimeFormatter$FormattedRelativeDateTime;", "RelativeTimeFormat.formatToParts"),
    ("android/icu/text/ListFormatter", None, "Intl.ListFormat"),
    ("android/icu/text/ListFormatter", "getInstance(Landroid/icu/util/ULocale;Landroid/icu/text/ListFormatter$Type;Landroid/icu/text/ListFormatter$Width;)Landroid/icu/text/ListFormatter;", "ListFormat type/style"),
    ("android/icu/text/ListFormatter", "formatToValue(Ljava/util/Collection;)Landroid/icu/text/ListFormatter$FormattedList;", "ListFormat.formatToParts"),
    ("android/icu/text/LocaleDisplayNames", "getInstance(Landroid/icu/util/ULocale;)Landroid/icu/text/LocaleDisplayNames;", "Intl.DisplayNames"),
    ("android/icu/text/BreakIterator", "getWordInstance(Landroid/icu/util/ULocale;)Landroid/icu/text/BreakIterator;", "Intl.Segmenter"),
    ("android/icu/text/BreakIterator", "getRuleStatus()I", "Segmenter isWordLike"),
    ("android/icu/text/DateIntervalFormat", None, "DateTimeFormat.formatRange"),
]


def find_api_versions():
    for base in (os.environ.get("ANDROID_HOME"),
                 os.path.expanduser("~/Library/Android/sdk"),
                 os.path.expanduser("~/Android/Sdk")):
        if not base:
            continue
        hits = sorted(glob.glob(os.path.join(base, "platforms", "android-*", "data", "api-versions.xml")))
        if hits:
            return hits[-1]
    return None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else find_api_versions()
    if not path or not os.path.exists(path):
        sys.exit("no api-versions.xml found; set ANDROID_HOME or pass a path")
    print(f"source: {path}\nminSdk: {MIN_SDK}\n")
    classes = {c.get("name"): c for c in ET.parse(path).getroot().findall("class")}

    def since(cls, method):
        c = classes.get(cls)
        if c is None:
            return None
        if method is None:
            return int(c.get("since", 1))
        for m in c.findall("method"):
            if m.get("name") == method:
                return int(m.get("since", c.get("since", 1)))
        return None

    bad = 0
    for label, rows, gate in (("REQUIRED", REQUIRED, True), ("CANDIDATE", CANDIDATE, False)):
        print(f"--- {label} ---")
        for cls, method, why in rows:
            s = since(cls, method)
            name = cls.split("/")[-1] + ("" if method is None else "." + method.split("(")[0])
            if s is None:
                print(f"  {'MISSING':>7}  {name:<52} {why}")
                bad += gate
            else:
                flag = "" if s <= MIN_SDK else "  <-- ABOVE minSdk"
                print(f"  API {s:>3}  {name:<52} {why}{flag}")
                if gate and s > MIN_SDK:
                    bad += 1
        print()
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
