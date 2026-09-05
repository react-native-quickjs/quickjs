/*
 * React-shaped work.
 *
 * These are not "React benchmarks" in the sense of running React — they are
 * models of the five things React's hot path actually does, extracted from
 * react-dom/react-reconciler and reduced to code that runs on any engine with
 * nothing but `print` and `Date.now`.
 *
 * What the reconciler actually is, mechanically:
 *
 *   - Small fixed-shape object allocation, constantly. A JSX element is a
 *     5-field object; a FiberNode is 22 fields; a hook is 5 fields and one is
 *     allocated per hook per render. Nothing here is Map-heavy or string-heavy
 *     outside of child reconciliation, and the inline-cache/hidden-class story
 *     for these shapes is most of the story.
 *   - Null checks and `===` identity checks, in enormous quantity.
 *   - A giant switch on a small integer `.tag`, plus bitwise lane arithmetic.
 *
 * So these workloads are deliberately allocation- and property-access-bound.
 * That is the honest shape of the work, not a choice made to favour an engine.
 *
 * Reference points measured on Hermes for the same shapes (so a number here
 * that is wildly below them means something got optimised away rather than
 * measured):
 *
 *   element alloc            69 ns
 *   element alloc with key  239 ns   (3.5x — extra object + for-in copy)
 *   FiberNode alloc         138 ns
 *   hook object alloc        23 ns
 *   areHookInputsEqual/3    111 ns
 *
 * The keyed-element gap is the single most surprising fact in here and it is
 * worth stating plainly: giving a React element a `key` makes creating it 3.5x
 * more expensive, because jsxProd stops being able to adopt the config object
 * as `props` and must allocate a second object and for-in copy every property
 * into it.
 */

/* ------------------------------------------------------------------ */
/* React element                                                       */
/* ------------------------------------------------------------------ */

/* The real one is Symbol.for('react.element'); the numeric fallback React
   itself uses when Symbol is absent is what we model, since the symbol lookup
   is not part of the per-element cost either way. */
var REACT_ELEMENT_TYPE = 0xeac7;

/*
 * jsxProd, from react/src/jsx/ReactJSXElement.js, with the __DEV__ branches
 * removed (they do not ship). The shape of the fast path is the point: when
 * there is no key and no ref the config object is *adopted* as props with zero
 * copying. Adding a key forces the else-branch below.
 */
function jsxProd(type, config, maybeKey) {
  var key = null;
  var ref = null;

  if (maybeKey !== undefined) key = '' + maybeKey;
  if (config.key !== undefined && config.key !== null) key = '' + config.key;
  if (config.ref !== undefined && config.ref !== null) ref = config.ref;

  var props;
  if (key !== null || ref !== null) {
    props = {};
    for (var propName in config) {
      if (propName !== 'key' && propName !== 'ref') {
        props[propName] = config[propName];
      }
    }
  } else {
    props = config;
  }

  return {
    $$typeof: REACT_ELEMENT_TYPE,
    type: type,
    key: key,
    ref: ref,
    props: props,
  };
}

var STYLE = { flex: 1, padding: 8 };
function noop() {}

bench({
  name: 'react/create-element',
  unit: 'element',
  run: function () {
    var e = jsxProd(
      'View',
      {
        className: 'row',
        style: STYLE,
        onPress: noop,
        testID: 'r',
        accessible: true,
        pointerEvents: 'auto',
        children: null,
      },
      undefined
    );
    /* Touch enough fields that the whole object has to exist. */
    return e.props.style.flex + e.type.length + (e.key === null ? 0 : 1);
  },
  expect: 5,
});

bench({
  name: 'react/create-element-keyed',
  unit: 'element',
  run: function () {
    var e = jsxProd(
      'View',
      {
        className: 'row',
        style: STYLE,
        onPress: noop,
        testID: 'r',
        accessible: true,
        pointerEvents: 'auto',
        children: null,
      },
      'k7'
    );
    return e.props.style.flex + e.type.length + e.key.length;
  },
  expect: 7,
});

