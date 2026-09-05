/*
 * React Native's native-prop payload path.
 *
 * This is the most RN-specific benchmark in the suite and, as far as we can
 * tell, it does not exist anywhere else. It matters because measurement puts it
 * an order of magnitude above the React bookkeeping it sits next to: allocating
 * a React element costs ~69 ns and a Fiber ~138 ns, while building the native
 * prop payload for one ordinary list-row <View> costs ~1,375 ns and diffing it
 * on update costs ~570-1,055 ns (Hermes, desktop arm64).
 *
 * It runs on every mount, every update, and — via
 * createAnimatedPropsHook -> setNativeProps -> createAttributePayload — on
 * every frame of every JS-driven animation.
 *
 * The code below is a faithful ES5 port of
 *   Libraries/ReactNative/ReactFabricPublicInstance/ReactNativeAttributePayload.js
 * (diffProperties / addNestedProperty / diffNestedProperty), plus
 *   Libraries/Utilities/differ/deepDiffer.js  and
 *   Libraries/StyleSheet/flattenStyle.js
 * from React Native 0.85. Ported rather than imported because the harness has
 * no module system, and kept structurally identical because the point is to
 * measure RN's actual algorithm, not a tidier one.
 *
 * What makes it hostile to an interpreter, and why it is worth optimizing:
 *   - TWO `for-in` loops per element per update (once over nextProps, once over
 *     prevProps). `for-in` is the single worst relative primitive measured.
 *   - a keyed load into a 187-key config object for every prop
 *   - the payload is built by dynamic keyed stores, so it takes a fresh shape
 *     transition per key rather than being a literal
 *   - `style` recurses into a 150-key nested config, over an array
 */

// --- view configs, at real scale ------------------------------------------
//
// BaseViewConfig.android.js has 157 non-event + 30 event keys = 187; and
// ReactNativeStyleAttributes has 150. The exact names past the real ones do
// not matter — the cost is the size of the object being probed — but the real
// ones are used first so the hot lookups hit realistic keys.

var STYLE_CONFIG = (function () {
  var real = [
    'width', 'height', 'top', 'left', 'right', 'bottom', 'margin', 'marginTop',
    'marginBottom', 'marginLeft', 'marginRight', 'marginHorizontal',
    'marginVertical', 'padding', 'paddingTop', 'paddingBottom', 'paddingLeft',
    'paddingRight', 'paddingHorizontal', 'paddingVertical', 'flex',
    'flexDirection', 'flexGrow', 'flexShrink', 'flexBasis', 'flexWrap',
    'justifyContent', 'alignItems', 'alignSelf', 'alignContent', 'position',
    'display', 'overflow', 'opacity', 'backgroundColor', 'borderColor',
    'borderWidth', 'borderRadius', 'borderTopLeftRadius', 'borderTopRightRadius',
    'borderBottomLeftRadius', 'borderBottomRightRadius', 'borderStyle',
    'color', 'fontSize', 'fontWeight', 'fontFamily', 'fontStyle', 'lineHeight',
    'letterSpacing', 'textAlign', 'textDecorationLine', 'textTransform',
    'shadowColor', 'shadowOffset', 'shadowOpacity', 'shadowRadius', 'elevation',
    'transform', 'zIndex', 'aspectRatio', 'minWidth', 'maxWidth', 'minHeight',
    'maxHeight', 'resizeMode', 'tintColor', 'includeFontPadding',
  ];
  var config = {};
  for (var i = 0; i < real.length; i++) config[real[i]] = true;
  // Colors carry a `process` function in the real config; that branch is a
  // different path through diffProperties and must be exercised.
  config.backgroundColor = { process: processColor };
  config.color = { process: processColor };
  config.borderColor = { process: processColor };
  config.shadowColor = { process: processColor };
  config.tintColor = { process: processColor };
  var n = real.length;
  while (n < 150) { config['styleProp' + n] = true; n++; }
  return config;
})();

