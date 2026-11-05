// A symbol that is a FinalizationRegistry target and a WeakMap key, whose WeakMap value is the registry. Freeing the symbol frees the
// registry from inside the register VM's reconcile; the callback object it held must not be freed while something still refers to it.
let key = Symbol("fortytwo");
let wm = new WeakMap();
let fr = new FinalizationRegistry(() => {});
fr.register(key, 42);
wm.set(key, fr);
fr = null;
key = null;