/* ------------------------------------------------------------------ */
/* Fiber                                                               */
/* ------------------------------------------------------------------ */

/* Work tags, from ReactWorkTags.js. Only the ones we model. */
var FunctionComponent = 0;
var HostRoot = 3;
var HostComponent = 5;
var HostText = 6;

var NoFlags = 0;
var Placement = 2;
var Update = 4;
var NoLanes = 0;

/*
 * FiberNode, verbatim in field count and order from
 * react-reconciler/src/ReactFiber.js. 22 fields, every one initialised in the
 * constructor — which is what gives every fiber the same hidden class, and is
 * why this allocation is as cheap as it is on an engine that has hidden
 * classes at all.
 */
function FiberNode(tag, pendingProps, key, mode) {
  // Instance
  this.tag = tag;
  this.key = key;
  this.elementType = null;
  this.type = null;
  this.stateNode = null;

  // Fiber
  this.return = null;
  this.child = null;
  this.sibling = null;
  this.index = 0;

  this.ref = null;
  this.refCleanup = null;

  this.pendingProps = pendingProps;
  this.memoizedProps = null;
  this.updateQueue = null;
  this.memoizedState = null;
  this.dependencies = null;

  this.mode = mode;

  // Effects
  this.flags = NoFlags;
  this.subtreeFlags = NoFlags;
  this.deletions = null;

  this.lanes = NoLanes;
  this.childLanes = NoLanes;

  this.alternate = null;
}

var FIBER_PROPS = { className: 'row', style: STYLE, children: null };

bench({
  name: 'react/fiber-alloc',
  unit: 'fiber',
  run: function () {
    var f = new FiberNode(HostComponent, FIBER_PROPS, null, 1);
    f.elementType = 'View';
    f.type = 'View';
    return f.tag + f.mode + f.flags;
  },
  expect: 6,
});

/*
 * createWorkInProgress, from ReactFiber.js. This is the double-buffering that
 * makes React's re-renders cheap: the fiber for the previous render is kept
 * alive as `.alternate` and reused, so a re-render is ~10 field stores instead
 * of a 22-field allocation. Whether an engine makes those stores cheap is
 * therefore worth more to React than allocation speed is.
 */
function createWorkInProgress(current, pendingProps) {
  var workInProgress = current.alternate;
  if (workInProgress === null) {
    workInProgress = new FiberNode(current.tag, pendingProps, current.key, current.mode);
    workInProgress.elementType = current.elementType;
    workInProgress.type = current.type;
    workInProgress.stateNode = current.stateNode;
    workInProgress.alternate = current;
    current.alternate = workInProgress;
  } else {
    workInProgress.pendingProps = pendingProps;
    workInProgress.type = current.type;
    workInProgress.flags = NoFlags;
    workInProgress.subtreeFlags = NoFlags;
    workInProgress.deletions = null;
  }

  workInProgress.lanes = current.lanes;
  workInProgress.childLanes = current.childLanes;
  workInProgress.child = current.child;
  workInProgress.memoizedProps = current.memoizedProps;
  workInProgress.memoizedState = current.memoizedState;
  workInProgress.updateQueue = current.updateQueue;
  workInProgress.dependencies = current.dependencies;
  workInProgress.sibling = current.sibling;
  workInProgress.index = current.index;
  workInProgress.ref = current.ref;
  workInProgress.refCleanup = current.refCleanup;

  return workInProgress;
}

var cloneCurrent = null;
var cloneProps = null;

bench({
  name: 'react/fiber-clone',
  unit: 'fiber',
  setup: function () {
    cloneCurrent = new FiberNode(HostComponent, FIBER_PROPS, 'k', 1);
    cloneCurrent.elementType = 'View';
    cloneCurrent.type = 'View';
    cloneCurrent.memoizedProps = FIBER_PROPS;
    cloneCurrent.lanes = 1;
    cloneCurrent.childLanes = 3;
    cloneProps = { className: 'row', style: STYLE, children: null };
    /* Prime the alternate so we measure the reuse path, not the alloc path —
       the reuse path is what steady-state React actually executes. */
    createWorkInProgress(cloneCurrent, cloneProps);
  },
  run: function () {
    var wip = createWorkInProgress(cloneCurrent, cloneProps);
    return wip.tag + wip.lanes + wip.childLanes;
  },
  expect: 9,
});

