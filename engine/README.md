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

`BC_VERSION` in `quickjs.c` changes only in a fork commit of its own, with the subject
`bump bytecode version to N`. The committed `bin/qjsc` compilers are rebuilt by the
`bytecode compilers` workflow when it differs from `bin/qjsc/manifest.json`.

## Syncing upstream

1. Update the fork's `master` from upstream: `gh repo sync react-native-quickjs/quickjs-ng -b master`.
2. In `engine/quickjs-ng`: `git fetch origin`, then on `develop` `git merge origin/master`,
   resolving conflicts in our commits.
3. Regenerate the built-in bytecode: `node scripts/regenerate-builtin-blobs.js`; commit the
   changed blobs in the fork.
4. Run the full suites, push the fork, and commit the new pointer as above.
