# engine

`engine/quickjs-ng` is the JavaScript engine: a git submodule pointing at our fork,
[react-native-quickjs/quickjs-ng](https://github.com/react-native-quickjs/quickjs-ng).

- `develop` (default branch): upstream plus our changes, one commit each. This is what the
  submodule records and what ships in the npm package (the files listed in `package.json`
  `"files"`).
- `master`: an untouched mirror of upstream [quickjs-ng](https://github.com/quickjs-ng/quickjs).

In the submodule, `origin` is the fork and `upstream` is quickjs-ng.

## Making an engine change

1. In `engine/quickjs-ng`, on `develop` (`git switch develop`; `git submodule update` leaves a
   detached HEAD), make the change and commit it with a single-line subject.
2. Push the fork: `git -C engine/quickjs-ng push origin develop`.
3. In the main repository, commit the new submodule pointer (`git add engine/quickjs-ng`).
   Push with `git push --recurse-submodules=on-demand`, so the engine commit the pointer names
   is always on the fork.

`node scripts/check-engine.js` (run by the commit hook, `prepack`, CI and the release script)
fails when the submodule is not at the recorded commit, has uncommitted edits, or an engine
file the build needs is missing from the tarball.

## Bytecode version

`BC_VERSION` in `quickjs.c` is the only bytecode version: the byte `JS_WriteObject` writes
first and `JS_ReadObject` checks, and the `bc=` key of an AOT profile header. It has one
value per VM build, since the two builds write different function bodies:

```c
#if QJS_REGVM
#define BC_VERSION (0x80 + 20) /* register bytecode */
#else
#define BC_VERSION 28 /* stack bytecode */
#endif
```

A file compiled by one build is refused by the other with "invalid version". When a
build's bytecode format changes, bump that build's value in a fork commit of its own,
with the subject `bump <stack|register> bytecode version to N`, and regenerate the
built-in blobs (`node scripts/regenerate-builtin-blobs.js` writes both `builtin-*.h` and
`builtin-*.regvm.h`). The committed `bin/qjsc` compilers are stack builds; the
`bytecode compilers` workflow rebuilds them when the stack value differs from
`bin/qjsc/manifest.json`.

## Syncing upstream

1. Update the fork's `master` from upstream: `gh repo sync react-native-quickjs/quickjs-ng -b master`.
2. In `engine/quickjs-ng`: `git fetch origin`, then on `develop` `git merge origin/master`,
   resolving conflicts in our commits.
3. Regenerate the built-in bytecode: `node scripts/regenerate-builtin-blobs.js`; commit the
   changed blobs in the fork.
4. Run the full suites, push the fork, and commit the new pointer as above.
