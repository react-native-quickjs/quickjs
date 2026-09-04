package com.intl

import android.content.Context
import com.facebook.soloader.SoLoader
import android.icu.lang.UCharacter
import android.icu.text.BreakIterator
import android.icu.text.CompactDecimalFormat
import android.icu.text.DateFormat
import android.icu.text.DateFormatSymbols
import android.icu.text.DateTimePatternGenerator
import android.icu.text.DecimalFormat
import android.icu.text.DecimalFormatSymbols
import android.icu.text.ListFormatter
import android.icu.text.LocaleDisplayNames
import android.icu.text.MeasureFormat
import android.icu.text.NumberFormat as IcuNumberFormat
import android.icu.text.NumberingSystem
import android.icu.text.PluralRules
import android.icu.text.RelativeDateTimeFormatter
import android.icu.text.RuleBasedCollator
import android.icu.text.SimpleDateFormat
import android.icu.util.Calendar
import android.icu.util.Currency
import android.icu.util.Measure
import android.icu.util.MeasureUnit
import android.icu.util.TimeZone
import android.icu.util.ULocale
import java.math.BigDecimal
import java.math.RoundingMode
import java.text.AttributedCharacterIterator
import java.util.Date
import java.util.concurrent.atomic.AtomicLong

/**
 * The Android half of the platform seam for react-native-quickjs-intl.
 *
 * ## Why the ICU work is here rather than in C++
 *
 * `android.icu` is a Java API. Reaching it from C++ means one JNI call per ICU
 * call plus reflection for every class, which is both slower and far more code
 * than doing the work on this side and returning a result. So this object is
 * the *coarse* end of the seam: one call per formatter construction, one call
 * per format, and nothing else on a hot path.
 *
 * That is deliberately unlike Hermes, whose Android bridge marshals a
 * `java.util.HashMap` of options and an `ArrayList` of locales across JNI for
 * every construction and calls into Java for every `format()`
 * (`PlatformIntlAndroid.cpp:47-90`). Here the options cross once, as eight
 * strings, and a `format` is one crossing carrying a `double`.
 *
 * ## API level
 *
 * Everything used here is `android.icu`, public since **API 24**, well below
 * React Native's minSdk. Nothing links ICU4C and nothing ships CLDR data —
 * the whole database is already in the OS. `docs/intl-platform-backed.md`
 * measures the alternative at 8.28 MB of bundle and 415 ms of startup for 15
 * locales.
 *
 * ## Formatter lifetime
 *
 * Formatters are held in a map keyed by a monotonically increasing `Long`,
 * because a JNI-held pointer to a Java object would have to be a global
 * reference and global references are exactly what leaks. The C++ side holds
 * only the `Long`, and the QuickJS finalizer on the JavaScript formatter object
 * calls [dtfClose]. Lifetime therefore follows the engine's refcounting, and an
 * `android.icu.text.DateFormat` is released when its JavaScript owner is.
 *
 * A leaked id would be a leaked formatter, so [dtfClose] is called from a
 * finalizer that QuickJS runs deterministically rather than from a Java
 * finalizer, which would not be deterministic at all.
 *
 * ## What this deliberately does not do
 *
 * No ECMA-402 semantics. It never sees an option name, a locale negotiation, or
 * a `resolvedOptions` object. Those live once, in `js/intl.js`, and are shared
 * with Apple.
 */
object IntlPlatform {
  private var appContext: Context? = null

  init {
    // The native half of this module lives in the engine's .so.
    SoLoader.loadLibrary("quickjsinstancejni")
  }

  @JvmStatic
  fun attach(context: Context) {
    appContext = context
    nativeAttach()
  }

  private external fun nativeAttach()

  // ---------------------------------------------------------------- locales

  @JvmStatic
  fun availableLocales(): Array<String> =
    ULocale.getAvailableLocales().map { it.toLanguageTag() }.toTypedArray()

  @JvmStatic
  fun defaultLocale(): String =
    ULocale.getDefault().toLanguageTag().ifEmpty { "en-US" }

  @JvmStatic
  fun defaultTimeZone(): String = TimeZone.getDefault().id.ifEmpty { "UTC" }

  /**
   * CLDR likely subtags.
   *
   * This is why `likelySubtags` is not in the JavaScript layer: that table is
   * 181,013 bytes, three times the module's entire 60 KB JS budget, and 85% of
   * what `@formatjs/intl-getcanonicallocales` ships.
   */
  @JvmStatic
  fun maximize(tag: String): String = try {
    ULocale.addLikelySubtags(ULocale.forLanguageTag(tag)).toLanguageTag()
  } catch (e: Exception) {
    ""
  }

  @JvmStatic
  fun minimize(tag: String): String = try {
    ULocale.minimizeSubtags(ULocale.forLanguageTag(tag)).toLanguageTag()
  } catch (e: Exception) {
    ""
  }

  /**
   * Platform canonicalization of an already structurally-valid tag.
   *
   * Structural validation happens in JavaScript and is deliberately not
   * delegated: `ULocale.forLanguageTag` accepts malformed input silently, and
   * ECMA-402 requires a RangeError. This exists only for legacy mappings the
   * alias tables in `js/intl.js` do not carry.
   */
  @JvmStatic
  fun canonicalize(tag: String): String = try {
    ULocale.forLanguageTag(tag).toLanguageTag()
  } catch (e: Exception) {
    ""
  }

  /**
   * Validates and canonicalizes an IANA timezone id.
   *
   * Returns "" for an unknown zone, which the JavaScript layer turns into the
   * RangeError ECMA-402 requires. `TimeZone.getTimeZone` returns GMT for an
   * unknown id rather than failing, so the id has to be checked against the
   * known list first — otherwise every typo would silently format in GMT.
   */
  @JvmStatic
  fun normalizeTimeZone(tz: String): String {
    val ids = TimeZone.getAvailableIDs()
    for (id in ids) {
      if (id.equals(tz, ignoreCase = true)) {
        return TimeZone.getCanonicalID(id) ?: id
      }
    }
    // Link names ("US/Pacific") resolve through getCanonicalID even when the
    // exact-case lookup above misses.
    val canonical = TimeZone.getCanonicalID(tz)
    return if (canonical != null && canonical.isNotEmpty() && canonical != "Etc/Unknown") {
      canonical
    } else {
      ""
    }
  }

  @JvmStatic
  fun timeZones(): Array<String> = TimeZone.getAvailableIDs()

