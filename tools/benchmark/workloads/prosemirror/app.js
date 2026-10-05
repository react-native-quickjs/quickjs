/*
 * ProseMirror Node.fromJSON workload.
 *
 * The real-world operation is: a JSON document arrives from the network,
 * `JSON.parse` turns it into a plain JS object, and `Node.fromJSON`
 * (`schema.nodeFromJSON`) turns that into a ProseMirror document tree.  This
 * is the hot path in ProseMirror's `Node.fromJSON` (and in the model's
 * `Fragment.fromJSON` / `Mark.fromJSON` / `MarkType.create` / `computeAttrs`
 * / `checkAttrs` helpers underneath it).
 *
 * Rows split parse from construction so the object-creation work can be
 * attributed:
 *   parse-only      JSON.parse(text)
 *   fromjson-only   schema.nodeFromJSON(json)          (json already parsed)
 *   parse-fromjson  nodeFromJSON(JSON.parse(text))     (the real workload)
 *   tojson          JSON.stringify(doc.toJSON())
 *   roundtrip       parse -> fromJSON -> toJSON -> stringify
 *
 * Every row carries an `expect` checksum (document nodeSize + FNV-1a of the
 * re-serialized JSON) so a wrong result is an error, not a number.
 *
 * This file is concatenated after the vendored module loader by build.mjs and
 * becomes tools/benchmark/workloads/95-prosemirror.js.  Globals provided by
 * the loader: OrderedMap, PM (prosemirror-model), schemaBasic.
 */

var Schema = PM.Schema;

/* --- schema: prosemirror-schema-basic + list nodes -------------------- */
function buildSchema() {
  var nodes = {};
  for (var k in schemaBasic.nodes) nodes[k] = schemaBasic.nodes[k];
  nodes.ordered_list = {
    attrs: { order: { default: 1, validate: 'number' } },
    content: 'list_item+',
    group: 'block',
    parseDOM: [{ tag: 'ol' }],
    toDOM: function (node) { return ['ol', { start: node.attrs.order }, 0]; }
  };
  nodes.bullet_list = {
    content: 'list_item+',
    group: 'block',
    parseDOM: [{ tag: 'ul' }],
    toDOM: function () { return ['ul', 0]; }
  };
  nodes.list_item = {
    content: 'paragraph block*',
    defining: true,
    parseDOM: [{ tag: 'li' }],
    toDOM: function () { return ['li', 0]; }
  };
  return new Schema({ nodes: nodes, marks: schemaBasic.marks });
}

var schema = buildSchema();

