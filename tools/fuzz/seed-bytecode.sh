#!/bin/bash
# seed-bytecode.sh QJSC OUTDIR JS... : compile each JS file to a bytecode container (the corpus for fuzz-bytecode)
q=$1; o=$2; shift 2; mkdir -p "$o"
for f in "$@"; do b=$(basename "$f" .js); "$q" "$f" "$o/$b.bin" >/dev/null 2>&1; done
ls "$o" | wc -l
