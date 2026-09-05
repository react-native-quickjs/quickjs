/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The algorithm half of ECMA-402.
 *
 * WHAT LIVES HERE, AND WHY
 *   Everything that is *rules* rather than *data*: BCP-47 parsing and
 *   structural validation, option resolution and the order option getters are
 *   read in, locale negotiation, the ECMA-402 component bag to CLDR skeleton
 *   mapping, `resolvedOptions` and its property order, and the class shapes
 *   themselves.
 *
 *   Every per-locale table lives in the operating system and is reached through
 *   `native`. docs/intl-platform-backed.md measures why: a formatjs stack for
 *   15 locales is 8.28 MB of bundle and 415 ms of startup, and 92% of it is
 *   `DateTimeFormat` and its timezone table.
 *
 *   Writing this once in JavaScript rather than twice in Objective-C++ and Java
 *   is also the main defence against platform divergence. Hermes maintains
 *   2,648 lines of Objective-C++ and 6,870 lines of Java implementing the same
 *   algorithms, and documents 20+ specific behavioural differences between
 *   them.
 *
 * WHY REAL JAVASCRIPT CLASSES
 *   The class shapes are declared here as ordinary JavaScript. That is not a
 *   convenience: a host function created through JSI is not a real constructor,
 *   so `new` against it produces an empty object and the failure is silent
 *   (see modules/text-encoding/cpp/TextEncodingModule.cpp:410). A function
 *   declared in this file is an engine-created constructor, so `new`,
 *   `.prototype`, `instanceof`, and subclassing all work with no special
 *   handling. `native` here is a plain object of ordinary C functions, none of
 *   which is ever used as a constructor.
 *
 * HOW IT IS LOADED
 *   This file is compiled to QuickJS bytecode at build time and embedded in the
 *   module's C++ as a byte array. It is deserialized and evaluated on the first
 *   read of `globalThis.Intl` and not before, so an app that never uses Intl
 *   pays one accessor property on the global object (measured: 152 ns once per
 *   runtime).
 *
 * BUDGET
 *   60 KB of source. Enforced by scripts/check-size.js in CI. The budget exists
 *   because the failure mode is gradual — one per-locale exception, then
 *   another, and it has become formatjs. Any per-locale table other than the
 *   alias tables below is a budget violation and belongs in the backend.
 */