/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

var objectIs = typeof Object.is === 'function' ? Object.is : function (x, y) {
  return (x === y && (x !== 0 || 1 / x === 1 / y)) || (x !== x && y !== y);
};

/* areHookInputsEqual, from ReactFiberHooks.js. Runs once per dep-carrying hook
   per render, so a component with useMemo/useCallback/useEffect runs it three
   times on every single render. */
function areHookInputsEqual(nextDeps, prevDeps) {
  if (prevDeps === null) return false;
  for (var i = 0; i < prevDeps.length && i < nextDeps.length; i++) {
    if (objectIs(nextDeps[i], prevDeps[i])) continue;
    return false;
  }
  return true;
}

var currentlyRenderingFiber = null;
var workInProgressHook = null;
var currentHook = null;
var isMountPhase = true;

/* The hook object: 5 fields, one per hook per render, always. */
function mountWorkInProgressHook() {
  var hook = {
    memoizedState: null,
    baseState: null,
    baseQueue: null,
    queue: null,
    next: null,
  };
  if (workInProgressHook === null) {
    currentlyRenderingFiber.memoizedState = workInProgressHook = hook;
  } else {
    workInProgressHook = workInProgressHook.next = hook;
  }
  return workInProgressHook;
}

function updateWorkInProgressHook() {
  var nextCurrentHook;
  if (currentHook === null) {
    var cur = currentlyRenderingFiber.alternate;
    nextCurrentHook = cur !== null ? cur.memoizedState : null;
  } else {
    nextCurrentHook = currentHook.next;
  }
  currentHook = nextCurrentHook;
  /* Note: React allocates a *fresh* hook object on update too. The list is
     rebuilt every render; only the values are carried over. */
  var newHook = {
    memoizedState: currentHook.memoizedState,
    baseState: currentHook.baseState,
    baseQueue: currentHook.baseQueue,
    queue: currentHook.queue,
    next: null,
  };
  if (workInProgressHook === null) {
    currentlyRenderingFiber.memoizedState = workInProgressHook = newHook;
  } else {
    workInProgressHook = workInProgressHook.next = newHook;
  }
  return workInProgressHook;
}

function dispatchSetState(fiber, queue, action) {
  queue.pending = action;
  fiber.lanes = fiber.lanes | 1;
}

function useState(initial) {
  if (isMountPhase) {
    var hook = mountWorkInProgressHook();
    hook.memoizedState = hook.baseState = initial;
    var queue = { pending: null, lastRenderedState: initial };
    hook.queue = queue;
    /* React really does allocate a bound function here, per useState, per
       mount. It is not free and it is not avoidable from userland. */
    return [initial, dispatchSetState.bind(null, currentlyRenderingFiber, queue)];
  }
  var h = updateWorkInProgressHook();
  var q = h.queue;
  var pending = q.pending;
  if (pending !== null) {
    q.pending = null;
    h.memoizedState = h.baseState = pending;
  }
  return [h.memoizedState, dispatchSetState.bind(null, currentlyRenderingFiber, q)];
}

function useMemo(nextCreate, deps) {
  if (isMountPhase) {
    var hook = mountWorkInProgressHook();
    var v = nextCreate();
    hook.memoizedState = [v, deps];
    return v;
  }
  var h = updateWorkInProgressHook();
  var prevState = h.memoizedState;
  if (prevState !== null && deps !== null) {
    if (areHookInputsEqual(deps, prevState[1])) return prevState[0];
  }
  var nv = nextCreate();
  h.memoizedState = [nv, deps];
  return nv;
}

