// Strings: concatenation, rope linearization, indexing, search, compare.
function k(n) {
  var s = "", i;
  for (i = 0; i < n; i++) s = s + (i & 7);
  var parts = [];
  for (i = 0; i < 5; i++) parts.push("p" + i);
  return [s.length, s.charCodeAt(3), s.indexOf("5"), parts.join("-"),
          "abc".slice(1), "ABC".toLowerCase(), "abc" < "abd"];
}
print(k(2000));