  /**
   * The calendars this platform honours, **probed rather than listed**.
   *
   * ECMA-402 requires `Intl.supportedValuesOf("calendar")` to be exactly the
   * set that round-trips through `DateTimeFormat.resolvedOptions().calendar`,
   * in both directions. A hand-written list satisfies that only by accident,
   * and the Apple backend's did not: it omitted four calendars NSCalendar
   * accepts and scored *below* the do-nothing default backend on test262's
   * `Intl` area because of it. So both backends now ask.
   *
   * `Calendar.getKeywordValuesForLocale` is ICU's own answer to this question
   * and needs no candidate list at all — which is one place Android is
   * strictly better served than Apple, where no equivalent API exists.
   * The values it returns are *legacy* ICU type names ("gregorian"), so they
   * go through [calendarKeywordOf] to become the BCP-47 `ca` values ECMA-402
   * reports ("gregory").
   */
  @JvmStatic
  fun calendars(): Array<String> = try {
    val seen = LinkedHashSet<String>()
    seen.add("gregory") // required by the spec, and always available
    for (type in Calendar.getKeywordValuesForLocale("calendar", ULocale.ENGLISH, false)) {
      val keyword = calendarKeywordOf(type)
      // Round-trip check: only report what dtfResolved will report back.
      val uloc = ULocale.Builder().setLanguageTag("en")
        .setUnicodeLocaleKeyword("ca", keyword).build()
      if (calendarKeywordOf(Calendar.getInstance(uloc).type) == keyword) seen.add(keyword)
    }
    seen.toTypedArray()
  } catch (e: Exception) {
    arrayOf("gregory")
  }

  /**
   * The numbering systems this platform honours.
   *
   * `NumberingSystem.getAvailableNames()` is the enumeration Foundation does
   * not have; the round-trip filter is still applied, because the reported
   * value comes from `NumberingSystem.getInstance(ULocale)` and an identifier
   * ICU lists but does not resolve would break the equivalence test262 checks.
   */
  @JvmStatic
  fun numberingSystems(): Array<String> = try {
    val seen = LinkedHashSet<String>()
    seen.add("latn")
    for (name in NumberingSystem.getAvailableNames()) {
      val uloc = ULocale.Builder().setLanguageTag("en")
        .setUnicodeLocaleKeyword("nu", name).build()
      if (NumberingSystem.getInstance(uloc)?.name == name) seen.add(name)
    }
    seen.toTypedArray()
  } catch (e: Exception) {
    arrayOf("latn")
  }

  /**
   * ICU legacy calendar type name -> the BCP-47 `ca` keyword value ECMA-402
   * reports. `Calendar.getType()` answers "gregorian" where ECMA-402 says
   * "gregory" and "ethiopic-amete-alem" where it says "ethioaa"; everything
   * else is spelled the same in both.
   */
  private fun calendarKeywordOf(type: String): String = when (type) {
    "gregorian" -> "gregory"
    "ethiopic-amete-alem" -> "ethioaa"
    "islamic-civil", "islamicc" -> "islamic-civil"
    else -> type
  }

  // ------------------------------------------------------------- formatters

  private class Entry(
    val format: SimpleDateFormat,
    val locale: ULocale,
    val timeZone: String,
    val pattern: String
  )

  private val nextId = AtomicLong(1)
  private val formatters = HashMap<Long, Entry>()

  /**
   * Opens a formatter. Returns 0 on failure.
   *
   * Eight strings rather than an options map, because a map costs a
   * `HashMap` allocation plus one JNI crossing per entry, and this is the only
   * place the options ever cross.
   */
  @JvmStatic
  fun dtfOpen(
    locale: String,
    calendar: String,
    numberingSystem: String,
    timeZone: String,
    hourCycle: String,
    skeleton: String,
    dateStyle: String,
    timeStyle: String
  ): Long {
    try {
      // Extension keywords go on the ULocale, so ICU picks the calendar and
      // numbering system rather than us patching the output afterwards.
      var builder = ULocale.Builder().setLanguageTag(locale)
      if (calendar.isNotEmpty()) builder = builder.setUnicodeLocaleKeyword("ca", calendar)
      if (numberingSystem.isNotEmpty()) builder = builder.setUnicodeLocaleKeyword("nu", numberingSystem)
      if (hourCycle.isNotEmpty()) builder = builder.setUnicodeLocaleKeyword("hc", hourCycle)
      val uloc = builder.build()

      val pattern: String = if (dateStyle.isNotEmpty() || timeStyle.isNotEmpty()) {
        val df = DateFormat.getDateTimeInstance(
          styleOf(dateStyle), styleOf(timeStyle), uloc
        )
        (df as? SimpleDateFormat)?.toPattern() ?: return 0
      } else {
        // The same mechanism as Apple's
        // -[NSDateFormatter setLocalizedDateFormatFromTemplate:]: hand the
        // platform a CLDR skeleton and let its own pattern generator choose the
        // locale's best pattern. That symmetry is why the ECMA-402 component
        // bag is translated into a skeleton exactly once, in JavaScript.
        DateTimePatternGenerator.getInstance(uloc).getBestPattern(skeleton)
      }
      if (pattern.isEmpty()) return 0

      val fmt = SimpleDateFormat(pattern, uloc)
      val zoneId = timeZone.ifEmpty { TimeZone.getDefault().id }
      fmt.timeZone = TimeZone.getTimeZone(zoneId)
      if (calendar.isNotEmpty()) {
        fmt.calendar = Calendar.getInstance(fmt.timeZone, uloc)
      }

      val id = nextId.getAndIncrement()
      formatters[id] = Entry(fmt, uloc, fmt.timeZone.id, pattern)
      return id
    } catch (e: Exception) {
      // A locale or calendar ICU does not know is not a program error; the
      // JavaScript layer falls back rather than throwing.
      return 0
    }
  }

  @JvmStatic
  fun dtfClose(id: Long) {
    formatters.remove(id)
  }

  @JvmStatic
  fun dtfFormat(id: Long, epochMs: Double): String? {
    val e = formatters[id] ?: return null
    return try {
      e.format.format(Date(epochMs.toLong()))
    } catch (ex: Exception) {
      null
    }
  }