var VIEW_CONFIG = (function () {
  var real = [
    'accessible', 'accessibilityLabel', 'accessibilityHint', 'accessibilityRole',
    'accessibilityState', 'accessibilityValue', 'accessibilityActions',
    'accessibilityLiveRegion', 'accessibilityElementsHidden', 'testID',
    'nativeID', 'collapsable', 'needsOffscreenAlphaCompositing', 'renderToHardwareTextureAndroid',
    'shouldRasterizeIOS', 'pointerEvents', 'removeClippedSubviews', 'importantForAccessibility',
    'hitSlop', 'onLayout', 'onMagicTap', 'onAccessibilityTap', 'onAccessibilityAction',
    'nativeBackgroundAndroid', 'nativeForegroundAndroid', 'focusable',
    'hasTVPreferredFocus', 'borderTopColor', 'borderBottomColor',
  ];
  var config = {};
  for (var i = 0; i < real.length; i++) config[real[i]] = true;
  config.style = STYLE_CONFIG; // the nested case
  // Event handlers are plain `true` in the config; diffProperties converts a
  // function value to the boolean `true` as a marker for native.
  var events = ['onTouchStart', 'onTouchMove', 'onTouchEnd', 'onTouchCancel',
                'onPress', 'onPressIn', 'onPressOut', 'onLongPress', 'onFocus', 'onBlur'];
  for (var e = 0; e < events.length; e++) config[events[e]] = true;
  var n = 0;
  while (Object.keys(config).length < 187) { config['viewProp' + n] = true; n++; }
  return config;
})();

// --- RN's helpers, ported verbatim in structure ---------------------------

/*
 * normalizeColor's real cost is that it tries a chain of regexes; a '#rrggbb'
 * string falls through several misses before matching. Only the shape matters
 * here, not the full 9-regex chain.
 */
var RE_HEX6 = /^#([0-9a-fA-F]{6})$/;
var RE_HEX3 = /^#([0-9a-fA-F]{3})$/;
var RE_RGB = /^rgb\(\s*([0-9]+)\s*,\s*([0-9]+)\s*,\s*([0-9]+)\s*\)$/;

function processColor(color) {
  if (typeof color === 'number') return color;
  if (typeof color !== 'string') return null;
  var m = RE_RGB.exec(color);
  if (m) {
    return ((255 << 24) | (+m[1] << 16) | (+m[2] << 8) | +m[3]) >>> 0;
  }
  m = RE_HEX3.exec(color);
  if (m) {
    var h = m[1];
    return parseInt('ff' + h[0] + h[0] + h[1] + h[1] + h[2] + h[2], 16) >>> 0;
  }
  m = RE_HEX6.exec(color);
  if (m) return parseInt('ff' + m[1], 16) >>> 0;
  return null;
}

/** Libraries/Utilities/differ/deepDiffer.js — recursive, two for-in loops. */
function deepDiffer(one, two, maxDepth) {
  if (maxDepth === undefined) maxDepth = -1;
  if (maxDepth === 0) return true;
  if (one === two) return false;
  if (typeof one === 'function' && typeof two === 'function') return false;
  if (typeof one !== 'object' || one === null) return one !== two;
  if (typeof two !== 'object' || two === null) return true;
  if (one.constructor !== two.constructor) return true;

  if (Array.isArray(one)) {
    if (one.length !== two.length) return true;
    for (var i = 0; i < one.length; i++) {
      if (deepDiffer(one[i], two[i], maxDepth - 1)) return true;
    }
    return false;
  }
  for (var k in one) {
    if (deepDiffer(one[k], two[k], maxDepth - 1)) return true;
  }
  for (var k2 in two) {
    if (one[k2] === undefined && two[k2] !== undefined) return true;
  }
  return false;
}

function defaultDiffer(prevProp, nextProp) {
  if (typeof nextProp !== 'object' || nextProp === null) return prevProp !== nextProp;
  return deepDiffer(prevProp, nextProp);
}

function addProperties(updatePayload, props, validAttributes) {
  return diffProperties(updatePayload, {}, props, validAttributes);
}

function addNestedProperty(payload, props, validAttributes) {
  if (!props) return payload;
  if (!Array.isArray(props)) return addProperties(payload, props, validAttributes);
  for (var i = 0; i < props.length; i++) {
    payload = addNestedProperty(payload, props[i], validAttributes);
  }
  return payload;
}

