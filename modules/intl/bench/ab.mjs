#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * A/B two `intl-cli` binaries against each other on the same source.
 *
 * PURPOSE
 *   `run.mjs` answers "how far are we from node?" and reports the median of
 *   per-run medians. That is the right instrument on a quiet machine and the
 *   wrong one on a busy one: this module's own benchmarking sessions have been
 *   run at `loadavg` 3.1, 9.9, 13.4 and 17.1, and at 17.1 `run.mjs` reported
 *   per-row spreads of 195% and 382% on rows whose true value is stable to a
 *   few percent. `docs/intl-vs-node.md` states the rule plainly -- "a
 *   difference smaller than the spread is not a result" -- which on a loaded
 *   machine means no result at all.
 *
 *   This tool answers the narrower question that an optimization actually
 *   needs: **is binary B faster than binary A on this row, and by how much?**
 *   It buys robustness three ways that `run.mjs` deliberately does not:
 *
 *     1. **Interleaving.** A and B alternate process invocations
 *        (A B A B ...), so a load spike lands on both arms rather than on
 *        whichever ran second. `run.mjs` runs each engine's N processes as a
 *        block, which is correct for its purpose and wrong for this one.
 *     2. **Min-of-mins.** Each process already reports the minimum of its
 *        `reps` timing blocks (`harness.js`). This aggregates by taking the
 *        minimum again across processes. Interference makes a measurement
 *        slower and never faster, so the minimum is the estimator that
 *        converges under load; the median does not. The median-of-medians is
 *        reported beside it so the two can be compared, and a row where they
 *        disagree in direction is flagged.
 *     3. **A matched control binary, not a stored file.** Both numbers come
 *        from the same session, the same minute and the same machine state.
 *
 * IT IS ALSO A DIFFERENTIAL TEST
 *   Every row's sink is compared between A and B and a mismatch is a hard
 *   failure, whatever the flags say. `sinkMayDiffer` exists in `run.mjs`
 *   because Apple and ICU legitimately disagree about a few locale renderings;
 *   two builds of *this* module have no such licence. This caught nothing on
 *   its first use, which is the point: it is the check that has to be in place
 *   before a seam change is believed.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   - It does not compare against node. That is `run.mjs`, and the final
 *     number in any document should still come from `run.mjs` on the quietest
 *     machine available.
 *   - It does not measure cold start or peak RSS. Those arms live in
 *     `run.mjs` and are not made more robust by interleaving.
 *   - It does not write a stored result. Its output is a decision -- keep or
 *     drop a change -- not a baseline.
 *
 * USAGE
 *   node modules/intl/bench/ab.mjs \
 *        --a /path/to/intl-cli-apple-before \
 *        --b build-intl/modules/intl/intl-cli-apple \
 *        --runs 9 --workload 06-
 *
 *   --a, --b     the two engine binaries (required; --b defaults to the
 *                in-tree build-intl one)
 *   --runs N     interleaved process pairs per workload (default 7)
 *   --workload S substring filter on the workload file name
 *   --row S      substring filter on row names, applied to the report only
 *
 * EXIT STATUS
 *   0   every run exited 0, printed `#END`, and every sink matched
 *   1   a sink diverged between the two binaries
 *   2   a run failed, or a binary is missing
 */

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, "..", "..", "..");

const argv = process.argv.slice(2);
function opt(name, dflt) {
  const i = argv.indexOf("--" + name);
  return i >= 0 && argv[i + 1] !== undefined ? argv[i + 1] : dflt;
}
if (argv.includes("--help")) {
  console.log(
    fs.readFileSync(fileURLToPath(import.meta.url), "utf8").slice(0, 3400)
  );
  process.exit(0);
}

const A = path.resolve(ROOT, opt("a", ""));
const B = path.resolve(ROOT, opt("b", "build-intl/modules/intl/intl-cli-apple"));
const RUNS = Number(opt("runs", 7));
const FILTER = opt("workload", null);
const ROWFILTER = opt("row", null);

for (const [label, p] of [["--a", A], ["--b", B]]) {
  if (!p || !fs.existsSync(p)) {
    console.error(`[intl-ab] ${label} binary not found: ${p}`);
    process.exit(2);
  }
}

const TMP = fs.mkdtempSync(path.join(os.tmpdir(), "intl-ab-"));
process.on("exit", () => {
  try {
    fs.rmSync(TMP, { recursive: true, force: true });
  } catch {}
});

const HARNESS = fs.readFileSync(path.join(HERE, "harness.js"), "utf8");
const TAIL = fs.readFileSync(path.join(HERE, "harness-tail.js"), "utf8");

/*
 * The same assembly `run.mjs` performs, and for the same reason: there is no
 * module loader in intl-cli, so the harness, the workload and the tail are
 * concatenated into one file and that exact file is handed to both binaries.
 * "Were they running the same source" is then not a question anyone can ask.
 */
function assemble(file) {
  const body = fs.readFileSync(path.join(HERE, "workloads", file), "utf8");
  const out = path.join(TMP, file);
  fs.writeFileSync(out, HARNESS + "\n" + body + "\n" + TAIL);
  return out;
}