  /**
   * formatToParts, flattened to `[type, value, type, value, ...]`.
   *
   * A flat string array rather than an array of objects: building objects here
   * would mean a class reference, a constructor id and two field writes per
   * part on the C++ side, for a result the C++ immediately takes apart again.
   *
   * The boundaries are **real**, not reconstructed.
   * `DateFormat.formatToCharacterIterator` is ICU's own decomposition and
   * reports a `DateFormat.Field` per run. This is the same source Hermes uses,
   * and it is why Hermes ships `NumberFormat.formatToParts` on Android and
   * aborts on Apple.
   */
  @JvmStatic
  fun dtfFormatToParts(id: Long, epochMs: Double): Array<String>? {
    val e = formatters[id] ?: return null
    return try {
      val it = e.format.formatToCharacterIterator(Date(epochMs.toLong()))
      val out = ArrayList<String>()
      val sb = StringBuilder()
      var index = it.beginIndex
      while (index < it.endIndex) {
        it.setIndex(index)
        val end = it.runLimit
        val attrs = it.attributes
        val field = attrs.keys.firstOrNull { it is DateFormat.Field }
        sb.setLength(0)
        var i = index
        it.setIndex(i)
        while (i < end) {
          sb.append(it.current())
          it.next()
          i++
        }
        out.add(partTypeOf(field))
        out.add(sb.toString())
        index = end
      }
      out.toTypedArray()
    } catch (ex: Exception) {
      null
    }
  }

  @JvmStatic
  fun dtfResolved(id: Long, key: String): String {
    val e = formatters[id] ?: return ""
    return when (key) {
      "locale" -> e.locale.toLanguageTag()
      "calendar" -> calendarKeywordOf(e.format.calendar?.type ?: "gregorian")
      "numberingSystem" -> NumberingSystem.getInstance(e.locale)?.name ?: "latn"
      "timeZone" -> e.timeZone
      "hourCycle" -> hourCycleOfPattern(e.pattern)
      "pattern" -> e.pattern
      else -> ""
    }
  }

  // ----------------------------------------------------------------- helpers

  private fun styleOf(s: String): Int = when (s) {
    "full" -> DateFormat.FULL
    "long" -> DateFormat.LONG
    "medium" -> DateFormat.MEDIUM
    "short" -> DateFormat.SHORT
    else -> DateFormat.NONE
  }

  /**
   * The resolved hour cycle is a property of the pattern the platform actually
   * chose, not of what was requested. Reading it back is the whole reason
   * `resolvedOptions` consults the backend rather than echoing the input.
   */
  private fun hourCycleOfPattern(pattern: String): String {
    var i = 0
    while (i < pattern.length) {
      val c = pattern[i]
      if (c == '\'') {
        i++
        while (i < pattern.length && pattern[i] != '\'') i++
        i++
        continue
      }
      when (c) {
        'h' -> return "h12"
        'H' -> return "h23"
        'K' -> return "h11"
        'k' -> return "h24"
      }
      i++
    }
    return ""
  }

  /**
   * ICU field -> the ECMA-402 part type name the JavaScript layer expects.
   *
   * `android.icu.text.DateFormat.Field` is a *subset* of ICU4J's: it has no
   * `RELATED_YEAR` and no `YEAR_NAME` constant, which is why those two are
   * matched by name instead. MEASURED — referencing them by constant is a
   * compile error against `android.jar` (API 35), and this file had never been
   * compiled before, so it was one.
   *
   * `AM_PM_MIDNIGHT_NOON` and `FLEXIBLE_DAY_PERIOD` both map to `dayPeriod`.
   * They are what ICU emits for the "12 in the morning" flexible day periods
   * that `docs/intl-platform-backed.md` records as six failing `intl402` tests
   * on the Apple side.
   */
  private fun partTypeOf(field: AttributedCharacterIterator.Attribute?): String =
    when (field) {
      null -> "literal"
      DateFormat.Field.ERA -> "era"
      DateFormat.Field.YEAR, DateFormat.Field.EXTENDED_YEAR,
      DateFormat.Field.YEAR_WOY -> "year"
      DateFormat.Field.MONTH -> "month"
      DateFormat.Field.DAY_OF_MONTH -> "day"
      DateFormat.Field.DAY_OF_WEEK, DateFormat.Field.DOW_LOCAL -> "weekday"
      DateFormat.Field.AM_PM, DateFormat.Field.AM_PM_MIDNIGHT_NOON,
      DateFormat.Field.FLEXIBLE_DAY_PERIOD -> "dayPeriod"
      DateFormat.Field.HOUR0, DateFormat.Field.HOUR1,
      DateFormat.Field.HOUR_OF_DAY0, DateFormat.Field.HOUR_OF_DAY1 -> "hour"
      DateFormat.Field.MINUTE -> "minute"
      DateFormat.Field.SECOND -> "second"
      DateFormat.Field.MILLISECOND -> "fractionalSecond"
      DateFormat.Field.TIME_ZONE -> "timeZoneName"
      else -> when (field.toString()) {
        // The two constants android.icu does not expose. Matching on the
        // attribute's name is stable: AttributedCharacterIterator.Attribute
        // names are the CLDR field names and are part of ICU's serialised form.
        "related year" -> "relatedYear"
        "year name" -> "yearName"
        else -> "unknown"
      }
    }


  // ==========================================================================
  // Stage two: NumberFormat, Collator, RelativeTimeFormat, ListFormat,
  // DisplayNames, Segmenter, case mapping and the locale enumerations.
  //
  // ## The shape of these entry points, and why it is not one argument per
  // ## option
  //
  // `NumberOptions` has twenty fields. Twenty JNI parameters is a signature
  // where inserting a field in the middle silently shifts every later one, and
  // JNI gives no diagnostic for that at all — it is the same class of bug as
  // registering a QuickJS setter under the wrong `JSCFunctionEnum`, which this
  // module has already shipped once.
  //
  // So an option bag crosses as a `String[]` in a documented field order, and
  // both sides pin the order with a named index constant.
  //
  // ## API levels — checked, not assumed
  //
  // `modules/intl/scripts/android-api-levels.py` reads the SDK's own
  // `api-versions.xml`. The members used below and their minimum levels:
  //
  //   DecimalFormat, CompactDecimalFormat, MeasureFormat, RuleBasedCollator,
  //   LocaleDisplayNames, BreakIterator, PluralRules, Currency, UCharacter   24
  //   ListFormatter (the class)                                              26
  //   ListFormatter.getInstance(ULocale, Type, Width)                        33
  //   RelativeDateTimeFormatter.formatNumeric                                28
  //
  // React Native's minSdk is 24, so the last three are guarded at runtime with
  // Build.VERSION.SDK_INT and each has a documented fallback. That is the whole
  // reason this file does not reach for `android.icu.number.NumberFormatter`,
  // which is API 30 and would either raise the module's minSdk or require two
  // number backends — the two-implementations-diverge failure this design
  // exists to avoid.