function diffNestedProperty(updatePayload, prevProp, nextProp, validAttributes) {
  if (!updatePayload && prevProp === nextProp) return updatePayload;
  if (!prevProp || !nextProp) {
    if (nextProp) return addNestedProperty(updatePayload, nextProp, validAttributes);
    if (prevProp) return clearNestedProperty(updatePayload, prevProp, validAttributes);
    return updatePayload;
  }
  if (!Array.isArray(prevProp) && !Array.isArray(nextProp)) {
    return diffProperties(updatePayload, prevProp, nextProp, validAttributes);
  }
  if (Array.isArray(prevProp) && Array.isArray(nextProp)) {
    var minLength = prevProp.length < nextProp.length ? prevProp.length : nextProp.length;
    var i;
    for (i = 0; i < minLength; i++) {
      updatePayload = diffNestedProperty(updatePayload, prevProp[i], nextProp[i], validAttributes);
    }
    for (; i < prevProp.length; i++) {
      updatePayload = clearNestedProperty(updatePayload, prevProp[i], validAttributes);
    }
    for (; i < nextProp.length; i++) {
      updatePayload = addNestedProperty(updatePayload, nextProp[i], validAttributes);
    }
    return updatePayload;
  }
  if (Array.isArray(prevProp)) {
    return diffProperties(updatePayload, flattenStyle(prevProp), nextProp, validAttributes);
  }
  return diffProperties(updatePayload, prevProp, flattenStyle(nextProp), validAttributes);
}

function clearProperties(updatePayload, prevProps, validAttributes) {
  for (var propKey in prevProps) {
    var config = validAttributes[propKey];
    if (!config) continue;
    if (updatePayload && updatePayload[propKey] !== undefined) continue;
    var prevProp = prevProps[propKey];
    if (prevProp === undefined) continue;
    if (typeof config !== 'object' || typeof config.diff === 'function' ||
        typeof config.process === 'function') {
      (updatePayload || (updatePayload = {}))[propKey] = null;
    } else {
      updatePayload = clearNestedProperty(updatePayload, prevProp, config);
    }
  }
  return updatePayload;
}

function clearNestedProperty(updatePayload, prevProp, validAttributes) {
  if (!prevProp) return updatePayload;
  if (!Array.isArray(prevProp)) return clearProperties(updatePayload, prevProp, validAttributes);
  for (var i = 0; i < prevProp.length; i++) {
    updatePayload = clearNestedProperty(updatePayload, prevProp[i], validAttributes);
  }
  return updatePayload;
}

/** The function this whole file exists to measure. */
function diffProperties(updatePayload, prevProps, nextProps, validAttributes) {
  var attributeConfig;
  var nextProp;
  var prevProp;

  for (var propKey in nextProps) {
    attributeConfig = validAttributes[propKey];
    if (!attributeConfig) continue;

    prevProp = prevProps[propKey];
    nextProp = nextProps[propKey];

    if (typeof nextProp === 'function') {
      var hasProcess = typeof attributeConfig === 'object' &&
                       typeof attributeConfig.process === 'function';
      if (!hasProcess) {
        nextProp = true;
        if (typeof prevProp === 'function') prevProp = true;
      }
    }

    if (typeof nextProp === 'undefined') {
      nextProp = null;
      if (typeof prevProp === 'undefined') prevProp = null;
    }

    if (updatePayload && updatePayload[propKey] !== undefined) {
      if (typeof attributeConfig !== 'object') {
        updatePayload[propKey] = nextProp;
      } else if (typeof attributeConfig.diff === 'function' ||
                 typeof attributeConfig.process === 'function') {
        updatePayload[propKey] = typeof attributeConfig.process === 'function'
          ? attributeConfig.process(nextProp)
          : nextProp;
      }
      continue;
    }

    if (prevProp === nextProp) continue; // the identity bail-out

    if (typeof attributeConfig !== 'object') {
      if (defaultDiffer(prevProp, nextProp)) {
        (updatePayload || (updatePayload = {}))[propKey] = nextProp;
      }
    } else if (typeof attributeConfig.diff === 'function' ||
               typeof attributeConfig.process === 'function') {
      var shouldUpdate = prevProp === undefined ||
        (typeof attributeConfig.diff === 'function'
          ? attributeConfig.diff(prevProp, nextProp)
          : defaultDiffer(prevProp, nextProp));
      if (shouldUpdate) {
        (updatePayload || (updatePayload = {}))[propKey] =
          typeof attributeConfig.process === 'function'
            ? attributeConfig.process(nextProp)
            : nextProp;
      }
    } else {
      updatePayload = diffNestedProperty(updatePayload, prevProp, nextProp, attributeConfig);
    }
  }

  // Second pass: catch props that were removed, so native can reset them.
  for (var prevKey in prevProps) {
    if (nextProps[prevKey] !== undefined) continue;
    attributeConfig = validAttributes[prevKey];
    if (!attributeConfig) continue;
    if (updatePayload && updatePayload[prevKey] !== undefined) continue;
    prevProp = prevProps[prevKey];
    if (prevProp === undefined) continue;
    if (typeof attributeConfig !== 'object' ||
        typeof attributeConfig.diff === 'function' ||
        typeof attributeConfig.process === 'function') {
      (updatePayload || (updatePayload = {}))[prevKey] = null;
    } else {
      updatePayload = clearNestedProperty(updatePayload, prevProp, attributeConfig);
    }
  }
  return updatePayload;
}