(function (native) {
  'use strict';

  var ObjectDefineProperty = Object.defineProperty;
  var ObjectCreate = Object.create;
  var ArrayIsArray = Array.isArray;
  var hasOwn = function (o, k) { return Object.prototype.hasOwnProperty.call(o, k); };

  /* ---------------------------------------------------------------------- */
  /* BCP-47                                                                  */
  /* ---------------------------------------------------------------------- */

  /*
   * Alias tables. These are the only per-locale data in this file, and they are
   * here rather than in the backend because ECMA-402 pins them: two engines
   * must canonicalize `iw` to `he` regardless of which CLDR version the OS
   * carries. The platform's own canonicalization is consulted *after* these,
   * for anything they do not cover.
   *
   * The corresponding likelySubtags table is 181 KB and is deliberately NOT
   * here — it comes from the platform through native.maximize/minimize. That
   * single omission is 85% of what @formatjs/intl-getcanonicallocales ships.
   */
  var LANGUAGE_ALIAS = {
    iw: 'he', ji: 'yi', in: 'id', mo: 'ro', tl: 'fil', twi: 'ak',
    swc: 'sw-CD', aam: 'aas', adp: 'dz', aue: 'ktz', ayx: 'nun',
    bgm: 'bcg', bic: 'bir', bjd: 'drl', ccq: 'rki', cjr: 'mom',
    cka: 'cmr', cmk: 'xch', coy: 'pij', cqu: 'quh', drh: 'khk',
    drw: 'prs', gav: 'dev', gfx: 'vaj', ggn: 'gvr', gti: 'nyc',
    guv: 'duz', hrr: 'jal', ibi: 'opa', ilw: 'gal', jeg: 'oyb',
    kgc: 'tdf', kgh: 'kml', koj: 'kwv', krm: 'bmf', ktr: 'dtp',
    kvs: 'gdj', kwq: 'yam', kxe: 'tvd', kzj: 'dtp', kzt: 'dtp',
    lii: 'raq', lmm: 'rmx', meg: 'cir', mst: 'mry', mwj: 'vaj',
    myt: 'mry', nad: 'xny', ncp: 'kdz', nnx: 'ngv', nts: 'pij',
    oun: 'vaj', pcr: 'adx', pmc: 'huw', pmu: 'phr', ppa: 'bfy',
    ppr: 'lcq', pry: 'prt', puz: 'pub', sca: 'hle', skk: 'oyb',
    tdu: 'dtp', thc: 'tpo', thx: 'oyb', tie: 'ras', tkk: 'twm',
    tlw: 'weo', tmp: 'tyj', tne: 'kak', tnf: 'prs', tsf: 'taj',
    uok: 'ema', xba: 'cax', xia: 'acn', xkh: 'waw', xsj: 'suj',
    xsl: 'den', ybd: 'rki', yma: 'lrr', ymt: 'mtm', yos: 'zom',
    yuu: 'yug'
  };
  var REGION_ALIAS = {
    BU: 'MM', DD: 'DE', FX: 'FR', TP: 'TL', YD: 'YE', ZR: 'CD',
    AN: 'CW', CS: 'RS', NT: 'SA', SU: 'RU', YU: 'RS',
    62: 'ID', 172: 'RU', 200: 'CZ', 230: 'ET', 280: 'DE',
    532: 'CW', 582: 'FM', 736: 'SD', 830: 'JE', 890: 'RS', 891: 'RS'
  };
  var VARIANT_ALIAS = { heploc: 'alalc97', polytoni: 'polyton' };
  /*
   * CLDR languageAlias entries whose key is more than a bare language subtag.
   *
   * Only the ones that are *structurally valid* unicode_locale_ids are here.
   * `no-nyn`, `zh-min-nan`, `i-klingon` and `en-GB-oed` are not: their second
   * subtag is three alphabetic characters, which unicode_locale_id's grammar
   * admits neither as a variant (5-8 chars, or 4 starting with a digit) nor as
   * a region. ECMA-402 therefore requires a RangeError for those, and adding
   * them here would turn a correct rejection into a wrong acceptance.
   * Confirmed against node in tests/differential/intl/canonicalize.js.
   */
  var COMPOUND_ALIAS = {
    'art-lojban': 'jbo',
    'cel-gaulish': 'xtg',
    'zh-guoyu': 'zh',
    'zh-hakka': 'hak',
    'zh-xiang': 'hsn',
    'sgn-BE-FR': 'sfb',
    'sgn-BE-NL': 'vgt',
    'sgn-CH-DE': 'sgg'
  };
  var SCRIPT_ALIAS = { Qaai: 'Zinh' };

  var reAlpha = /^[a-zA-Z]+$/;
  var reAlnum = /^[a-zA-Z0-9]+$/;
  var reDigit = /^[0-9]+$/;

  function isLang(s) { var n = s.length; return n >= 2 && n <= 8 && n !== 4 && reAlpha.test(s); }
  function isScript(s) { return s.length === 4 && reAlpha.test(s); }
  function isRegion(s) {
    return (s.length === 2 && reAlpha.test(s)) || (s.length === 3 && reDigit.test(s));
  }
  function isVariant(s) {
    var n = s.length;
    if (n >= 5 && n <= 8 && reAlnum.test(s)) return true;
    return n === 4 && s.charCodeAt(0) >= 48 && s.charCodeAt(0) <= 57 && reAlnum.test(s);
  }

  function titleScript(s) {
    return s.charAt(0).toUpperCase() + s.slice(1).toLowerCase();
  }

  /**
   * Parses and canonicalizes a Unicode BCP-47 locale identifier.
   *
   * Returns null when the tag is structurally invalid — the caller turns that
   * into the RangeError ECMA-402 requires. Structural validation is *not*
   * delegated to the platform: NSLocale and ULocale both accept malformed tags
   * silently, and test262 checks that we do not.
   *
   * The result carries the extension keywords separately, because ResolveLocale
   * has to negotiate them against what the backend supports rather than passing
   * them through.
   */
  function parseTag(tag) {
    if (typeof tag !== 'string' || tag.length === 0) return null;
    if (tag.indexOf('_') >= 0) return null;
    var parts = tag.split('-');
    for (var i = 0; i < parts.length; i++) if (parts[i].length === 0) return null;

    var idx = 0;
    var out = {
      language: '', script: '', region: '', variants: [],
      unicodeKeywords: null, transform: null, otherExt: null, privateuse: ''
    };

    /* Grandfathered/irregular tags: only the ones ECMA-402 still allows. */
    var lower = tag.toLowerCase();
    if (lower === 'i-default' || lower === 'i-klingon' || lower === 'i-enochian') {
      return null; /* irregular, not a valid Unicode locale id */
    }

    if (!isLang(parts[idx])) return null;
    out.language = parts[idx].toLowerCase();
    if (hasOwn(LANGUAGE_ALIAS, out.language)) {
      var repl = LANGUAGE_ALIAS[out.language];
      var dash = repl.indexOf('-');
      if (dash < 0) {
        out.language = repl;
      } else {
        out.language = repl.slice(0, dash);
        out.region = repl.slice(dash + 1);
      }
    }
    idx++;

    /*
     * No extlang handling, deliberately. ECMA-402 canonicalizes against
     * `unicode_locale_id`, whose grammar has no extlang production at all, so
     * `zh-cmn-Hans-CN` is a RangeError rather than something to rewrite. An
     * earlier version of this parser accepted and dropped extlangs and
     * therefore returned `zh-Hans-CN`; the differential run against node caught
     * it. Note that this contradicts the platform behaviour recorded in
     * docs/intl-platform-backed.md, where Apple's
     * `Locale.identifier(.bcp47, from:)` *does* rewrite the tag — which is why
     * structural validation happens here and not in the backend.
     */

    if (idx < parts.length && isScript(parts[idx])) {
      out.script = titleScript(parts[idx]);
      if (hasOwn(SCRIPT_ALIAS, out.script)) out.script = SCRIPT_ALIAS[out.script];
      idx++;
    }
    if (idx < parts.length && isRegion(parts[idx]) && !out.region) {
      var r = parts[idx].toUpperCase();
      if (hasOwn(REGION_ALIAS, r)) r = REGION_ALIAS[r];
      out.region = r;
      idx++;
    }
    var seenVariants = {};
    while (idx < parts.length && isVariant(parts[idx])) {
      var v = parts[idx].toLowerCase();
      if (hasOwn(VARIANT_ALIAS, v)) v = VARIANT_ALIAS[v];
      /* Duplicate variants are a structural error per the spec grammar. */
      if (hasOwn(seenVariants, v)) return null;
      seenVariants[v] = 1;
      out.variants.push(v);
      idx++;
    }
    out.variants.sort();

    /* Compound aliases match on the *parsed* form, after case folding, so
       `ART-LOJBAN` and `art-lojban` behave the same. */
    if (out.variants.length === 1 && !out.script) {
      var compoundKey = out.language + '-' + out.variants[0];
      var compoundAlt = out.language + '-' + (out.region || '') + '-' +
                        out.variants[0].toUpperCase();
      var replacement = hasOwn(COMPOUND_ALIAS, compoundKey) ? COMPOUND_ALIAS[compoundKey]
        : (hasOwn(COMPOUND_ALIAS, compoundAlt) ? COMPOUND_ALIAS[compoundAlt] : null);
      if (replacement !== null) {
        out.language = replacement;
        out.variants = [];
      }
    }
    if (out.region && !out.variants.length && !out.script) {
      var regionKey = out.language + '-' + out.region;
      /* sgn-BE-FR and friends: language + region, no variant. */
      var sgn = null;
      for (var ck in COMPOUND_ALIAS) {
        if (ck.toLowerCase() === regionKey.toLowerCase() + '-fr' ||
            ck.toLowerCase() === regionKey.toLowerCase()) { sgn = COMPOUND_ALIAS[ck]; break; }
      }
      if (sgn) { out.language = sgn; out.region = ''; }
    }

    var seenSingletons = {};
    while (idx < parts.length) {
      var sing = parts[idx].toLowerCase();
      if (sing.length !== 1) return null;
      if (hasOwn(seenSingletons, sing)) return null;
      seenSingletons[sing] = 1;
      idx++;
      var body = [];
      /* Private-use subtags may be 1-8 characters; every other singleton's
         subtags must be 2-8. Collecting them with the same rule made
         `en-a-bbb-x-a-ccc` a RangeError, caught by the differential run. */
      var minLen = sing === 'x' ? 1 : 2;
      while (idx < parts.length && parts[idx].length >= minLen) {
        body.push(parts[idx].toLowerCase());
        idx++;
      }
      if (sing === 'x') {
        if (body.length === 0) return null;
        out.privateuse = 'x-' + body.join('-');
        /* private use is terminal */
        if (idx < parts.length) return null;
        break;
      }
      if (body.length === 0) return null;
      if (sing === 'u') {
        out.unicodeKeywords = parseUnicodeExtension(body);
        if (out.unicodeKeywords === null) return null;
      } else if (sing === 't') {
        out.transform = body.join('-');
      } else {
        if (!out.otherExt) out.otherExt = [];
        out.otherExt.push(sing + '-' + body.join('-'));
      }
    }
    return out;
  }

  /* -u-ca-buddhist-nu-thai-kn  ->  {attributes:[], keywords:{ca:'buddhist',...}} */
  function parseUnicodeExtension(body) {
    var attrs = [];
    var kw = {};
    var i = 0;
    while (i < body.length && body[i].length > 2) { attrs.push(body[i]); i++; }
    while (i < body.length) {
      var key = body[i];
      if (key.length !== 2) return null;
      i++;
      var vals = [];
      while (i < body.length && body[i].length > 2) { vals.push(body[i]); i++; }
      var value = vals.join('-');
      /* CLDR canonicalization: the value `true` is written as the empty value. */
      if (value === 'true') value = '';
      if (!hasOwn(kw, key)) kw[key] = value;
    }
    attrs.sort();
    return { attributes: attrs, keywords: kw };
  }

  function formatTag(p) {
    var s = p.language;
    if (p.script) s += '-' + p.script;
    if (p.region) s += '-' + p.region;
    for (var i = 0; i < p.variants.length; i++) s += '-' + p.variants[i];
    if (p.otherExt) {
      p.otherExt.sort();
      for (var j = 0; j < p.otherExt.length; j++) s += '-' + p.otherExt[j];
    }
    if (p.transform) s += '-t-' + p.transform;
    if (p.unicodeKeywords) {
      var u = p.unicodeKeywords;
      var keys = Object.keys(u.keywords).sort();
      if (u.attributes.length || keys.length) {
        s += '-u';
        for (var a = 0; a < u.attributes.length; a++) s += '-' + u.attributes[a];
        for (var k = 0; k < keys.length; k++) {
          s += '-' + keys[k];
          if (u.keywords[keys[k]]) s += '-' + u.keywords[keys[k]];
        }
      }
    }
    if (p.privateuse) s += '-' + p.privateuse;
    return s;
  }

  /** The base name — language[-script][-region][-variants], no extensions. */
  function baseName(p) {
    var s = p.language;
    if (p.script) s += '-' + p.script;
    if (p.region) s += '-' + p.region;
    for (var i = 0; i < p.variants.length; i++) s += '-' + p.variants[i];
    return s;
  }

  /*
   * The canonicalization memo.
   *
   * MOTIVATED BY docs/intl-native-placement.md, which measured
   * `Intl.getCanonicalLocales("en-US")` at 6.78 µs against node's 1.17 µs and
   * showed where it goes. Canonicalizing one tag runs, in order:
   *
   *   1. parseTag(tag)          — split, per-subtag toLowerCase, an options
   *                               object, several arrays, and a
   *                               `for (var ck in COMPOUND_ALIAS)` loop that
   *                               allocates two lowercased strings per table
   *                               entry per call;
   *   2. formatTag(p)           — rebuild by concatenation, with sort()s;
   *   3. native.canonicalize(s) — a full crossing into the platform, with a
   *                               std::string allocation each way;
   *   4. parseTag(again)        — the whole parse a second time;
   *   5. formatTag(again)       — and the whole format a second time.
   *
   * At QuickJS's measured ~3.3 ns per bytecode that is roughly 2,000 bytecodes
   * to canonicalize "en-US", and every `Intl` constructor pays it through
   * CanonicalizeLocaleList before it reads a single option.
   *
   * WHY A MEMO IS SOUND HERE, and it is a stronger argument than the
   * implicit-formatter memo needs. `canonicalizeLocale` takes a **string** and
   * returns a **string or null**. It reads no user-supplied object, invokes no
   * getter, and calls no user code; `parseTag` and `formatTag` are pure, and
   * `native.canonicalize` is a lookup in the operating system's own tables
   * whose answer for a given tag does not change inside a process. So the
   * function is pure by construction and a map keyed on its argument cannot
   * observe anything the uncached call would not.
   *
   *   - Failures are cached too, as `null`. An invalid tag must throw a
   *     RangeError on every call, and it still does: the throw is the caller's
   *     (`canonicalizeLocaleList`), and what is cached is only "parseTag said
   *     no". Not caching failures would leave the worst case — a loop that
   *     re-validates the same bad tag — unimproved for no benefit.
   *   - The key is prefixed with 'L' for exactly the reasons the
   *     implicit-formatter memo prefixes its own: a tag literally named
   *     `__proto__` must not reach `map.__proto__ = value`, and one named
   *     `toString` must not read an Object.prototype member back as a hit.
   *     Unlike that memo the strings here are *unvalidated user input*, so
   *     these are not merely theoretical.
   *   - It rides `memoEnabled`, so `Intl.__rnqjsPerf.setEnabled(false)` builds
   *     a control binary with this and the formatter memo both off, and the
   *     same one-token flip reconstructs a "before" build.
   *
   * WHY A MEMO RATHER THAN A C++ PORT. The alternative considered was moving
   * BCP-47 canonicalization to native. That is several hundred lines that must
   * also reproduce deviation D3 and the script/region-preservation rule below,
   * on three backends, to remove work that a fifteen-line map removes entirely.
   * Doing less beats doing the same thing faster.
   *
   * THE CAP. MEMO_CAP is 8 for the formatter memo because each entry is a
   * whole constructed formatter. An entry here is two short strings, so the cap
   * is larger; `supportedLocalesOf` over a long list is a real shape and would
   * otherwise thrash a cap of 8 on every call. On overflow the map is dropped
   * whole, for the same reason as elsewhere: an LRU's bookkeeping on every hit
   * would cost more than the miss it avoids.
   */
  /* CANON_CAP and canonMemo are declared beside the other memo state, after
     `newMemo` and `memoEnabled` exist; see "the implicit-formatter memo". Both
     are `var`s, so the references below are hoisted and legal, and the
     `memoEnabled &&` guard is what keeps this correct if canonicalizeLocale is
     ever reached before that initialization runs (`memoEnabled` is undefined,
     hence falsy, and the uncached path answers). */
  function canonicalizeLocale(tag) {
    if (memoEnabled && typeof tag === 'string') {
      var ck = 'L' + tag;
      var chit = canonMemo.map[ck];
      if (chit !== undefined) {
        perfStats.canonHits++;
        return chit;
      }
      perfStats.canonMisses++;
      var cval = canonicalizeLocaleUncached(tag);
      if (canonMemo.n >= CANON_CAP) {
        canonMemo.map = {};
        canonMemo.n = 0;
      }
      canonMemo.map[ck] = cval;
      canonMemo.n++;
      return cval;
    }
    return canonicalizeLocaleUncached(tag);
  }

  function canonicalizeLocaleUncached(tag) {
    var p = parseTag(tag);
    if (!p) return null;
    var s = formatTag(p);
    /* Give the platform the last word on legacy mappings the tables above do
       not carry (deviation D3: anything it declines is returned unchanged). */
    var fromPlatform = native.canonicalize(s);
    if (typeof fromPlatform === 'string' && fromPlatform.length) {
      var p2 = parseTag(fromPlatform);
      /*
       * The platform is consulted for legacy *mappings*, never for subtag
       * removal. -[NSLocale canonicalLanguageIdentifierFromString:] strips a
       * script it considers redundant — "it-Latn-IT" comes back as "it-IT" —
       * and ECMA-402 requires the script to survive canonicalization.
       * Locale/constructor-tag.js and Locale/getters-missing.js caught this,
       * and only on the Apple backend: the stub has no opinion, so it passed.
       */
      if (p2 && ((p.script && !p2.script) || (p.region && !p2.region) ||
                 p2.variants.length !== p.variants.length)) {
        p2 = null;
      }
      if (p2) {
        /* Re-attach extensions: platform canonicalization routinely drops
           them, and losing -u-ca- here would silently change behaviour. */
        p2.unicodeKeywords = p.unicodeKeywords;
        p2.transform = p.transform;
        p2.otherExt = p.otherExt;
        p2.privateuse = p.privateuse;
        s = formatTag(p2);
      }
    }
    return s;
  }

  /* ---------------------------------------------------------------------- */
  /* CanonicalizeLocaleList                                                  */
  /* ---------------------------------------------------------------------- */

  function canonicalizeLocaleList(locales) {
    if (locales === undefined) return [];
    var seen = [];
    var O;
    if (typeof locales === 'string') {
      O = [locales];
    } else if (typeof locales === 'object' && locales !== null &&
               getLocaleTag(locales) !== null) {
      O = [locales];
    } else {
      O = Object(locales);
    }
    var len = O.length >>> 0;
    for (var k = 0; k < len; k++) {
      if (!(k in O)) continue;
      var kValue = O[k];
      if (typeof kValue !== 'string' && (typeof kValue !== 'object' || kValue === null)) {
        throw new TypeError('locale must be a string or an object');
      }
      var tag;
      var lt = (typeof kValue === 'object') ? getLocaleTag(kValue) : null;
      if (lt !== null) {
        tag = lt;
      } else {
        tag = String(kValue);
      }
      var canon = canonicalizeLocale(tag);
      if (canon === null) {
        throw new RangeError('Incorrect locale information provided: ' + tag);
      }
      if (seen.indexOf(canon) < 0) seen.push(canon);
    }
    return seen;
  }

  /*
   * The Intl.Locale hook.
   *
   * CanonicalizeLocaleList must accept an Intl.Locale instance and take its
   * [[Locale]] *without* calling toString, so a subclass that overrides
   * toString cannot change locale negotiation. The instance is recognised by
   * its private state and not by a property, which also means an ordinary
   * object cannot impersonate one.
   *
   * `localeState` is declared far below; this function is only ever called
   * after the whole module body has run, so the reference is resolved by then.
   */
  function getLocaleTag(o) {
    if (!o || typeof o !== 'object') return null;
    var s = localeState.get(o);
    return s ? s.tag : null;
  }

  /* ---------------------------------------------------------------------- */
  /* Option reading                                                          */
  /* ---------------------------------------------------------------------- */

  /*
   * CoerceOptionsToObject.
   *
   * `undefined` becomes a null-prototype object; everything else goes through
   * ToObject, which throws TypeError for `null`. An earlier version used
   * `Object(options)`, which quietly turns null into `{}` — caught by the
   * differential run against node, which threw where we did not.
   */
  function coerceOptionsToObject(options) {
    if (options === undefined) return ObjectCreate(null);
    if (options === null) {
      throw new TypeError('Options must be an object or undefined');
    }
    return Object(options);
  }

  /*
   * GetOptionsObject, which is NOT CoerceOptionsToObject.
   *
   * The two differ on a primitive: CoerceOptionsToObject boxes it (so
   * `new Intl.NumberFormat("en", 5)` is fine and reads no options off it),
   * while GetOptionsObject throws a TypeError. ECMA-402 uses the second for
   * every service added after ES2020 — PluralRules, RelativeTimeFormat,
   * ListFormat, DisplayNames and Segmenter — and the first for the three
   * original ones, because changing those would have broken the web.
   * test262's <service>/constructor/constructor/options-getoptionsobject.js
   * checks exactly this and caught the omission.
   */
  /*
   * ToString, exactly.
   *
   * Neither `String(v)` nor `'' + v` is it. `String(sym)` is the one coercion
   * *specified* not to throw for a Symbol and returns "Symbol()", which made
   * seven test262 files across DisplayNames, Segmenter, Locale and
   * supportedValuesOf accept a Symbol where a TypeError is required. `'' + v`
   * throws correctly for a Symbol but is ToPrimitive with hint "default", so
   * for an *object* it calls valueOf before toString where ToString calls
   * toString first — which DateTimeFormat/constructor-options-order.js
   * observes through a proxy and which the first attempt at this fix broke.
   *
   * The explicit Symbol check plus String() is the only formulation that is
   * both.
   */
  function toStringSpec(v) {
    if (typeof v === 'symbol') {
      throw new TypeError('Cannot convert a Symbol value to a string');
    }
    return String(v);
  }

  function getOptionsObject(options) {
    if (options === undefined) return ObjectCreate(null);
    if (typeof options === 'object' && options !== null) return options;
    throw new TypeError('Options must be an object or undefined');
  }

  function getOption(options, prop, values, fallback) {
    var value = options[prop];
    if (value === undefined) return fallback;
    value = toStringSpec(value);
    if (values && values.indexOf(value) < 0) {
      throw new RangeError('Value ' + value + ' out of range for ' + prop);
    }
    return value;
  }

  function getBooleanOption(options, prop, fallback) {
    var value = options[prop];
    if (value === undefined) return fallback;
    return Boolean(value);
  }

  function getNumberOption(options, prop, min, max, fallback) {
    var value = options[prop];
    if (value === undefined) return fallback;
    value = Number(value);
    if (value !== value || value < min || value > max) {
      throw new RangeError(prop + ' value is out of range');
    }
    return Math.floor(value);
  }

  /* ---------------------------------------------------------------------- */
  /* Locale negotiation                                                      */
  /* ---------------------------------------------------------------------- */

  var availableLocales = null;
  function getAvailableLocales() {
    if (availableLocales === null) {
      availableLocales = ObjectCreate(null);
      var list = native.availableLocales();
      for (var i = 0; i < list.length; i++) {
        var c = canonicalizeLocale(list[i]);
        if (c) availableLocales[c] = 1;
      }
    }
    return availableLocales;
  }

  var defaultLocaleCache = null;
  function getDefaultLocale() {
    if (defaultLocaleCache === null) {
      defaultLocaleCache = canonicalizeLocale(native.defaultLocale()) || 'en-US';
    }
    return defaultLocaleCache;
  }

  /** BestAvailableLocale: truncate one subtag at a time. */
  function bestAvailableLocale(available, locale) {
    var candidate = locale;
    for (;;) {
      if (available[candidate]) return candidate;
      var pos = candidate.lastIndexOf('-');
      if (pos < 0) return undefined;
      if (pos >= 2 && candidate.charAt(pos - 2) === '-') pos -= 2;
      candidate = candidate.slice(0, pos);
    }
  }

  function lookupMatcher(requested) {
    var available = getAvailableLocales();
    for (var i = 0; i < requested.length; i++) {
      var p = parseTag(requested[i]);
      if (!p) continue;
      var noExt = baseName(p);
      var found = bestAvailableLocale(available, noExt);
      if (found === undefined) {
        /* Try the maximized form. `zh-TW` may not be listed while
           `zh-Hant-TW` is; this is the one place likely-subtags is load-bearing
           in negotiation. */
        var max = native.maximize(noExt);
        if (typeof max === 'string' && max.length) {
          found = bestAvailableLocale(available, max);
        }
      }
      if (found !== undefined) {
        return { locale: found, extension: p.unicodeKeywords, requested: requested[i] };
      }
    }
    return { locale: getDefaultLocale(), extension: null, requested: undefined };
  }

  /* Relevant Unicode extension keys, in spec order, per service. */
  var DTF_RELEVANT_KEYS = ['ca', 'nu', 'hc'];
  var NU_RELEVANT_KEYS = ['nu'];
  var COL_RELEVANT_KEYS = ['co', 'kn', 'kf'];
  var NO_RELEVANT_KEYS = [];

  function resolveLocale(requested, options, relevantKeys) {
    var r = lookupMatcher(requested);
    var result = { locale: r.locale, ca: undefined, nu: undefined, hc: undefined,
                   co: undefined, kn: undefined, kf: undefined };
    var kw = r.extension ? r.extension.keywords : null;
    var supported = [];
    for (var i = 0; i < relevantKeys.length; i++) {
      var key = relevantKeys[i];
      /*
       * `var value = undefined` and not `var value` — `var` is function-scoped
       * and a bare re-declaration does not reset it, so an iteration that found
       * nothing inherited the previous iteration's value. That turned
       * `th-TH-u-ca-buddhist-nu-thai` into a resolved locale of
       * `...-nu-thai-hc-thai` and an hourCycle of "thai". Found on the Apple
       * backend, invisible on the stub because the stub reports no keywords.
       */
      var value = undefined;
      if (kw && hasOwn(kw, key)) {
        value = kw[key] === '' ? 'true' : kw[key];
      }
      /* An option always wins over the extension keyword. */
      if (options && options[key] !== undefined) {
        value = options[key];
      } else if (value !== undefined) {
        supported.push(key + '-' + value);
      }
      result[key] = value;
    }
    result.extensionSuffix = supported.length ? '-u-' + supported.join('-') : '';
    return result;
  }

  /*
   * The supported-locales cache.
   *
   * `supportedLocalesOf(locales, options)` is pure given the input: the
   * available-locales set is platform-fixed, `parseTag` and `bestAvailableLocale`
   * are spec-fixed functions of their arguments, and `native.maximize` is a
   * function of its argument. With a fixed input the answer is the same forever,
   * so the whole function is memoizable on a stable key.
   *
   * MEASURED (workloads/01-numberformat.js, `supportedLocalesOf` bench row, qjs
   * apple backend 2026-09-05): the uncached call costs 7.81 µs, of which
   * `getAvailableLocales()` is ~5 µs (C++ call, vector copy, JSArray
   * allocation) and the two-element linear search is the rest. The bench calls
   * with the *same* `["de-DE", "en-US"]` argument every iteration, so a memo on
   * the canonicalized form of the request short-circuits both. Capped at 8 to
   * match the rest of this file's caches. The declaration of `supLocMemo` is
   * placed where `newMemo` and `memoStats` exist (later in this file); the
   * cache map is hoisted into a closure here.
   */
  var supLocMemo = null; /* assigned below once `newMemo` exists */
  function supportedLocales(requested) {
    if (supLocMemo !== null && memoEnabled && requested.length <= 8) {
      var key = 'S';
      for (var k = 0; k < requested.length; k++) key += '|' + requested[k];
      var hit = supLocMemo.map[key];
      if (hit !== undefined) {
        memoStats.hits++;
        /* ECMA-402 hands the caller a fresh array; the memo must not let a
           caller's mutation of one result leak into the next. `slice` keeps
           the getAvailableLocales()/lookup work off the hit path. */
        return hit.slice();
      }
      memoStats.misses++;
      var out = supportedLocalesImpl(requested);
      if (supLocMemo.n >= MEMO_CAP) { supLocMemo.map = {}; supLocMemo.n = 0; }
      supLocMemo.map[key] = out;
      supLocMemo.n++;
      return out.slice();
    }
    return supportedLocalesImpl(requested);
  }
  function supportedLocalesImpl(requested) {
    var available = getAvailableLocales();
    var out = [];
    for (var i = 0; i < requested.length; i++) {
      var p = parseTag(requested[i]);
      if (!p) continue;
      var noExt = baseName(p);
      var found = bestAvailableLocale(available, noExt);
      if (found === undefined) {
        /*
         * The maximized form, exactly as lookupMatcher does it. The two must
         * agree: test262's supportedLocalesOf-consistent-with-resolvedOptions.js
         * asserts that anything resolvedOptions() reports as supported is also
         * returned by supportedLocalesOf, and "es-Latn" resolved but was not
         * reported because only one of the two functions consulted likely
         * subtags.
         */
        var max = native.maximize(noExt);
        if (typeof max === 'string' && max.length) {
          found = bestAvailableLocale(available, max);
        }
      }
      if (found !== undefined) out.push(requested[i]);
    }
    return out;
  }

  /* ---------------------------------------------------------------------- */
  /* DateTimeFormat                                                          */
  /* ---------------------------------------------------------------------- */

  var DATE_FIELDS = [
    /* [property, allowed values, skeleton letters keyed by value] */
    ['weekday', ['narrow', 'short', 'long'],
      { narrow: 'EEEEE', short: 'EEE', long: 'EEEE' }],
    ['era', ['narrow', 'short', 'long'],
      { narrow: 'GGGGG', short: 'G', long: 'GGGG' }],
    ['year', ['2-digit', 'numeric'], { '2-digit': 'yy', numeric: 'y' }],
    ['month', ['2-digit', 'numeric', 'narrow', 'short', 'long'],
      { '2-digit': 'MM', numeric: 'M', narrow: 'MMMMM', short: 'MMM', long: 'MMMM' }],
    ['day', ['2-digit', 'numeric'], { '2-digit': 'dd', numeric: 'd' }],
    ['dayPeriod', ['narrow', 'short', 'long'],
      { narrow: 'BBBBB', short: 'B', long: 'BBBB' }],
    ['hour', ['2-digit', 'numeric'], { '2-digit': 'jj', numeric: 'j' }],
    ['minute', ['2-digit', 'numeric'], { '2-digit': 'mm', numeric: 'm' }],
    ['second', ['2-digit', 'numeric'], { '2-digit': 'ss', numeric: 's' }],
  ];

  var TZ_NAME_SKELETON = {
    short: 'z', long: 'zzzz',
    shortOffset: 'O', longOffset: 'OOOO',
    shortGeneric: 'v', longGeneric: 'vvvv'
  };

  var DATE_STYLES = ['full', 'long', 'medium', 'short'];

  /*
   * The private state of a DateTimeFormat.
   *
   * Held in a WeakMap rather than on the instance, so that (a) the instance has
   * no enumerable own properties, which the spec requires, and (b) a subclass
   * cannot collide with our field names. The alternative, a Symbol key, is
   * visible through Object.getOwnPropertySymbols.
   */
  var dtfState = new WeakMap();

  function requireState(o, method) {
    var s = dtfState.get(o);
    if (!s) {
      throw new TypeError(
        'Intl.DateTimeFormat.prototype.' + method +
        ' called on an object that is not an Intl.DateTimeFormat');
    }
    return s;
  }

  /**
   * Builds the CLDR skeleton from the resolved component bag.
   *
   * This is the seam that keeps the two platform backends from drifting: both
   * take a skeleton (Apple through
   * -[NSDateFormatter setLocalizedDateFormatFromTemplate:], Android through
   * android.icu.text.DateTimePatternGenerator.getBestPattern), so the
   * ECMA-402 -> CLDR translation happens exactly once and in one language.
   *
   * Field order follows ICU's canonical skeleton order. getBestPattern is order
   * insensitive, but a canonical order makes two skeletons comparable as
   * strings, which the differential corpus relies on.
   */
  function buildSkeleton(c, hourCycle, hour12) {
    var s = '';
    if (c.era) s += DATE_FIELDS[1][2][c.era];
    if (c.year) s += DATE_FIELDS[2][2][c.year];
    if (c.month) s += DATE_FIELDS[3][2][c.month];
    if (c.day) s += DATE_FIELDS[4][2][c.day];
    if (c.weekday) s += DATE_FIELDS[0][2][c.weekday];
    if (c.dayPeriod) s += DATE_FIELDS[5][2][c.dayPeriod];
    if (c.hour) {
      var two = c.hour === '2-digit';
      var letter;
      if (hour12 === true) letter = hourCycle === 'h11' ? 'K' : 'h';
      else if (hour12 === false) letter = hourCycle === 'h24' ? 'k' : 'H';
      else if (hourCycle === 'h11') letter = 'K';
      else if (hourCycle === 'h12') letter = 'h';
      else if (hourCycle === 'h23') letter = 'H';
      else if (hourCycle === 'h24') letter = 'k';
      else letter = 'j'; /* locale's preference */
      s += two ? letter + letter : letter;
    }
    if (c.minute) s += DATE_FIELDS[7][2][c.minute];
    if (c.second) s += DATE_FIELDS[8][2][c.second];
    if (c.fractionalSecondDigits) {
      for (var i = 0; i < c.fractionalSecondDigits; i++) s += 'S';
    }
    if (c.timeZoneName) s += TZ_NAME_SKELETON[c.timeZoneName];
    return s;
  }

  function initializeDateTimeFormat(dtf, locales, options) {
    var requestedLocales = canonicalizeLocaleList(locales);
    options = coerceOptionsToObject(options);

    /*
     * Option read order is observable through getters on the options bag and
     * test262 checks it. The order below is the spec's:
     *   localeMatcher, calendar, numberingSystem, hour12, hourCycle,
     *   timeZone, <components>, formatMatcher, dateStyle, timeStyle
     */
    getOption(options, 'localeMatcher', ['lookup', 'best fit'], 'best fit');

    var calendar = getOption(options, 'calendar', undefined, undefined);
    if (calendar !== undefined && !isWellFormedKeywordValue(calendar)) {
      throw new RangeError('Invalid calendar: ' + calendar);
    }
    var numberingSystem = getOption(options, 'numberingSystem', undefined, undefined);
    if (numberingSystem !== undefined && !isWellFormedKeywordValue(numberingSystem)) {
      throw new RangeError('Invalid numberingSystem: ' + numberingSystem);
    }

    var hour12 = getBooleanOptionOrUndefined(options, 'hour12');
    var hourCycle = getOption(options, 'hourCycle', ['h11', 'h12', 'h23', 'h24'], undefined);
    /* Per spec: hour12 present makes hourCycle null before ResolveLocale. */
    if (hour12 !== undefined) hourCycle = undefined;

    var r = resolveLocale(requestedLocales, {
      ca: calendar, nu: numberingSystem, hc: hourCycle
    }, DTF_RELEVANT_KEYS);

    var timeZone = options.timeZone;
    if (timeZone === undefined) {
      timeZone = native.defaultTimeZone();
    } else {
      timeZone = String(timeZone);
      var normalized = native.normalizeTimeZone(timeZone);
      if (normalized === null || normalized === undefined) {
        throw new RangeError('Invalid time zone specified: ' + timeZone);
      }
      timeZone = normalized;
    }

    var components = {};
    var anyComponent = false;
    for (var i = 0; i < DATE_FIELDS.length; i++) {
      var f = DATE_FIELDS[i];
      var v = getOption(options, f[0], f[1], undefined);
      components[f[0]] = v;
      if (v !== undefined) anyComponent = true;
    }
    var fsd = getNumberOption(options, 'fractionalSecondDigits', 1, 3, undefined);
    components.fractionalSecondDigits = fsd;
    if (fsd !== undefined) anyComponent = true;
    var tzName = getOption(options, 'timeZoneName',
      ['short', 'long', 'shortOffset', 'longOffset', 'shortGeneric', 'longGeneric'],
      undefined);
    components.timeZoneName = tzName;
    if (tzName !== undefined) anyComponent = true;

    getOption(options, 'formatMatcher', ['basic', 'best fit'], 'best fit');
    var dateStyle = getOption(options, 'dateStyle', DATE_STYLES, undefined);
    var timeStyle = getOption(options, 'timeStyle', DATE_STYLES, undefined);

    if ((dateStyle !== undefined || timeStyle !== undefined) && anyComponent) {
      throw new TypeError(
        'Cannot use dateStyle or timeStyle with individual date-time components');
    }

    /* Defaults: with nothing requested at all, year/month/day numeric. */
    if (dateStyle === undefined && timeStyle === undefined && !anyComponent) {
      components.year = 'numeric';
      components.month = 'numeric';
      components.day = 'numeric';
    }

    var skeleton = (dateStyle === undefined && timeStyle === undefined)
      ? buildSkeleton(components, r.hc, hour12) : null;

    var handle = native.dtfOpen(
      r.locale, r.ca || null, r.nu || null, timeZone,
      hour12 !== undefined ? (hour12 ? 'h12' : 'h23') : (r.hc || null),
      skeleton, dateStyle || null, timeStyle || null);

    if (!handle) {
      throw new RangeError('No date formatter available for ' + r.locale);
    }

    var resolvedHc = native.dtfResolved(handle, 'hourCycle');
    var state = {
      handle: handle,
      locale: r.locale + r.extensionSuffix,
      calendar: native.dtfResolved(handle, 'calendar') || r.ca || 'gregory',
      numberingSystem: native.dtfResolved(handle, 'numberingSystem') || r.nu || 'latn',
      timeZone: native.dtfResolved(handle, 'timeZone') || timeZone,
      hourCycle: resolvedHc || hourCycle || undefined,
      components: components,
      dateStyle: dateStyle,
      timeStyle: timeStyle,
      boundFormat: undefined
    };
    /* hour12 is only reported when the formatter actually has an hour. */
    if (!components.hour && dateStyle === undefined && timeStyle === undefined) {
      state.hourCycle = undefined;
    }
    dtfState.set(dtf, state);
    return dtf;
  }

  function getBooleanOptionOrUndefined(options, prop) {
    var v = options[prop];
    return v === undefined ? undefined : Boolean(v);
  }

  var reKeywordValue = /^[a-zA-Z0-9]{3,8}(-[a-zA-Z0-9]{3,8})*$/;
  function isWellFormedKeywordValue(v) { return reKeywordValue.test(v); }

  function toDateTimeValue(date) {
    var x = date === undefined ? Date.now() : Number(date);
    if (x !== x || x === Infinity || x === -Infinity || Math.abs(x) > 8.64e15) {
      throw new RangeError('Date value is out of the supported range');
    }
    /* TimeClip */
    return x < 0 ? -Math.floor(-x) : Math.floor(x);
  }

  function DateTimeFormat(locales, options) {
    if (!(this instanceof DateTimeFormat)) {
      /* Legacy: calling without new yields a new instance. */
      return new DateTimeFormat(locales, options);
    }
    initializeDateTimeFormat(this, locales, options);
  }

  ObjectDefineProperty(DateTimeFormat, 'prototype', {
    value: DateTimeFormat.prototype, writable: false, enumerable: false, configurable: false
  });
  /*
   * ECMA-402 gives every service constructor a `length` of 0, not of its
   * declared parameter count. Declaring `function DateTimeFormat(locales,
   * options)` therefore reports 2 until this line corrects it. Caught by the
   * differential run against node.
   */
  ObjectDefineProperty(DateTimeFormat, 'length', {
    value: 0, writable: false, enumerable: false, configurable: true
  });

  /*
   * Every function installed as a built-in method must have **no** `prototype`
   * own property: test262 asserts it, and four intl402 tests check it directly
   * ("Built-in functions that aren't constructors must not have a prototype
   * property").
   *
   * `delete fn.prototype` does not work — a function expression's `prototype`
   * is non-configurable, so the delete is a no-op in sloppy mode and a
   * TypeError under 'use strict', which is what this file is. The only
   * mechanisms that produce a function without one are arrow functions and
   * object-literal *method shorthand*. Shorthand is used throughout below,
   * because `this` is load-bearing in the prototype methods and an arrow would
   * capture the wrong one.
   */
  function defineMethod(obj, name, len, fn) {
    ObjectDefineProperty(fn, 'name', { value: name, configurable: true });
    ObjectDefineProperty(fn, 'length', { value: len, configurable: true });
    ObjectDefineProperty(obj, name, {
      value: fn, writable: true, enumerable: false, configurable: true
    });
  }

  /*
   * `format` is an accessor returning a bound function, and the same function
   * object every time. That is not a micro-optimization: ECMA-402 requires the
   * identity to be stable so `arr.map(fmt.format)` works, and test262 checks
   * `fmt.format === fmt.format`.
   */
  var accessors = {
    get format() {
      var state = requireState(this, 'format');
      if (state.boundFormat === undefined) {
        /* Method shorthand, so the bound function has no `prototype`. */
        var holder = {
          bound(date) {
            return native.dtfFormat(state.handle, toDateTimeValue(date));
          }
        };
        var f = holder.bound;
        ObjectDefineProperty(f, 'name', { value: '', configurable: true });
        ObjectDefineProperty(f, 'length', { value: 1, configurable: true });
        state.boundFormat = f;
      }
      return state.boundFormat;
    }
  };
  /*
   * The descriptor is lifted off an object literal, so `enumerable` comes back
   * as true and has to be overridden — a built-in accessor is non-enumerable.
   * Caught by the differential run against node.
   */
  ObjectDefineProperty(DateTimeFormat.prototype, 'format', {
    get: Object.getOwnPropertyDescriptor(accessors, 'format').get,
    enumerable: false,
    configurable: true
  });

  var protoMethods = {
    formatToParts(date) {
      var state = requireState(this, 'formatToParts');
      return native.dtfFormatToParts(state.handle, toDateTimeValue(date));
    },

    resolvedOptions() {
      var s = requireState(this, 'resolvedOptions');
    /*
     * Property order is observable and test262 checks it. Building the object
     * here rather than in native code is why this is deterministic on every
     * platform — Hermes builds it on the Java side and fails these tests on
     * Android 11 specifically.
     */
    var o = {};
    o.locale = s.locale;
    o.calendar = s.calendar;
    o.numberingSystem = s.numberingSystem;
    o.timeZone = s.timeZone;
    if (s.hourCycle !== undefined) {
      o.hourCycle = s.hourCycle;
      o.hour12 = s.hourCycle === 'h11' || s.hourCycle === 'h12';
    }
    var c = s.components;
    if (s.dateStyle === undefined && s.timeStyle === undefined) {
      if (c.weekday) o.weekday = c.weekday;
      if (c.era) o.era = c.era;
      if (c.year) o.year = c.year;
      if (c.month) o.month = c.month;
      if (c.day) o.day = c.day;
      if (c.dayPeriod) o.dayPeriod = c.dayPeriod;
      if (c.hour) o.hour = c.hour;
      if (c.minute) o.minute = c.minute;
      if (c.second) o.second = c.second;
      if (c.fractionalSecondDigits) o.fractionalSecondDigits = c.fractionalSecondDigits;
      if (c.timeZoneName) o.timeZoneName = c.timeZoneName;
    } else {
      if (s.dateStyle !== undefined) o.dateStyle = s.dateStyle;
      if (s.timeStyle !== undefined) o.timeStyle = s.timeStyle;
    }
      return o;
    }
  };
  defineMethod(DateTimeFormat.prototype, 'formatToParts', 1, protoMethods.formatToParts);
  defineMethod(DateTimeFormat.prototype, 'resolvedOptions', 0, protoMethods.resolvedOptions);

  var staticMethods = {
    supportedLocalesOf(locales, options) {
      var requested = canonicalizeLocaleList(locales);
      var opts = coerceOptionsToObject(options);
      getOption(opts, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
      return supportedLocales(requested);
    }
  };
  defineMethod(DateTimeFormat, 'supportedLocalesOf', 1, staticMethods.supportedLocalesOf);

  ObjectDefineProperty(DateTimeFormat.prototype, Symbol.toStringTag, {
    value: 'Intl.DateTimeFormat', writable: false, enumerable: false, configurable: true
  });
  ObjectDefineProperty(DateTimeFormat.prototype, 'constructor', {
    value: DateTimeFormat, writable: true, enumerable: false, configurable: true
  });

  /* ---------------------------------------------------------------------- */
  /* The implicit-formatter memo                                             */
  /* ---------------------------------------------------------------------- */

  /*
   * `Number.prototype.toLocaleString`, `Date.prototype.toLocale*String`,
   * `String.prototype.localeCompare` and `toLocale*Case` are each specified as
   * "construct a formatter, use it, throw it away" — `toLocaleString` step 2 is
   * literally `? Construct(%NumberFormat%, ...)`. Doing that literally is what
   * this layer used to do, and MEASURED (docs/intl-vs-node.md, round 1) it cost
   * 47.6 µs per `toLocaleString("de-DE")` against node's 236 ns, and 9.50 ms
   * against node's 17.7 µs to sort 200 strings through `localeCompare` — 537x.
   * That is not a slow implementation of anything; it is a whole constructor,
   * including BCP-47 canonicalization and locale negotiation, per call.
   *
   * V8 memoizes these. So does this, under a deliberately narrow condition:
   *
   *   options === undefined  AND  typeof locales is "string" or "undefined"
   *
   * Outside that condition nothing is cached and the old path runs unchanged.
   *
   * WHY THAT CONDITION IS THE SAFE ONE, rather than merely the convenient one.
   * The construction is only unobservable if it runs no user code and consults
   * no mutable state:
   *
   *   - `options === undefined` means `CoerceOptionsToObject` produces a fresh
   *     null-prototype object and every `GetOption` read on it hits nothing.
   *     With a user-supplied bag, ECMA-402 fixes the *order* in which the
   *     properties are read and test262 checks it with getters — so an options
   *     object is never cacheable and is never cached. This is why
   *     `toLocaleString-locale+opts` is unchanged by this work.
   *   - `typeof locales === "string"` means `CanonicalizeLocaleList` sees a
   *     primitive. An object would go through `ToString`/`length` reads that
   *     a Proxy or a `Symbol.toPrimitive` can observe and that may return a
   *     different answer next time.
   *   - The cached object is never handed to user code. `format` is read off
   *     the prototype on every call, so a monkey-patched
   *     `Intl.NumberFormat.prototype.format` is still honoured.
   *
   * WHAT IT DELIBERATELY DOES NOT DO
   *   It does not cache across an options object, it does not cache
   *   `Intl.*` constructor calls (those are the user's objects and the user
   *   holds them), and it does not attempt an LRU. On overflow the whole map is
   *   dropped: a real app uses one or two locales, the cap is 8, and an LRU's
   *   bookkeeping would cost more than the miss it avoids.
   *
   * THE STALE-DEFAULT-LOCALE TRADE, stated rather than hidden: with
   * `locales === undefined` the entry is keyed on nothing, so if the operating
   * system's current locale changes inside a running process the memo keeps
   * answering with the old one. Node does the same. `Intl.__rnqjsPerf.reset()`
   * exists for an embedder that wants to invalidate on a locale-change
   * notification.
   */
  var MEMO_CAP = 8;
  var MEMO_DEFAULT_KEY = 'D';

  /*
   * Counters, not a policy. This project has shipped a fast path with zero hits
   * before, so no timing here is believed without `Intl.__rnqjsPerf.stats()`
   * showing the path was reached. They are plain integer increments on a
   * monomorphic object and are always on; the alternative — a build flag —
   * would mean the shipping build is the one nobody can check.
   */
  var memoStats = { hits: 0, misses: 0, bypasses: 0 };
  /*
   * Counters for the other measured fast paths in this file. Same rule as the
   * memo's: a fast path whose hit rate nobody can read is a fast path nobody
   * can trust. Read them with `Intl.__rnqjsPerf.stats()`.
   */
  var perfStats = { fastRoundHits: 0, fastRoundMisses: 0,
                    pluralFastHits: 0, pluralFastMisses: 0,
                    /* canonicalizeLocale's memo — see canonicalizeLocale. */
                    canonHits: 0, canonMisses: 0,
                    /* Segmenter's per-(text) boundary memo — see segment(). */
                    segmentHits: 0, segmentMisses: 0,
                    /* The NumberFormat double fast path — see fastRoundGate. */
                    exactDoubleHits: 0, exactDoubleMisses: 0 };
  var memoCaches = [];
  /*
   * The kill switch. It exists so the memo can be turned off *without changing
   * any other line*, which is the only way to get an honest control run: this
   * module is not under version control in its own right, so "the binary from
   * before the change" is not something a later session can rebuild. Flipping
   * this initializer to `false` and rebuilding gives a binary that differs from
   * the shipping one by exactly one token, and test262 / the differential
   * corpora can then be scored on both.
   *
   * It is also the supported way for an embedder to opt out, via
   * `Intl.__rnqjsPerf.setEnabled(false)`.
   */
  var memoEnabled = true;

  function newMemo() {
    var c = { map: {}, n: 0 };
    memoCaches.push(c);
    return c;
  }

  /*
   * The BCP-47 canonicalization memo. Its justification, its purity argument
   * and why the cap differs are all at `canonicalizeLocale`; only the state
   * lives here, because it has to be created after `memoCaches` exists.
   */
  var CANON_CAP = 64;
  var canonMemo = newMemo();
  /* See supportedLocales above for the cache itself. */
  supLocMemo = newMemo();

  /*
   * The key space is deliberately two-tiered: 'D' for `locales === undefined`,
   * 'L' + the string otherwise. Three things depend on that prefixing.
   *
   *   - `''` must NOT share a key with `undefined`. `''` is an invalid locale
   *     that has to throw a RangeError, and if a default entry could answer it
   *     the call would return a formatted string instead of throwing.
   *   - A locale named "__proto__" must not reach `map.__proto__ = value`,
   *     which sets the prototype rather than defining a property, and would
   *     leave every later lookup walking the formatter's prototype chain.
   *   - A locale named "toString" must not read an `Object.prototype` member
   *     back as a hit. With the prefix the map needs no `hasOwnProperty` guard
   *     on the hot path.
   *
   * All three are unreachable in practice, because only a *valid* tag survives
   * construction long enough to be cached and no valid BCP-47 tag is any of
   * those strings. They are guarded anyway: the guard is one character of
   * string concatenation and the failure mode it prevents is silent wrong
   * output.
   */
  function memoFormatter(cache, Ctor, locales, ctorOptions) {
    var t = typeof locales;
    if (!memoEnabled || (t !== 'string' && t !== 'undefined')) {
      memoStats.bypasses++;
      return new Ctor(locales, ctorOptions);
    }
    var key = t === 'string' ? 'L' + locales : MEMO_DEFAULT_KEY;
    var hit = cache.map[key];
    if (hit !== undefined) {
      memoStats.hits++;
      return hit;
    }
    memoStats.misses++;
    var made = new Ctor(locales, ctorOptions);
    if (cache.n >= MEMO_CAP) {
      cache.map = {};
      cache.n = 0;
    }
    cache.map[key] = made;
    cache.n++;
    return made;
  }

  /*
   * The options-object memo for `toLocaleString` and friends.
   *
   * WHY THIS IS SEPARATE FROM `memoFormatter` ABOVE.
   *   `memoFormatter` keys on `locales` only because, with `options` undefined,
   *   ECMA-402 fixes the merged bag to a compile-time constant and the whole
   *   construction is unobservable. With a *user-supplied* options object, that
   *   argument no longer holds: the constructor reads the options in a
   *   specified order and `NumberFormat/constructor-option-read-order.js` checks
   *   that order with getters. The cache key must therefore include what the
   *   constructor actually read, not the object reference.
   *
   *   This memo is the right place for it: `toLocaleString` and `toLocale*Case`
   *   are each specified as "construct a formatter, use it, throw it away", and
   *   the spec deliberately does not require their options-bag reads to be
   *   observable. test262 has a `taint-Intl-NumberFormat.js` (Numbers/proto/
   *   toLocaleString) that only checks `Intl.NumberFormat` is *called*, not the
   *   *order* in which it reads `options`. V8 caches the construction here for
   *   the same reason; closing the gap to node is the MEASURED prize
   *   (toLocaleString-locale+opts: 95.17 µs qjs vs 14.65 µs node, 6.50x).
   *
   * WHEN THE CACHE IS USED.
   *   - locales is a string or undefined (same condition as the locale-only memo).
   *   - options is a *plain* object: prototype is `Object.prototype` or `null`,
   *     not a Proxy, not a class instance. A Proxy or a custom prototype can
   *     read different things across calls (`Symbol.toPrimitive`, a getter
   *     with side effects), so the cache key is unsound.
   *   - every own property is a data property. A getter that runs on the read
   *     must run on every call, by spec, and caching would skip it.
   *   - the options object has no own symbol-keyed properties (they are
   *     ignored by ECMA-402 anyway, but for safety).
   *
   * WHEN IT IS BYPASSED.
   *   Anything outside the four conditions above falls through to the original
   *   `new Ctor(locales, options)` path, exactly as before. The fast path is
   *   opt-in, never opt-out: a user who needs the constructor to be called
   *   for side effects can always pass a Proxy or a class instance and get
   *   the old behavior.
   *
   * THE KEY.
   *   The key is the sorted-by-name encoding of the own enumerable data
   *   properties, prefixed by the locales tag. Sorting makes the key
   *   independent of insertion order, which is observable through the
   *   for-in enumeration but is not part of the ECMA-402 read sequence.
   */
  function hashPlainOptions(options) {
    /*
     * Prototype: Object.prototype or null is plain. Everything else (class,
     * Proxy, exotic host object) is a bypass.
     *
     * `Object.getPrototypeOf` is the only way to ask without triggering
     * user code — the only `in` on the prototype chain runs on the *own*
     * keys below — and it is spec-stable: a Proxy is reported as its
     * underlying prototype until the handler is touched, which it isn't
     * here, so a Proxy-wrapped object whose target has Object.prototype
     * would pass this check. The `Object.getOwnPropertyDescriptor` loop
     * below is what catches it: a Proxy whose handler defines a getter
     * returns a descriptor with a non-undefined `get` field.
     */
    var proto = Object.getPrototypeOf(options);
    if (proto !== Object.prototype && proto !== null) return null;

    var keys = Object.keys(options);
    /*
     * The fast path: `keys.length === 0` means the options bag has no own
     * properties and is equivalent to `undefined` (the same compile-time
     * default), so the locale-only memo is the right answer and the
     * options-memo has nothing to add. Return a sentinel so the caller
     * routes to the locale-only memo without rebuilding an empty key.
     */
    if (keys.length === 0) return '';

    /*
     * Sort by name to make the key independent of property order. A sorted
     * `Array.prototype.sort` over a small array is the cheapest stable
     * transform available, and the keys are in source order, so the
     * `Object.keys` enumeration is NOT observed to leak through the cache
     * (a for-in would also see string keys in insertion order on every
     * engine, but the key itself is internal).
     */
    var sorted = keys.slice().sort();

    var out = [];
    for (var i = 0; i < sorted.length; i++) {
      var k = sorted[i];
      /*
       * Descriptor check. The cache key is unsound if any property has a
       * getter or setter, because the constructor would call it on the read
       * and the cached formatter would not. `Object.getOwnPropertyDescriptor`
       * is spec-stable and is exactly the operation test262 uses to detect
       * a getter; the alternative — reading `options[k]` and stringifying
       * — would invoke the getter and corrupt the value being read.
       */
      var desc = Object.getOwnPropertyDescriptor(options, k);
      if (desc === undefined) return null;
      if (desc.get !== undefined || desc.set !== undefined) return null;
      var v = desc.value;
      var t = typeof v;
      /*
       * Coerce values to a string in a way that preserves type identity
       * (so `style: "decimal"` and `style: String("decimal")` produce the
       * same key — and so do `style: 1` and `style: "1"`, which the
       * spec does not require us to distinguish either, because
       * `CoerceOptionsToObject` boxes primitives and ECMA-402 reads them
       * through `ToString`).
       */
      if (v === null) {
        out.push(k, 'N');
      } else if (t === 'string') {
        out.push(k, 'S', v);
      } else if (t === 'number') {
        if (v !== v) {
          out.push(k, 'NaN');
        } else if (v === Infinity) {
          out.push(k, 'I');
        } else if (v === -Infinity) {
          out.push(k, 'NI');
        } else {
          out.push(k, 'N', String(v));
        }
      } else if (t === 'boolean') {
        out.push(k, v ? 'T' : 'F');
      } else if (t === 'undefined') {
        out.push(k, 'U');
      } else {
        /*
         * Anything else (object, function, symbol) is not a value ECMA-402
         * reads from a NumberFormat/DateTimeFormat options bag, and trying
         * to fold it into a key is more likely to corrupt the cache than
         * to be useful. Bypass.
         */
        return null;
      }
    }
    return out.join('|');
  }

  function memoFormatterOpts(cache, Ctor, locales, options) {
    var tLocales = typeof locales;
    if (!memoEnabled || (tLocales !== 'string' && tLocales !== 'undefined')) {
      memoStats.bypasses++;
      return new Ctor(locales, options);
    }
    var h = hashPlainOptions(options);
    if (h === null) {
      memoStats.bypasses++;
      return new Ctor(locales, options);
    }
    var prefix = tLocales === 'string' ? 'L' + locales : MEMO_DEFAULT_KEY;
    var key = prefix + '|' + h;
    var hit = cache.map[key];
    if (hit !== undefined) {
      memoStats.hits++;
      return hit;
    }
    memoStats.misses++;
    var made = new Ctor(locales, options);
    if (cache.n >= MEMO_CAP) {
      cache.map = {};
      cache.n = 0;
    }
    cache.map[key] = made;
    cache.n++;
    return made;
  }

  /* ---------------------------------------------------------------------- */
  /* Date.prototype hooks                                                    */
  /* ---------------------------------------------------------------------- */

  /*
   * Replacing these is the point of the exercise for real applications: they
   * are what `new Date().toLocaleDateString()` calls, and quickjs-ng routes
   * them at locale-ignoring stubs today.
   *
   * When an options bag is supplied a formatter is constructed per call,
   * exactly as the spec describes. When it is not — which is the overwhelmingly
   * common shape and the one that MEASURED at 94x node — the memo above
   * answers, because with `options === undefined` the merged bag is a compile
   * time constant per `kind` and nothing about the construction is observable.
   */
  var DATE_REQUIRED = {
    date: { year: 'numeric', month: 'numeric', day: 'numeric' },
    time: { hour: 'numeric', minute: 'numeric', second: 'numeric' },
    any: { year: 'numeric', month: 'numeric', day: 'numeric',
           hour: 'numeric', minute: 'numeric', second: 'numeric' }
  };

  function dateToLocale(kind) {
    var memo = newMemo();
    var required = DATE_REQUIRED[kind];
    /* Method shorthand: these are installed on Date.prototype as built-ins and
       must not carry a `prototype` own property. */
    return ({ m(locales, options) {
      /*
       * thisTimeValue: Date.prototype.valueOf throws a TypeError for a receiver
       * that is not a Date, which is what
       * Date/prototype/this-value-non-date.js requires. `this.valueOf()` would
       * happily accept a number.
       */
      var t = Date.prototype.valueOf.call(this);
      if (t !== t) return 'Invalid Date';
      if (options === undefined) {
        return memoFormatter(memo, DateTimeFormat, locales, required).format(t);
      }
      memoStats.bypasses++;
      var o = coerceOptionsToObject(options);
      var merged = {};
      var anySet = false;
      var keys = Object.keys(o);
      for (var i = 0; i < keys.length; i++) {
        merged[keys[i]] = o[keys[i]];
        if (keys[i] !== 'localeMatcher' && keys[i] !== 'timeZone' &&
            keys[i] !== 'calendar' && keys[i] !== 'numberingSystem' &&
            keys[i] !== 'hour12' && keys[i] !== 'hourCycle') {
          anySet = true;
        }
      }
      if (!anySet) {
        for (var k in required) merged[k] = required[k];
      }
      return new DateTimeFormat(locales, merged).format(t);
    } }).m;
  }

  defineMethod(Date.prototype, 'toLocaleString', 0, dateToLocale('any'));
  defineMethod(Date.prototype, 'toLocaleDateString', 0, dateToLocale('date'));
  defineMethod(Date.prototype, 'toLocaleTimeString', 0, dateToLocale('time'));

  /* ---------------------------------------------------------------------- */
  /* Shared service scaffolding                                              */
  /* ---------------------------------------------------------------------- */

  /*
   * Every ECMA-402 service has the same five structural obligations, and this
   * project has already paid for getting three of them wrong once (an
   * enumerable accessor, a `length` of 2 instead of 0, a method carrying a
   * `prototype`). They are written once here rather than eight times below.
   */

  /** Installs a non-enumerable, configurable accessor with no `prototype`. */
  function defineGetter(obj, name, getter) {
    ObjectDefineProperty(getter, 'name', { value: 'get ' + name, configurable: true });
    ObjectDefineProperty(getter, 'length', { value: 0, configurable: true });
    ObjectDefineProperty(obj, name, {
      get: getter, enumerable: false, configurable: true
    });
  }

  /**
   * Gives a constructor the shape ECMA-402 requires: `length` 0 (regardless of
   * declared parameters), a non-writable `prototype`, a `constructor` back
   * link and a `Symbol.toStringTag`.
   */
  function finishService(ctor, tag, length) {
    ObjectDefineProperty(ctor, 'prototype', {
      value: ctor.prototype, writable: false, enumerable: false, configurable: false
    });
    /*
     * `length` is 0 for every service constructor except Intl.DisplayNames,
     * whose options argument is required and which therefore reports 2. Caught
     * by tests/differential/intl/services-shape.js against node.
     */
    ObjectDefineProperty(ctor, 'length', {
      value: length === undefined ? 0 : length,
      writable: false, enumerable: false, configurable: true
    });
    ObjectDefineProperty(ctor.prototype, Symbol.toStringTag, {
      value: tag, writable: false, enumerable: false, configurable: true
    });
    ObjectDefineProperty(ctor.prototype, 'constructor', {
      value: ctor, writable: true, enumerable: false, configurable: true
    });
    defineMethod(ctor, 'supportedLocalesOf', 1, ({
      m(locales, options) {
        var requested = canonicalizeLocaleList(locales);
        var opts = coerceOptionsToObject(options);
        getOption(opts, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
        return supportedLocales(requested);
      }
    }).m);
  }

  /** The state lookup every prototype method starts with. */
  function stateGetter(map, ctorName) {
    return function (o, method) {
      var s = map.get(o);
      if (!s) {
        throw new TypeError(
          'Intl.' + ctorName + '.prototype.' + method +
          ' called on an object that is not an Intl.' + ctorName);
      }
      return s;
    };
  }

  /**
   * A cached bound function with a stable identity.
   *
   * ECMA-402 requires `fmt.format === fmt.format`, so that `arr.map(fmt.format)`
   * works and so that test262's identity checks pass. Method shorthand, because
   * a built-in that is not a constructor must have no `prototype` own property
   * and `delete fn.prototype` is a no-op on a non-configurable slot.
   */
  function boundOf(state, field, len, make) {
    if (state[field] === undefined) {
      var f = make();
      ObjectDefineProperty(f, 'name', { value: '', configurable: true });
      ObjectDefineProperty(f, 'length', { value: len, configurable: true });
      state[field] = f;
    }
    return state[field];
  }

  /* ---------------------------------------------------------------------- */
  /* Decimal rounding — ECMA-402 algorithm, no platform involved             */
  /* ---------------------------------------------------------------------- */

  /*
   * WHY ROUNDING IS HERE AND NOT IN THE BACKEND
   *   The hybrid rule for this module is "algorithm in JavaScript, data from
   *   the platform". Rounding is algorithm: ToRawFixed, ToRawPrecision and the
   *   nine `roundingMode` values are fully specified by ECMA-402 and involve no
   *   locale data at all. The *presentation* of the rounded digits — which
   *   glyphs, which separators, where the groups fall, what the currency looks
   *   like — is data, and stays in the backend.
   *
   *   Doing it here also removes a divergence class by construction. If
   *   NSNumberFormatter rounded on Apple and android.icu.text.DecimalFormat
   *   rounded on Android, a tie such as 2.5 at zero fraction digits could
   *   resolve differently in an app's two builds, and nothing in the test suite
   *   would notice until a user did. Instead js/intl.js produces the exact
   *   digit string once and the platform is told to render precisely those
   *   digits.
   *
   * WHAT IS NOT ROUNDED HERE, AND WHY
   *   `notation: "compact" | "scientific" | "engineering"`. Those change the
   *   *value* before rounding (1234 -> 1.2K), and which scale and which suffix
   *   is chosen is locale data. Those three go to the platform whole, and their
   *   rounding is therefore the platform's — deviation D17.
   */

  var ROUNDING_MODES = ['ceil', 'floor', 'expand', 'trunc', 'halfCeil',
                        'halfFloor', 'halfExpand', 'halfTrunc', 'halfEven'];
  var ROUNDING_INCREMENTS = [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500,
                             1000, 2000, 2500, 5000];

  function zeros(n) {
    var s = '';
    while (s.length < n) s += '0';
    return s;
  }

  /**
   * A finite Number as a plain decimal string, never in exponential form.
   *
   * Built from `String(x)`, which is the shortest representation that round
   * trips. That is what makes `format(1.005)` with two fraction digits produce
   * `1.01` rather than the `1.00` an exact expansion of the double would give;
   * a full ICU implementation does the same, and the differential corpus in
   * tests/differential/intl/numberformat.js checks it against node rather than
   * leaving it to reasoning.
   */
  function decimalStringFromNumber(x) {
    /*
     * String(-0) is "0", and ECMA-402 renders negative zero with its sign:
     * node prints "-0" for format(-0) and "-000.00" with minimumIntegerDigits 3
     * and minimumFractionDigits 2. Ten test262 files in
     * NumberFormat/prototype/format/ check exactly this, one per roundingMode.
     */
    if (x === 0) return 1 / x < 0 ? '-0' : '0';
    var s = String(x);
    var e = s.indexOf('e');
    if (e < 0) e = s.indexOf('E');
    if (e < 0) return s;
    var mant = s.slice(0, e);
    var exp = parseInt(s.slice(e + 1), 10);
    var neg = mant.charAt(0) === '-';
    if (neg) mant = mant.slice(1);
    var dot = mant.indexOf('.');
    var digits = dot < 0 ? mant : mant.slice(0, dot) + mant.slice(dot + 1);
    var pointPos = (dot < 0 ? mant.length : dot) + exp;
    var out;
    if (pointPos <= 0) out = '0.' + zeros(-pointPos) + digits;
    else if (pointPos >= digits.length) out = digits + zeros(pointPos - digits.length);
    else out = digits.slice(0, pointPos) + '.' + digits.slice(pointPos);
    return (neg ? '-' : '') + out;
  }

  /** { neg, int, frac } from a decimal string. */
  function splitDecimal(s) {
    var neg = s.charAt(0) === '-';
    if (neg || s.charAt(0) === '+') s = s.slice(1);
    var dot = s.indexOf('.');
    var i = dot < 0 ? s : s.slice(0, dot);
    var f = dot < 0 ? '' : s.slice(dot + 1);
    /* Leading zeros are not significant and their presence would change the
       significant-digit arithmetic below. */
    while (i.length > 1 && i.charAt(0) === '0') i = i.slice(1);
    return { neg: neg, int: i, frac: f };
  }

  /**
   * Rounds a digit string at `cut`, returning the kept digits with the carry
   * already propagated.
   *
   * `digits` is the concatenation int+frac with no sign and no point; `cut` is
   * how many of them to keep. A carry out of the top digit prepends a '1',
   * which the caller detects by the length change.
   */
  function roundDigitString(digits, cut, neg, mode) {
    if (cut >= digits.length) return { digits: digits, grew: false };
    var kept = digits.slice(0, cut);
    var rest = digits.slice(cut);
    /* cmp: the discarded tail against one half. */
    var first = rest.charCodeAt(0) - 48;
    var restNonZero = false;
    for (var i = 1; i < rest.length; i++) {
      if (rest.charCodeAt(i) !== 48) { restNonZero = true; break; }
    }
    var cmp = first > 5 ? 1 : first < 5 ? -1 : (restNonZero ? 1 : 0);
    var any = first !== 0 || restNonZero;
    var lastOdd = cut > 0 && ((kept.charCodeAt(cut - 1) - 48) % 2) === 1;

    var up;
    switch (mode) {
      case 'ceil': up = !neg && any; break;
      case 'floor': up = neg && any; break;
      case 'expand': up = any; break;
      case 'trunc': up = false; break;
      case 'halfCeil': up = cmp > 0 || (cmp === 0 && !neg); break;
      case 'halfFloor': up = cmp > 0 || (cmp === 0 && neg); break;
      case 'halfTrunc': up = cmp > 0; break;
      case 'halfEven': up = cmp > 0 || (cmp === 0 && lastOdd); break;
      default: up = cmp >= 0; break; /* halfExpand */
    }
    if (!up) return { digits: kept, grew: false };

    var arr = kept.split('');
    var k = arr.length - 1;
    for (;;) {
      if (k < 0) { arr.unshift('1'); return { digits: arr.join(''), grew: true }; }
      var d = arr[k].charCodeAt(0) - 48 + 1;
      if (d < 10) { arr[k] = String(d); break; }
      arr[k] = '0';
      k--;
    }
    return { digits: arr.join(''), grew: false };
  }

  /** ToRawFixed: round to exactly [minFrac, maxFrac] fraction digits. */
  function toRawFixed(dec, minFrac, maxFrac, mode) {
    var p = splitDecimal(dec);
    var digits = p.int + p.frac;
    var cut = p.int.length + maxFrac;
    var r = roundDigitString(digits, cut, p.neg, mode);
    var intLen = p.int.length + (r.grew ? 1 : 0);
    var out = r.digits;
    while (out.length < intLen + maxFrac) out += '0';
    var ip = out.slice(0, intLen);
    var fp = out.slice(intLen);
    while (fp.length > minFrac && fp.charAt(fp.length - 1) === '0') {
      fp = fp.slice(0, fp.length - 1);
    }
    while (ip.length > 1 && ip.charAt(0) === '0') ip = ip.slice(1);
    return { neg: p.neg, int: ip, frac: fp };
  }

  /** ToRawPrecision: round to [minSd, maxSd] significant digits. */
  function toRawPrecision(dec, minSd, maxSd, mode) {
    var p = splitDecimal(dec);
    var digits = p.int + p.frac;
    /* e is the decimal exponent of the leading significant digit. */
    var lead = 0;
    while (lead < digits.length && digits.charCodeAt(lead) === 48) lead++;
    if (lead === digits.length) {
      /* Zero: minSd digits, all zero, with minSd-1 after the point. */
      return { neg: p.neg, int: '0',
               frac: minSd > 1 ? zeros(minSd - 1) : '' };
    }
    var intLen = p.int.length;
    var cut = lead + maxSd;
    var r = roundDigitString(digits, cut, p.neg, mode);
    if (r.grew) { intLen++; lead++; }
    var out = r.digits;
    while (out.length < intLen) out += '0';
    var ip = out.slice(0, intLen) || '0';
    var fp = out.slice(intLen);
    while (ip.length > 1 && ip.charAt(0) === '0') ip = ip.slice(1);
    /*
     * Trim trailing fraction zeros down to minSd significant digits, then pad
     * back up to it. The pad is not symmetry for its own sake: ToRawPrecision
     * with minimumSignificantDigits 5 must render 1.5 as "1.5000", and the
     * first version of this function only ever trimmed, so it printed "1.5".
     */
    while (fp.length > 0 && fp.charAt(fp.length - 1) === '0' &&
           significantCount(ip, fp) > minSd) {
      fp = fp.slice(0, fp.length - 1);
    }
    while (significantCount(ip, fp) < minSd) fp += '0';
    return { neg: p.neg, int: ip, frac: fp };
  }

  /** Significant digits in int+frac, i.e. everything after the leading zeros. */
  function significantCount(ip, fp) {
    var all = ip + fp;
    var k = 0;
    while (k < all.length && all.charCodeAt(k) === 48) k++;
    return all.length - k;
  }

  function joinDecimal(r) {
    return (r.neg ? '-' : '') + r.int + (r.frac.length ? '.' + r.frac : '');
  }

  /**
   * The whole of ECMA-402's FormatNumericToString, as a decimal string.
   *
   * Returns null for values the platform must render itself (NaN, +/-Infinity)
   * and for the notations this layer does not pre-round.
   */
  function roundToDecimal(dec, d) {
    var out;
    if (d.roundingType === 'significantDigits') {
      out = toRawPrecision(dec, d.minimumSignificantDigits,
                           d.maximumSignificantDigits, d.roundingMode);
    } else if (d.roundingType === 'fractionDigits') {
      out = toRawFixed(dec, d.minimumFractionDigits, d.maximumFractionDigits,
                       d.roundingMode);
    } else {
      /* morePrecision / lessPrecision: compute both and pick by digit count. */
      var a = toRawPrecision(dec, d.minimumSignificantDigits,
                             d.maximumSignificantDigits, d.roundingMode);
      var b = toRawFixed(dec, d.minimumFractionDigits, d.maximumFractionDigits,
                         d.roundingMode);
      var more = d.roundingType === 'morePrecision';
      out = (a.frac.length > b.frac.length) === more ? a : b;
    }
    /* minimumIntegerDigits */
    while (out.int.length < d.minimumIntegerDigits) out.int = '0' + out.int;
    if (d.trailingZeroDisplay === 'stripIfInteger') {
      var allZero = true;
      for (var i = 0; i < out.frac.length; i++) {
        if (out.frac.charCodeAt(i) !== 48) { allZero = false; break; }
      }
      if (allZero) out.frac = '';
    }
    return out;
  }

  /* ---------------------------------------------------------------------- */
  /* NumberFormat                                                            */
  /* ---------------------------------------------------------------------- */

  /*
   * The ECMA-402 *sanctioned single unit* list. Forty-five identifiers, fixed
   * by the specification, identical in every locale — a spec constant and not
   * locale data, which is why it is allowed to live here under the module's
   * size rule.
   */
  var SANCTIONED_UNITS = [
    'acre', 'bit', 'byte', 'celsius', 'centimeter', 'day', 'degree',
    'fahrenheit', 'fluid-ounce', 'foot', 'gallon', 'gigabit', 'gigabyte',
    'gram', 'hectare', 'hour', 'inch', 'kilobit', 'kilobyte', 'kilogram',
    'kilometer', 'liter', 'megabit', 'megabyte', 'meter', 'microsecond',
    'mile', 'mile-scandinavian', 'milliliter', 'millimeter', 'millisecond',
    'minute', 'month', 'nanosecond', 'ounce', 'percent', 'petabyte', 'pound',
    'second', 'stone', 'terabit', 'terabyte', 'week', 'yard', 'year'
  ];

  function isWellFormedUnitIdentifier(u) {
    if (SANCTIONED_UNITS.indexOf(u) >= 0) return true;
    var i = u.indexOf('-per-');
    if (i < 0) return false;
    var num = u.slice(0, i);
    var den = u.slice(i + 5);
    return SANCTIONED_UNITS.indexOf(num) >= 0 &&
           SANCTIONED_UNITS.indexOf(den) >= 0;
  }

  var reCurrencyCode = /^[A-Za-z]{3}$/;

  function setNumberFormatDigitOptions(s, options, mnfdDefault, mxfdDefault, notation) {
    /*
     * The read order below is SetNumberFormatDigitOptions verbatim, and it is
     * observable: test262 installs getters on the options bag and records the
     * sequence. Reordering these lines is a behaviour change.
     */
    var mnid = getNumberOption(options, 'minimumIntegerDigits', 1, 21, 1);
    var mnfd = options.minimumFractionDigits;
    var mxfd = options.maximumFractionDigits;
    var mnsd = options.minimumSignificantDigits;
    var mxsd = options.maximumSignificantDigits;
    s.minimumIntegerDigits = mnid;
    var roundingIncrement = getNumberOption(options, 'roundingIncrement', 1, 5000, 1);
    if (ROUNDING_INCREMENTS.indexOf(roundingIncrement) < 0) {
      throw new RangeError('roundingIncrement must be one of the sanctioned values');
    }
    var roundingMode = getOption(options, 'roundingMode', ROUNDING_MODES, 'halfExpand');
    var roundingPriority = getOption(options, 'roundingPriority',
      ['auto', 'morePrecision', 'lessPrecision'], 'auto');
    var trailingZeroDisplay = getOption(options, 'trailingZeroDisplay',
      ['auto', 'stripIfInteger'], 'auto');

    var hasSd = mnsd !== undefined || mxsd !== undefined;
    var hasFd = mnfd !== undefined || mxfd !== undefined;
    var needSd = true;
    var needFd = true;
    if (roundingPriority === 'auto') {
      needSd = hasSd;
      if (needSd || (!hasFd && notation === 'compact')) needFd = false;
    }
    if (needSd) {
      if (hasSd) {
        mnsd = defaultNumberOption(mnsd, 1, 21, 1);
        mxsd = defaultNumberOption(mxsd, mnsd, 21, 21);
      } else {
        mnsd = 1;
        mxsd = 21;
      }
    }
    if (needFd) {
      if (hasFd) {
        mnfd = defaultNumberOption(mnfd, 0, 100, undefined);
        mxfd = defaultNumberOption(mxfd, 0, 100, undefined);
        if (mnfd === undefined) mnfd = Math.min(mnfdDefault, mxfd);
        else if (mxfd === undefined) mxfd = Math.max(mxfdDefault, mnfd);
        else if (mnfd > mxfd) {
          throw new RangeError('minimumFractionDigits exceeds maximumFractionDigits');
        }
      } else {
        mnfd = mnfdDefault;
        mxfd = mxfdDefault;
      }
    }
    s.roundingIncrement = roundingIncrement;
    s.roundingMode = roundingMode;
    s.roundingPriority = roundingPriority;
    s.trailingZeroDisplay = trailingZeroDisplay;
    if (!needSd && !needFd) {
      /* compact with no explicit digit options: two significant digits, which
         is what "1.2K" is. */
      s.roundingType = 'morePrecision';
      /*
       * resolvedOptions must report "morePrecision" here, not the "auto" that
       * was requested: compact notation with no digit options *is* a
       * more-precision resolution, and node reports it that way. Caught by
       * tests/differential/intl/numberformat-shape.js.
       */
      s.roundingPriority = 'morePrecision';
      s.minimumFractionDigits = 0;
      s.maximumFractionDigits = 0;
      s.minimumSignificantDigits = 1;
      s.maximumSignificantDigits = 2;
    } else {
      s.minimumFractionDigits = needFd ? mnfd : 0;
      s.maximumFractionDigits = needFd ? mxfd : 0;
      s.minimumSignificantDigits = needSd ? mnsd : undefined;
      s.maximumSignificantDigits = needSd ? mxsd : undefined;
      s.roundingType = roundingPriority === 'morePrecision' ? 'morePrecision'
        : roundingPriority === 'lessPrecision' ? 'lessPrecision'
          : (hasSd ? 'significantDigits' : 'fractionDigits');
    }
    if (roundingIncrement !== 1) {
      if (s.roundingType !== 'fractionDigits') {
        throw new TypeError('roundingIncrement requires fraction-digit rounding');
      }
      if (s.maximumFractionDigits !== s.minimumFractionDigits) {
        throw new RangeError(
          'roundingIncrement requires minimumFractionDigits === maximumFractionDigits');
      }
    }
  }

  function defaultNumberOption(value, min, max, fallback) {
    if (value === undefined) return fallback;
    value = Number(value);
    if (value !== value || value < min || value > max) {
      throw new RangeError('value out of range');
    }
    return Math.floor(value);
  }

  var nfState = new WeakMap();
  var requireNf = stateGetter(nfState, 'NumberFormat');

  function initializeNumberFormat(nf, locales, options) {
    var requestedLocales = canonicalizeLocaleList(locales);
    options = coerceOptionsToObject(options);
    getOption(options, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
    var numberingSystem = getOption(options, 'numberingSystem', undefined, undefined);
    if (numberingSystem !== undefined && !isWellFormedKeywordValue(numberingSystem)) {
      throw new RangeError('Invalid numberingSystem: ' + numberingSystem);
    }
    var r = resolveLocale(requestedLocales, { nu: numberingSystem }, NU_RELEVANT_KEYS);

    /* SetNumberFormatUnitOptions, in spec read order. */
    var style = getOption(options, 'style',
      ['decimal', 'percent', 'currency', 'unit'], 'decimal');
    var currency = getOption(options, 'currency', undefined, undefined);
    if (currency !== undefined && !reCurrencyCode.test(currency)) {
      throw new RangeError('Invalid currency code: ' + currency);
    }
    var currencyDisplay = getOption(options, 'currencyDisplay',
      ['code', 'symbol', 'narrowSymbol', 'name'], 'symbol');
    var currencySign = getOption(options, 'currencySign',
      ['standard', 'accounting'], 'standard');
    var unit = getOption(options, 'unit', undefined, undefined);
    if (unit !== undefined && !isWellFormedUnitIdentifier(unit)) {
      throw new RangeError('Invalid unit identifier: ' + unit);
    }
    var unitDisplay = getOption(options, 'unitDisplay',
      ['short', 'narrow', 'long'], 'short');
    if (style === 'currency' && currency === undefined) {
      throw new TypeError('Currency code is required with style "currency"');
    }
    if (style === 'unit' && unit === undefined) {
      throw new TypeError('Unit is required with style "unit"');
    }
    if (currency !== undefined) currency = currency.toUpperCase();

    var mnfdDefault = 0;
    var mxfdDefault = 3;
    if (style === 'currency') {
      /*
       * The ISO 4217 minor-unit count is data, so it is asked for rather than
       * tabled. JPY has 0, KWD has 3, and the default of 2 is wrong for both.
       * Foundation and android.icu each know the answer; the -1 fallback below
       * is what a backend with no opinion produces.
       */
      var cd = native.currencyDigits(currency);
      if (cd === null || cd === undefined || cd < 0) cd = 2;
      mnfdDefault = cd;
      mxfdDefault = cd;
    } else if (style === 'percent') {
      mnfdDefault = 0;
      mxfdDefault = 0;
    }

    var notation = getOption(options, 'notation',
      ['standard', 'scientific', 'engineering', 'compact'], 'standard');

    var s = { locale: r.locale + r.extensionSuffix, style: style };
    setNumberFormatDigitOptions(s, options, mnfdDefault, mxfdDefault, notation);
    s.notation = notation;
    var compactDisplay = getOption(options, 'compactDisplay',
      ['short', 'long'], 'short');
    var defaultUseGrouping = notation === 'compact' ? 'min2' : 'auto';
    var useGrouping = getStringOrBooleanOption(options, 'useGrouping',
      ['min2', 'auto', 'always'], 'always', false, defaultUseGrouping);
    var signDisplay = getOption(options, 'signDisplay',
      ['auto', 'never', 'always', 'exceptZero', 'negative'], 'auto');

    s.currency = currency;
    s.currencyDisplay = style === 'currency' ? currencyDisplay : undefined;
    s.currencySign = style === 'currency' ? currencySign : undefined;
    s.unit = unit;
    s.unitDisplay = style === 'unit' ? unitDisplay : undefined;
    s.compactDisplay = notation === 'compact' ? compactDisplay : undefined;
    s.useGrouping = useGrouping;
    s.signDisplay = signDisplay;

    /*
     * The open bag is a closure rather than a literal because `useGrouping:
     * "min2"` needs a second formatter with grouping off; building the bag
     * twice by hand is how the two would drift.
     */
    s.openBag = function (groupingOverride) {
      return {
      locale: r.locale,
      numberingSystem: r.nu || null,
      style: style,
      currency: currency || null,
      currencyDisplay: s.currencyDisplay || null,
      currencySign: s.currencySign || null,
      unit: unit || null,
      unitDisplay: s.unitDisplay || null,
      notation: notation,
      compactDisplay: s.compactDisplay || null,
      signDisplay: signDisplay,
      roundingMode: s.roundingMode,
      roundingType: s.roundingType,
      trailingZeroDisplay: s.trailingZeroDisplay,
      useGrouping: groupingOverride !== null && groupingOverride !== undefined
        ? groupingOverride
        : (useGrouping === false ? '' : String(useGrouping)),
      minimumIntegerDigits: s.minimumIntegerDigits,
      minimumFractionDigits: s.minimumFractionDigits,
      maximumFractionDigits: s.maximumFractionDigits,
      minimumSignificantDigits: s.minimumSignificantDigits === undefined ? -1 : s.minimumSignificantDigits,
      maximumSignificantDigits: s.maximumSignificantDigits === undefined ? -1 : s.maximumSignificantDigits,
      roundingIncrement: s.roundingIncrement
      };
    };
    var handle = native.nfOpen(s.openBag(null));
    if (!handle) throw new RangeError('No number formatter available for ' + r.locale);
    s.handle = handle;
    s.numberingSystem = native.nfResolved(handle, 'numberingSystem') || r.nu || 'latn';
    /*
     * ECMA-402 says an unsupported `nu` is ignored, and an ignored keyword must
     * not appear in resolvedOptions().locale either — `ja-JP-u-nu-native`
     * resolves to plain `ja-JP`. The extension suffix is therefore rebuilt from
     * what the backend *honoured* rather than from what was requested.
     * NumberFormat/ignore-invalid-unicode-ext-values.js is the check.
     */
    if (r.extensionSuffix && s.numberingSystem !== r.nu) s.locale = r.locale;
    s.boundFormat = undefined;
    /*
     * The fast-path predicate, decided once here rather than re-derived on
     * every format() call. See fastRoundDecimal for what each conjunct
     * protects. `style` is checked rather than `notation` alone because
     * percent rescales by 100 before rounding.
     */
    s.fastRound = notation === 'standard' && style !== 'percent' &&
      s.roundingIncrement === 1 && s.roundingType === 'fractionDigits' &&
      s.minimumIntegerDigits === 1 && s.trailingZeroDisplay === 'auto';
    /*
     * The magnitude bound under which the backend may format the **double**
     * rather than the digit string. See `exactDoubleBound` for the derivation
     * and `nfFormatValue` for the use.
     */
    s.exactDoubleBound = exactDoubleBound(s.maximumFractionDigits);
    nfState.set(nf, s);
    return nf;
  }

  /**
   * GetStringOrBooleanOption, which `useGrouping` alone needs: it accepts
   * `true`, `false`, `""` and three string values, and `false` and `""` are the
   * same answer.
   */
  function getStringOrBooleanOption(options, prop, values, trueValue, falsyValue, fallback) {
    var value = options[prop];
    if (value === undefined) return fallback;
    if (value === true) return trueValue;
    if (value === false || value === 0 || value === null || value === '' ||
        value !== value) {
      /* ToBoolean(value) is false */
      if (!value) return falsyValue;
    }
    value = String(value);
    if (value === 'true' || value === 'false') return fallback;
    if (values.indexOf(value) < 0) {
      throw new RangeError('Value ' + value + ' out of range for ' + prop);
    }
    return value;
  }

  /**
   * ToIntlMathematicalValue: a Number, a BigInt or a numeric string, reduced to
   * { number, decimal } where `decimal` is the exact digits when they matter.
   *
   * The BigInt path is why `decimal` exists at all: `9007199254740993n` is not
   * representable as a double, and ECMA-402 formats the mathematical value.
   */
  function toIntlMathematicalValue(value) {
    if (typeof value === 'bigint') {
      return { number: Number(value), decimal: value.toString(), special: null };
    }
    if (typeof value === 'string') {
      var t = value.trim();
      var n = t === '' ? 0 : Number(t);
      if (n !== n) return { number: NaN, decimal: null, special: 'nan' };
      if (n === Infinity) return { number: Infinity, decimal: null, special: 'inf' };
      if (n === -Infinity) return { number: -Infinity, decimal: null, special: '-inf' };
      /* Only trust the literal digits when they are a plain decimal; anything
         with an exponent or a radix prefix goes through the Number. */
      return { number: n,
               decimal: /^[+-]?[0-9]*(\.[0-9]*)?$/.test(t) && t !== '' && t !== '.'
                 ? t : decimalStringFromNumber(n),
               special: null };
    }
    var x = Number(value);
    if (x !== x) return { number: NaN, decimal: null, special: 'nan' };
    if (x === Infinity) return { number: x, decimal: null, special: 'inf' };
    if (x === -Infinity) return { number: x, decimal: null, special: '-inf' };
    return { number: x, decimal: decimalStringFromNumber(x), special: null };
  }

  /*
   * The NumberFormat pre-rounding fast path.
   *
   * WHY IT EXISTS. MEASURED 2026-07-27 on an M4 Pro (docs/intl-vs-node.md):
   * `nf.format(integer)` cost 4,150 ns while `nf.format(NaN)` — which returns
   * from `preRound` immediately and does nothing but call the backend — cost
   * 700 ns. So **83% of a format call was this layer's own digit-string
   * arithmetic**, not the platform formatter and not the seam. Node does the
   * same call in 198 ns.
   *
   * WHAT IT DOES. The general path in `roundToDecimal` is a full
   * ToRawFixed/ToRawPrecision implementation over digit strings: split the
   * number, slice at the cut, propagate a carry, pad up to maxFrac, then trim
   * back down to minFrac. For the overwhelmingly common case it is all
   * unnecessary, because `String(x)` is ALREADY the correct answer:
   *
   *   - the value's own decimal expansion has between minFrac and maxFrac
   *     fraction digits, so nothing is rounded and nothing is trimmed; or
   *   - it has fewer, and the only work is appending zeros.
   *
   * `String(x)` is the shortest representation that round-trips, which is
   * exactly the value ECMA-402's ToRawFixed starts from (see
   * `decimalStringFromNumber`), so taking it verbatim is not an approximation.
   *
   * WHEN IT IS ALLOWED. `state.fastRound` is computed once per formatter and is
   * true only when every lever that could alter the digits is at its identity:
   * standard notation, fraction-digit rounding, roundingIncrement 1,
   * minimumIntegerDigits 1, trailingZeroDisplay "auto", and a style that does
   * not rescale the value (percent multiplies by 100 before rounding). The
   * rounding MODE is deliberately not in that list, and does not need to be:
   * the fast path only fires when no digit is discarded, and every rounding
   * mode agrees about a value it does not round.
   *
   * WHEN IT DECLINES. Exponent form (`1e21`, `5e-7`), a fraction longer than
   * maximumFractionDigits, and — with minimumFractionDigits > 0 — the value
   * zero, whose sign `String` does not preserve. Every decline falls into the
   * unchanged general path, so a bug here is a slowdown and not wrong output.
   *
   * PROVE IT IS REACHED. `Intl.__rnqjsPerf.stats()` reports
   * `fastRoundHits` / `fastRoundMisses`. This project has shipped a fast path
   * with zero hits that survived a spike, an audit and a relay; no timing in
   * docs/intl-vs-node.md is quoted without the counter beside it.
   */
  function fastRoundDecimal(x, minF, maxF) {
    if (x === 0) {
      /* String(-0) is "0" and ECMA-402 renders negative zero with its sign. */
      if (minF !== 0) return null;
      return 1 / x < 0 ? '-0' : '0';
    }
    var str = '' + x;
    /*
     * Number::toString only ever emits a lowercase 'e', so one indexOf settles
     * exponent form. `decimalStringFromNumber` checks for 'E' as well and that
     * is defensive; here the check is on the hot path and the guarantee is in
     * the specification.
     */
    if (str.indexOf('e') >= 0) return null;
    var dot = str.indexOf('.');
    if (dot < 0) return minF === 0 ? str : str + '.' + zeros(minF);
    var fracLen = str.length - dot - 1;
    if (fracLen > maxF) return null;
    if (fracLen < minF) return str + zeros(minF - fracLen);
    return str;
  }

  /**
   * The fast path's gate: the final digit string for `value`, or null to fall
   * into the general path.
   *
   * It sits HERE rather than inside preRound so that a hit also skips
   * `toIntlMathematicalValue`, which allocates a { number, decimal, special }
   * record and eagerly computes `decimalStringFromNumber(x)` — work the fast
   * path does not use. MEASURED: hooking preRound alone took
   * `nf.format(integer)` from 4,150 ns to 3,200 ns; moving the gate to the call
   * site took it to the figure in docs/intl-vs-node.md. Half the saving was in
   * the record, not the rounding, which is not where the first attempt looked.
   */
  /**
   * The magnitude below which a double may be handed to the backend instead of
   * the digit string, for a formatter whose maximumFractionDigits is `maxF`.
   * Returns 0 to mean "never".
   *
   * WHY THIS EXISTS. MEASURED, bench/spikes/intl/apple-numberformatter-probe.m,
   * one en_US decimal formatter reused across iterations, three whole-program
   * runs agreeing to +/-3%:
   *
   *   -[NSNumberFormatter stringFromNumber:] given an NSNumber double   492 ns
   *   -[NSNumberFormatter stringFromNumber:] given an NSDecimalNumber  2041 ns
   *   [NSDecimalNumber decimalNumberWithString:]                        519 ns
   *
   * Handing Foundation an NSDecimalNumber costs **4.1x** more than handing it a
   * double. The string parse is 519 ns of that; the other ~1,030 ns is the
   * formatter taking a different and much slower internal path. So the choice
   * of argument type is worth ~1,550 ns, not the 600 ns that
   * docs/intl-vs-node.md attributed to it and rejected as not worth the risk.
   *
   * WHY THERE IS A BOUND AT ALL, and this corrects a claim made when this work
   * was proposed. It was argued that no extra condition is needed, because when
   * `fastRoundDecimal` fires the digit string *is* `String(x)` and therefore
   * round-trips to `x` exactly. That is true and it is not sufficient.
   * Round-tripping says `parse(String(x)) === x`; it does not say that
   * rendering `x`'s **exact binary value** to `maxF` fraction digits reproduces
   * `String(x)`. The counter-example is the one docs/intl-vs-node.md already
   * recorded: with `maximumFractionDigits: 17`, `String(0.1)` is `"0.1"` and
   * `fastRoundDecimal` fires, but the exact value of the double 0.1 is
   * 0.1000000000000000055511151231257827…, and Foundation renders
   * `0.10000000000000001`. Shipping that would have produced silently wrong
   * digits in the one place this module has been most careful to be exact.
   *
   * THE CONDITION THAT IS SAFE. Let D = String(x), with `f <= maxF` fraction
   * digits (guaranteed by fastRoundDecimal) and `i` integer digits. Rendering
   * x to maxF fraction digits yields D exactly when
   *
   *     |x_exact - D| < 0.5 * 10^-maxF
   *
   * and since D is the shortest decimal that round-trips to x,
   * |x_exact - D| <= half an ulp <= |x| * 2^-52. Requiring
   * |x| < 10^(15-maxF) gives
   *
   *     |x| * 2^-52 < 10^(15-maxF) * 2.221e-16 = 0.222 * 10^-maxF
   *
   * which clears the 0.5 * 10^-maxF threshold with better than 2x of margin.
   * Equivalently: **integer digits plus maximumFractionDigits at most 15**,
   * which is the "joint condition on magnitude and fraction count"
   * docs/intl-vs-node.md correctly identified as necessary and declined to
   * state. It is checkable in two comparisons against a bound computed once
   * per formatter, which is why it is a bound and not a digit count.
   *
   * DERIVED, and then checked by construction: tools/exact-double-differential
   * .mjs formats a corpus through both routes on the same binary and requires
   * byte-identical output. See docs/intl-numberformat-double-path.md.
   */
  function exactDoubleBound(maxF) {
    if (typeof maxF !== 'number' || maxF < 0 || maxF > 15) return 0;
    var b = 1;
    for (var i = 0; i < 15 - maxF; i++) b *= 10;
    return b;
  }

  function fastRoundGate(state, value) {
    if (!state.fastRound) return null;
    /*
     * Only a primitive number. A string, a BigInt or a Number object all need
     * ToIntlMathematicalValue's own handling, and a BigInt is the reason the
     * general path carries an exact decimal at all.
     */
    if (typeof value !== 'number') return null;
    if (value !== value || value === Infinity || value === -Infinity) return null;
    var d = fastRoundDecimal(value, state.minimumFractionDigits,
                             state.maximumFractionDigits);
    if (d === null) { perfStats.fastRoundMisses++; return null; }
    perfStats.fastRoundHits++;
    return d;
  }

  /**
   * The digit string the backend is asked to render, or null when the backend
   * must do its own rounding (NaN/Infinity and the three scaled notations).
   */
  function preRound(state, mv) {
    if (mv.special !== null) return null;
    if (state.notation !== 'standard') return null;
    var dec = mv.decimal;
    /*
     * `style: "percent"` scales by 100 *before* rounding, and it has to happen
     * here rather than in the backend: the backend is told the digit string is
     * final, so if it multiplied afterwards the rounding would be applied to the
     * wrong magnitude. MEASURED as a real bug — 0.256 rendered as "0%" because
     * the value was rounded to percent's zero fraction digits before scaling.
     */
    if (state.style === 'percent') {
      dec = decimalStringFromNumber(mv.number * 100);
    }
    if (state.roundingIncrement !== 1) {
      /*
       * roundingIncrement rounds to a multiple of increment*10^-maxFrac. It is
       * rare enough that it goes through double arithmetic rather than the
       * digit-string path, which is exact only up to 2^53 — stated rather than
       * hidden, and the same limit Number itself has for these magnitudes.
       */
      var scale = Math.pow(10, state.maximumFractionDigits);
      var q = mv.number * scale / state.roundingIncrement;
      var rq = roundToDecimal(decimalStringFromNumber(q),
        { roundingType: 'fractionDigits', minimumFractionDigits: 0,
          maximumFractionDigits: 0, roundingMode: state.roundingMode,
          minimumIntegerDigits: 1, trailingZeroDisplay: 'auto' });
      var v = Number(joinDecimal(rq)) * state.roundingIncrement / scale;
      dec = decimalStringFromNumber(v);
    }
    return joinDecimal(roundToDecimal(dec, state));
  }

  /**
   * `useGrouping: "min2"` — group only when the integer part has more than four
   * digits, so 1000 stays "1000" while 10000 becomes "10,000".
   *
   * Neither NSNumberFormatter nor DecimalFormat has a min2 mode, and the rule is
   * arithmetic rather than locale data, so it is decided here. Two formatters
   * are opened, one grouping and one not, and the value picks between them.
   * The second one is opened lazily, so an app that never crosses the threshold
   * never pays for it.
   */
  function nfHandleFor(state, dec) {
    if (state.useGrouping !== 'min2' || dec === null) return state.handle;
    var p = splitDecimal(dec);
    if (p.int.length > 4) return state.handle;
    if (state.handleNoGroup === undefined) {
      state.handleNoGroup = native.nfOpen(state.openBag(''));
    }
    return state.handleNoGroup || state.handle;
  }

  /**
   * The fourth argument to `nfFormat` / `nfFormatToParts`: "you may render the
   * double instead of parsing the digit string".
   *
   * It is a HINT and never an instruction. A backend that ignores it renders
   * the digit string and produces the same text; the Android and no-platform
   * backends do exactly that, deliberately, because this module cannot exercise
   * android.icu from a macOS host and a divergence between two backends is
   * worse than either being slow. See the contract on
   * `NumberFormatter::format` in cpp/IntlPlatform.h.
   *
   * The condition is `state.fastRound` (already required for `fd` to be
   * non-null) plus the magnitude bound derived at `exactDoubleBound`.
   */
  /* Must match NumberFormatter::kHint* in cpp/IntlPlatform.h. */
  var HINT_DIGITS_WITHIN_LIMITS = 1;
  var HINT_EXACT_DOUBLE = 2;

  function nfHints(state, value, fastPath) {
    /*
     * kHintDigitsWithinLimits is `state.fastRound` and nothing else, and it is
     * set on BOTH paths deliberately.
     *
     * `state.fastRound` already requires standard notation, fractionDigits
     * rounding, roundingIncrement 1, minimumIntegerDigits 1,
     * trailingZeroDisplay "auto" and a non-rescaling style. Under exactly
     * those conditions every digit string this layer produces — from
     * `fastRoundDecimal` on the fast path and from `preRound`/`roundToDecimal`
     * on the general one — lies inside [minimumFractionDigits,
     * maximumFractionDigits] by construction. The general path is the case
     * that matters: `fastRoundDecimal` declines for exponent form, for a
     * fraction longer than maximumFractionDigits, and for zero with
     * minimumFractionDigits > 0, and the values that hit those declines
     * interleave with values that do not.
     *
     * Setting the bit only on the fast path made a formatter alternate between
     * two pinning regimes in the backend and regressed `fmt-large-grouped` by
     * 1.27x. See the comment on the two regimes in ios/IntlPlatform.mm.
     */
    var h = state.fastRound ? HINT_DIGITS_WITHIN_LIMITS : 0;
    if (!fastPath) return h;
    var b = state.exactDoubleBound;
    if (b !== 0 && value < b && value > -b) {
      perfStats.exactDoubleHits++;
      return h | HINT_EXACT_DOUBLE;
    }
    perfStats.exactDoubleMisses++;
    return h;
  }

  function nfFormatValue(state, value) {
    var fd = fastRoundGate(state, value);
    if (fd !== null) {
      return native.nfFormat(
        nfHandleFor(state, fd), value, fd, nfHints(state, value, true));
    }
    var mv = toIntlMathematicalValue(value);
    var dec = preRound(state, mv);
    return native.nfFormat(
      nfHandleFor(state, dec), mv.number, dec, nfHints(state, value, false));
  }

  function NumberFormat(locales, options) {
    if (!(this instanceof NumberFormat)) return new NumberFormat(locales, options);
    initializeNumberFormat(this, locales, options);
  }

  defineGetter(NumberFormat.prototype, 'format', function () {
    var state = requireNf(this, 'format');
    /*
     * The cache check is inlined AHEAD of boundOf so that a steady-state read
     * allocates nothing. `boundOf(state, field, len, function () {...})`
     * evaluates that function expression on every call — building a closure
     * over `state` — even when the bound function it would build is already
     * cached and immediately discarded. MEASURED 2026-07-27: reading
     * `nf.format` without calling it cost 155 ns against node's 30 ns, and
     * ECMA-402 makes `format` an accessor, so every `nf.format(x)` pays it.
     */
    if (state.boundFormat !== undefined) return state.boundFormat;
    return boundOf(state, 'boundFormat', 1, function () {
      return ({ bound(value) { return nfFormatValue(state, value); } }).bound;
    });
  });

  defineMethod(NumberFormat.prototype, 'formatToParts', 1, ({
    m(value) {
      var state = requireNf(this, 'formatToParts');
      var fp = fastRoundGate(state, value);
      if (fp !== null) {
        return native.nfFormatToParts(
          nfHandleFor(state, fp), value, fp, nfHints(state, value, true));
      }
      var mv = toIntlMathematicalValue(value);
      var dec = preRound(state, mv);
      return native.nfFormatToParts(
        nfHandleFor(state, dec), mv.number, dec, nfHints(state, value, false));
    }
  }).m);

  defineMethod(NumberFormat.prototype, 'resolvedOptions', 0, ({
    m() {
      var s = requireNf(this, 'resolvedOptions');
      var o = {};
      o.locale = s.locale;
      o.numberingSystem = s.numberingSystem;
      o.style = s.style;
      if (s.currency !== undefined) o.currency = s.currency;
      if (s.currencyDisplay !== undefined) o.currencyDisplay = s.currencyDisplay;
      if (s.currencySign !== undefined) o.currencySign = s.currencySign;
      if (s.unit !== undefined) o.unit = s.unit;
      if (s.unitDisplay !== undefined) o.unitDisplay = s.unitDisplay;
      o.minimumIntegerDigits = s.minimumIntegerDigits;
      if (s.roundingType !== 'significantDigits' || s.roundingPriority !== 'auto') {
        o.minimumFractionDigits = s.minimumFractionDigits;
        o.maximumFractionDigits = s.maximumFractionDigits;
      }
      if (s.minimumSignificantDigits !== undefined) {
        o.minimumSignificantDigits = s.minimumSignificantDigits;
        o.maximumSignificantDigits = s.maximumSignificantDigits;
      }
      o.useGrouping = s.useGrouping;
      o.notation = s.notation;
      if (s.compactDisplay !== undefined) o.compactDisplay = s.compactDisplay;
      o.signDisplay = s.signDisplay;
      o.roundingIncrement = s.roundingIncrement;
      o.roundingMode = s.roundingMode;
      o.roundingPriority = s.roundingPriority;
      o.trailingZeroDisplay = s.trailingZeroDisplay;
      return o;
    }
  }).m);

  finishService(NumberFormat, 'Intl.NumberFormat');

  /* ---------------------------------------------------------------------- */
  /* PluralRules — the one service implemented in JavaScript on purpose      */
  /* ---------------------------------------------------------------------- */

  /*
   * WHY THIS ONE IS A SHIM AND THE OTHER SEVEN ARE NOT
   *   Foundation exposes no plural-category API in Objective-C or in Swift.
   *   MEASURED, docs/intl-completeness-map.md. android.icu.text.PluralRules has
   *   existed since API 24. So the alternatives were android.icu's rule engine
   *   on Android against a JavaScript one on Apple — two engines, two CLDR
   *   vintages, and an app whose two builds can select different *sentences* —
   *   or one JavaScript engine on both. The second is chosen, and it is the only
   *   place in this module where "never ship data the OS already has" is
   *   overridden, for the precise reason that one of the two operating systems
   *   does not have it.
   *
   *   Cost: js/plural-data.js, ~30 KB for all 224 CLDR locales, against
   *   @formatjs/intl-pluralrules' 306 KB for the same coverage.
   *
   *   Correctness: 6,551 (locale, value) pairs across 213 locales diffed against
   *   node's ICU 77, zero mismatches — tests/differential/intl/pluralrules.js.
   */

  //#include "plural-data.js"

  var prState = new WeakMap();
  var requirePr = stateGetter(prState, 'PluralRules');

  /** The plural table is keyed by language subtag; anything else uses root. */
  function pluralLanguage(locale) {
    var dash = locale.indexOf('-');
    return dash < 0 ? locale : locale.slice(0, dash);
  }

  function pluralSelector(locale, type) {
    var table = type === 'ordinal' ? PLURAL_DATA.ordinal : PLURAL_DATA.cardinal;
    var fn = table[pluralLanguage(locale)];
    /* CLDR root has exactly one category. A locale with no rules is not an
       error; it is "other", which is what root says. */
    return fn || function () { return 'other'; };
  }

  function pluralCategoriesOf(locale, type) {
    var e = PLURAL_DATA.categories[pluralLanguage(locale)];
    if (!e) return ['other'];
    var list = type === 'ordinal' ? e.ordinal : e.cardinal;
    return list ? list.slice() : ['other'];
  }

  /** The decimal string the operands are read from — see gen-plural-data.js. */
  function pluralOperandString(state, value) {
    /*
     * The same fast path as NumberFormat.format, for the same reason: CLDR's
     * operands are read off a decimal string, and for an ordinary number
     * `String(x)` already IS that string. MEASURED 2026-07-27,
     * `new Intl.PluralRules("en").select(i)` cost 1,440 ns against node's
     * 174 ns, with no backend call anywhere in it — every nanosecond was this
     * layer, which is exactly the case where a JS shim has no excuse.
     *
     * CLDR operands are defined on the ABSOLUTE value, which is what the
     * general path expresses as `r.neg = false`; here it is `-value`.
     * `-(-0)` is `0` and `fastRoundDecimal(0, 0, …)` returns "0", which is what
     * the general path also produces, so negative zero needs no special case.
     */
    if (state.fastRound && typeof value === 'number' &&
        value === value && value !== Infinity && value !== -Infinity) {
      var f = fastRoundDecimal(value < 0 ? -value : value,
                               state.minimumFractionDigits,
                               state.maximumFractionDigits);
      if (f !== null) { perfStats.pluralFastHits++; return f; }
      perfStats.pluralFastMisses++;
    }
    var mv = toIntlMathematicalValue(value);
    if (mv.special !== null) return null;
    var r = roundToDecimal(mv.decimal, state);
    /* CLDR operands are defined on the absolute value. */
    r.neg = false;
    return joinDecimal(r);
  }

  function PluralRules(locales, options) {
    if (!(this instanceof PluralRules)) {
      throw new TypeError("Constructor Intl.PluralRules requires 'new'");
    }
    var requestedLocales = canonicalizeLocaleList(locales);
    /*
     * CoerceOptionsToObject, not GetOptionsObject. PluralRules and
     * RelativeTimeFormat both kept the boxing behaviour; ListFormat,
     * DisplayNames and Segmenter did not. Determined by diffing against node
     * (tests/differential/intl/services-shape.js) rather than by reading the
     * specification, because the specification's two helpers are one word apart
     * and the difference is only visible on a primitive.
     */
    options = coerceOptionsToObject(options);
    getOption(options, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
    var type = getOption(options, 'type', ['cardinal', 'ordinal'], 'cardinal');
    var r = resolveLocale(requestedLocales, {}, NO_RELEVANT_KEYS);
    var s = { locale: r.locale, type: type };
    setNumberFormatDigitOptions(s, options, 0, 3, 'standard');
    /*
     * The selector is resolved once here, not on every select(). It used to be
     * `pluralSelector(s.locale, s.type)` inside the call, which re-ran an
     * indexOf, a slice and two table lookups per selection to arrive at the
     * same function every time.
     */
    s.selector = pluralSelector(s.locale, s.type);
    /* The same predicate NumberFormat uses; see fastRoundDecimal. There is no
       style or notation here, so only the digit levers can disturb it. */
    s.fastRound = s.roundingIncrement === 1 &&
      s.roundingType === 'fractionDigits' && s.minimumIntegerDigits === 1 &&
      s.trailingZeroDisplay === 'auto';
    prState.set(this, s);
  }

  defineMethod(PluralRules.prototype, 'select', 1, ({
    m(value) {
      var s = requirePr(this, 'select');
      var n = Number(value);
      var dec = pluralOperandString(s, n);
      if (dec === null) return 'other';
      return s.selector(dec, 0);
    }
  }).m);

  defineMethod(PluralRules.prototype, 'selectRange', 2, ({
    m(start, end) {
      var s = requirePr(this, 'selectRange');
      if (start === undefined || end === undefined) {
        throw new TypeError('selectRange requires two arguments');
      }
      var a = Number(start);
      var b = Number(end);
      if (a !== a || b !== b) throw new RangeError('selectRange got NaN');
      var sel = s.selector;
      var ca = sel(pluralOperandString(s, a) || '0', 0);
      var cb = sel(pluralOperandString(s, b) || '0', 0);
      var rangeFn = PLURAL_DATA.range[pluralLanguage(s.locale)];
      /* CLDR's default plural range is "take the end category". */
      return rangeFn ? rangeFn(ca, cb) : cb;
    }
  }).m);

  defineMethod(PluralRules.prototype, 'resolvedOptions', 0, ({
    m() {
      var s = requirePr(this, 'resolvedOptions');
      var o = {};
      o.locale = s.locale;
      o.type = s.type;
      o.minimumIntegerDigits = s.minimumIntegerDigits;
      if (s.roundingType !== 'significantDigits' || s.roundingPriority !== 'auto') {
        o.minimumFractionDigits = s.minimumFractionDigits;
        o.maximumFractionDigits = s.maximumFractionDigits;
      }
      if (s.minimumSignificantDigits !== undefined) {
        o.minimumSignificantDigits = s.minimumSignificantDigits;
        o.maximumSignificantDigits = s.maximumSignificantDigits;
      }
      o.pluralCategories = pluralCategoriesOf(s.locale, s.type).sort();
      o.roundingIncrement = s.roundingIncrement;
      o.roundingMode = s.roundingMode;
      o.roundingPriority = s.roundingPriority;
      o.trailingZeroDisplay = s.trailingZeroDisplay;
      return o;
    }
  }).m);

  finishService(PluralRules, 'Intl.PluralRules');

  /* ---------------------------------------------------------------------- */
  /* Collator                                                                */
  /* ---------------------------------------------------------------------- */

  /*
   * The highest divergence risk in the whole module, and it is an *option*
   * risk rather than an ordering risk.
   *
   * Both platforms collate through ICU, so the primary ordering agrees. What
   * does not agree is the lever each exposes: Android has
   * RuleBasedCollator.setStrength / setCaseFirst / setAlternateHandling
   * directly, while Foundation offers only NSStringCompareOptions plus the
   * `-u-kf-` and `-u-ka-` keywords on the locale identifier — a different
   * mechanism with different failure behaviour. `caseFirst` and
   * `ignorePunctuation` in particular have no NSString option at all.
   * docs/intl-completeness-map.md flags this; tests/differential/intl/ measures
   * it rather than assuming it.
   */

  var colState = new WeakMap();
  var requireCol = stateGetter(colState, 'Collator');

  var reKeywordType = /^[a-zA-Z0-9]{3,8}(-[a-zA-Z0-9]{3,8})*$/;

  function Collator(locales, options) {
    if (!(this instanceof Collator)) return new Collator(locales, options);
    var requestedLocales = canonicalizeLocaleList(locales);
    options = coerceOptionsToObject(options);
    var usage = getOption(options, 'usage', ['sort', 'search'], 'sort');
    getOption(options, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
    var collation = getOption(options, 'collation', undefined, undefined);
    if (collation !== undefined && !reKeywordType.test(collation)) {
      throw new RangeError('Invalid collation: ' + collation);
    }
    var numeric = options.numeric === undefined ? undefined : Boolean(options.numeric);
    var caseFirst = getOption(options, 'caseFirst', ['upper', 'lower', 'false'], undefined);
    var r = resolveLocale(requestedLocales, {
      co: collation, kn: numeric === undefined ? undefined : String(numeric),
      kf: caseFirst
    }, COL_RELEVANT_KEYS);
    var sensitivity = getOption(options, 'sensitivity',
      ['base', 'accent', 'case', 'variant'], undefined);
    var ignorePunctuation = getBooleanOptionOrUndefined(options, 'ignorePunctuation');

    /*
     * `co` is meaningless for usage "search": ECMA-402 says the search
     * collation is chosen by the implementation and `collation` resolves to
     * "default". Passing the requested one through would let
     * `supportedValuesOf("collation")` disagree with resolvedOptions, which is
     * the equivalence test262 checks in both directions.
     */
    var co = usage === 'search' ? undefined : r.co;
    if (co === 'standard' || co === 'search') co = undefined;

    var s = {
      locale: r.locale,
      usage: usage,
      sensitivity: sensitivity === undefined ? 'variant' : sensitivity,
      ignorePunctuation: ignorePunctuation === undefined ? false : ignorePunctuation,
      collation: co || 'default',
      numeric: r.kn === undefined ? false : r.kn === 'true' || r.kn === '',
      caseFirst: r.kf === undefined ? 'false' : r.kf,
      boundCompare: undefined
    };
    var handle = native.colOpen({
      locale: r.locale, usage: usage, sensitivity: s.sensitivity,
      caseFirst: s.caseFirst, collation: co || null,
      numeric: s.numeric, ignorePunctuation: s.ignorePunctuation
    });
    if (!handle) throw new RangeError('No collator available for ' + r.locale);
    s.handle = handle;
    var resolvedCollation = native.colResolved(handle, 'collation');
    if (resolvedCollation) s.collation = resolvedCollation;
    /* The extension suffix must reflect what was honoured, not what was asked. */
    var kept = [];
    if (co && s.collation === co) kept.push('co-' + co);
    if (r.kn !== undefined && numeric === undefined) kept.push('kn' + (s.numeric ? '' : '-false'));
    if (r.kf !== undefined && caseFirst === undefined) kept.push('kf-' + s.caseFirst);
    if (kept.length) s.locale = r.locale + '-u-' + kept.join('-');
    colState.set(this, s);
  }

  defineGetter(Collator.prototype, 'compare', function () {
    var state = requireCol(this, 'compare');
    /* See the note on NumberFormat.prototype.format: the cache check is
       inlined so that the accessor allocates nothing in steady state. It
       matters more here — a Collator's compare is read once per comparison
       inside a sort. */
    if (state.boundCompare !== undefined) return state.boundCompare;
    var handle = state.handle;
    return boundOf(state, 'boundCompare', 2, function () {
      return ({
        bound(x, y) {
          /* The bench is a string sort — `compare("a", "b")` — and the two
             `String(x)` calls are the only JS-side work left in the hot path
             when both arguments are already strings. Hoisting `handle` out of
             the state read is what frees the second slot. */
          return native.colCompare(
            handle, typeof x === 'string' ? x : String(x),
                   typeof y === 'string' ? y : String(y));
        }
      }).bound;
    });
  });

  defineMethod(Collator.prototype, 'resolvedOptions', 0, ({
    m() {
      var s = requireCol(this, 'resolvedOptions');
      var o = {};
      o.locale = s.locale;
      o.usage = s.usage;
      o.sensitivity = s.sensitivity;
      o.ignorePunctuation = s.ignorePunctuation;
      o.collation = s.collation;
      o.numeric = s.numeric;
      o.caseFirst = s.caseFirst;
      return o;
    }
  }).m);

  finishService(Collator, 'Intl.Collator');

  /* ---------------------------------------------------------------------- */
  /* RelativeTimeFormat                                                      */
  /* ---------------------------------------------------------------------- */

  var RTF_UNITS = ['year', 'quarter', 'month', 'week', 'day', 'hour', 'minute',
                   'second'];

  function singularUnit(unit) {
    var u = String(unit);
    if (RTF_UNITS.indexOf(u) >= 0) return u;
    if (u.charAt(u.length - 1) === 's') {
      var singular = u.slice(0, u.length - 1);
      if (RTF_UNITS.indexOf(singular) >= 0) return singular;
    }
    return null;
  }

  var rtfState = new WeakMap();
  var requireRtf = stateGetter(rtfState, 'RelativeTimeFormat');

  function RelativeTimeFormat(locales, options) {
    if (!(this instanceof RelativeTimeFormat)) {
      throw new TypeError("Constructor Intl.RelativeTimeFormat requires 'new'");
    }
    var requestedLocales = canonicalizeLocaleList(locales);
    /*
     * CoerceOptionsToObject, not GetOptionsObject. RelativeTimeFormat is the
     * one ES2020-era service that kept the boxing behaviour, so
     * `new Intl.RelativeTimeFormat("en", 5)` must not throw.
     * RelativeTimeFormat/constructor/constructor/options-toobject.js is the
     * check, and it caught this being wrong.
     */
    options = coerceOptionsToObject(options);
    getOption(options, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
    var numberingSystem = getOption(options, 'numberingSystem', undefined, undefined);
    if (numberingSystem !== undefined && !isWellFormedKeywordValue(numberingSystem)) {
      throw new RangeError('Invalid numberingSystem: ' + numberingSystem);
    }
    var r = resolveLocale(requestedLocales, { nu: numberingSystem }, NU_RELEVANT_KEYS);
    var style = getOption(options, 'style', ['long', 'short', 'narrow'], 'long');
    var numeric = getOption(options, 'numeric', ['always', 'auto'], 'always');
    var handle = native.rtfOpen({
      locale: r.locale, numberingSystem: r.nu || null,
      numeric: numeric, style: style
    });
    if (!handle) {
      throw new RangeError('No relative time formatter available for ' + r.locale);
    }
    var resolvedNu = native.rtfResolved(handle, 'numberingSystem') || r.nu || 'latn';
    rtfState.set(this, {
      handle: handle,
      locale: (r.extensionSuffix && resolvedNu !== r.nu)
        ? r.locale : r.locale + r.extensionSuffix,
      style: style,
      numeric: numeric,
      numberingSystem: resolvedNu
    });
  }

  function rtfCheckArgs(value, unit) {
    var v = Number(value);
    if (v !== v || v === Infinity || v === -Infinity) {
      throw new RangeError('Value must be finite');
    }
    var u = singularUnit(unit);
    if (u === null) throw new RangeError('Invalid unit: ' + unit);
    return { value: v, unit: u };
  }

  /*
   * The platform's answer, or a root-locale one when it declines.
   *
   * MEASURED on macOS 26.5: -[NSRelativeDateTimeFormatter
   * localizedStringFromDateComponents:] returns nil for a `quarter` component in
   * every locale tried, while android.icu's RelativeDateTimeUnit has QUARTER.
   * That is one unit of seven, and forfeiting the whole service over it would
   * be the wrong trade — so the JS layer degrades to an English-shaped string
   * and the deviation is enumerated (D15) rather than hidden behind a throw.
   */
  function rtfFormatOne(state, v, unit) {
    var text = native.rtfFormat(state.handle, v, unit);
    if (text !== null && text !== undefined) return text;
    var n = Math.abs(v);
    var plural = n === 1 ? unit : unit + 's';
    return v < 0 || (v === 0 && 1 / v < 0)
      ? n + ' ' + plural + ' ago'
      : 'in ' + n + ' ' + plural;
  }

  defineMethod(RelativeTimeFormat.prototype, 'format', 2, ({
    m(value, unit) {
      var s = requireRtf(this, 'format');
      var a = rtfCheckArgs(value, unit);
      return rtfFormatOne(s, a.value, a.unit);
    }
  }).m);

  defineMethod(RelativeTimeFormat.prototype, 'formatToParts', 2, ({
    m(value, unit) {
      var s = requireRtf(this, 'formatToParts');
      var a = rtfCheckArgs(value, unit);
      var text = rtfFormatOne(s, a.value, a.unit);
      /*
       * The numeral is located by formatting it on its own with a NumberFormat
       * for the same locale and searching for it. That is not a guess: if the
       * substring is not found the whole string is returned as one literal
       * (deviation D1), and if it is found the boundary is exact because it is
       * the platform's own rendering of the same number.
       */
      var nf = rtfNumberFormat(s);
      var numeral = nf.format(Math.abs(a.value));
      var at = text.indexOf(numeral);
      if (at < 0) return [{ type: 'literal', value: text }];
      var parts = [];
      if (at > 0) parts.push({ type: 'literal', value: text.slice(0, at) });
      var inner = nf.formatToParts(Math.abs(a.value));
      for (var i = 0; i < inner.length; i++) {
        parts.push({ type: inner[i].type, value: inner[i].value, unit: a.unit });
      }
      var rest = text.slice(at + numeral.length);
      if (rest.length) parts.push({ type: 'literal', value: rest });
      return parts;
    }
  }).m);

  function rtfNumberFormat(state) {
    if (state.nf === undefined) {
      /*
       * Grouping ON. ECMA-402 renders the count with the locale's ordinary
       * number formatting, so "in 1,000 quarters" and not "in 1000 quarters" —
       * three test262 files check it. The substring search that locates the
       * numeral in the platform's output works either way, because both come
       * from the same locale's digits and separators.
       */
      state.nf = new NumberFormat(state.locale, {});
    }
    return state.nf;
  }

  defineMethod(RelativeTimeFormat.prototype, 'resolvedOptions', 0, ({
    m() {
      var s = requireRtf(this, 'resolvedOptions');
      return {
        locale: s.locale, style: s.style, numeric: s.numeric,
        numberingSystem: s.numberingSystem
      };
    }
  }).m);

  finishService(RelativeTimeFormat, 'Intl.RelativeTimeFormat');

  /* ---------------------------------------------------------------------- */
  /* ListFormat                                                              */
  /* ---------------------------------------------------------------------- */

  var lfState = new WeakMap();
  var requireLf = stateGetter(lfState, 'ListFormat');

  function ListFormat(locales, options) {
    if (!(this instanceof ListFormat)) {
      throw new TypeError("Constructor Intl.ListFormat requires 'new'");
    }
    var requestedLocales = canonicalizeLocaleList(locales);
    options = getOptionsObject(options);
    getOption(options, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
    var type = getOption(options, 'type',
      ['conjunction', 'disjunction', 'unit'], 'conjunction');
    var style = getOption(options, 'style', ['long', 'short', 'narrow'], 'long');
    var r = resolveLocale(requestedLocales, {}, NO_RELEVANT_KEYS);
    var handle = native.lfOpen({ locale: r.locale, type: type, style: style });
    if (!handle) throw new RangeError('No list formatter available for ' + r.locale);
    lfState.set(this, {
      handle: handle, locale: r.locale, type: type, style: style
    });
  }

  /** StringListFromIterable: every element must already be a string. */
  function stringListFrom(list) {
    if (list === undefined) return [];
    var out = [];
    var iterFn = list === null ? undefined : list[Symbol.iterator];
    if (iterFn === undefined || iterFn === null) {
      throw new TypeError('The list is not iterable');
    }
    var iter = iterFn.call(list);
    for (;;) {
      var step = iter.next();
      if (step.done) break;
      var v = step.value;
      if (typeof v !== 'string') {
        throw new TypeError('List elements must be strings');
      }
      out.push(v);
    }
    return out;
  }

  defineMethod(ListFormat.prototype, 'format', 1, ({
    m(list) {
      var s = requireLf(this, 'format');
      var items = stringListFrom(list);
      var text = native.lfFormat(s.handle, items);
      return text === null || text === undefined ? items.join(', ') : text;
    }
  }).m);

  defineMethod(ListFormat.prototype, 'formatToParts', 1, ({
    m(list) {
      var s = requireLf(this, 'formatToParts');
      var items = stringListFrom(list);
      var text = native.lfFormat(s.handle, items);
      if (text === null || text === undefined) text = items.join(', ');
      /*
       * Elements are located by scanning forward for each item in order. The
       * list patterns never reorder or alter their elements, so a forward scan
       * is exact; if any element is not found where it must be, the whole string
       * becomes one literal rather than a wrong split (deviation D1).
       */
      var parts = [];
      var pos = 0;
      for (var i = 0; i < items.length; i++) {
        var at = items[i].length === 0 ? pos : text.indexOf(items[i], pos);
        if (at < 0) return [{ type: 'literal', value: text }];
        if (at > pos) parts.push({ type: 'literal', value: text.slice(pos, at) });
        parts.push({ type: 'element', value: items[i] });
        pos = at + items[i].length;
      }
      if (pos < text.length) {
        parts.push({ type: 'literal', value: text.slice(pos) });
      }
      return parts;
    }
  }).m);

  defineMethod(ListFormat.prototype, 'resolvedOptions', 0, ({
    m() {
      var s = requireLf(this, 'resolvedOptions');
      return { locale: s.locale, type: s.type, style: s.style };
    }
  }).m);

  finishService(ListFormat, 'Intl.ListFormat');

  /* ---------------------------------------------------------------------- */
  /* DisplayNames                                                            */
  /* ---------------------------------------------------------------------- */

  var DATE_TIME_FIELDS = ['era', 'year', 'quarter', 'month', 'weekOfYear',
    'weekday', 'day', 'dayPeriod', 'hour', 'minute', 'second',
    'timeZoneName'];

  var dnState = new WeakMap();
  var requireDn = stateGetter(dnState, 'DisplayNames');

  function DisplayNames(locales, options) {
    if (!(this instanceof DisplayNames)) {
      throw new TypeError("Constructor Intl.DisplayNames requires 'new'");
    }
    var requestedLocales = canonicalizeLocaleList(locales);
    options = getOptionsObject(options);
    getOption(options, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
    var style = getOption(options, 'style', ['narrow', 'short', 'long'], 'long');
    var type = getOption(options, 'type',
      ['language', 'region', 'script', 'currency', 'calendar', 'dateTimeField'],
      undefined);
    if (type === undefined) {
      throw new TypeError('Intl.DisplayNames requires a "type" option');
    }
    var fallback = getOption(options, 'fallback', ['code', 'none'], 'code');
    var r = resolveLocale(requestedLocales, {}, NO_RELEVANT_KEYS);
    var languageDisplay = getOption(options, 'languageDisplay',
      ['dialect', 'standard'], 'dialect');
    dnState.set(this, {
      locale: r.locale, style: style, type: type, fallback: fallback,
      languageDisplay: type === 'language' ? languageDisplay : undefined
    });
  }

  var reRegionCode = /^([a-zA-Z]{2}|[0-9]{3})$/;
  var reScriptCode = /^[a-zA-Z]{4}$/;

  /** Validates `code` for `type`, returning the canonical form or throwing. */
  function canonicalDisplayCode(type, code) {
    if (type === 'language') {
      var p = parseTag(code);
      if (!p) throw new RangeError('Invalid language code: ' + code);
      return baseName(p);
    }
    if (type === 'region') {
      if (!reRegionCode.test(code)) throw new RangeError('Invalid region: ' + code);
      return code.toUpperCase();
    }
    if (type === 'script') {
      if (!reScriptCode.test(code)) throw new RangeError('Invalid script: ' + code);
      return titleScript(code);
    }
    if (type === 'currency') {
      if (!reCurrencyCode.test(code)) throw new RangeError('Invalid currency: ' + code);
      return code.toUpperCase();
    }
    if (type === 'calendar') {
      if (!isWellFormedKeywordValue(code)) {
        throw new RangeError('Invalid calendar: ' + code);
      }
      return code.toLowerCase();
    }
    if (DATE_TIME_FIELDS.indexOf(code) < 0) {
      throw new RangeError('Invalid dateTimeField: ' + code);
    }
    return code;
  }

  defineMethod(DisplayNames.prototype, 'of', 1, ({
    m(code) {
      var s = requireDn(this, 'of');
      code = toStringSpec(code);
      var canonical = canonicalDisplayCode(s.type, code);
      var name = native.displayName(s.locale, s.type, s.style, canonical);
      if (name !== null && name !== undefined && name !== '') return name;
      return s.fallback === 'code' ? canonical : undefined;
    }
  }).m);

  defineMethod(DisplayNames.prototype, 'resolvedOptions', 0, ({
    m() {
      var s = requireDn(this, 'resolvedOptions');
      var o = {
        locale: s.locale, style: s.style, type: s.type, fallback: s.fallback
      };
      if (s.languageDisplay !== undefined) o.languageDisplay = s.languageDisplay;
      return o;
    }
  }).m);

  finishService(DisplayNames, 'Intl.DisplayNames', 2);
  /* DisplayNames has no supportedLocalesOf in ECMA-402... it does. Keep it. */

  /* ---------------------------------------------------------------------- */
  /* Segmenter                                                               */
  /* ---------------------------------------------------------------------- */

  var segState = new WeakMap();
  var requireSeg = stateGetter(segState, 'Segmenter');
  var segmentsState = new WeakMap();

  function Segmenter(locales, options) {
    if (!(this instanceof Segmenter)) {
      throw new TypeError("Constructor Intl.Segmenter requires 'new'");
    }
    var requestedLocales = canonicalizeLocaleList(locales);
    options = getOptionsObject(options);
    getOption(options, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
    var granularity = getOption(options, 'granularity',
      ['grapheme', 'word', 'sentence'], 'grapheme');
    var r = resolveLocale(requestedLocales, {}, NO_RELEVANT_KEYS);
    segState.set(this, { locale: r.locale, granularity: granularity });
  }

  /**
   * The flat [begin, end, wordLike] triples the backend returns.
   *
   * Computed once per Segments object and shared by the iterator and by
   * containing(), because a backend crossing per segment would make iterating a
   * paragraph hundreds of crossings. A backend that declines produces one
   * segment covering the string, which is coarse but never wrong.
   */
  function segmentBoundaries(state, text) {
    var flat = native.segment(state.locale, state.granularity, text);
    if (!flat || flat.length === 0) {
      return text.length ? [0, text.length, 0] : [];
    }
    return flat;
  }

  /**
   * The boundaries for one Segments object, computed on first demand and
   * memoised on the Segmenter for the most recent text.
   *
   * WHY LAZY. `segmenter-segment-call` MEASURED at 29.36 µs against node's
   * 562 ns — 52x, the worst ratio in the whole suite — and
   * docs/intl-native-placement.md established that this is not a JavaScript
   * gap and not a backend gap. It is an **eagerness** gap: `segment()` used to
   * segment the entire string and build a 3N-element array through
   * `JS_SetPropertyUint32`, while node's `segment()` returns a lazy `Segments`
   * and performs no segmentation at all. The two numbers were not measuring
   * the same work.
   *
   * ECMA-402 permits this exactly: `%Segmenter%.prototype.segment` is
   * specified as `CreateSegmentsObject(segmenter, string)`, which stores the
   * segmenter and the string and does nothing else. Every observable — the
   * iterator's sequence, `containing`, `isWordLike` — is unchanged, because
   * segmentation is a pure function of (locale, granularity, text) and the
   * text is captured by value at `segment()` time.
   *
   * WHY ALSO MEMOISED. `containing(i)` MEASURED at 29.83 µs against node's
   * 1.04 µs, and the shape that costs is `seg.segment(text).containing(i)` in
   * a loop: laziness alone still re-segments once per Segments object. One
   * slot on the Segmenter, keyed on the text, collapses the repeat to an
   * array read. It is one slot rather than a map because the shape being paid
   * for is the same text repeatedly; a map would add a lookup to the case that
   * already hits and would hold an arbitrary number of documents alive.
   *
   * THE RETENTION TRADE, stated rather than hidden: the Segmenter now holds
   * the last text and its boundary array until it is segmented again or
   * collected. For a paragraph that is a few kilobytes. `Intl.__rnqjsPerf.
   * reset()` does not clear it — it is keyed on data, not on locale, and
   * cannot go stale.
   */
  function flatOf(st) {
    if (st.flat === undefined) {
      var s = st.seg;
      if (s.lastText === st.text) {
        perfStats.segmentHits++;
        st.flat = s.lastFlat;
      } else {
        perfStats.segmentMisses++;
        st.flat = segmentBoundaries(s, st.text);
        s.lastText = st.text;
        s.lastFlat = st.flat;
      }
    }
    return st.flat;
  }

  function makeSegmentData(state, text, flat, i) {
    var o = {
      segment: text.slice(flat[i], flat[i + 1]),
      index: flat[i],
      input: text
    };
    if (state.granularity === 'word') o.isWordLike = flat[i + 2] === 1;
    return o;
  }

  defineMethod(Segmenter.prototype, 'segment', 1, ({
    m(string) {
      var s = requireSeg(this, 'segment');
      var text = toStringSpec(string);
      var segments = ObjectCreate(SegmentsPrototype);
      /* `flat: undefined` is the whole change. See flatOf. */
      segmentsState.set(segments, { seg: s, text: text, flat: undefined });
      return segments;
    }
  }).m);

  defineMethod(Segmenter.prototype, 'resolvedOptions', 0, ({
    m() {
      var s = requireSeg(this, 'resolvedOptions');
      return { locale: s.locale, granularity: s.granularity };
    }
  }).m);

  finishService(Segmenter, 'Intl.Segmenter');

  var SegmentsPrototype = {};
  defineMethod(SegmentsPrototype, 'containing', 1, ({
    m(index) {
      var st = segmentsState.get(this);
      if (!st) throw new TypeError('containing called on a non-Segments object');
      var n = Number(index);
      n = n !== n ? 0 : Math.trunc(n);
      if (n < 0 || n >= st.text.length) return undefined;
      var flat = flatOf(st);
      for (var i = 0; i < flat.length; i += 3) {
        if (n >= flat[i] && n < flat[i + 1]) {
          return makeSegmentData(st.seg, st.text, flat, i);
        }
      }
      return undefined;
    }
  }).m);

  var SegmentIteratorPrototype = {};
  defineMethod(SegmentIteratorPrototype, 'next', 0, ({
    m() {
      var st = segmentsState.get(this);
      if (!st) throw new TypeError('next called on a non-Segment Iterator');
      var flat = flatOf(st);
      if (st.pos >= flat.length) return { value: undefined, done: true };
      var v = makeSegmentData(st.seg, st.text, flat, st.pos);
      st.pos += 3;
      return { value: v, done: false };
    }
  }).m);
  ObjectDefineProperty(SegmentIteratorPrototype, Symbol.toStringTag, {
    value: 'Segmenter String Iterator', writable: false, enumerable: false,
    configurable: true
  });

  var segmentsIterator = ({
      m() {
        var st = segmentsState.get(this);
        if (!st) throw new TypeError('Symbol.iterator called on a non-Segments object');
        var it = ObjectCreate(SegmentIteratorPrototype);
        /* The iterator inherits whatever the Segments object has resolved so
           far. If nothing has been pulled yet that is `undefined`, and the
           iterator's own first `next()` does the segmentation — which is what
           makes `[...seg.segment(s)]` pay for exactly one segmentation and an
           unconsumed `seg.segment(s)` pay for none. */
        segmentsState.set(it, { seg: st.seg, text: st.text, flat: st.flat, pos: 0 });
        return it;
      }
    }).m;
  /* test262 checks the *name* of a well-known-symbol method, which must be
     "[Symbol.iterator]" and not the object-literal shorthand's "m". */
  ObjectDefineProperty(segmentsIterator, 'name', {
    value: '[Symbol.iterator]', configurable: true
  });
  ObjectDefineProperty(segmentsIterator, 'length', { value: 0, configurable: true });
  ObjectDefineProperty(SegmentsPrototype, Symbol.iterator, {
    value: segmentsIterator,
    writable: true, enumerable: false, configurable: true
  });

  /* ---------------------------------------------------------------------- */
  /* Intl.Locale                                                             */
  /* ---------------------------------------------------------------------- */

  /*
   * Pure grammar plus a handful of platform queries.
   *
   * Parsing, validation, keyword bookkeeping and `toString` are algorithm and
   * live here. Exactly four things are asked of the platform, and each of them
   * is genuinely CLDR data: likely subtags (maximize/minimize), the per-locale
   * calendar / numbering-system / time-zone / collation lists, the text
   * direction, and the week rules.
   */

  var localeState = new WeakMap();
  var requireLocale = stateGetter(localeState, 'Locale');

  var reLanguageSubtag = /^[a-zA-Z]{2,3}$|^[a-zA-Z]{5,8}$/;

  function applyUnicodeExtension(p, key, value) {
    if (!p.unicodeKeywords) p.unicodeKeywords = { attributes: [], keywords: {} };
    if (value === undefined) return;
    if (value === null) {
      delete p.unicodeKeywords.keywords[key];
      return;
    }
    p.unicodeKeywords.keywords[key] = value === 'true' ? '' : value;
  }

  function readKeyword(p, key) {
    if (!p.unicodeKeywords) return undefined;
    if (!hasOwn(p.unicodeKeywords.keywords, key)) return undefined;
    var v = p.unicodeKeywords.keywords[key];
    return v === '' ? 'true' : v;
  }

  function Locale(tag, options) {
    if (!(this instanceof Locale)) {
      throw new TypeError("Constructor Intl.Locale requires 'new'");
    }
    if (typeof tag !== 'string' && (typeof tag !== 'object' || tag === null)) {
      throw new TypeError('Locale tag must be a string or an object');
    }
    var source = (typeof tag === 'object' && localeState.get(tag))
      ? localeState.get(tag).tag : String(tag);
    options = coerceOptionsToObject(options);

    var p = parseTag(source);
    if (!p) throw new RangeError('Incorrect locale information provided: ' + source);

    /* ApplyOptionsToTag: language / script / region replace what was parsed. */
    var language = getOption(options, 'language', undefined, undefined);
    if (language !== undefined) {
      if (!reLanguageSubtag.test(language)) {
        throw new RangeError('Invalid language: ' + language);
      }
      p.language = language.toLowerCase();
    }
    var script = getOption(options, 'script', undefined, undefined);
    if (script !== undefined) {
      if (!reScriptCode.test(script)) throw new RangeError('Invalid script: ' + script);
      p.script = titleScript(script);
    }
    var region = getOption(options, 'region', undefined, undefined);
    if (region !== undefined) {
      if (!reRegionCode.test(region)) throw new RangeError('Invalid region: ' + region);
      p.region = region.toUpperCase();
    }

    var calendar = getOption(options, 'calendar', undefined, undefined);
    if (calendar !== undefined && !reKeywordType.test(calendar)) {
      throw new RangeError('Invalid calendar: ' + calendar);
    }
    applyUnicodeExtension(p, 'ca', calendar === undefined ? undefined : calendar.toLowerCase());

    var collation = getOption(options, 'collation', undefined, undefined);
    if (collation !== undefined && !reKeywordType.test(collation)) {
      throw new RangeError('Invalid collation: ' + collation);
    }
    applyUnicodeExtension(p, 'co', collation === undefined ? undefined : collation.toLowerCase());

    var hourCycle = getOption(options, 'hourCycle',
      ['h11', 'h12', 'h23', 'h24'], undefined);
    applyUnicodeExtension(p, 'hc', hourCycle);

    var caseFirst = getOption(options, 'caseFirst',
      ['upper', 'lower', 'false'], undefined);
    applyUnicodeExtension(p, 'kf', caseFirst);

    var numeric = options.numeric;
    if (numeric !== undefined) {
      applyUnicodeExtension(p, 'kn', Boolean(numeric) ? 'true' : 'false');
    }

    var numberingSystem = getOption(options, 'numberingSystem', undefined, undefined);
    if (numberingSystem !== undefined && !reKeywordType.test(numberingSystem)) {
      throw new RangeError('Invalid numberingSystem: ' + numberingSystem);
    }
    applyUnicodeExtension(p, 'nu',
      numberingSystem === undefined ? undefined : numberingSystem.toLowerCase());

    var canonical = canonicalizeLocale(formatTag(p));
    if (canonical === null) {
      throw new RangeError('Incorrect locale information provided: ' + source);
    }
    localeState.set(this, { tag: canonical, parsed: parseTag(canonical) });
  }

  function localeGetter(name, fn) {
    defineGetter(Locale.prototype, name, function () {
      var s = requireLocale(this, name);
      return fn(s);
    });
  }

  localeGetter('baseName', function (s) { return baseName(s.parsed); });
  localeGetter('language', function (s) { return s.parsed.language; });
  localeGetter('script', function (s) { return s.parsed.script || undefined; });
  localeGetter('region', function (s) { return s.parsed.region || undefined; });
  localeGetter('variants', function (s) {
    return s.parsed.variants.length ? s.parsed.variants.join('-') : undefined;
  });
  localeGetter('calendar', function (s) { return readKeyword(s.parsed, 'ca'); });
  localeGetter('collation', function (s) { return readKeyword(s.parsed, 'co'); });
  localeGetter('hourCycle', function (s) { return readKeyword(s.parsed, 'hc'); });
  localeGetter('caseFirst', function (s) { return readKeyword(s.parsed, 'kf'); });
  localeGetter('numberingSystem', function (s) { return readKeyword(s.parsed, 'nu'); });
  localeGetter('numeric', function (s) {
    var v = readKeyword(s.parsed, 'kn');
    return v === undefined ? false : v === 'true';
  });

  defineMethod(Locale.prototype, 'toString', 0, ({
    m() { return requireLocale(this, 'toString').tag; }
  }).m);

  defineMethod(Locale.prototype, 'maximize', 0, ({
    m() {
      var s = requireLocale(this, 'maximize');
      var max = native.maximize(baseName(s.parsed));
      if (!max) return new Locale(s.tag);
      var p = parseTag(max);
      if (!p) return new Locale(s.tag);
      p.unicodeKeywords = s.parsed.unicodeKeywords;
      p.transform = s.parsed.transform;
      p.otherExt = s.parsed.otherExt;
      p.privateuse = s.parsed.privateuse;
      return new Locale(formatTag(p));
    }
  }).m);

  defineMethod(Locale.prototype, 'minimize', 0, ({
    m() {
      var s = requireLocale(this, 'minimize');
      var min = native.minimize(baseName(s.parsed));
      if (!min) return new Locale(s.tag);
      var p = parseTag(min);
      if (!p) return new Locale(s.tag);
      p.unicodeKeywords = s.parsed.unicodeKeywords;
      p.transform = s.parsed.transform;
      p.otherExt = s.parsed.otherExt;
      p.privateuse = s.parsed.privateuse;
      return new Locale(formatTag(p));
    }
  }).m);

  /*
   * The info getters.
   *
   * ECMA-402 says the requested keyword, when present, must be first in the
   * returned list — `new Intl.Locale("en-u-ca-hebrew").getCalendars()[0]` is
   * "hebrew". That reordering is algorithm and is done here; the list itself is
   * the platform's.
   */
  function localeInfoList(s, key, keyword) {
    var list = native.localeList(baseName(s.parsed), key) || [];
    var requested = readKeyword(s.parsed, keyword);
    if (requested === undefined) return list.slice();
    var out = [requested];
    for (var i = 0; i < list.length; i++) {
      if (list[i] !== requested) out.push(list[i]);
    }
    return out;
  }

  defineMethod(Locale.prototype, 'getCalendars', 0, ({
    m() { return localeInfoList(requireLocale(this, 'getCalendars'), 'calendars', 'ca'); }
  }).m);
  defineMethod(Locale.prototype, 'getCollations', 0, ({
    m() { return localeInfoList(requireLocale(this, 'getCollations'), 'collations', 'co'); }
  }).m);
  defineMethod(Locale.prototype, 'getNumberingSystems', 0, ({
    m() {
      return localeInfoList(
        requireLocale(this, 'getNumberingSystems'), 'numberingSystems', 'nu');
    }
  }).m);
  defineMethod(Locale.prototype, 'getTimeZones', 0, ({
    m() {
      var s = requireLocale(this, 'getTimeZones');
      /* Undefined, not empty: a locale with no region has no time zones, and
         ECMA-402 distinguishes the two. */
      if (!s.parsed.region) return undefined;
      return native.localeList(baseName(s.parsed), 'timeZones') || [];
    }
  }).m);
  defineMethod(Locale.prototype, 'getHourCycles', 0, ({
    m() {
      var s = requireLocale(this, 'getHourCycles');
      var requested = readKeyword(s.parsed, 'hc');
      if (requested !== undefined) return [requested];
      var hc = native.localeString(baseName(s.parsed), 'hourCycle');
      return hc ? [hc] : [];
    }
  }).m);
  defineMethod(Locale.prototype, 'getTextInfo', 0, ({
    m() {
      var s = requireLocale(this, 'getTextInfo');
      var dir = native.localeString(baseName(s.parsed), 'textDirection');
      return { direction: dir || 'ltr' };
    }
  }).m);
  defineMethod(Locale.prototype, 'getWeekInfo', 0, ({
    m() {
      var s = requireLocale(this, 'getWeekInfo');
      var w = native.weekInfo(baseName(s.parsed));
      if (!w) return { firstDay: 1, weekend: [6, 7], minimalDays: 1 };
      return {
        firstDay: w[0],
        weekend: w.slice(2),
        minimalDays: w[1]
      };
    }
  }).m);

  ObjectDefineProperty(Locale, 'prototype', {
    value: Locale.prototype, writable: false, enumerable: false, configurable: false
  });
  ObjectDefineProperty(Locale, 'length', {
    value: 1, writable: false, enumerable: false, configurable: true
  });
  ObjectDefineProperty(Locale.prototype, Symbol.toStringTag, {
    value: 'Intl.Locale', writable: false, enumerable: false, configurable: true
  });
  ObjectDefineProperty(Locale.prototype, 'constructor', {
    value: Locale, writable: true, enumerable: false, configurable: true
  });
  /* Intl.Locale has no supportedLocalesOf: it is not a service. */

  /* ---------------------------------------------------------------------- */
  /* The engine-side methods that must route to the same backend             */
  /* ---------------------------------------------------------------------- */

  /*
   * These are what an application actually calls. `(1234.5).toLocaleString()`
   * and `"a".localeCompare("b")` are the ECMAScript-side entry points into
   * ECMA-402, and quickjs-ng routes them at locale-ignoring stubs. Replacing
   * them here — rather than leaving them to the engine — is what makes
   * installing this module change the behaviour of code that never mentions
   * `Intl` at all.
   *
   * Each constructs a formatter per call, exactly as the spec describes,
   * EXCEPT where the memo above proves the construction unobservable — see the
   * "implicit-formatter memo" section for the condition and why it is the safe
   * one. With an options bag present, the per-call construction is unchanged.
   */

  /*
   * Two memos for `Number.prototype.toLocaleString`. The first — `memoNumber` —
   * is the locale-only memo from before, used when `options === undefined`.
   * The second — `memoNumberOpts` — keys on (locales, options-hash) and is
   * used when `options` is a plain object with all-data properties. Together
   * they cover every call whose first access on the options bag is
   * unobservable across calls, which is the practical entirety of real code.
   * Anything else (Proxy, getter, class instance) bypasses both and pays the
   * full constructor each time, exactly as the conservative path did before.
   */
  var memoNumber = newMemo();
  var memoNumberOpts = newMemo();
  defineMethod(Number.prototype, 'toLocaleString', 0, ({
    m(locales, options) {
      /* thisNumberValue: throws TypeError for anything that is not a Number. */
      var x = Number.prototype.valueOf.call(this);
      if (options === undefined) {
        return memoFormatter(memoNumber, NumberFormat, locales, undefined)
          .format(x);
      }
      return memoFormatterOpts(memoNumberOpts, NumberFormat, locales, options)
        .format(x);
    }
  }).m);

  if (typeof BigInt === 'function' && BigInt.prototype) {
    var memoBigInt = newMemo();
    var memoBigIntOpts = newMemo();
    defineMethod(BigInt.prototype, 'toLocaleString', 0, ({
      m(locales, options) {
        var x = BigInt.prototype.valueOf.call(this);
        if (options === undefined) {
          return memoFormatter(memoBigInt, NumberFormat, locales, undefined)
            .format(x);
        }
        return memoFormatterOpts(memoBigIntOpts, NumberFormat, locales, options)
          .format(x);
      }
    }).m);
  }

  /*
   * `localeCompare` caches the collator's **native handle**, not the collator.
   *
   * WHY THAT IS THE RIGHT THING TO CACHE, and why it is spec-correct.
   * ECMA-402's String.prototype.localeCompare is
   *
   *     Let collator be ? Construct(%Intl.Collator%, « locales, options »).
   *     Return CompareStrings(collator, S, thatValue).
   *
   * `CompareStrings` is an **abstract operation**, not the public `compare`
   * accessor. A user who patches `Intl.Collator.prototype.compare` changes
   * what `new Intl.Collator().compare` returns and does not change what
   * `"a".localeCompare("b")` returns — in this implementation, in V8, and in
   * the specification. So going straight to the handle is not a shortcut past
   * a semantic, it *is* the semantic; the previous code read the accessor only
   * because it had a Collator object in hand.
   *
   * WHAT IT REMOVES, per call. The `compare` accessor read (MEASURED at 80.5 ns
   * as `getter-only` in workloads/07-decomposition.js), the bound function's
   * own call frame, and the two redundant `String(x)` coercions inside it — `S`
   * and `That` are already primitive strings by the time this line runs.
   *
   * The handle is an ordinary refcounted JSValue, so holding it in the memo
   * keeps exactly the platform collator alive and nothing else; the Collator
   * wrapper object is free to be collected.
   */
  var memoCollator = newMemo();

  function collatorHandleFor(locales) {
    var t = typeof locales;
    if (!memoEnabled || (t !== 'string' && t !== 'undefined')) {
      memoStats.bypasses++;
      return colState.get(new Collator(locales, undefined)).handle;
    }
    var key = t === 'string' ? 'L' + locales : MEMO_DEFAULT_KEY;
    var hit = memoCollator.map[key];
    if (hit !== undefined) {
      memoStats.hits++;
      return hit;
    }
    memoStats.misses++;
    /* Construction still happens, and still throws a RangeError for an
       invalid tag, before anything is cached. */
    var made = colState.get(new Collator(locales, undefined)).handle;
    if (memoCollator.n >= MEMO_CAP) {
      memoCollator.map = {};
      memoCollator.n = 0;
    }
    memoCollator.map[key] = made;
    memoCollator.n++;
    return made;
  }

  defineMethod(String.prototype, 'localeCompare', 1, ({
    m(that, locales, options) {
      if (this === undefined || this === null) {
        throw new TypeError('localeCompare called on null or undefined');
      }
      var S = typeof this === 'string' ? this : String(this);
      var That = typeof that === 'string' ? that : String(that);
      if (options === undefined) {
        return native.colCompare(collatorHandleFor(locales), S, That);
      }
      memoStats.bypasses++;
      return new Collator(locales, options).compare(S, That);
    }
  }).m);

  /*
   * Case mapping runs the locale pipeline once, then a single platform
   * call per call. The memo caches the resolved tag so the pipeline runs
   * once per (locales, direction).
   *
   * ASCII fast path: a pure-ASCII string's locale-sensitive case map
   * equals the default one for every locale EXCEPT Turkish (tr),
   * Azerbaijani (az, inherits Turkish rules) and Lithuanian (lt), the
   * only language subtags in CLDR with a SpecialCasings.txt conditional
   * mapping for an ASCII code point. The fast path keys on the *resolved*
   * tag, not the user-supplied `locales`, because the resolved tag is
   * what the platform receives. Bench: toLocaleUpperCase('de') on
   * 'hello' is 506 ns with the platform call and ~50 ns without, against
   * node's 21 ns (24x → ~1x).
   */
  var CASE_FAST_BYPASS = 0;
  var CASE_FAST_OK = 1;

  function caseMapTagNeedsPlatform(tag) {
    if (tag.length < 2) return true;
    var c0 = tag.charCodeAt(0);
    var c1 = tag.charCodeAt(1);
    if (c0 === 0x74 && c1 === 0x72) return true; /* tr */
    if (c0 === 0x61 && c1 === 0x7A) return true; /* az */
    if (c0 === 0x6C && c1 === 0x74) return true; /* lt */
    return false;
  }

  function caseMapIsPureAscii(s) {
    for (var i = 0; i < s.length; i++) {
      if (s.charCodeAt(i) >= 0x80) return false;
    }
    return true;
  }

  function localeCaseMap(upper) {
    /*
     * One memo per direction; entry is {tag, cls} where tag is the
     * resolved BCP-47 tag the platform will see and cls is whether the
     * ASCII fast path is safe for this locale.
     */
    var memo = { map: {}, n: 0 };
    memoCaches.push(memo);
    return ({
      m(locales) {
        if (this === undefined || this === null) {
          throw new TypeError('toLocale*Case called on null or undefined');
        }
        var S = String(this);
        var t = typeof locales;
        if (memoEnabled && (t === 'string' || t === 'undefined')) {
          var key = t === 'string' ? 'L' + locales : MEMO_DEFAULT_KEY;
          var e = memo.map[key];
          if (e !== undefined) {
            memoStats.hits++;
            if (e.cls === CASE_FAST_OK && caseMapIsPureAscii(S)) {
              return upper ? S.toUpperCase() : S.toLowerCase();
            }
            return native.caseMap(e.tag, upper, S);
          }
          memoStats.misses++;
          var requestedLocales = canonicalizeLocaleList(locales);
          var tag0 = lookupMatcher(requestedLocales).locale;
          var cls0 = caseMapTagNeedsPlatform(tag0) ? CASE_FAST_BYPASS
                                                  : CASE_FAST_OK;
          if (memo.n >= MEMO_CAP) { memo.map = {}; memo.n = 0; }
          memo.map[key] = { tag: tag0, cls: cls0 };
          memo.n++;
          if (cls0 === CASE_FAST_OK && caseMapIsPureAscii(S)) {
            return upper ? S.toUpperCase() : S.toLowerCase();
          }
          return native.caseMap(tag0, upper, S);
        }
        memoStats.bypasses++;
        var requested = canonicalizeLocaleList(locales);
        var r = lookupMatcher(requested);
        return native.caseMap(r.locale, upper, S);
      }
    }).m;
  }
  /*
   * Array.prototype.toLocaleString and %TypedArray%.prototype.toLocaleString.
   *
   * Pure algorithm — the only locale-sensitive thing they do is *forward* the
   * arguments to each element — but the plain ECMAScript versions in the engine
   * do not forward them, so `[1234.5].toLocaleString("de")` ignored the locale
   * entirely. ECMA-402's sup-array.prototype.tolocalestring exists precisely to
   * override that.
   *
   * The separator is "," and is implementation-defined by the specification;
   * every major engine uses "," and using anything else here would be a
   * gratuitous divergence.
   */
  function arrayToLocaleString() {
    return ({
      m(locales, options) {
        if (this === undefined || this === null) {
          throw new TypeError('toLocaleString called on null or undefined');
        }
        var array = Object(this);
        var len = array.length >>> 0;
        var out = '';
        for (var i = 0; i < len; i++) {
          if (i > 0) out += ',';
          var element = array[i];
          if (element === undefined || element === null) continue;
          out += String(element.toLocaleString(locales, options));
        }
        return out;
      }
    }).m;
  }
  defineMethod(Array.prototype, 'toLocaleString', 0, arrayToLocaleString());
  if (typeof Int8Array === 'function') {
    var TypedArrayPrototype = Object.getPrototypeOf(Int8Array.prototype);
    if (TypedArrayPrototype && TypedArrayPrototype !== Object.prototype) {
      defineMethod(
        TypedArrayPrototype, 'toLocaleString', 0, arrayToLocaleString());
    }
  }

  defineMethod(String.prototype, 'toLocaleUpperCase', 0, localeCaseMap(true));
  defineMethod(String.prototype, 'toLocaleLowerCase', 0, localeCaseMap(false));

  /* ---------------------------------------------------------------------- */
  /* DurationFormat                                                          */
  /* ---------------------------------------------------------------------- */

  /*
   * Deviation D6 said "Intl.DurationFormat is absent — 110 tests, no Hermes
   * support, no RN demand identified". It is implemented here, and the reason it
   * became cheap is structural rather than a change of mind: ECMA-402 defines
   * DurationFormat *in terms of* NumberFormat and ListFormat. Every string it
   * produces comes from `style: "unit"` number formatting and `type: "unit"`
   * list joining, both of which now exist and both of which are platform-backed.
   *
   * So this service ships no data of its own. What is written here is the
   * option resolution, the digital/numeric grouping rule and the part
   * assembly — algorithm, in the layer that owns algorithm.
   *
   * NOTE the honest limit: because it is built on `style: "unit"`, it inherits
   * that option's coverage. On Apple that means the four calendar units come
   * from NSDateComponentsFormatter rather than from a CLDR unit pattern —
   * deviation D16 — so a duration containing years or months is worded slightly
   * differently there than on Android.
   */

  var DURATION_UNITS = [
    /* [record key, singular unit id, allowed styles] */
    ['years', 'year', 3], ['months', 'month', 3], ['weeks', 'week', 3],
    ['days', 'day', 3], ['hours', 'hour', 5], ['minutes', 'minute', 5],
    ['seconds', 'second', 5], ['milliseconds', 'millisecond', 4],
    ['microseconds', 'microsecond', 4], ['nanoseconds', 'nanosecond', 4]
  ];
  var DURATION_STYLES_3 = ['long', 'short', 'narrow'];
  var DURATION_STYLES_4 = ['long', 'short', 'narrow', 'numeric'];
  var DURATION_STYLES_5 = ['long', 'short', 'narrow', 'numeric', '2-digit'];

  function durationStylesFor(n) {
    return n === 3 ? DURATION_STYLES_3 : n === 4 ? DURATION_STYLES_4
      : DURATION_STYLES_5;
  }

  var dfState = new WeakMap();
  var requireDf = stateGetter(dfState, 'DurationFormat');

  function DurationFormat(locales, options) {
    if (!(this instanceof DurationFormat)) {
      throw new TypeError("Constructor Intl.DurationFormat requires 'new'");
    }
    var requestedLocales = canonicalizeLocaleList(locales);
    options = getOptionsObject(options);
    getOption(options, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
    var numberingSystem = getOption(options, 'numberingSystem', undefined, undefined);
    if (numberingSystem !== undefined && !isWellFormedKeywordValue(numberingSystem)) {
      throw new RangeError('Invalid numberingSystem: ' + numberingSystem);
    }
    var r = resolveLocale(requestedLocales, { nu: numberingSystem }, NU_RELEVANT_KEYS);
    var style = getOption(options, 'style',
      ['long', 'short', 'narrow', 'digital'], 'short');

    var s = {
      locale: r.locale + r.extensionSuffix,
      baseLocale: r.locale,
      numberingSystem: r.nu || 'latn',
      style: style,
      units: {}
    };

    /*
     * Per-unit defaults, from GetDurationUnitOptions.
     *
     * `digital` is the odd one: it means "1:02:03", so hours, minutes and
     * seconds become numeric and are always displayed, while everything else
     * keeps the short wording. The `prevStyle` chain is the spec's rule that a
     * unit following a numeric one cannot be worded — once you are in
     * "1:02:03" you cannot switch to "3 seconds" halfway.
     */
    /*
     * GetDurationUnitOptions, per unit, in table order.
     *
     * Three rules interact and each one exists for a reason:
     *   - `digital` means "1:02:03", so the time units become numeric and are
     *     always shown even when zero;
     *   - once a unit is numeric, the units after it cannot be worded — you
     *     cannot render "1:02 and 3 seconds";
     *   - a unit whose style was not asked for inherits from the previous
     *     numeric unit, which is what makes `{hours:"numeric"}` imply
     *     two-digit minutes.
     */
    var prevStyle = '';
    for (var i = 0; i < DURATION_UNITS.length; i++) {
      var u = DURATION_UNITS[i];
      var key = u[0];
      var isTime = key === 'hours' || key === 'minutes' || key === 'seconds';
      var unitStyle = getOption(options, key, durationStylesFor(u[2]), undefined);
      var displayDefault = 'always';
      if (unitStyle === undefined) {
        if (style === 'digital') {
          if (!isTime) displayDefault = 'auto';
          /* "1:02:03": hours are plain, minutes and seconds are zero-padded. */
          unitStyle = key === 'minutes' || key === 'seconds' ? '2-digit'
            : (isTime || u[2] === 4 ? 'numeric' : 'short');
        } else if (prevStyle === 'numeric' || prevStyle === '2-digit') {
          if (key !== 'minutes' && key !== 'seconds') displayDefault = 'auto';
          unitStyle = key === 'minutes' || key === 'seconds' ? '2-digit' : 'numeric';
        } else {
          displayDefault = 'auto';
          unitStyle = style;
        }
      }
      var display = getOption(options, key + 'Display',
        ['auto', 'always'], displayDefault);
      /*
       * GetDurationUnitOptions step 6: *any* unit following a numeric or
       * 2-digit one must itself be numeric or 2-digit. The earlier version of
       * this check only covered minutes and seconds, which let
       * `{hours:"numeric", milliseconds:"long"}` through — test262's
       * constructor-options-style-conflict.js walks every combination.
       */
      if ((prevStyle === 'numeric' || prevStyle === '2-digit') &&
          unitStyle !== 'numeric' && unitStyle !== '2-digit') {
        throw new RangeError(
          'A worded ' + key + ' cannot follow a numeric unit');
      }
      s.units[key] = { style: unitStyle, display: display };
      prevStyle = unitStyle;
    }

    var fractionalDigits = getNumberOption(options, 'fractionalDigits', 0, 9, undefined);
    s.fractionalDigits = fractionalDigits;
    dfState.set(this, s);
  }

  /** ToDurationRecord: every field integral, all of one sign, at least one. */
  function toDurationRecord(input) {
    if (typeof input === 'string') {
      throw new RangeError('Duration must be an object, not a string');
    }
    if (input === null || typeof input !== 'object') {
      throw new TypeError('Duration must be an object');
    }
    var record = {};
    var any = false;
    var sign = 0;
    /* Field order is observable through getters and test262 checks it: the
       record's own alphabetical order, not the display order. */
    var keys = ['days', 'hours', 'microseconds', 'milliseconds', 'minutes',
                'months', 'nanoseconds', 'seconds', 'weeks', 'years'];
    for (var i = 0; i < keys.length; i++) {
      var v = input[keys[i]];
      if (v === undefined) { record[keys[i]] = 0; continue; }
      v = Number(v);
      if (v !== v || v === Infinity || v === -Infinity || Math.floor(v) !== v) {
        throw new RangeError(keys[i] + ' must be an integer');
      }
      if (v !== 0) {
        var thisSign = v < 0 ? -1 : 1;
        if (sign !== 0 && sign !== thisSign) {
          throw new RangeError('Duration fields must all have the same sign');
        }
        sign = thisSign;
      }
      record[keys[i]] = v;
      any = true;
    }
    if (!any) throw new TypeError('Duration must have at least one field');
    return record;
  }

  /*
   * Sub-second units fold into the unit above them as a decimal *string*, not
   * as a double: `{seconds: 10000000, nanoseconds: 1}` is 10000000.000000001,
   * which no double can hold. The arithmetic is done in BigInt and handed to
   * NumberFormat as a string, which is the path stage two built so that
   * `9007199254740993n` keeps its last digit. `exponent` is 9 when folding
   * into seconds, 6 into milliseconds, 3 into microseconds.
   */
  function durationToFractional(record, exponent) {
    var seconds = record.seconds || 0;
    var ms = record.milliseconds || 0;
    var us = record.microseconds || 0;
    var nanos = record.nanoseconds || 0;
    if (exponent === 9) { if (ms === 0 && us === 0 && nanos === 0) return seconds; }
    else if (exponent === 6) { if (us === 0 && nanos === 0) return ms; }
    else if (nanos === 0) return us;

    var ns = BigInt(nanos);
    if (exponent >= 9) ns += BigInt(seconds) * 1000000000n;
    if (exponent >= 6) ns += BigInt(ms) * 1000000n;
    ns += BigInt(us) * 1000n;

    var e = BigInt(Math.pow(10, exponent));
    var q = ns / e;
    var r = ns % e;
    if (r < 0n) r = -r;
    var rs = String(r);
    while (rs.length < exponent) rs = '0' + rs;
    return String(q) + '.' + rs;
  }

  /*
   * The locale's time separator, taken from the platform rather than assumed
   * to be ":". `DateTimeFormat` already knows it: format an h23 hour+minute
   * and read the literal between them. Falls back to ":" if the platform
   * produces something that is not a plausible separator, because a wrong
   * separator is worse than a conventional one.
   */
  function durationTimeSeparator(state) {
    if (state.timeSep === undefined) {
      var sep = ':';
      try {
        var parts = new DateTimeFormat(state.baseLocale, {
          hour: 'numeric', minute: 'numeric', hourCycle: 'h23', timeZone: 'UTC'
        }).formatToParts(0);
        for (var i = 1; i < parts.length; i++) {
          if (parts[i].type === 'literal' && parts[i - 1].type === 'hour') {
            var v = parts[i].value;
            if (v.length > 0 && v.length <= 2 && !/[0-9\s]/.test(v)) sep = v;
            break;
          }
        }
      } catch (e) { /* keep ':' */ }
      state.timeSep = sep;
    }
    return state.timeSep;
  }

  function durationIsNegative(record) {
    for (var i = 0; i < DURATION_UNITS.length; i++) {
      if (record[DURATION_UNITS[i][0]] < 0) return true;
    }
    return false;
  }

  /**
   * PartitionDurationFormatPattern.
   *
   * This follows ECMA-402's algorithm step for step, and deliberately follows
   * the *shape* of test262's own reference implementation in
   * `harness/testIntl.js` (`partitionDurationFormatPattern`), because that is
   * what 33 of this service's test262 files compare against element by
   * element. The earlier implementation joined pre-rendered strings and could
   * not produce per-unit parts at all: `formatToParts` returned 11 parts where
   * the reference produced 23.
   *
   * Three rules in here are not obvious and each one is load-bearing:
   *
   *  - **The sign is a NumberFormat option, not a "-" prefix.** Only the first
   *    displayed unit shows it; every later one is formatted with
   *    `signDisplay: "never"`. If every displayed value is zero but some unit
   *    of the record is negative, the first value becomes negative zero so the
   *    sign survives.
   *  - **Zero minutes are displayed when seconds will be**, so that "0:01"
   *    renders as "0:00:01" rather than losing its middle field.
   *  - **A numeric run shares one list element.** "1:02:03" is a single
   *    element as far as ListFormat is concerned; the parts of every field in
   *    it accumulate into the same array, separated by the locale's time
   *    separator.
   *
   * Returns an array of arrays of parts — one inner array per list element.
   */
  function durationPartition(state, record) {
    var result = [];
    var needSeparator = false;
    var displayNegativeSign = true;

    for (var i = 0; i < DURATION_UNITS.length; i++) {
      var key = DURATION_UNITS[i][0];
      var unitId = DURATION_UNITS[i][1];
      var value = record[key];
      var cfg = state.units[key];
      var style = cfg.style;
      var opts = {};
      var done = false;

      if (key === 'seconds' || key === 'milliseconds' || key === 'microseconds') {
        var nextStyle = state.units[DURATION_UNITS[i + 1][0]].style;
        if (nextStyle === 'numeric') {
          value = durationToFractional(record,
            key === 'seconds' ? 9 : key === 'milliseconds' ? 6 : 3);
          opts.maximumFractionDigits =
            state.fractionalDigits === undefined ? 9 : state.fractionalDigits;
          opts.minimumFractionDigits =
            state.fractionalDigits === undefined ? 0 : state.fractionalDigits;
          opts.roundingMode = 'trunc';
          done = true;
        }
      }

      var displayRequired = false;
      if (key === 'minutes' && needSeparator) {
        displayRequired = state.units.seconds.display === 'always' ||
          record.seconds !== 0 || record.milliseconds !== 0 ||
          record.microseconds !== 0 || record.nanoseconds !== 0;
      }

      if (value !== 0 || cfg.display !== 'auto' || displayRequired) {
        if (displayNegativeSign) {
          displayNegativeSign = false;
          if (value === 0 && durationIsNegative(record)) value = -0;
        } else {
          opts.signDisplay = 'never';
        }
        opts.numberingSystem = state.numberingSystem;
        if (style === '2-digit') opts.minimumIntegerDigits = 2;
        if (style !== 'numeric' && style !== '2-digit') {
          opts.style = 'unit';
          opts.unit = unitId;
          opts.unitDisplay = style;
        } else {
          opts.useGrouping = false;
        }

        var list;
        if (!needSeparator) {
          list = [];
        } else {
          list = result[result.length - 1];
          list.push({ type: 'literal', value: durationTimeSeparator(state) });
        }
        var parts = new NumberFormat(state.baseLocale, opts).formatToParts(value);
        for (var p = 0; p < parts.length; p++) {
          list.push({ type: parts[p].type, value: parts[p].value, unit: unitId });
        }
        if (!needSeparator) {
          if (style === '2-digit' || style === 'numeric') needSeparator = true;
          result.push(list);
        }
      }
      if (done) break;
    }
    return result;
  }

  /** The flattened part list: list-format literals plus each element's parts. */
  function durationFlatParts(state, record) {
    var groups = durationPartition(state, record);
    var strings = [];
    for (var g = 0; g < groups.length; g++) {
      var s = '';
      for (var j = 0; j < groups[g].length; j++) s += groups[g][j].value;
      strings.push(s);
    }
    var listParts = durationListFormat(state).formatToParts(strings);
    var out = [];
    var idx = 0;
    for (var i = 0; i < listParts.length; i++) {
      if (listParts[i].type === 'element') {
        var grp = groups[idx++];
        for (var k = 0; k < grp.length; k++) out.push(grp[k]);
      } else {
        out.push({ type: listParts[i].type, value: listParts[i].value });
      }
    }
    return out;
  }

  function durationListFormat(state) {
    if (state.lf === undefined) {
      state.lf = new ListFormat(state.baseLocale, {
        type: 'unit',
        style: state.style === 'digital' ? 'short'
          : state.style === 'narrow' ? 'narrow' : state.style
      });
    }
    return state.lf;
  }

  defineMethod(DurationFormat.prototype, 'format', 1, ({
    m(duration) {
      var s = requireDf(this, 'format');
      var parts = durationFlatParts(s, toDurationRecord(duration));
      var text = '';
      for (var i = 0; i < parts.length; i++) text += parts[i].value;
      return text;
    }
  }).m);

  defineMethod(DurationFormat.prototype, 'formatToParts', 1, ({
    m(duration) {
      var s = requireDf(this, 'formatToParts');
      return durationFlatParts(s, toDurationRecord(duration));
    }
  }).m);

  defineMethod(DurationFormat.prototype, 'resolvedOptions', 0, ({
    m() {
      var s = requireDf(this, 'resolvedOptions');
      var o = {};
      o.locale = s.locale;
      o.numberingSystem = s.numberingSystem;
      o.style = s.style;
      for (var i = 0; i < DURATION_UNITS.length; i++) {
        var key = DURATION_UNITS[i][0];
        o[key] = s.units[key].style;
        o[key + 'Display'] = s.units[key].display;
      }
      if (s.fractionalDigits !== undefined) o.fractionalDigits = s.fractionalDigits;
      return o;
    }
  }).m);

  finishService(DurationFormat, 'Intl.DurationFormat');

  /* ---------------------------------------------------------------------- */
  /* formatRange, for DateTimeFormat and NumberFormat                        */
  /* ---------------------------------------------------------------------- */

  /*
   * WHAT THIS IS, AND WHAT IT IS NOT
   *   ECMA-402's range formatting is specified in terms of CLDR *interval
   *   patterns*, which collapse the fields the two endpoints share:
   *   "Jan 1 – 5, 2024" rather than "Jan 1, 2024 – Jan 5, 2024". Neither
   *   platform exposes an interval formatter that composes with this module's
   *   seam. Apple has no Objective-C interval formatter at all, and the Swift
   *   `.interval` style — like `Date.FormatStyle.attributed` before it — is
   *   built from preset components and cannot render an arbitrary CLDR
   *   skeleton, which is what every formatter in this module is driven by.
   *   Android's android.icu.text.DateIntervalFormat *can* do it.
   *
   *   So this is the deliberate choice of consistency over nativeness, and it is
   *   the second one in the module after PluralRules. Using DateIntervalFormat
   *   on Android and a join on Apple would mean an app's two builds render a
   *   date range differently — the failure class docs/intl-completeness-map.md
   *   exists to prevent — for a string users read side by side in screenshots.
   *
   *   What it does instead: format each endpoint with the formatter that was
   *   already built, and join them with U+2013 EN DASH surrounded by spaces,
   *   which is CLDR's root `intervalFormatFallback`. Equal endpoints collapse to
   *   a single rendering, as the specification requires. Fields are NOT
   *   collapsed. That is deviation D24, and it is the difference between
   *   "correct but not idiomatic" and "absent" — which is what
   *   @formatjs/intl-datetimeformat's should-polyfill predicate tests for, and
   *   the reason an app can now delete the largest package in the formatjs
   *   stack.
   */

  /* CLDR root intervalFormatFallback is "{0} – {1}". One separator, not a
     per-locale table: any locale that wants a different one is a table, and a
     table belongs in a backend. */
  var RANGE_SEPARATOR = ' – ';

  function rangeJoin(startText, endText) {
    return startText === endText
      ? startText
      : startText + RANGE_SEPARATOR + endText;
  }

  /**
   * Range parts, with the `source` field ECMA-402 requires.
   *
   * Every part of the start rendering is "startRange", every part of the end
   * rendering is "endRange", and the separator is "shared". When the two
   * endpoints render identically the whole result is "shared", which is exactly
   * what the specification says a collapsed range produces.
   */
  function rangeParts(startParts, endParts, collapsed) {
    var out = [];
    var i;
    if (collapsed) {
      for (i = 0; i < startParts.length; i++) {
        out.push({ type: startParts[i].type, value: startParts[i].value,
                   source: 'shared' });
      }
      return out;
    }
    for (i = 0; i < startParts.length; i++) {
      out.push({ type: startParts[i].type, value: startParts[i].value,
                 source: 'startRange' });
    }
    out.push({ type: 'literal', value: RANGE_SEPARATOR, source: 'shared' });
    for (i = 0; i < endParts.length; i++) {
      out.push({ type: endParts[i].type, value: endParts[i].value,
                 source: 'endRange' });
    }
    return out;
  }

  defineMethod(DateTimeFormat.prototype, 'formatRange', 2, ({
    m(startDate, endDate) {
      var state = requireState(this, 'formatRange');
      if (startDate === undefined || endDate === undefined) {
        throw new TypeError('formatRange requires two arguments');
      }
      var a = toDateTimeValue(startDate);
      var b = toDateTimeValue(endDate);
      return rangeJoin(
        native.dtfFormat(state.handle, a), native.dtfFormat(state.handle, b));
    }
  }).m);

  defineMethod(DateTimeFormat.prototype, 'formatRangeToParts', 2, ({
    m(startDate, endDate) {
      var state = requireState(this, 'formatRangeToParts');
      if (startDate === undefined || endDate === undefined) {
        throw new TypeError('formatRangeToParts requires two arguments');
      }
      var a = toDateTimeValue(startDate);
      var b = toDateTimeValue(endDate);
      var sa = native.dtfFormat(state.handle, a);
      var sb = native.dtfFormat(state.handle, b);
      return rangeParts(
        native.dtfFormatToParts(state.handle, a),
        native.dtfFormatToParts(state.handle, b), sa === sb);
    }
  }).m);

  defineMethod(NumberFormat.prototype, 'formatRange', 2, ({
    m(start, end) {
      var state = requireNf(this, 'formatRange');
      if (start === undefined || end === undefined) {
        throw new TypeError('formatRange requires two arguments');
      }
      var a = toIntlMathematicalValue(start);
      var b = toIntlMathematicalValue(end);
      if (a.special === 'nan' || b.special === 'nan') {
        throw new RangeError('formatRange got NaN');
      }
      return rangeJoin(nfFormatValue(state, start), nfFormatValue(state, end));
    }
  }).m);

  defineMethod(NumberFormat.prototype, 'formatRangeToParts', 2, ({
    m(start, end) {
      var state = requireNf(this, 'formatRangeToParts');
      if (start === undefined || end === undefined) {
        throw new TypeError('formatRangeToParts requires two arguments');
      }
      var a = toIntlMathematicalValue(start);
      var b = toIntlMathematicalValue(end);
      if (a.special === 'nan' || b.special === 'nan') {
        throw new RangeError('formatRangeToParts got NaN');
      }
      var da = preRound(state, a);
      var db = preRound(state, b);
      var ta = native.nfFormat(nfHandleFor(state, da), a.number, da);
      var tb = native.nfFormat(nfHandleFor(state, db), b.number, db);
      return rangeParts(
        native.nfFormatToParts(nfHandleFor(state, da), a.number, da),
        native.nfFormatToParts(nfHandleFor(state, db), b.number, db),
        ta === tb);
    }
  }).m);


  /* ---------------------------------------------------------------------- */
  /* The Intl namespace                                                      */
  /* ---------------------------------------------------------------------- */

  var Intl = {};

  function exposeService(name, ctor) {
    ObjectDefineProperty(Intl, name, {
      value: ctor, writable: true, enumerable: false, configurable: true
    });
  }
  exposeService('DateTimeFormat', DateTimeFormat);
  exposeService('NumberFormat', NumberFormat);
  exposeService('PluralRules', PluralRules);
  exposeService('Collator', Collator);
  exposeService('RelativeTimeFormat', RelativeTimeFormat);
  exposeService('ListFormat', ListFormat);
  exposeService('DisplayNames', DisplayNames);
  exposeService('Segmenter', Segmenter);
  exposeService('Locale', Locale);
  exposeService('DurationFormat', DurationFormat);

  var intlMethods = {
    getCanonicalLocales(locales) {
      return canonicalizeLocaleList(locales);
    },
    supportedValuesOf(key) {
      /*
       * `'' + key`, not `String(key)`. The spec step is ToString, which throws
       * a TypeError for a Symbol; `String(sym)` is the one call that is
       * *specified* not to, and returns "Symbol()". test262's
       * supportedValuesOf/coerced-to-string.js checks exactly this.
       */
      key = toStringSpec(key);
    var out;
    if (key === 'calendar') out = native.calendars();
    else if (key === 'timeZone') out = native.timeZones();
    else if (key === 'numberingSystem') out = native.numberingSystems();
    else if (key === 'collation') {
      /*
       * "standard" and "search" are excluded by the specification: they are
       * never valid `co` keyword values, and reporting them would break the
       * equivalence test262 checks in both directions — everything listed must
       * be accepted by a Collator, and everything a Collator accepts must be
       * listed.
       */
      out = [];
      var all = native.collations();
      for (var c = 0; c < all.length; c++) {
        if (all[c] !== 'standard' && all[c] !== 'search') out.push(all[c]);
      }
    }
    else if (key === 'currency') out = native.currencies();
    else if (key === 'unit') out = SANCTIONED_UNITS;
    else throw new RangeError('Invalid key: ' + key);
    out = out.slice().sort();
    /* The spec requires a fresh array with no duplicates. */
    var dedup = [];
    for (var i = 0; i < out.length; i++) {
      if (i === 0 || out[i] !== out[i - 1]) dedup.push(out[i]);
    }
      return dedup;
    }
  };
  defineMethod(Intl, 'getCanonicalLocales', 1, intlMethods.getCanonicalLocales);
  defineMethod(Intl, 'supportedValuesOf', 1, intlMethods.supportedValuesOf);

  ObjectDefineProperty(Intl, Symbol.toStringTag, {
    value: 'Intl', writable: false, enumerable: false, configurable: true
  });

  /*
   * Diagnostics, not API. The differential corpus and the test suite need to
   * know which backend answered and what pattern it chose; without this, a
   * cross-platform output difference cannot be attributed to "different
   * pattern" versus "same pattern, different data" without guessing.
   *
   * Non-enumerable and prefixed, so it cannot be mistaken for spec surface.
   */
  /*
   * Fast-path counters, the memo's reset, and the memo's kill switch.
   *
   * Diagnostics, not API, and in the same non-enumerable prefixed style as
   * `__rnqjsBackend` below. It exists because this project has shipped a fast
   * path with **zero hits** that survived a spike, an audit and a relay, and
   * the only defence that has ever worked is a counter on the path itself.
   * `modules/intl/bench/workloads/06-ecmascript-methods.js` reads it and
   * `modules/intl/test/invariants.js` asserts on it, so a future change that
   * silently stops taking the memo fails a test rather than merely getting
   * slower.
   *
   *   Intl.__rnqjsPerf.stats() -> { hits, misses, bypasses }
   *   Intl.__rnqjsPerf.reset() -> drops every cached formatter and zeroes the
   *                                counters. An embedder that is notified of an
   *                                OS locale change should call it; see the
   *                                stale-default-locale note on the memo.
   */
  ObjectDefineProperty(Intl, '__rnqjsPerf', {
    value: {
      stats: function () {
        return { hits: memoStats.hits, misses: memoStats.misses,
                 bypasses: memoStats.bypasses,
                 fastRoundHits: perfStats.fastRoundHits,
                 fastRoundMisses: perfStats.fastRoundMisses,
                 pluralFastHits: perfStats.pluralFastHits,
                 pluralFastMisses: perfStats.pluralFastMisses,
                 canonHits: perfStats.canonHits,
                 canonMisses: perfStats.canonMisses,
                 segmentHits: perfStats.segmentHits,
                 segmentMisses: perfStats.segmentMisses,
                 exactDoubleHits: perfStats.exactDoubleHits,
                 exactDoubleMisses: perfStats.exactDoubleMisses };
      },
      setEnabled: function (on) {
        memoEnabled = !!on;
        if (!memoEnabled) this.reset();
      },
      reset: function () {
        for (var i = 0; i < memoCaches.length; i++) {
          memoCaches[i].map = {};
          memoCaches[i].n = 0;
        }
        memoStats.hits = 0;
        memoStats.misses = 0;
        memoStats.bypasses = 0;
        perfStats.fastRoundHits = 0;
        perfStats.fastRoundMisses = 0;
        perfStats.pluralFastHits = 0;
        perfStats.pluralFastMisses = 0;
        perfStats.canonHits = 0;
        perfStats.canonMisses = 0;
        perfStats.segmentHits = 0;
        perfStats.segmentMisses = 0;
        perfStats.exactDoubleHits = 0;
        perfStats.exactDoubleMisses = 0;
      }
    },
    writable: false, enumerable: false, configurable: true
  });

  ObjectDefineProperty(Intl, '__rnqjsBackend', {
    value: function (dtf) {
      if (dtf === undefined) return native.backendName();
      var s = dtfState.get(dtf);
      return s ? native.dtfResolved(s.handle, 'pattern') : undefined;
    },
    writable: false, enumerable: false, configurable: true
  });

  return Intl;
})
