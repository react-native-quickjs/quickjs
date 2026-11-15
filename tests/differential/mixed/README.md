# Mixed programs

A host part compiled ahead of time and payloads compiled at run time, running
together. Each file is ordinary corpus code whose payload is source text handed
to eval, indirect eval or `new Function`: run through `--via-bytecode` with the
engine in the fast compile mode, the host is qjsc's optimised bytecode and every
payload is compiled by the fast mode, so the two kinds of function call each
other, share closures, objects and inline caches, and throw through each other.
The output is compared with node like any corpus file.

    node tests/differential/run.mjs --corpus mixed --via-bytecode --qjs-arg --fast-compile --leak-check

Run from source with and without `--fast-compile` too: then both parts share a
mode, which separates a mixing bug from a mode bug.