function useCallback(callback, deps) {
  if (isMountPhase) {
    var hook = mountWorkInProgressHook();
    hook.memoizedState = [callback, deps];
    return callback;
  }
  var h = updateWorkInProgressHook();
  var prevState = h.memoizedState;
  if (prevState !== null && deps !== null) {
    if (areHookInputsEqual(deps, prevState[1])) return prevState[0];
  }
  h.memoizedState = [callback, deps];
  return callback;
}

function useEffect(create, deps) {
  if (isMountPhase) {
    var hook = mountWorkInProgressHook();
    currentlyRenderingFiber.flags = currentlyRenderingFiber.flags | Update;
    hook.memoizedState = { tag: 1, create: create, destroy: null, deps: deps, next: null };
    return;
  }
  var h = updateWorkInProgressHook();
  var prevEffect = h.memoizedState;
  if (prevEffect !== null && deps !== null) {
    if (areHookInputsEqual(deps, prevEffect.deps)) {
      h.memoizedState = { tag: 0, create: create, destroy: prevEffect.destroy, deps: deps, next: null };
      return;
    }
  }
  currentlyRenderingFiber.flags = currentlyRenderingFiber.flags | Update;
  h.memoizedState = { tag: 1, create: create, destroy: null, deps: deps, next: null };
}

function renderWithHooks(workInProgress, current, Component, props) {
  currentlyRenderingFiber = workInProgress;
  workInProgressHook = null;
  currentHook = null;
  isMountPhase = current === null || current.memoizedState === null;
  workInProgress.memoizedState = null;
  workInProgress.updateQueue = null;
  var children = Component(props);
  currentlyRenderingFiber = null;
  workInProgressHook = null;
  currentHook = null;
  return children;
}

/* A component with the hook mix a real screen has: two states, a memo, a
   callback and an effect — five hooks, three of which compare deps. */
function HookyComponent(props) {
  var s0 = useState(0);
  var s1 = useState(props.name);
  var total = useMemo(function () {
    return props.a + props.b + props.c;
  }, [props.a, props.b, props.c]);
  var onPress = useCallback(function () {
    return s0[0];
  }, [props.a, props.b]);
  useEffect(function () {
    return null;
  }, [props.a, total]);
  return total + s1[0].length + (onPress === null ? 1 : 0);
}

var hookProps = { name: 'row', a: 1, b: 2, c: 3 };

bench({
  name: 'react/hooks',
  unit: 'render-pair',
  run: function () {
    /* One mount and one update of the same component: the mount allocates the
       hook list, the update rebuilds it and runs three dep comparisons, all of
       which hit and bail. That pairing is what a React app does on every
       interaction. */
    var fiber = new FiberNode(FunctionComponent, hookProps, null, 1);
    var mounted = renderWithHooks(fiber, null, HookyComponent, hookProps);
    var wip = createWorkInProgress(fiber, hookProps);
    var updated = renderWithHooks(wip, fiber, HookyComponent, hookProps);
    return mounted + updated;
  },
  expect: 18,
});

var depsA = [1, 'two', STYLE];
var depsB = [1, 'two', STYLE];

bench({
  name: 'react/hook-deps',
  unit: 'compare',
  run: function () {
    /* The equal case, which is the case that matters: a dep comparison that
       fails exits on its first element, a dep comparison that succeeds walks
       the whole array. Successful bailouts are the common path. */
    return areHookInputsEqual(depsA, depsB) ? 1 : 0;
  },
  expect: 1,
});

/* ------------------------------------------------------------------ */
/* Child reconciliation                                                */
/* ------------------------------------------------------------------ */

var isArray = Array.isArray;

function createFiberFromElement(element) {
  var type = element.type;
  var tag = typeof type === 'function' ? FunctionComponent : HostComponent;
  var fiber = new FiberNode(tag, element.props, element.key, 1);
  fiber.elementType = type;
  fiber.type = type;
  return fiber;
}

function createFiberFromText(content) {
  return new FiberNode(HostText, content, null, 1);
}

function useFiber(fiber, pendingProps) {
  var clone = createWorkInProgress(fiber, pendingProps);
  clone.index = 0;
  clone.sibling = null;
  return clone;
}

