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

/* Each row: cache the serialized text, parse+checksum per iteration. */
bench({ name: 'jsonparse/int-array-2000', unit: 'parse', run: (function(){ var t=JSON.stringify(intArray(2000)); var e=checksum(JSON.parse(t)); return function(){ return checksum(JSON.parse(t)); }; })(), expect: (function(){ var j=intArray(2000); return checksum(j); })() });
bench({ name: 'jsonparse/dbl-array-2000', unit: 'parse', run: (function(){ var t=JSON.stringify(dblArray(2000)); return function(){ return checksum(JSON.parse(t)); }; })(), expect: (function(){ return checksum(dblArray(2000)); })() });
bench({ name: 'jsonparse/repeated-objects-800', unit: 'parse', run: (function(){ var t=JSON.stringify(repeatedObjects(800)); return function(){ return checksum(JSON.parse(t)); }; })(), expect: (function(){ return checksum(repeatedObjects(800)); })() });
bench({ name: 'jsonparse/small-nested', unit: 'parse', run: (function(){ var t=JSON.stringify(smallNested()); return function(){ return checksum(JSON.parse(t)); }; })(), expect: (function(){ return checksum(smallNested()); })() });
bench({ name: 'jsonparse/large-flat-300', unit: 'parse', run: (function(){ var t=JSON.stringify(largeFlat(300)); return function(){ return checksum(JSON.parse(t)); }; })(), expect: (function(){ return checksum(largeFlat(300)); })() });
bench({ name: 'jsonparse/string-heavy-120', unit: 'parse', run: (function(){ var t=JSON.stringify(stringHeavy(120)); return function(){ return checksum(JSON.parse(t)); }; })(), expect: (function(){ return checksum(stringHeavy(120)); })() });
bench({ name: 'jsonparse/escaped-nonascii', unit: 'parse', run: (function(){ var t=JSON.stringify(escaped()); return function(){ return checksum(JSON.parse(t)); }; })(), expect: (function(){ return checksum(escaped()); })() });
bench({ name: 'jsonparse/network-payload', unit: 'parse', run: (function(){ var t=JSON.stringify(networkPayload()); return function(){ return checksum(JSON.parse(t)); }; })(), expect: (function(){ return checksum(networkPayload()); })() });
bench({ name: 'jsonparse/network-reviver', unit: 'parse', run: (function(){ var t=JSON.stringify(networkPayload()); return function(){ return checksum(JSON.parse(t,function(k,v){return v;})); }; })(), expect: (function(){ return checksum(JSON.parse(JSON.stringify(networkPayload()))); })() });