  // Field order of the NumberFormat option bag. Must match
  // kNumberOptionOrder in android/src/main/cpp/IntlPlatform.cpp.
  private const val NF_LOCALE = 0
  private const val NF_NUMBERING = 1
  private const val NF_STYLE = 2
  private const val NF_CURRENCY = 3
  private const val NF_CURRENCY_DISPLAY = 4
  private const val NF_CURRENCY_SIGN = 5
  private const val NF_UNIT = 6
  private const val NF_UNIT_DISPLAY = 7
  private const val NF_NOTATION = 8
  private const val NF_COMPACT_DISPLAY = 9
  private const val NF_SIGN_DISPLAY = 10
  private const val NF_ROUNDING_MODE = 11
  private const val NF_USE_GROUPING = 12
  private const val NF_MIN_INT = 13
  private const val NF_MIN_FRAC = 14
  private const val NF_MAX_FRAC = 15
  private const val NF_MIN_SIG = 16
  private const val NF_MAX_SIG = 17
  private const val NF_ROUNDING_INCREMENT = 18
  private const val NF_FIELD_COUNT = 19

  private class NumberHandle(
    val format: IcuNumberFormat,
    val opts: Array<String>,
    val measure: MeasureFormat?,
    val unit: MeasureUnit?
  ) {
    // Captured on first use rather than at construction, because applying a
    // sign strategy overwrites them and the originals must survive.
    var basePositivePrefix: String? = null
    var baseNegativePrefix: String? = null
  }

  private val numberFormats = HashMap<Long, NumberHandle>()
  private val collators = HashMap<Long, RuleBasedCollator>()
  private val relativeFormats = HashMap<Long, RelativeDateTimeFormatter>()
  private val listFormats = HashMap<Long, Any>()
  private val nextHandleId = AtomicLong(1)

  private fun opt(o: Array<String>, i: Int): String = if (i < o.size) o[i] else ""
  private fun optInt(o: Array<String>, i: Int, fallback: Int): Int =
    opt(o, i).toIntOrNull() ?: fallback

  @JvmStatic
  fun nfOpen(o: Array<String>): Long {
    if (o.size < NF_FIELD_COUNT) return 0L
    return try {
      val locale = ULocale.forLanguageTag(opt(o, NF_LOCALE)).let {
        if (opt(o, NF_NUMBERING).isEmpty()) it
        else ULocale.Builder().setLocale(it)
          .setUnicodeLocaleKeyword("nu", opt(o, NF_NUMBERING)).build()
      }
      val style = opt(o, NF_STYLE)
      val notation = opt(o, NF_NOTATION)
      val nf: IcuNumberFormat = when {
        notation == "compact" -> CompactDecimalFormat.getInstance(
          locale,
          if (opt(o, NF_COMPACT_DISPLAY) == "long") CompactDecimalFormat.CompactStyle.LONG
          else CompactDecimalFormat.CompactStyle.SHORT)
        notation == "scientific" || notation == "engineering" ->
          IcuNumberFormat.getScientificInstance(locale)
        style == "currency" && opt(o, NF_CURRENCY_SIGN) == "accounting" ->
          IcuNumberFormat.getInstance(locale, IcuNumberFormat.ACCOUNTINGCURRENCYSTYLE)
        style == "currency" && opt(o, NF_CURRENCY_DISPLAY) == "code" ->
          IcuNumberFormat.getInstance(locale, IcuNumberFormat.ISOCURRENCYSTYLE)
        style == "currency" && opt(o, NF_CURRENCY_DISPLAY) == "name" ->
          IcuNumberFormat.getInstance(locale, IcuNumberFormat.PLURALCURRENCYSTYLE)
        style == "currency" ->
          IcuNumberFormat.getInstance(locale, IcuNumberFormat.CURRENCYSTYLE)
        // `percent` deliberately uses the DECIMAL instance: js/intl.js has
        // already multiplied by 100 (the decimalString contract in
        // cpp/IntlPlatform.h), and PERCENTSTYLE would scale a second time.
        else -> IcuNumberFormat.getInstance(locale, IcuNumberFormat.NUMBERSTYLE)
      }
      if (style == "currency") {
        nf.currency = Currency.getInstance(opt(o, NF_CURRENCY))
      }
      if (style == "percent" && nf is DecimalFormat) {
        val pct = DecimalFormatSymbols.getInstance(locale).percent.toString()
        nf.positiveSuffix = pct
        nf.negativeSuffix = pct
      }
      if (notation == "engineering" && nf is DecimalFormat) {
        // ICU expresses engineering notation as "##0.###E0": one to three
        // integer digits in the mantissa. There is no property for it.
        val frac = optInt(o, NF_MAX_FRAC, 3).coerceAtLeast(0)
        nf.applyPattern("##0." + "#".repeat(if (frac > 0) frac else 3) + "E0")
      }
      nf.isGroupingUsed = opt(o, NF_USE_GROUPING).isNotEmpty()
      nf.minimumIntegerDigits = optInt(o, NF_MIN_INT, 1)
      val minSig = optInt(o, NF_MIN_SIG, -1)
      val maxSig = optInt(o, NF_MAX_SIG, -1)
      if ((minSig > 0 || maxSig > 0) && nf is DecimalFormat) {
        nf.setSignificantDigitsUsed(true)
        if (minSig > 0) nf.minimumSignificantDigits = minSig
        if (maxSig > 0) nf.maximumSignificantDigits = maxSig
      } else {
        val minFrac = optInt(o, NF_MIN_FRAC, -1)
        val maxFrac = optInt(o, NF_MAX_FRAC, -1)
        if (minFrac >= 0) nf.minimumFractionDigits = minFrac
        if (maxFrac >= 0) nf.maximumFractionDigits = maxFrac
      }
      nf.roundingMode = icuRoundingMode(opt(o, NF_ROUNDING_MODE))
      val inc = optInt(o, NF_ROUNDING_INCREMENT, 1)
      if (inc != 1 && nf is DecimalFormat) {
        nf.roundingIncrement =
          BigDecimal(inc).movePointLeft(optInt(o, NF_MAX_FRAC, 0))
      }

      var measure: MeasureFormat? = null
      var unit: MeasureUnit? = null
      if (style == "unit") {
        unit = measureUnitOf(opt(o, NF_UNIT))
        if (unit != null) {
          measure = MeasureFormat.getInstance(
            locale,
            when (opt(o, NF_UNIT_DISPLAY)) {
              "long" -> MeasureFormat.FormatWidth.WIDE
              "narrow" -> MeasureFormat.FormatWidth.NARROW
              else -> MeasureFormat.FormatWidth.SHORT
            },
            nf)
        }
      }
      val id = nextHandleId.getAndIncrement()
      numberFormats[id] = NumberHandle(nf, o, measure, unit)
      id
    } catch (e: Exception) {
      0L
    }
  }