/** Libraries/StyleSheet/flattenStyle.js */
function flattenStyle(style) {
  if (style === null || typeof style !== 'object') return undefined;
  if (!Array.isArray(style)) return style;
  var result = {};
  for (var i = 0; i < style.length; i++) {
    var computed = flattenStyle(style[i]);
    if (computed) {
      for (var key in computed) result[key] = computed[key];
    }
  }
  return result;
}

// --- the props of one ordinary list row ------------------------------------

function makeRowProps(i) {
  return {
    testID: 'row-' + i,
    accessible: true,
    accessibilityLabel: 'Item ' + i,
    collapsable: false,
    onTouchStart: function () {},
    onPress: function () {},
    // A style ARRAY, which is what every real component produces once a base
    // style is combined with a conditional one. This is the recursive path.
    style: [
      { flex: 1, flexDirection: 'row', paddingHorizontal: 16, paddingVertical: 12,
        backgroundColor: '#ffffff', borderRadius: 8 },
      // The conditional style OVERRIDES a key of the base style. That is what
      // an array style is for, and until 2026-07-26 no style object here
      // overlapped with another, so `flattenStyle`'s last-wins rule was
      // never exercised: a mutant that made it first-wins passed every gate.
      { opacity: i % 2 === 0 ? 1 : 0.9, marginTop: i % 3,
        backgroundColor: i % 2 === 0 ? '#ffffff' : '#f5f5f5' },
    ],
  };
}

/*
 * The same row after a re-render, for the commit benchmark.
 *
 * Until 2026-07-26 `commit-20-rows` diffed 20 rows against structurally
 * identical copies, so all 20 payloads were empty and the row's answer was 0.
 * That models one real case — a re-render that changed nothing — but it is
 * already covered by `diff-new-objects`, and it means the row never once
 * executed the branch its own file header advertises: "the payload is built by
 * dynamic keyed stores, so it takes a fresh shape transition per key".
 *
 * The mix below models a list re-rendering after its data source updated, and
 * every bucket is a branch of `diffProperties` that the old inputs never
 * reached (MEASURED: the old inputs produced a null payload for all 20 rows):
 *
 *   i % 5 == 0  (4 rows)  nothing changed          -> null payload, identity
 *                                                     bail-out on every key
 *   i % 5 == 1  (4 rows)  one scalar string        -> defaultDiffer, one
 *                                                     dynamic keyed store
 *   i % 5 == 2  (4 rows)  scalar + a style value   -> the nested/array style
 *                                                     path, two stores
 *   i % 5 == 3  (4 rows)  scalar + a COLOR         -> attributeConfig.process,
 *                                                     i.e. processColor's
 *                                                     regex chain
 *   i % 5 == 4  (4 rows)  a handler goes away      -> 2 rows via React's
 *                                                     `cond ? fn : undefined`
 *                                                     idiom (same key set) and
 *                                                     2 rows by omitting the
 *                                                     key entirely, which is
 *                                                     the only thing in this
 *                                                     file that reaches
 *                                                     diffProperties' SECOND
 *                                                     pass over prevProps.
 *
 * ASSUMED, not measured: that 16-of-20-rows-changed is the representative
 * frame. It is a modelling choice, argued from what a data-refresh commit does;
 * the all-unchanged corner is retained as bucket 0 and as `diff-new-objects`.
 * A real-bundle measurement would settle it and has not been done.
 *
 * NOTE for anyone measuring shape-based fast paths on this file: the two
 * key-omitting rows give `prev` and `next` DIFFERENT shapes on purpose, which
 * is realistic (a conditional prop really does produce a different props
 * object) but does lower the prev/next shape-sharing rate that
 * docs/for-in-pinned-shape-spike.md measured at 66.7% against the OLD inputs.
 * That figure must be re-measured, not carried over.
 */
