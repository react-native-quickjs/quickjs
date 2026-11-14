// The legacy RegExp statics (RegExp.$1-$9, input/$_, lastMatch/$&, lastParen/$+, leftContext/$`, rightContext/$') are
// set by each successful match of a regexp made by this realm's RegExp: exec, test, search, split and the replace
// loop (the callback sees the last match). A failed match leaves them as they were; RegExp.input can be assigned.
// JetStream 3 Basic and WSL read them.
var r = /(\d+)-(\d+)?/;
print(RegExp.$1 === "", RegExp.lastMatch === "", RegExp.input === "");
r.exec("ab 12- cd");
print(RegExp.$1, "[" + RegExp.$2 + "]", RegExp.lastMatch, RegExp["$&"], "[" + RegExp.lastParen + "]", "[" + RegExp.leftContext + "]", "[" + RegExp.rightContext + "]", RegExp.input, RegExp.$_);
"x=5;y=7".replace(/(\w)=(\d)/g, function () { return RegExp.$1 + RegExp.$2; });
print(RegExp.$1, RegExp.$2, RegExp["$+"], RegExp["$`"], RegExp["$'"]);
/zz/.exec("nomatch"); print(RegExp.$1);
"a,b;c".split(/[,;]/); print(RegExp.lastMatch, RegExp.leftContext);
RegExp.input = 42; print(typeof RegExp.input, RegExp.input, RegExp.$_, RegExp.lastMatch);
/q(u)/.test("aquab"); print(RegExp.$1, RegExp.input, RegExp.rightContext);
"hello".search(/l+/); print(RegExp.lastMatch, RegExp.$1 === "");
print(Object.getOwnPropertyNames(RegExp).filter(function (k) { return k[0] === "$" || /input|last|Context/.test(k); }).sort().join(" "));
// A hot regexp with groups: in an AOT build its match runs in the unit's compiled matcher, and the statics must still be
// the last match's.
var mail = /([a-z]+)@([a-z]+)\.(com|org)/, hits = 0;
for (var k = 0; k < 2000; k++) if (mail.test("to: bob" + (k % 10) + " cc ann" + (k % 7 ? "" : "e") + "@site.org;")) hits++;
print(hits, RegExp.$1, RegExp.$2, RegExp.$3, RegExp.lastMatch, RegExp.lastParen, RegExp.leftContext, RegExp.rightContext);