  @JvmStatic
  fun nfClose(id: Long) {
    numberFormats.remove(id)
  }

  /**
   * Formats one value.
   *
   * `decimalString` is empty or the **final** digits, per the contract in
   * `cpp/IntlPlatform.h`: already rounded by `js/intl.js`, already scaled for
   * percent, and carrying exactly the fraction digits to render. When it is
   * present the formatter is pinned to those digits and a `BigDecimal` is used,
   * so a BigInt beyond 2^53 keeps its last digit — ICU's BigDecimal path has no
   * precision limit at all, where Apple's NSDecimalNumber stops at 38
   * significant digits (deviation D19).
   */
  @JvmStatic
  fun nfFormat(id: Long, value: Double, decimalString: String): String? {
    val h = numberFormats[id] ?: return null
    return try {
      if (decimalString.isNotEmpty()) {
        val dot = decimalString.indexOf('.')
        val frac = if (dot < 0) 0 else decimalString.length - dot - 1
        (h.format as? DecimalFormat)?.setSignificantDigitsUsed(false)
        h.format.minimumFractionDigits = frac
        h.format.maximumFractionDigits = frac
        val bd = BigDecimal(decimalString)
        applySign(h, bd.signum() < 0 || decimalString.startsWith("-"))
        if (h.measure != null && h.unit != null) {
          h.measure.format(Measure(bd, h.unit))
        } else {
          h.format.format(bd)
        }
      } else {
        applySign(h, value < 0.0 || (value == 0.0 && 1.0 / value < 0.0))
        if (h.measure != null && h.unit != null) {
          h.measure.format(Measure(value, h.unit))
        } else {
          h.format.format(value)
        }
      }
    } catch (e: Exception) {
      null
    }
  }

  /**
   * `signDisplay`, expressed through the affixes.
   *
   * `android.icu.text.DecimalFormat` has no sign-display mode below API 30, so
   * this is the same mechanism the Apple backend uses — and using the same
   * mechanism on both is what stops the two disagreeing about `exceptZero`.
   */
  private fun applySign(h: NumberHandle, negative: Boolean) {
    val f = h.format as? DecimalFormat ?: return
    val sym = f.decimalFormatSymbols
    val plus = sym.plusSign.toString()
    val minus = sym.minusSign.toString()
    if (h.basePositivePrefix == null) {
      h.basePositivePrefix = f.positivePrefix
      h.baseNegativePrefix = f.negativePrefix
    }
    val pos = h.basePositivePrefix!!
    val neg = h.baseNegativePrefix!!
    when (opt(h.opts, NF_SIGN_DISPLAY)) {
      "never" -> { f.positivePrefix = pos; f.negativePrefix = neg.replace(minus, "") }
      "always" -> { f.positivePrefix = plus + pos; f.negativePrefix = neg }
      "exceptZero" -> {
        f.positivePrefix = pos
        f.negativePrefix = neg
        if (!negative) f.positivePrefix = plus + pos
      }
      "negative" -> { f.positivePrefix = pos; f.negativePrefix = neg }
      else -> { f.positivePrefix = pos; f.negativePrefix = neg }
    }
  }

  /**
   * The symbols behind the shared part decomposition, in a fixed order that
   * `cpp/IntlPlatform.cpp`'s `numberFormatToParts` consumes:
   *
   *   0 decimal, 1 group, 2 minus, 3 plus, 4 percent, 5 exponent, 6 NaN,
   *   7 infinity, 8 currency text, 9.. the ten digits.
   *
   * The digits are *rendered*, not read from a table, for the same reason as on
   * Apple: an algorithmic numbering system does not have ten glyphs, and the
   * empty answer is the signal the decomposition needs.
   */
  @JvmStatic
  fun nfSymbols(id: Long): Array<String> {
    val h = numberFormats[id] ?: return emptyArray()
    return try {
      val df = h.format as? DecimalFormat
      val sym = df?.decimalFormatSymbols
        ?: DecimalFormatSymbols.getInstance(ULocale.forLanguageTag(opt(h.opts, NF_LOCALE)))
      val out = ArrayList<String>(19)
      out.add(sym.decimalSeparatorString)
      out.add(sym.groupingSeparatorString)
      out.add(sym.minusSignString)
      out.add(sym.plusSignString)
      out.add(sym.percentString)
      out.add(sym.exponentSeparator)
      out.add(sym.naN)
      out.add(sym.infinity)
      out.add(
        if (opt(h.opts, NF_STYLE) != "currency") ""
        else if (opt(h.opts, NF_CURRENCY_DISPLAY) == "code") opt(h.opts, NF_CURRENCY)
        else h.format.currency?.getSymbol(
          ULocale.forLanguageTag(opt(h.opts, NF_LOCALE))) ?: "")
      val plain = IcuNumberFormat.getInstance(
        ULocale.forLanguageTag(opt(h.opts, NF_LOCALE)).let {
          if (opt(h.opts, NF_NUMBERING).isEmpty()) it
          else ULocale.Builder().setLocale(it)
            .setUnicodeLocaleKeyword("nu", opt(h.opts, NF_NUMBERING)).build()
        })
      plain.isGroupingUsed = false
      plain.maximumFractionDigits = 0
      for (d in 0..9) out.add(plain.format(d.toLong()))
      out.toTypedArray()
    } catch (e: Exception) {
      emptyArray()
    }
  }

  @JvmStatic
  fun nfResolved(id: Long, key: String): String {
    val h = numberFormats[id] ?: return ""
    return try {
      when (key) {
        // android.icu.text.NumberFormat has no getLocale(ULocale.Type): that
        // accessor is ICU4J-only, and the same is true of Collator below. An
        // empty answer means "no opinion" and js/intl.js then reports the
        // locale it asked for, which is correct because it only ever asks for
        // one it negotiated against availableLocales().
        "numberingSystem" -> NumberingSystem.getInstance(
          ULocale.forLanguageTag(opt(h.opts, NF_LOCALE)).let {
            if (opt(h.opts, NF_NUMBERING).isEmpty()) it
            else ULocale.Builder().setLocale(it)
              .setUnicodeLocaleKeyword("nu", opt(h.opts, NF_NUMBERING)).build()
          }).name
        else -> ""
      }
    } catch (e: Exception) {
      ""
    }
  }