function makeRowPropsNoPress(i) {
  return {
    testID: 'row-' + i,
    accessible: true,
    accessibilityLabel: 'Item ' + i,
    collapsable: false,
    onTouchStart: function () {},
    style: [
      { flex: 1, flexDirection: 'row', paddingHorizontal: 16, paddingVertical: 12,
        backgroundColor: '#ffffff', borderRadius: 8 },
      // The conditional style OVERRIDES a key of the base style. That is what
      // an array style is for, and until 2026-07-26 no style object here
      // overlapped with another, so `flattenStyle`'s last-wins rule was
      // never exercised: a mutant that made it first-wins passed every gate.
      { opacity: i % 2 === 0 ? 1 : 0.9, marginTop: i % 3,
        backgroundColor: i % 2 === 0 ? '#ffffff' : '#f5f5f5' },
    ],
  };
}

function makeNextRowProps(i) {
  var p;
  switch (i % 5) {
    case 0:
      return makeRowProps(i);
    case 1:
      p = makeRowProps(i);
      p.accessibilityLabel = 'Item ' + i + ' (2 unread)';
      return p;
    case 2:
      p = makeRowProps(i);
      p.accessibilityLabel = 'Item ' + i + ' (selected)';
      p.style[1].opacity = 0.5;
      return p;
    case 3:
      p = makeRowProps(i);
      p.testID = 'row-' + i + '-alt';
      p.style[0].backgroundColor = '#eef2f7';
      return p;
    default:
      if (i < 10) {
        /* React's `onPress={enabled ? handler : undefined}` — same key set. */
        p = makeRowProps(i);
        p.onPress = undefined;
        return p;
      }
      /* `{...(enabled && {onPress})}` — the key is genuinely absent. */
      return makeRowPropsNoPress(i);
  }
}

function countKeys(o) {
  if (!o) return 0;
  var n = 0;
  for (var k in o) n++;
  return n;
}

// --- how these rows are gated ----------------------------------------------
//
// Until 2026-07-26 four of the six rows here had no `expect` at all, and the
// two that had one were gated on a key COUNT. An audit found that
// `commit-20-rows` — the row patch 0014 is sold on and the row
// docs/dispatch-floor.md prices its prize against — returned **0**: its inputs
// were structurally identical, so `diffProperties` produced an empty payload
// and the row's entire observable output was indistinguishable from
// `diffProperties` returning `null` on entry. MEASURED: deleting the body of
// `diffProperties` from the old file left `diff-identical`'s `expect: 0`
// PASSING and every other row ungated. Adding `expect: 0` to the rest would
// have gated nothing for exactly the same reason.
//
// Three mechanisms replace it. See docs/rn-props-benchmark-gates.md.
//
//  1. `run()` returns the PAYLOAD ITSELF (or, for the commit row, the array of
//     20 payloads) rather than a summary of it. Nothing is summarised inside
//     the timed loop.
//
//  2. `expect` is a FUNCTION, so the check runs once, outside timing. It
//     digests the payload with `payloadDigest` — an exact, order- and
//     value-sensitive rendering — and compares it to a literal. The digest is
//     a readable string on purpose: when a gate fires, the failure message
//     shows the payload that was actually produced, which a hash cannot.
//     MEASURED: this design costs the commit row nothing (59.9 µs with the
//     gate against 59.7 µs with `run()` returning a constant), where an
//     in-loop checksum cost 32 µs — 34% of the row — because `charCodeAt` is
//     a method call per character.
//
//  3. A setup-time WITNESS for the two rows whose payload is legitimately
//     empty (`diff-identical`, `diff-new-objects`). An empty payload's digest
//     is a constant, so it cannot prove the traversal happened. Those rows run
//     the same differ once in `setup()` on a pair that MUST produce a payload
//     and check that digest in `expect` as well. Do not delete the witness on
//     the grounds that the row "already has an expect" — for an empty payload
//     that expect is satisfied by a differ that does nothing at all.
//
// Both the digest and every value in it are produced from `String(value)` and
// property enumeration order only, never from bit patterns, so two
// spec-compliant engines agree exactly. VERIFIED: QuickJS, Hermes and node all
// produce byte-identical digests for all six rows.