/*
 * The keyed-list reconciler, from ReactChildFiber.js. Two passes: a linear
 * pass that works as long as keys line up positionally, then a Map-based pass
 * for whatever is left. The Map is built only when the first pass gives up —
 * so an append is Map-free and a reorder is not, which is exactly what these
 * two benchmarks separate.
 */
function mapRemainingChildren(currentFirstChild) {
  var existingChildren = new Map();
  var existingChild = currentFirstChild;
  while (existingChild !== null) {
    if (existingChild.key !== null) {
      existingChildren.set(existingChild.key, existingChild);
    } else {
      existingChildren.set(existingChild.index, existingChild);
    }
    existingChild = existingChild.sibling;
  }
  return existingChildren;
}

function updateElement(returnFiber, current, element) {
  if (current !== null && current.elementType === element.type) {
    var existing = useFiber(current, element.props);
    existing.return = returnFiber;
    return existing;
  }
  var created = createFiberFromElement(element);
  created.return = returnFiber;
  return created;
}

function updateSlot(returnFiber, oldFiber, newChild) {
  var key = oldFiber !== null ? oldFiber.key : null;
  if (newChild.key !== key) return null;
  return updateElement(returnFiber, oldFiber, newChild);
}

function updateFromMap(existingChildren, returnFiber, newIdx, newChild) {
  var matchedFiber =
    newChild.key === null
      ? existingChildren.get(newIdx) || null
      : existingChildren.get(newChild.key) || null;
  return updateElement(returnFiber, matchedFiber, newChild);
}

function placeChild(newFiber, lastPlacedIndex, newIndex) {
  newFiber.index = newIndex;
  var current = newFiber.alternate;
  if (current !== null) {
    var oldIndex = current.index;
    if (oldIndex < lastPlacedIndex) {
      newFiber.flags = newFiber.flags | Placement;
      return lastPlacedIndex;
    }
    return oldIndex;
  }
  newFiber.flags = newFiber.flags | Placement;
  return lastPlacedIndex;
}

/* Returns the number of children that had to be moved (Placement), which is
   the thing the algorithm exists to minimise and therefore the thing worth
   gating on. */
function reconcileChildrenArray(returnFiber, currentFirstChild, newChildren) {
  var resultingFirstChild = null;
  var previousNewFiber = null;
  var oldFiber = currentFirstChild;
  var lastPlacedIndex = 0;
  var newIdx = 0;
  var nextOldFiber = null;
  var placements = 0;

  for (; oldFiber !== null && newIdx < newChildren.length; newIdx++) {
    if (oldFiber.index > newIdx) {
      nextOldFiber = oldFiber;
      oldFiber = null;
    } else {
      nextOldFiber = oldFiber.sibling;
    }
    var newFiber = updateSlot(returnFiber, oldFiber, newChildren[newIdx]);
    if (newFiber === null) {
      if (oldFiber === null) oldFiber = nextOldFiber;
      break;
    }
    var before = newFiber.flags;
    lastPlacedIndex = placeChild(newFiber, lastPlacedIndex, newIdx);
    if (newFiber.flags !== before) placements++;
    if (previousNewFiber === null) resultingFirstChild = newFiber;
    else previousNewFiber.sibling = newFiber;
    previousNewFiber = newFiber;
    oldFiber = nextOldFiber;
  }

  if (newIdx === newChildren.length) {
    return placements;
  }

  /* Second pass: whatever is left goes through the Map. */
  var existingChildren = mapRemainingChildren(oldFiber);
  for (; newIdx < newChildren.length; newIdx++) {
    var mapped = updateFromMap(existingChildren, returnFiber, newIdx, newChildren[newIdx]);
    if (mapped.alternate !== null) {
      existingChildren.delete(mapped.key === null ? newIdx : mapped.key);
    }
    var wasFlags = mapped.flags;
    lastPlacedIndex = placeChild(mapped, lastPlacedIndex, newIdx);
    if (mapped.flags !== wasFlags) placements++;
    if (previousNewFiber === null) resultingFirstChild = mapped;
    else previousNewFiber.sibling = mapped;
    previousNewFiber = mapped;
  }

  return placements;
}