  private fun icuRoundingMode(m: String): Int = when (m) {
    "ceil" -> BigDecimal.ROUND_CEILING
    "floor" -> BigDecimal.ROUND_FLOOR
    "expand" -> BigDecimal.ROUND_UP
    "trunc" -> BigDecimal.ROUND_DOWN
    "halfEven" -> BigDecimal.ROUND_HALF_EVEN
    // halfCeil and halfFloor have no ICU rounding mode, exactly as they have no
    // NSNumberFormatterRoundingMode. They only reach the platform for the
    // notations js/intl.js does not pre-round. Deviation D17.
    "halfTrunc", "halfFloor" -> BigDecimal.ROUND_HALF_DOWN
    else -> BigDecimal.ROUND_HALF_UP
  }

  /**
   * The sanctioned unit -> MeasureUnit map.
   *
   * Unlike Apple, android.icu has all forty-five: `MeasureUnit.YEAR`,
   * `MONTH`, `WEEK`, `DAY` and `PERCENT` all exist, where NSUnitDuration stops
   * at hours and there is no NSUnit for a percentage. That is the one place the
   * Android backend is *more* complete than the Apple one for NumberFormat, and
   * it is why deviation D16 is Apple-only.
   */
  private fun measureUnitOf(unit: String): MeasureUnit? {
    val per = unit.indexOf("-per-")
    if (per > 0) {
      val num = singleMeasureUnit(unit.substring(0, per)) ?: return null
      val den = singleMeasureUnit(unit.substring(per + 5)) ?: return null
      // MeasureUnit.getPerUnit is ICU4J-only; android.icu has no compound-unit
      // constructor at any API level this module targets. `x-per-y` therefore
      // renders as a plain number on Android, where Apple derives a "/" join
      // from NSMeasurementFormatter. Deviation D16, and it differs between the
      // two backends, which is exactly the kind of thing the cross-backend
      // corpus is for.
      return null
    }
    return singleMeasureUnit(unit)
  }

  private fun singleMeasureUnit(u: String): MeasureUnit? = when (u) {
    "acre" -> MeasureUnit.ACRE
    "bit" -> MeasureUnit.BIT
    "byte" -> MeasureUnit.BYTE
    "celsius" -> MeasureUnit.CELSIUS
    "centimeter" -> MeasureUnit.CENTIMETER
    "day" -> MeasureUnit.DAY
    "degree" -> MeasureUnit.DEGREE
    "fahrenheit" -> MeasureUnit.FAHRENHEIT
    "fluid-ounce" -> MeasureUnit.FLUID_OUNCE
    "foot" -> MeasureUnit.FOOT
    "gallon" -> MeasureUnit.GALLON
    "gigabit" -> MeasureUnit.GIGABIT
    "gigabyte" -> MeasureUnit.GIGABYTE
    "gram" -> MeasureUnit.GRAM
    "hectare" -> MeasureUnit.HECTARE
    "hour" -> MeasureUnit.HOUR
    "inch" -> MeasureUnit.INCH
    "kilobit" -> MeasureUnit.KILOBIT
    "kilobyte" -> MeasureUnit.KILOBYTE
    "kilogram" -> MeasureUnit.KILOGRAM
    "kilometer" -> MeasureUnit.KILOMETER
    "liter" -> MeasureUnit.LITER
    "megabit" -> MeasureUnit.MEGABIT
    "megabyte" -> MeasureUnit.MEGABYTE
    "meter" -> MeasureUnit.METER
    "microsecond" -> MeasureUnit.MICROSECOND
    "mile" -> MeasureUnit.MILE
    "mile-scandinavian" -> MeasureUnit.MILE_SCANDINAVIAN
    "milliliter" -> MeasureUnit.MILLILITER
    "millimeter" -> MeasureUnit.MILLIMETER
    "millisecond" -> MeasureUnit.MILLISECOND
    "minute" -> MeasureUnit.MINUTE
    "month" -> MeasureUnit.MONTH
    "nanosecond" -> MeasureUnit.NANOSECOND
    "ounce" -> MeasureUnit.OUNCE
    "percent" -> MeasureUnit.PERCENT
    "petabyte" -> MeasureUnit.PETABYTE
    "pound" -> MeasureUnit.POUND
    "second" -> MeasureUnit.SECOND
    "stone" -> MeasureUnit.STONE
    "terabit" -> MeasureUnit.TERABIT
    "terabyte" -> MeasureUnit.TERABYTE
    "week" -> MeasureUnit.WEEK
    "yard" -> MeasureUnit.YARD
    "year" -> MeasureUnit.YEAR
    else -> null
  }

  // ------------------------------------------------------------- Collator

  /**
   * `RuleBasedCollator`, which unlike Foundation exposes every ECMA-402 lever
   * directly: strength, caseFirst (`setUpperCaseFirst`/`setLowerCaseFirst`) and
   * alternate handling for `ignorePunctuation`.
   *
   * That asymmetry is the module's highest divergence risk and it is stated
   * rather than smoothed over: Apple has to express the same three options as
   * `-u-` keywords on the locale, a *different* mechanism with different failure
   * behaviour. `tests/differential/intl/cross-backend.js` is what measures
   * whether the two agree.
   */
  @JvmStatic
  fun colOpen(o: Array<String>): Long {
    // 0 locale, 1 usage, 2 sensitivity, 3 caseFirst, 4 collation,
    // 5 numeric ("1"/""), 6 ignorePunctuation ("1"/"")
    if (o.size < 7) return 0L
    return try {
      var builder = ULocale.Builder().setLocale(ULocale.forLanguageTag(o[0]))
      if (o[4].isNotEmpty() && o[1] != "search") {
        builder = builder.setUnicodeLocaleKeyword("co", o[4])
      }
      val collator = android.icu.text.Collator.getInstance(builder.build())
      if (collator is RuleBasedCollator) {
        collator.strength = when (o[2]) {
          "base" -> android.icu.text.Collator.PRIMARY
          "accent" -> android.icu.text.Collator.SECONDARY
          "case" -> android.icu.text.Collator.PRIMARY
          else -> android.icu.text.Collator.TERTIARY
        }
        if (o[2] == "case") collator.isCaseLevel = true
        when (o[3]) {
          "upper" -> collator.setUpperCaseFirst(true)
          "lower" -> collator.setLowerCaseFirst(true)
        }
        collator.numericCollation = o[5].isNotEmpty()
        collator.isAlternateHandlingShifted = o[6].isNotEmpty()
        val id = nextHandleId.getAndIncrement()
        collators[id] = collator
        id
      } else {
        0L
      }
    } catch (e: Exception) {
      0L
    }
  }