function describeValue(v) {
  if (v === null) return 'null';
  var t = typeof v;
  if (t === 'string') return '"' + v + '"';
  if (t === 'number' || t === 'boolean') return '' + v;
  if (t === 'undefined') return 'undef';
  if (t === 'function') return 'fn';
  var s, i;
  if (Array.isArray(v)) {
    s = '[';
    for (i = 0; i < v.length; i++) s += describeValue(v[i]) + ',';
    return s + ']';
  }
  s = '{';
  for (var k in v) s += k + ':' + describeValue(v[k]) + ',';
  return s + '}';
}

/* Exact rendering of one update payload. `null` — RN's "nothing to send to
   native" answer — renders as '-', which is distinguishable from an empty
   object but NOT from a differ that bailed out on entry; that is what the
   witness is for. */
function payloadDigest(p) {
  if (p === null || p === undefined) return '-';
  var s = '';
  for (var k in p) s += k + ':' + describeValue(p[k]) + ';';
  return s;
}

function payloadListDigest(list) {
  var s = '';
  for (var i = 0; i < list.length; i++) s += payloadDigest(list[i]) + '|';
  return s;
}

// --- benchmarks ------------------------------------------------------------

var rowProps, rowPropsSame, rowPropsOneChanged, rowPropsNewObjects;
var identicalWitness = '';
var newObjectsWitness = '';

bench({
  name: 'rnprops/create-mount',
  unit: 'element',
  setup: function () { rowProps = makeRowProps(1); },
  // The mount path: no previous props, so every valid prop lands in the
  // payload and the style array is walked in full.
  run: function () { return addProperties(null, rowProps, VIEW_CONFIG); },
  expect: function (got) {
    return payloadDigest(got) ===
      'testID:"row-1";accessible:true;accessibilityLabel:"Item 1";collapsable:false;' +
      'onTouchStart:true;onPress:true;flex:1;flexDirection:"row";paddingHorizontal:16;' +
      'paddingVertical:12;backgroundColor:4294309365;borderRadius:8;opacity:0.9;marginTop:1;';
  },
});

bench({
  name: 'rnprops/diff-identical',
  unit: 'element',
  setup: function () {
    rowProps = makeRowProps(1); rowPropsSame = rowProps;
    /* Witness — see "how these rows are gated" above. This row's payload is
       legitimately null, so its digest alone is satisfied by a differ that
       does nothing. This runs the SAME differ, once, outside the timed loop,
       on a pair that must produce a payload. */
    var changed = makeRowProps(1);
    changed.accessibilityLabel = 'witness';
    identicalWitness = payloadDigest(diffProperties(null, rowProps, changed, VIEW_CONFIG));
  },
  // The best case, and the common one: React re-rendered but the props object
  // is identical, so every key hits the `prevProp === nextProp` bail-out. This
  // measures the pure traversal cost of doing nothing.
  run: function () { return diffProperties(null, rowProps, rowPropsSame, VIEW_CONFIG); },
  expect: function (got) {
    return payloadDigest(got) === '-' &&
           identicalWitness === 'accessibilityLabel:"witness";';
  },
});