/* --- deterministic document generator -------------------------------- */
function mulberry32(a) {
  return function () {
    a |= 0; a = (a + 0x6d2b79f5) | 0;
    var t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

var WORDS = ('the quick brown fox jumps over a lazy dog and then some editors ' +
  'parse rich documents with marks nodes attrs schema fragment text heading ' +
  'paragraph blockquote code image link strong emphasis nested lists tables ' +
  'position mapping selection transaction step plugin view state').split(' ');

function sentence(rnd, minWords, maxWords) {
  var n = minWords + Math.floor(rnd() * (maxWords - minWords + 1));
  var parts = new Array(n);
  for (var i = 0; i < n; i++) parts[i] = WORDS[Math.floor(rnd() * WORDS.length)];
  return parts.join(' ');
}

function randMark(rnd) {
  var r = rnd();
  if (r < 0.28) return [{ type: 'strong' }];
  if (r < 0.52) return [{ type: 'em' }];
  if (r < 0.62) return [{ type: 'code' }];
  if (r < 0.72) return [{ type: 'link', attrs: { href: 'https://example.com/' + ((rnd() * 1000) | 0), title: null } }];
  if (r < 0.80) return [{ type: 'strong' }, { type: 'em' }];
  return null;
}

/* A run of inline content: text nodes carrying marks, plus the occasional
 * hard_break and inline image. */
function inlineContent(rnd) {
  var out = [];
  var runs = 1 + Math.floor(rnd() * 3);
  for (var i = 0; i < runs; i++) {
    var text = sentence(rnd, 2, 9);
    if (rnd() < 0.5) {
      out.push({ type: 'text', text: text + ' ' });
    } else {
      var n = { type: 'text', text: text };
      var m = randMark(rnd);
      if (m) n.marks = m;
      out.push(n);
    }
    if (rnd() < 0.06) out.push({ type: 'hard_break' });
    if (rnd() < 0.04) out.push({ type: 'image', attrs: { src: 'img/' + ((rnd() * 100) | 0) + '.png', alt: null, title: null } });
  }
  return out;
}

function paragraph(rnd) { return { type: 'paragraph', content: inlineContent(rnd) }; }

function heading(rnd) {
  return { type: 'heading', attrs: { level: 1 + Math.floor(rnd() * 3) }, content: [{ type: 'text', text: sentence(rnd, 2, 5) }] };
}

function blockquote(rnd) {
  var content = [];
  var n = 1 + Math.floor(rnd() * 2);
  for (var i = 0; i < n; i++) content.push(paragraph(rnd));
  return { type: 'blockquote', content: content };
}

function codeBlock(rnd) {
  return { type: 'code_block', content: [{ type: 'text', text: sentence(rnd, 4, 14) }] };
}

function list(rnd, ordered, depth) {
  var items = [];
  var n = 2 + Math.floor(rnd() * 4);
  for (var i = 0; i < n; i++) {
    var content = [paragraph(rnd)];
    if (depth < 2 && rnd() < 0.25) content.push(list(rnd, rnd() < 0.5, depth + 1));
    items.push({ type: 'list_item', content: content });
  }
  var node = { type: ordered ? 'ordered_list' : 'bullet_list', content: items };
  if (ordered) node.attrs = { order: 1 + ((rnd() * 3) | 0) };
  return node;
}

function buildDoc(blocks, seed) {
  var rnd = mulberry32(seed);
  var content = new Array(blocks);
  for (var i = 0; i < blocks; i++) {
    var r = rnd();
    if (r < 0.70) content[i] = paragraph(rnd);
    else if (r < 0.78) content[i] = heading(rnd);
    else if (r < 0.84) content[i] = blockquote(rnd);
    else if (r < 0.90) content[i] = codeBlock(rnd);
    else if (r < 0.95) content[i] = list(rnd, false, 0);
    else if (r < 0.98) content[i] = list(rnd, true, 0);
    else content[i] = { type: 'horizontal_rule' };
  }
  return { type: 'doc', content: content };
}

/* --- checksum -------------------------------------------------------- */
function fnv1a(s) {
  var h = 2166136261 >>> 0;
  for (var i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619) >>> 0;
  }
  return h >>> 0;
}

function docChecksum(node) {
  return ((node.nodeSize * 2654435761) >>> 0 ^ fnv1a(JSON.stringify(node.toJSON()))) >>> 0;
}

/* --- rows ------------------------------------------------------------ */
/*
 * Row names are written as literals (not built by concatenation) because the
 * benchmark harness discovers rows by scanning this file's text for the
 * bench-call name field.
 */
var jsonS = buildDoc(150, 0xC0FFEE + 150);
var textS = JSON.stringify(jsonS);
var docS = schema.nodeFromJSON(jsonS);
var cksS = docChecksum(docS);
var toS = fnv1a(JSON.stringify(docS.toJSON()));

var jsonM = buildDoc(600, 0xC0FFEE + 600);
var textM = JSON.stringify(jsonM);
var docM = schema.nodeFromJSON(jsonM);
var cksM = docChecksum(docM);
var toM = fnv1a(JSON.stringify(docM.toJSON()));

var jsonL = buildDoc(2000, 0xC0FFEE + 2000);
var textL = JSON.stringify(jsonL);
var docL = schema.nodeFromJSON(jsonL);
var cksL = docChecksum(docL);
var toL = fnv1a(JSON.stringify(docL.toJSON()));

bench({
  name: 'prosemirror/S/parse-only', unit: 'parse',
  run: function () { return JSON.parse(textS); },
  expect: function (v) { return v !== null && v.type === 'doc'; }
});
bench({
  name: 'prosemirror/S/fromjson-only', unit: 'fromJSON',
  run: function () { return schema.nodeFromJSON(jsonS); },
  expect: function (v) { return docChecksum(v) === cksS; }
});
bench({
  name: 'prosemirror/S/parse-fromjson', unit: 'parse+fromJSON',
  run: function () { return schema.nodeFromJSON(JSON.parse(textS)); },
  expect: function (v) { return docChecksum(v) === cksS; }
});
bench({
  name: 'prosemirror/S/tojson', unit: 'toJSON',
  run: function () { return JSON.stringify(docS.toJSON()); },
  expect: function (v) { return fnv1a(v) === toS; }
});
bench({
  name: 'prosemirror/S/roundtrip', unit: 'roundtrip',
  run: function () { return JSON.stringify(schema.nodeFromJSON(JSON.parse(textS)).toJSON()); },
  expect: function (v) { return fnv1a(v) === toS; }
});

bench({
  name: 'prosemirror/M/parse-only', unit: 'parse',
  run: function () { return JSON.parse(textM); },
  expect: function (v) { return v !== null && v.type === 'doc'; }
});
bench({
  name: 'prosemirror/M/fromjson-only', unit: 'fromJSON',
  run: function () { return schema.nodeFromJSON(jsonM); },
  expect: function (v) { return docChecksum(v) === cksM; }
});
bench({
  name: 'prosemirror/M/parse-fromjson', unit: 'parse+fromJSON',
  run: function () { return schema.nodeFromJSON(JSON.parse(textM)); },
  expect: function (v) { return docChecksum(v) === cksM; }
});
bench({
  name: 'prosemirror/M/tojson', unit: 'toJSON',
  run: function () { return JSON.stringify(docM.toJSON()); },
  expect: function (v) { return fnv1a(v) === toM; }
});
bench({
  name: 'prosemirror/M/roundtrip', unit: 'roundtrip',
  run: function () { return JSON.stringify(schema.nodeFromJSON(JSON.parse(textM)).toJSON()); },
  expect: function (v) { return fnv1a(v) === toM; }
});

bench({
  name: 'prosemirror/L/parse-only', unit: 'parse',
  run: function () { return JSON.parse(textL); },
  expect: function (v) { return v !== null && v.type === 'doc'; }
});
bench({
  name: 'prosemirror/L/fromjson-only', unit: 'fromJSON',
  run: function () { return schema.nodeFromJSON(jsonL); },
  expect: function (v) { return docChecksum(v) === cksL; }
});
bench({
  name: 'prosemirror/L/parse-fromjson', unit: 'parse+fromJSON',
  run: function () { return schema.nodeFromJSON(JSON.parse(textL)); },
  expect: function (v) { return docChecksum(v) === cksL; }
});
bench({
  name: 'prosemirror/L/tojson', unit: 'toJSON',
  run: function () { return JSON.stringify(docL.toJSON()); },
  expect: function (v) { return fnv1a(v) === toL; }
});
bench({
  name: 'prosemirror/L/roundtrip', unit: 'roundtrip',
  run: function () { return JSON.stringify(schema.nodeFromJSON(JSON.parse(textL)).toJSON()); },
  expect: function (v) { return fnv1a(v) === toL; }
});