  @JvmStatic
  fun colClose(id: Long) {
    collators.remove(id)
  }

  @JvmStatic
  fun colCompare(id: Long, a: String, b: String): Int {
    val c = collators[id] ?: return if (a < b) -1 else if (a > b) 1 else 0
    val r = c.compare(a, b)
    // ECMA-402 requires exactly -1, 0 or 1, not an arbitrary sign.
    return if (r < 0) -1 else if (r > 0) 1 else 0
  }

  @JvmStatic
  fun colResolved(id: Long, key: String): String {
    val c = collators[id] ?: return ""
    return try {
      when (key) {
        // See nfResolved: getLocale(ULocale.Type) is ICU4J-only. The requested
        // collation is echoed back by js/intl.js, which has already checked it
        // against collations().
        else -> ""
      }
    } catch (e: Exception) {
      ""
    }
  }

  // ------------------------------------------------- RelativeTimeFormat

  @JvmStatic
  fun rtfOpen(o: Array<String>): Long {
    // 0 locale, 1 numberingSystem, 2 numeric, 3 style
    if (o.size < 4) return 0L
    return try {
      val locale = ULocale.forLanguageTag(o[0]).let {
        if (o[1].isEmpty()) it
        else ULocale.Builder().setLocale(it).setUnicodeLocaleKeyword("nu", o[1]).build()
      }
      val f = RelativeDateTimeFormatter.getInstance(
        locale,
        null,
        when (o[3]) {
          "short" -> RelativeDateTimeFormatter.Style.SHORT
          "narrow" -> RelativeDateTimeFormatter.Style.NARROW
          else -> RelativeDateTimeFormatter.Style.LONG
        },
        android.icu.text.DisplayContext.CAPITALIZATION_NONE)
      val id = nextHandleId.getAndIncrement()
      relativeFormats[id] = f
      id
    } catch (e: Exception) {
      0L
    }
  }

  @JvmStatic
  fun rtfClose(id: Long) {
    relativeFormats.remove(id)
  }

  @JvmStatic
  fun rtfFormat(id: Long, value: Double, unit: String, numeric: String): String? {
    val f = relativeFormats[id] ?: return null
    // RelativeDateTimeUnit, not RelativeUnit: the (double, unit) overloads
    // android.icu exposes take the former, and QUARTER exists here where
    // -[NSRelativeDateTimeFormatter localizedStringFromDateComponents:] returns
    // nil for it. That asymmetry is deviation D15 and it is Apple-only.
    val u = when (unit) {
      "year" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.YEAR
      "quarter" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.QUARTER
      "month" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.MONTH
      "week" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.WEEK
      "day" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.DAY
      "hour" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.HOUR
      "minute" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.MINUTE
      "second" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.SECOND
      else -> return null
    }
    return try {
      if (numeric == "always") {
        // formatNumeric is API 28. Below that, format() is used and `numeric:
        // "always"` degrades to the idiomatic wording ("yesterday" rather than
        // "1 day ago") on API 24-27. Deviation D23.
        if (android.os.Build.VERSION.SDK_INT >= 28) f.formatNumeric(value, u)
        else f.format(value, u)
      } else {
        f.format(value, u)
      }
    } catch (e: Exception) {
      null
    }
  }

  @JvmStatic
  fun rtfResolved(id: Long, key: String): String {
    val f = relativeFormats[id] ?: return ""
    return try {
      when (key) {
        else -> ""
      }
    } catch (e: Exception) {
      ""
    }
  }

  // ------------------------------------------------------------ ListFormat

  @JvmStatic
  fun lfOpen(o: Array<String>): Long {
    // 0 locale, 1 type, 2 style
    if (o.size < 3) return 0L
    if (android.os.Build.VERSION.SDK_INT < 26) return 0L  // ListFormatter is API 26
    return try {
      val locale = ULocale.forLanguageTag(o[0])
      val f: ListFormatter =
        if (android.os.Build.VERSION.SDK_INT >= 33) {
          ListFormatter.getInstance(
            locale,
            when (o[1]) {
              "disjunction" -> ListFormatter.Type.OR
              "unit" -> ListFormatter.Type.UNITS
              else -> ListFormatter.Type.AND
            },
            when (o[2]) {
              "short" -> ListFormatter.Width.SHORT
              "narrow" -> ListFormatter.Width.NARROW
              else -> ListFormatter.Width.WIDE
            })
        } else {
          // API 26-32 has only the conjunction/wide instance. Disjunction and
          // the short/narrow widths degrade to it. Deviation D23, and it is a
          // *visible* degradation: resolvedOptions still reports what was asked.
          ListFormatter.getInstance(locale)
        }
      val id = nextHandleId.getAndIncrement()
      listFormats[id] = f
      id
    } catch (e: Exception) {
      0L
    }
  }

  @JvmStatic
  fun lfClose(id: Long) {
    listFormats.remove(id)
  }

  @JvmStatic
  fun lfFormat(id: Long, items: Array<String>): String? {
    val f = listFormats[id] as? ListFormatter ?: return null
    return try {
      f.format(items.toList())
    } catch (e: Exception) {
      null
    }
  }

  // ---------------------------------------------------------- DisplayNames

  @JvmStatic
  fun displayName(locale: String, type: String, style: String, code: String): String {
    return try {
      val loc = ULocale.forLanguageTag(locale)
      val dn = LocaleDisplayNames.getInstance(
        loc,
        when (style) {
          "short" -> LocaleDisplayNames.DialectHandling.STANDARD_NAMES
          else -> LocaleDisplayNames.DialectHandling.DIALECT_NAMES
        })
      when (type) {
        "language" -> dn.localeDisplayName(ULocale.forLanguageTag(code))
        "region" -> dn.regionDisplayName(code)
        "script" -> dn.scriptDisplayName(code)
        "currency" -> Currency.getInstance(code).getName(
          loc, Currency.LONG_NAME, BooleanArray(1))
        "calendar" -> dn.keyValueDisplayName("calendar", code)
        // dateTimeField is the one type Apple has no API for at all; android
        // does. Reporting it here rather than suppressing it for symmetry is
        // deliberate — a platform that can answer should answer, and the
        // difference is enumerated as deviation D21.
        "dateTimeField" -> DateTimePatternGenerator.getInstance(loc)
          .getAppendItemName(dateTimeFieldOf(code))
        else -> ""
      } ?: ""
    } catch (e: Exception) {
      ""
    }
  }

