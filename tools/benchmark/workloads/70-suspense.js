/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Exception-based control flow, in the shape React actually uses it.
 *
 * React does not only throw on errors. Suspense *suspends* a component by
 * throwing a thenable out of the render function; the reconciler catches it
 * several frames up, records the promise, and re-renders later. Error
 * boundaries are the same unwind path with a different payload. So on any
 * screen using `lazy()`, a data-fetching library built on Suspense, or an
 * error boundary, `throw` is on the render path rather than the failure path.
 *
 * None of the other workloads here throw at all — a `build_backtrace` counter
 * recorded zero calls across the whole suite and across a 990 KB production
 * bundle's startup. That is exactly why this file exists: the cost of throwing
 * is invisible to every other measurement, and it is a tail-latency cliff
 * rather than a throughput tax. A component that suspends pays it per suspend,
 * inside the frame budget.
 *
 * What each row isolates:
 *
 *   - `suspend-deep`: the real shape. A thenable thrown from a leaf render
 *     function, caught by a boundary five frames up. Nothing about the thrown
 *     value can carry a stack, and nothing reads one.
 *   - `suspend-shallow`: the same throw caught in the throwing frame — the
 *     floor of what an unwind can cost.
 *   - `error-boundary`: a real Error thrown and caught five frames up, with
 *     the boundary reading `.message`. This one legitimately builds a stack,
 *     at construction; it is here so a change that makes the cheap cases fast
 *     by making this case slow cannot hide.
 *   - `render-baseline`: the identical five-deep call chain with no throw, so
 *     the unwind cost is readable as a difference rather than a total.
 */

(function () {
  /* A thenable, as React's `lazy` and `use` produce: an object with `then`.
     Reused, because React reuses the same promise across suspends of the
     same resource, and because allocating one per iteration would measure
     the allocator instead of the unwind. */
  var pending = {
    then: function (resolve) {
      pending.resolvers.push(resolve);
    },
    resolvers: [],
  };

  /* A five-frame render stack: boundary -> host -> list -> row -> leaf.
     Five is not arbitrary — it is about the shallowest a real component tree
     gets between an error boundary and the component that suspends. */
  function renderLeaf(i, mode) {
    if (mode === 0) throw pending;
    if (mode === 1) throw new Error('boundary ' + (i & 7));
    return i & 7;
  }
  function renderRow(i, mode) {
    return renderLeaf(i, mode) + 1;
  }
  function renderList(i, mode) {
    return renderRow(i, mode) + 1;
  }
  function renderHost(i, mode) {
    return renderList(i, mode) + 1;
  }
  function renderTree(i, mode) {
    return renderHost(i, mode) + 1;
  }

  bench({
    name: 'suspense/suspend-deep',
    unit: '100 suspends',
    run: function () {
      var suspended = 0;
      for (var i = 0; i < 100; i++) {
        try {
          renderTree(i, 0);
        } catch (thrown) {
          if (thrown !== null && typeof thrown.then === 'function') {
            suspended++;
          } else {
            throw thrown;
          }
        }
      }
      return suspended;
    },
    expect: 100,
  });

  bench({
    name: 'suspense/suspend-shallow',
    unit: '100 suspends',
    run: function () {
      var suspended = 0;
      for (var i = 0; i < 100; i++) {
        try {
          throw pending;
        } catch (thrown) {
          if (typeof thrown.then === 'function') suspended++;
        }
      }
      return suspended;
    },
    expect: 100,
  });

  bench({
    name: 'suspense/error-boundary',
    unit: '100 errors',
    run: function () {
      var caught = 0;
      for (var i = 0; i < 100; i++) {
        try {
          renderTree(i, 1);
        } catch (err) {
          caught += err.message.length > 0 ? 1 : 0;
        }
      }
      return caught;
    },
    expect: 100,
  });

  bench({
    name: 'suspense/render-baseline',
    unit: '100 renders',
    run: function () {
      var total = 0;
      for (var i = 0; i < 100; i++) {
        total += renderTree(i, 2);
      }
      return total;
    },
    expect: 742,
  });
})();