var CHILD_COUNT = 20;
var reconcileParent = null;
var reconcileCurrentFirst = null;
var childrenInOrder = null;
var childrenReordered = null;

function buildChildElements(order) {
  var out = [];
  for (var i = 0; i < order.length; i++) {
    out.push(jsxProd('Item', { index: order[i], style: STYLE }, 'k' + order[i]));
  }
  return out;
}

function setupReconcile() {
  reconcileParent = new FiberNode(HostComponent, null, null, 1);

  var inOrder = [];
  for (var i = 0; i < CHILD_COUNT; i++) inOrder.push(i);
  childrenInOrder = buildChildElements(inOrder);

  /* The reorder case: move the last child to the front. That is the smallest
     edit that defeats the positional fast path entirely — every subsequent
     child mismatches on key, so the whole list goes through the Map. */
  var moved = [CHILD_COUNT - 1];
  for (var j = 0; j < CHILD_COUNT - 1; j++) moved.push(j);
  childrenReordered = buildChildElements(moved);

  /* Build the "current" child list the reconciler will match against. */
  var prev = null;
  for (var k = 0; k < CHILD_COUNT; k++) {
    var f = createFiberFromElement(childrenInOrder[k]);
    f.return = reconcileParent;
    f.index = k;
    if (prev === null) reconcileCurrentFirst = f;
    else prev.sibling = f;
    prev = f;
  }
  /* Prime every alternate, so both benchmarks measure steady-state re-render
     rather than first-mount allocation. */
  reconcileChildrenArray(reconcileParent, reconcileCurrentFirst, childrenInOrder);
}

bench({
  name: 'react/reconcile-children',
  unit: 'list',
  setup: setupReconcile,
  run: function () {
    /* Same keys, same order: the linear pass handles all 20 and the Map is
       never allocated. */
    return reconcileChildrenArray(reconcileParent, reconcileCurrentFirst, childrenInOrder);
  },
  expect: 0,
});

bench({
  name: 'react/reconcile-children-reorder',
  unit: 'list',
  setup: function () {
    if (reconcileCurrentFirst === null) setupReconcile();
    reconcileChildrenArray(reconcileParent, reconcileCurrentFirst, childrenReordered);
  },
  run: function () {
    /* One child moved to the front: the fast path dies at index 0 and all 20
       go through mapRemainingChildren + 20 Map lookups + 19 deletes. */
    return reconcileChildrenArray(reconcileParent, reconcileCurrentFirst, childrenReordered);
  },
  expect: function (got) {
    return got === 19;
  },
});

/* ------------------------------------------------------------------ */
/* Whole-tree render                                                   */
/* ------------------------------------------------------------------ */

/*
 * The closest thing here to a real render: mount a 37-element / 3-level tree
 * and then re-render it, using the element, fiber, hook and reconciliation
 * code above. Everything a React commit does *after* reconciliation (host
 * mutation, effects, layout) is absent, which is correct for this comparison —
 * on React Native that part is C++, not JS.
 */

function Cell(props) {
  return jsxProd('Text', { className: 'cell', total: props.total, children: props.text });
}

function Row(props) {
  var state = useState(props.index);
  var total = useMemo(function () {
    return props.index * 10;
  }, [props.index]);
  var onPress = useCallback(function () {
    return props.index;
  }, [props.index]);
  /* Keyed children, i.e. the expensive element path — which is also the path
     any list in a real app takes. */
  var children = [];
  for (var i = 0; i < 5; i++) {
    children.push(jsxProd(Cell, { text: 'c', index: i, total: total, onPress: onPress }, i));
  }
  return jsxProd('View', { className: 'row', state: state[0], children: children });
}

function App(props) {
  var rows = [];
  for (var i = 0; i < 3; i++) {
    rows.push(jsxProd(Row, { index: i, gen: props.gen }, i));
  }
  return jsxProd('View', { className: 'app', children: rows });
}