  private fun dateTimeFieldOf(code: String): Int = when (code) {
    "era" -> DateTimePatternGenerator.ERA
    "year" -> DateTimePatternGenerator.YEAR
    "quarter" -> DateTimePatternGenerator.QUARTER
    "month" -> DateTimePatternGenerator.MONTH
    "weekOfYear" -> DateTimePatternGenerator.WEEK_OF_YEAR
    "weekday" -> DateTimePatternGenerator.WEEKDAY
    "day" -> DateTimePatternGenerator.DAY
    "dayPeriod" -> DateTimePatternGenerator.DAYPERIOD
    "hour" -> DateTimePatternGenerator.HOUR
    "minute" -> DateTimePatternGenerator.MINUTE
    "second" -> DateTimePatternGenerator.SECOND
    else -> DateTimePatternGenerator.ZONE
  }

  // ------------------------------------------------------------- Segmenter

  /**
   * `BreakIterator`, with `getRuleStatus()` for `isWordLike`.
   *
   * This is ICU's own answer to "is this run a word", where the Apple backend
   * has to derive it structurally from which runs
   * `-enumerateSubstringsInRange:` yielded. The two agree on ordinary text and
   * are not the same question — that is exactly the divergence
   * `tests/differential/intl/cross-backend.js` exists to measure.
   *
   * Returns a flat int array [begin, end, isWordLike, ...] so a
   * sentence-segmented paragraph is one JNI crossing rather than hundreds.
   */
  @JvmStatic
  fun segment(locale: String, granularity: String, text: String): IntArray {
    return try {
      val loc = ULocale.forLanguageTag(locale)
      val it = when (granularity) {
        "word" -> BreakIterator.getWordInstance(loc)
        "sentence" -> BreakIterator.getSentenceInstance(loc)
        else -> BreakIterator.getCharacterInstance(loc)
      }
      it.setText(text)
      val out = ArrayList<Int>(64)
      var start = it.first()
      var end = it.next()
      while (end != BreakIterator.DONE) {
        out.add(start)
        out.add(end)
        out.add(
          if (granularity == "word" && it.ruleStatus != BreakIterator.WORD_NONE) 1
          else 0)
        start = end
        end = it.next()
      }
      out.toIntArray()
    } catch (e: Exception) {
      IntArray(0)
    }
  }

  @JvmStatic
  fun caseMap(locale: String, upper: Boolean, text: String): String = try {
    val loc = ULocale.forLanguageTag(locale)
    if (upper) UCharacter.toUpperCase(loc, text) else UCharacter.toLowerCase(loc, text)
  } catch (e: Exception) {
    text
  }

  // ---------------------------------------------------------- enumerations

  @JvmStatic
  fun collations(): Array<String> = try {
    android.icu.text.Collator.getKeywordValues("collation")
      .filter { it != "standard" && it != "search" }
      .toTypedArray()
  } catch (e: Exception) {
    emptyArray()
  }

  @JvmStatic
  fun currencies(): Array<String> = try {
    Currency.getAvailableCurrencies().map { it.currencyCode }
      .filter { it.length == 3 }.toTypedArray()
  } catch (e: Exception) {
    emptyArray()
  }

  @JvmStatic
  fun currencyDigits(code: String): Int = try {
    Currency.getInstance(code).defaultFractionDigits
  } catch (e: Exception) {
    -1
  }

  @JvmStatic
  fun localeList(locale: String, key: String): Array<String> = try {
    val loc = ULocale.forLanguageTag(locale)
    when (key) {
      "calendars" -> Calendar.getKeywordValuesForLocale("calendar", loc, false)
        .map { calendarKeywordOf(it) }.toTypedArray()
      "numberingSystems" -> arrayOf(NumberingSystem.getInstance(loc).name)
      "timeZones" -> if (loc.country.isEmpty()) emptyArray()
        else TimeZone.getAvailableIDs(loc.country)
      "collations" -> collations()
      else -> emptyArray()
    }
  } catch (e: Exception) {
    emptyArray()
  }

  @JvmStatic
  fun localeString(locale: String, key: String): String = try {
    val loc = ULocale.forLanguageTag(locale)
    when (key) {
      "hourCycle" -> hourCycleOfPattern(
        DateTimePatternGenerator.getInstance(loc).getBestPattern("j"))
      "textDirection" ->
        if (loc.getCharacterOrientation() == "right-to-left") "rtl" else "ltr"
      else -> ""
    }
  } catch (e: Exception) {
    ""
  }

  /**
   * [firstDay, minimalDays, ...weekend] in ECMA-402's Monday-based numbering.
   *
   * ICU numbers weekdays Sunday=1..Saturday=7, so the conversion happens here:
   * the seam carries spec units and never platform units, which is what stops
   * the two backends disagreeing by exactly one day.
   */
  @JvmStatic
  fun weekInfo(locale: String): IntArray = try {
    val cal = Calendar.getInstance(ULocale.forLanguageTag(locale))
    val toSpec = { d: Int -> if (d == Calendar.SUNDAY) 7 else d - 1 }
    /*
     * android.icu.util.Calendar has no getDayOfWeekType(): that accessor is
     * ICU4J-only. isWeekend(Date) is public, so the weekend is *probed* over
     * seven consecutive days from a known Monday — which is exactly what the
     * Apple backend does with -isDateInWeekend:, so the two derive the answer
     * the same way rather than from two different APIs.
     *
     * 2024-01-01 was a Monday, so day i of the run is ECMA-402 weekday i+1.
     */
    val weekend = ArrayList<Int>(3)
    val probe = Calendar.getInstance(ULocale.forLanguageTag(locale))
    val mondayMs = 1704067200000L  // 2024-01-01T00:00:00Z, a Monday
    for (i in 0..6) {
      probe.timeInMillis = mondayMs + i * 86400000L + 43200000L
      if (cal.isWeekend(Date(mondayMs + i * 86400000L + 43200000L))) {
        weekend.add(i + 1)
      }
    }
    // Only the weekend is sorted. Sorting the whole array would reorder
    // firstDay and minimalDays into the weekend list, which is a bug that
    // produces plausible-looking numbers.
    weekend.sort()
    val out = ArrayList<Int>(2 + weekend.size)
    out.add(toSpec(cal.firstDayOfWeek))
    out.add(cal.minimalDaysInFirstWeek)
    out.addAll(weekend)
    out.toIntArray()
  } catch (e: Exception) {
    IntArray(0)
  }

}
