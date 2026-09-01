# Pending corpus files

These are differential fixtures for engine patches that are not in this tree
yet. `run.mjs` does not look here, so they do not gate the build; they are kept
so that the patch they belong to arrives with its proof already written.

Each one was run against the engine in this tree and the difference below is
what node actually reported.

| File | What differs today | Waits on |
|---|---|---|
| `bigint-number-compare.js` | `-1.7976931348623157e+308 < -9007199254740993n` is `false`; it is `true` | the fix for BigInt/float64 comparison when the double is large and negative |
| `string-rope-vs-flat.js` | a long concatenation compared to a flat string of the same characters: `==` says false where `===` says true | the fix for rope-vs-flat comparison |
| `cmp-br-widen-arms.js` | the same rope-vs-flat disagreement, reached through the fused compare-and-branch arms | as above |
| `cmp-widen-fast-arms.js` | as above, through the widened comparison arms | as above |
| `object-assign-set-semantics.js` | `{...'ab'}` is `{}`; it is `{"0":"a","1":"b"}` | the object-spread fast path and the Set-vs-Define split that goes with it |
| `element-store-template.js` | `obj[key] = rhs` evaluates the key before the right-hand side; the spec order is the other way round | the element-store template rewrite |
| `inline-js-frames.js` | stack overflow: every JavaScript-to-JavaScript call recurses into the interpreter, so the recursion depth the file needs is not reachable | inline JS-to-JS call frames |
| `return-tos-participation.js` | stack overflow, same cause | as above |

Two of these are correctness bugs in the engine as it stands, not merely
missing optimizations: the BigInt comparison and the object spread. They are
listed here rather than fixed because the fix belongs with the patch series
that already carries it.

To run one anyway:

    node tests/differential/run.mjs --corpus pending --qjs build/tests/qjs-run <name>