var treeUnits = 0;

function componentOf(fiber) {
  return fiber.type;
}

function mountUnit(child, returnFiber) {
  treeUnits++;
  if (typeof child === 'string') {
    var t = createFiberFromText(child);
    t.return = returnFiber;
    t.memoizedProps = child;
    return t;
  }
  var fiber = createFiberFromElement(child);
  fiber.return = returnFiber;
  if (fiber.tag === FunctionComponent) {
    var next = renderWithHooks(fiber, null, componentOf(fiber), fiber.pendingProps);
    mountChildren(fiber, next);
  } else {
    mountChildren(fiber, fiber.pendingProps.children);
  }
  fiber.memoizedProps = fiber.pendingProps;
  return fiber;
}

function mountChildren(fiber, children) {
  if (children === null || children === undefined) {
    fiber.child = null;
    return;
  }
  if (isArray(children)) {
    var prev = null;
    for (var i = 0; i < children.length; i++) {
      var c = mountUnit(children[i], fiber);
      c.index = i;
      if (prev === null) fiber.child = c;
      else prev.sibling = c;
      prev = c;
    }
  } else {
    fiber.child = mountUnit(children, fiber);
  }
}

function matches(fiber, child) {
  if (typeof child === 'string') return fiber.tag === HostText;
  return fiber.elementType === child.type && fiber.key === child.key;
}

function updateUnit(current, child, returnFiber) {
  treeUnits++;
  if (typeof child === 'string') {
    var wipText = createWorkInProgress(current, child);
    wipText.return = returnFiber;
    wipText.sibling = null;
    wipText.memoizedProps = child;
    return wipText;
  }
  var wip = createWorkInProgress(current, child.props);
  wip.return = returnFiber;
  wip.sibling = null;
  if (wip.tag === FunctionComponent) {
    var next = renderWithHooks(wip, current, componentOf(wip), wip.pendingProps);
    updateChildren(wip, next);
  } else {
    updateChildren(wip, wip.pendingProps.children);
  }
  wip.memoizedProps = wip.pendingProps;
  return wip;
}

/* createWorkInProgress already copied current.child onto wip, so wip.child is
   the previous child list on entry — the same aliasing React relies on. */
function updateChildren(wip, children) {
  var oldFiber = wip.child;
  if (children === null || children === undefined) {
    wip.child = null;
    return;
  }
  var prev = null;
  var f;
  if (isArray(children)) {
    for (var i = 0; i < children.length; i++) {
      var nextOld = oldFiber !== null ? oldFiber.sibling : null;
      if (oldFiber !== null && matches(oldFiber, children[i])) {
        f = updateUnit(oldFiber, children[i], wip);
      } else {
        f = mountUnit(children[i], wip);
      }
      f.index = i;
      f.sibling = null;
      if (prev === null) wip.child = f;
      else prev.sibling = f;
      prev = f;
      oldFiber = nextOld;
    }
  } else {
    if (oldFiber !== null && matches(oldFiber, children)) {
      f = updateUnit(oldFiber, children, wip);
    } else {
      f = mountUnit(children, wip);
    }
    f.index = 0;
    f.sibling = null;
    wip.child = f;
  }
}

function renderTree() {
  var rootFiber = new FiberNode(HostRoot, null, null, 1);

  var el1 = jsxProd(App, { gen: 1 });
  rootFiber.child = mountUnit(el1, rootFiber);

  /* Re-render. New elements, so nothing bails out on props identity — which is
     what happens in a real app whenever the parent re-renders and the child is
     not wrapped in memo(). */
  var el2 = jsxProd(App, { gen: 2 });
  var wipRoot = createWorkInProgress(rootFiber, null);
  updateChildren(wipRoot, el2);

  return wipRoot.child.child.child.index;
}

bench({
  name: 'react/tree-render',
  unit: 'tree',
  minMs: 150,
  run: function () {
    treeUnits = 0;
    renderTree();
    return treeUnits;
  },
  expect: 106,
});