bench({
  name: 'rnprops/diff-one-changed',
  unit: 'element',
  setup: function () {
    rowProps = makeRowProps(1);
    rowPropsOneChanged = makeRowProps(1);
    rowPropsOneChanged.accessibilityLabel = 'Item changed';
  },
  // One scalar prop differs — a typical state update.
  run: function () {
    return diffProperties(null, rowProps, rowPropsOneChanged, VIEW_CONFIG);
  },
  expect: function (got) {
    return payloadDigest(got) === 'accessibilityLabel:"Item changed";';
  },
});

bench({
  name: 'rnprops/diff-new-objects',
  unit: 'element',
  setup: function () {
    rowProps = makeRowProps(1);
    /* Witness: this row's payload is legitimately null too. */
    var changed = makeRowProps(1);
    changed.style[1].opacity = 0.25;
    newObjectsWitness = payloadDigest(diffProperties(null, rowProps, changed, VIEW_CONFIG));
  },
  // The pathological case that real apps hit constantly: the component
  // re-rendered and built structurally identical but freshly-allocated props,
  // so the identity bail-out never fires and every value is deep-compared.
  // This is what an inline style object in a render function costs.
  run: function () {
    return diffProperties(null, rowProps, makeRowProps(1), VIEW_CONFIG);
  },
  expect: function (got) {
    return payloadDigest(got) === '-' && newObjectsWitness === 'opacity:0.25;';
  },
});

bench({
  name: 'rnprops/flatten-style',
  unit: 'style',
  setup: function () { rowProps = makeRowProps(1); },
  run: function () { return flattenStyle(rowProps.style); },
  expect: function (got) {
    return payloadDigest(got) ===
      'flex:1;flexDirection:"row";paddingHorizontal:16;paddingVertical:12;' +
      'backgroundColor:"#f5f5f5";borderRadius:8;opacity:0.9;marginTop:1;';
  },
});

/* The expected payloads of one commit, written out. Long on purpose: this is
   the only place in the suite that states what RN's differ is supposed to
   produce, and it is what makes a wrong payload distinguishable from a wrong
   payload SIZE. `-` is a row whose payload is null. */
var COMMIT_20_EXPECT =
  '-|' +
  'accessibilityLabel:"Item 1 (2 unread)";|' +
  'accessibilityLabel:"Item 2 (selected)";opacity:0.5;|' +
  'testID:"row-3-alt";backgroundColor:4294309365;|' +
  'onPress:null;|' +
  '-|' +
  'accessibilityLabel:"Item 6 (2 unread)";|' +
  'accessibilityLabel:"Item 7 (selected)";opacity:0.5;|' +
  'testID:"row-8-alt";backgroundColor:4294967295;|' +
  'onPress:null;|' +
  '-|' +
  'accessibilityLabel:"Item 11 (2 unread)";|' +
  'accessibilityLabel:"Item 12 (selected)";opacity:0.5;|' +
  'testID:"row-13-alt";backgroundColor:4294309365;|' +
  'onPress:null;|' +
  '-|' +
  'accessibilityLabel:"Item 16 (2 unread)";|' +
  'accessibilityLabel:"Item 17 (selected)";opacity:0.5;|' +
  'testID:"row-18-alt";backgroundColor:4294967295;|' +
  'onPress:null;|';

bench({
  name: 'rnprops/commit-20-rows',
  unit: 'commit',
  setup: function () {
    rowProps = [];
    rowPropsNewObjects = [];
    for (var i = 0; i < 20; i++) {
      rowProps.push(makeRowProps(i));
      rowPropsNewObjects.push(makeNextRowProps(i));
    }
  },
  // A whole commit's worth of prop work: 20 visible rows re-diffed after a
  // data update, which is roughly what one frame of a list re-render costs at
  // the prop layer. See makeNextRowProps for which rows change and why; 16 of
  // the 20 produce a non-empty payload. Collecting the payloads into an array
  // is what RN does too — each row's payload is handed to native — and it
  // MEASURED as free (59.9 µs against 59.7 µs for returning a constant).
  run: function () {
    var out = [];
    for (var i = 0; i < 20; i++) {
      out.push(diffProperties(null, rowProps[i], rowPropsNewObjects[i], VIEW_CONFIG));
    }
    return out;
  },
  expect: function (got) { return payloadListDigest(got) === COMMIT_20_EXPECT; },
});