function runOnce(engine, label, scriptPath) {
  const r = spawnSync(engine, [scriptPath], {
    encoding: "utf8",
    maxBuffer: 64 * 1024 * 1024,
  });
  if (r.error) throw new Error(`${label}: ${r.error.message}`);
  if (r.status !== 0) {
    throw new Error(
      `${label} exited ${r.status} on ${path.basename(scriptPath)}\n` +
        (r.stderr || "").split("\n").slice(0, 12).join("\n")
    );
  }
  const rows = new Map();
  let end = null;
  for (const line of (r.stdout || "").split("\n")) {
    if (line.startsWith("#ROW\t")) {
      const [, name, min, med, n, reps, sink, flags] = line.split("\t");
      rows.set(name, { min: Number(min), med: Number(med), n: Number(n), sink, flags });
    } else if (line.startsWith("#END\t")) {
      end = Number(line.slice(5));
    }
  }
  // Both checks exist because this module has produced a cost table from runs
  // that exited 1, and a complete-looking table from a workload that threw
  // halfway. Neither is detectable from the numbers.
  if (end === null) throw new Error(`${label}: no #END sentinel`);
  if (end !== rows.size) throw new Error(`${label}: #END ${end} != ${rows.size} rows`);
  return rows;
}

const fmt = (ns) =>
  ns >= 1e6
    ? (ns / 1e6).toFixed(2) + " ms"
    : ns >= 1e3
    ? (ns / 1e3).toFixed(2) + " µs"
    : ns.toFixed(1) + " ns";

const median = (a) => {
  const s = [...a].sort((x, y) => x - y);
  const m = s.length >> 1;
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
};

const workloads = fs
  .readdirSync(path.join(HERE, "workloads"))
  .filter((f) => f.endsWith(".js"))
  .filter((f) => !FILTER || f.includes(FILTER))
  .sort();

console.log(`[intl-ab] A = ${A}`);
console.log(`[intl-ab] B = ${B}`);
console.log(`[intl-ab] ${RUNS} interleaved process pairs per workload`);
console.log(`[intl-ab] loadavg ${os.loadavg().map((x) => x.toFixed(2)).join(" ")}`);

let sinkFailures = 0;

for (const w of workloads) {
  const script = assemble(w);
  const runsA = [];
  const runsB = [];
  for (let i = 0; i < RUNS; i++) {
    // Alternate the leading arm as well, so neither side systematically
    // occupies the same phase of an external load cycle.
    if (i % 2 === 0) {
      runsA.push(runOnce(A, "A", script));
      runsB.push(runOnce(B, "B", script));
    } else {
      runsB.push(runOnce(B, "B", script));
      runsA.push(runOnce(A, "A", script));
    }
  }

  const names = [...runsA[0].keys()].filter(
    (n) => !ROWFILTER || n.includes(ROWFILTER)
  );

  console.log(`\n### ${w}  (${RUNS} pairs)`);
  console.log(
    "row".padEnd(34) +
      "A min".padStart(11) +
      "B min".padStart(11) +
      "B/A min".padStart(9) +
      "A med".padStart(11) +
      "B med".padStart(11) +
      "B/A med".padStart(9) +
      "  verdict"
  );
  console.log("-".repeat(112));

  for (const name of names) {
    const a = runsA.map((r) => r.get(name));
    const b = runsB.map((r) => r.get(name));
    if (a.some((x) => !x) || b.some((x) => !x)) {
      console.log(`${name.padEnd(34)}  MISSING FROM ONE BINARY`);
      sinkFailures++;
      continue;
    }
    // The differential half. Two builds of this module must agree byte for
    // byte on every sink; there is no legitimate reason for them not to.
    const sinkA = a[0].sink;
    const sinkB = b[0].sink;
    if (sinkA !== sinkB) {
      console.log(
        `${name.padEnd(34)}  SINK DIVERGED  A=${sinkA}  B=${sinkB}`
      );
      sinkFailures++;
      continue;
    }
    const aMin = Math.min(...a.map((x) => x.min));
    const bMin = Math.min(...b.map((x) => x.min));
    const aMed = median(a.map((x) => x.med));
    const bMed = median(b.map((x) => x.med));
    const rMin = bMin / aMin;
    const rMed = bMed / aMed;
    // A verdict is only offered when the two estimators agree in direction and
    // the min-based ratio is outside +-5%. Anything else is reported as a
    // number and left to the reader, which is the honest treatment of a row
    // whose two estimators disagree.
    let verdict = "";
    if (rMin < 0.95 && rMed < 1.0) verdict = `FASTER ${(1 / rMin).toFixed(2)}x`;
    else if (rMin > 1.05 && rMed > 1.0) verdict = `SLOWER ${rMin.toFixed(2)}x`;
    else if ((rMin < 0.95) !== (rMed < 1.0) && Math.abs(rMin - 1) > 0.05)
      verdict = "unclear (min and median disagree)";
    console.log(
      name.padEnd(34) +
        fmt(aMin).padStart(11) +
        fmt(bMin).padStart(11) +
        rMin.toFixed(3).padStart(9) +
        fmt(aMed).padStart(11) +
        fmt(bMed).padStart(11) +
        rMed.toFixed(3).padStart(9) +
        "  " +
        verdict
    );
  }
}

if (sinkFailures > 0) {
  console.error(`\n[intl-ab] FAIL: ${sinkFailures} row(s) diverged between the two binaries`);
  process.exit(1);
}
console.log("\n[intl-ab] ok: every sink identical between A and B");
