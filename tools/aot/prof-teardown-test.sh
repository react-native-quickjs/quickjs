#!/bin/bash
#
# prof-teardown-test.sh -- the profiling engine tears down cleanly.
#
# A profile site takes a counted reference on each shape it records. A function that is collected before the
# profile dump is freed together with its site records, so those references have to be released then; if they
# are not, JS_FreeRuntime finds a shape left on the collector's list and fails
# `assert(list_empty(&rt->gc_obj_list))`. This runs scripts whose functions become garbage before the end of the
# run, under the profiling engine, and requires each to exit 0.
#
#   tools/aot/prof-teardown-test.sh <qjsprof-binary>   (or QJS_AOT_PROF_BIN), a -DQJS_REGVM=1 -DQJS_AOT_PROFILE=1 runner
set -u
bin=${1:-${QJS_AOT_PROF_BIN:-}}
[ -x "$bin" ] || { echo "usage: $0 <qjsprof-binary>   (or set QJS_AOT_PROF_BIN)"; echo "note: build with -DQJS_REGVM=1 -DQJS_AOT_PROFILE=1"; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/prof-teardown.XXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT
cat > "$T/collected-closure.js" <<'JS'
(function () {
  var r;
  var f4 = function (n) { var out = "?"; Array.fromAsync([1, 2, 3]).then(function (a) { out = a.join(","); }); return function () { return out; }; };
  for (var k = 0; k < 200; k++) r = f4(0);
})();
JS
cat > "$T/collected-methods.js" <<'JS'
(function () {
  function P(x) { this.x = x; }
  P.prototype.get = function () { return this.x; };
  var s = 0;
  for (var i = 0; i < 300; i++) { var p = new P(i); s += p.get(); }
  return s;
})();
JS
fail=0
for f in "$T"/*.js; do
  QJS_AOT_PROFILE_OUT="$T/out.qjsprof" "$bin" "$f" > "$T/run.out" 2>&1
  rc=$?
  if [ $rc -eq 0 ]; then echo "ok   $(basename "$f")"; else echo "FAIL $(basename "$f") (exit $rc): $(grep -a -m1 Assertion "$T/run.out")"; fail=1; fi
done
exit $fail
