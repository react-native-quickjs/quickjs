/*
 * JSON.parse coverage for the benchmark harness.
 *
 * One row per payload shape, so parse cost can be studied separately.
 * Each row carries an `expect` guard (a checksum) so a wrong parse is
 * reported as an error, not a number.
 */

/* --- payload builders -------------------------------------------------- */
function intArray(n) { var a=new Array(n); for(var i=0;i<n;i++) a[i]=i*7-999; return a; }
function dblArray(n) { var a=new Array(n); for(var i=0;i<n;i++) a[i]=i*0.5+0.25; return a; }
function repeatedObjects(n) { var a=new Array(n); for(var i=0;i<n;i++) a[i]={id:i,type:'a',active:true,score:3.5}; return a; }
function smallNested() { return {a:{b:{c:{d:{e:{v:1}}}}}}; }
function largeFlat(k) { var o={}; for(var i=0;i<k;i++) o['key_'+i]=i; return o; }
function stringHeavy(n) { var o={}; for(var i=0;i<n;i++) o['s'+i]='string_value_'+i; return o; }
function escaped() { return {ascii:'plain',escaped:'has\ttab and "quote" and \\ slash',control:'ctrl\nnewline',nonascii:'caf\u00e9 \u00e9\u00e8 se\u00f1or',emoji:'\ud83d\ude00 smile',loneSur:'\ud800 lone'}; }
function networkPayload() {
  var feeds=new Array(60);
  for(var i=0;i<60;i++) feeds[i]={id:'post-'+i,title:'Post '+i+' headline',body:'Content of the post that has some length to it '.repeat(3),author:{id:'u'+i,name:'User '+i,verified:i%2===0},stats:{likes:i*13,comments:i*3,views:i*1000,shares:i},tags:['news','tech','mobile'].slice(0,i%4),created_at:1600000000000+i*1000,updated_at:1600000000000+i*2000};
  return {status:'ok',feeds:feeds,meta:{total:60,page:1,page_size:20}};
}

/* Stable checksum over a parsed value. */
function checksum(v){var h=0;(function walk(x){if(typeof x==='number'){h+=Math.floor(x*65536)|0;return;}if(typeof x==='string'){for(var i=0;i<x.length;i++)h+=x.charCodeAt(i);return;}if(typeof x==='boolean'){h+=x?7:3;return;}if(Array.isArray(x)){for(var i=0;i<x.length;i++)walk(x[i]);h++;return;}var ks=Object.keys(x).sort();for(var j=0;j<ks.length;j++){h+=ks[j].length;walk(x[ks[j]]);}})(v);return h;}

/* Keep parsing, traversal, and the real-world combined operation separate. */
function addJsonParseRows(name, text, expected, reviver) {
  var parse = function() { return JSON.parse(text, reviver); };
  var parsed = JSON.parse(text, reviver);
  bench({
    name: 'jsonparse/' + name + '/parse-only', unit: 'parse', run: parse,
    expect: function(v) { return v !== null; }
  });
  bench({
    name: 'jsonparse/' + name + '/traversal-only', unit: 'traversal',
    run: function() { return checksum(parsed); }, expect: expected
  });
  bench({
    name: 'jsonparse/' + name + '/parse-consume', unit: 'parse+traversal',
    run: function() { return checksum(parse()); }, expect: expected
  });
}

addJsonParseRows('int-array-2000', JSON.stringify(intArray(2000)),
                checksum(intArray(2000)));
addJsonParseRows('dbl-array-2000', JSON.stringify(dblArray(2000)),
                checksum(dblArray(2000)));
addJsonParseRows('repeated-objects-800', JSON.stringify(repeatedObjects(800)),
                checksum(repeatedObjects(800)));
addJsonParseRows('small-nested', JSON.stringify(smallNested()),
                checksum(smallNested()));
addJsonParseRows('large-flat-300', JSON.stringify(largeFlat(300)),
                checksum(largeFlat(300)));
addJsonParseRows('string-heavy-120', JSON.stringify(stringHeavy(120)),
                checksum(stringHeavy(120)));
addJsonParseRows('escaped-nonascii', JSON.stringify(escaped()),
                checksum(escaped()));
addJsonParseRows('network-payload', JSON.stringify(networkPayload()),
                checksum(networkPayload()));
addJsonParseRows(
    'network-reviver', JSON.stringify(networkPayload()),
    checksum(JSON.parse(JSON.stringify(networkPayload()), function(k, v) {
      return v;
    })), function(k, v) { return v; });
