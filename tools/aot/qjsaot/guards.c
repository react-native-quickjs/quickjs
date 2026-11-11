/* Guard numbering, classification and the execution census of a finished unit.

   A guard is an `if` statement one of whose branches is an exit: E_GOTO(kx, pc), `goto rx_N` (a region exit), `goto pexit` or E_RET_G(..).
   The pass reads the finished unit text and
     - numbers the E_GOTO exits: `E_GOTO(vx, 107)` becomes `E_GOTO(vx, 107, <id>)` (the id is what an exit report names), and writes
       `<unit>.guards` with each id, its line, the kind of test, whether it re-checks something already known, and the guard's text;
     - with QJS_AOT_GUARDCOUNT=1 in the environment, wraps the condition of every guard as `(Q2_GC(kind, site, label), (COND))`, so a run counts
       the executions of each test per kind and per site (`<unit>.gcsites` lists the sites), and prints the totals at exit.
   Kinds say what the test checks; labels say why it may be unnecessary:
     claim    the test is a profile claim re-checked at a use (typed-slot value claim, claimed layout)
     repeat   the same condition text, free of scratch names, was already tested earlier in the same function (an upper bound: the tested
            variable may have changed in between; conditions on scratch names such as _i or _e are never labelled repeat)
     entry    the test sits at pc 0, where a fact is enforced for the first time
     unproven everything else. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GK_TAG, GK_SHAPE, GK_OVERFLOW, GK_BOUNDS, GK_EPOCH, GK_CALLEE, GK_CALLRET, GK_OTHER, GK_N };
static const char *const guard_kind_names[GK_N] = {"tag", "shape", "overflow", "bounds", "epoch", "callee", "callret", "other"};
static const char *const guard_label_names[4] = {"unproven", "claim", "repeat", "entry"};

#define GL_NSEEN 8192
#define GL_MAXSPAN 12000
#define GL_MAXBODY 400

typedef struct {
  long cs, ce; /* the condition that is counted (input offsets) */
  long st, en; /* the whole statement */
  int kind, label, gcid;
} GSite;

static int has_tok(const char *s, const char *e, const char *tok) {
  size_t n = strlen(tok);
  for (; s + n <= e; s++)
    if (*s == tok[0] && strncmp(s, tok, n) == 0) return 1;
  return 0;
}

static int guard_kind_of(const char *c, size_t n) {
  char t[600];
  if (n >= sizeof(t)) n = sizeof(t) - 1;
  memcpy(t, c, n);
  t[n] = 0;
  if (strstr(t, "EPOCH") || strstr(t, "LAZY")) return GK_EPOCH;
  /* the result of a call or runtime helper, not a speculation: exception and hand-off checks */
  if (strstr(t, "s_ !=") || strstr(t, "s_ ==") || strstr(t, "JS_IsException") || strstr(t, "qjs_aot_") || strstr(t, "rg_nof") || strstr(t, "HAS_REF_COUNT") || strstr(t, "load_const")) return GK_CALLRET;
  if (strstr(t, "q2_is_intr") || strstr(t, "q2_intr") || strstr(t, "q2_is_proto") || strstr(t, "is_fn_call") || strstr(t, "callee")) return GK_CALLEE;
  if (strstr(t, "off_ >=") || strstr(t, "ic_ !=")) return GK_BOUNDS;
  if (strstr(t, "elem_hit") || strstr(t, "q2_array") || strstr(t, ">= an") || strstr(t, "_i >=") || strstr(t, "_v >=") || strstr(t, "bound") ||
      strstr(t, "RG_OWNCAP"))
    return GK_BOUNDS;
  if (strstr(t, "sh_") || strstr(t, "np_ !=")) return GK_SHAPE;
  if (strstr(t, "shape") || strstr(t, "global_hit") || strstr(t, "_hit(") || strstr(t, "poly_read") || strstr(t, "_psh") || strstr(t, "pr_ ==") ||
      strstr(t, "q2_cell_raw") || strstr(t, "EXTENSIBLE") || strstr(t, "Q2_LAYOUT_BROKEN"))
    return GK_SHAPE;
  if (strstr(t, "INT32_M") || strstr(t, "overflow")) return GK_OVERFLOW;
  if (strstr(t, "TAG") || strstr(t, "I32_OK") || strstr(t, "claim") || strstr(t, "CLAIM") || strstr(t, "ARR_KIND")) return GK_TAG;
  return GK_OTHER;
}

static int guard_label_of(const char *c, size_t n, int kind, int at_entry, unsigned long long *seen, int *nseen) {
  unsigned long long h = 1469598103934665603ull;
  size_t i;
  int j;
  char t[400];
  size_t m = n < sizeof(t) - 1 ? n : sizeof(t) - 1;
  for (i = 0; i < n; i++) h = (h ^ (unsigned char)c[i]) * 1099511628211ull;
  if (at_entry) return 3;
  memcpy(t, c, m);
  t[m] = 0;
  if (strstr(t, "claim") || strstr(t, "CLAIM")) return 1;
  if (kind == GK_EPOCH) return 0; /* a post-call check is new after every call */
  /* A scratch name (_i, _e, _t, _c0, ...) is reused by unrelated statements: the same text then says nothing about the same value. */
  for (i = 0; i < m; i++)
    if (t[i] == '_' && t[i + 1] != '_' && (i == 0 || !((t[i - 1] >= 'a' && t[i - 1] <= 'z') || (t[i - 1] >= 'A' && t[i - 1] <= 'Z') ||
                                                         (t[i - 1] >= '0' && t[i - 1] <= '9') || t[i - 1] == '_')) &&
        ((t[i + 1] >= 'a' && t[i + 1] <= 'z') || (t[i + 1] >= '0' && t[i + 1] <= '9')))
      return 0;
  for (j = 0; j < *nseen; j++)
    if (seen[j] == h) return 2;
  if (*nseen < GL_NSEEN) seen[(*nseen)++] = h;
  return 0;
}

/* Lexical helpers over the unit text. All of them return NULL when the text does not parse within GL_MAXSPAN characters. */
static const char *skip_line(const char *p, const char *end) {
  while (p < end && *p != '\n') {
    if (*p == '\\' && p + 1 < end && p[1] == '\n') p++;
    p++;
  }
  return p;
}

/* p at an opening ( [ or {; returns the character after the matching closer. Strings, comments and directive lines are skipped. */
static const char *match_close(const char *p, const char *end) {
  int depth = 0;
  const char *lim = p + GL_MAXSPAN < end ? p + GL_MAXSPAN : end;
  int bol = 0;
  for (; p < lim; p++) {
    char c = *p;
    if (c == '\n') {
      bol = 1;
      continue;
    }
    if (bol && (c == ' ' || c == '\t')) continue;
    if (bol && c == '#') {
      p = skip_line(p, lim) - 1;
      continue;
    }
    bol = 0;
    if (c == '"' || c == '\'') {
      p++;
      while (p < lim && *p != c) p += *p == '\\' ? 2 : 1;
      continue;
    }
    if (c == '/' && p + 1 < lim && p[1] == '*') {
      const char *q = p + 2;
      while (q + 1 < lim && !(q[0] == '*' && q[1] == '/')) q++;
      p = q + 1;
      continue;
    }
    if (c == '/' && p + 1 < lim && p[1] == '/') {
      p = skip_line(p, lim) - 1;
      continue;
    }
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') {
      if (--depth == 0) return p + 1;
      if (depth < 0) return NULL;
    }
  }
  return NULL;
}

static const char *skip_ws(const char *p, const char *end) {
  for (;;) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\\')) p++;
    if (p < end && *p == '#') {
      /* a directive line between the tokens of a statement */
      p = skip_line(p, end);
      continue;
    }
    if (p + 1 < end && p[0] == '/' && p[1] == '*') {
      const char *q = p + 2;
      while (q + 1 < end && !(q[0] == '*' && q[1] == '/')) q++;
      p = q + 2 < end ? q + 2 : end;
      continue;
    }
    return p;
  }
}

/* The end of the statement starting at p: a braced block, or text up to the next top-level `;`. NULL if it is an `if` (its else is not ours). */
static const char *stmt_end(const char *p, const char *end, int *is_if) {
  const char *lim = p + GL_MAXSPAN < end ? p + GL_MAXSPAN : end;
  *is_if = 0;
  if (p >= end) return NULL;
  if (*p == '{') return match_close(p, end);
  if (p[0] == 'i' && p[1] == 'f' && (p[2] == ' ' || p[2] == '(')) *is_if = 1;
  for (; p < lim; p++) {
    if (*p == '"' || *p == '\'') {
      char c = *p++;
      while (p < lim && *p != c) p += *p == '\\' ? 2 : 1;
    } else if (*p == '(' || *p == '[' || *p == '{') {
      p = match_close(p, end);
      if (p == NULL) return NULL;
      p--;
    } else if (*p == ';') {
      return p + 1;
    }
  }
  return NULL;
}

static int body_is_exit(const char *s, const char *e) {
  if (e - s > GL_MAXBODY) return 0;
  return has_tok(s, e, "E_GOTO(") || has_tok(s, e, "goto rx_") || has_tok(s, e, "goto pexit") || has_tok(s, e, "E_RET_G(");
}

/* Parse the `if` at p; on success fill the site. */
static int parse_if(const char *b, const char *end, const char *p, GSite *s) {
  const char *cp, *ce, *c0, *c1, *t0, *t1, *e0 = NULL, *e1 = NULL, *q;
  int is_if, then_exit, else_exit;
  cp = p + 2;
  while (cp < end && *cp == ' ') cp++;
  if (cp >= end || *cp != '(') return 0;
  ce = match_close(cp, end);
  if (ce == NULL) return 0;
  c0 = cp + 1;
  c1 = ce - 1;
  if (c1 - c0 > 17 && strncmp(c0, "__builtin_expect(", 17) == 0) {
    /* the condition is the first argument */
    const char *r = c0 + 17;
    int depth = 0;
    const char *last = NULL;
    for (; r < c1; r++) {
      if (*r == '"' || *r == '\'') {
        char c = *r++;
        while (r < c1 && *r != c) r += *r == '\\' ? 2 : 1;
      } else if (*r == '(' || *r == '[' || *r == '{') depth++;
      else if (*r == ')' || *r == ']' || *r == '}') depth--;
      else if (*r == ',' && depth == 0) last = r;
    }
    if (last == NULL) return 0;
    c0 += 17;
    c1 = last;
  }
  if (c1 - c0 == 1 && *c0 == '0') return 0;
  t0 = skip_ws(ce, end);
  t1 = stmt_end(t0, end, &is_if);
  if (t1 == NULL) return 0;
  then_exit = body_is_exit(t0, t1);
  else_exit = 0;
  q = skip_ws(t1, end);
  if (!is_if && q + 4 < end && strncmp(q, "else", 4) == 0 && (q[4] == ' ' || q[4] == '{' || q[4] == '\n')) {
    int e_is_if;
    e0 = skip_ws(q + 4, end);
    e1 = stmt_end(e0, end, &e_is_if);
    if (e1 != NULL && !e_is_if) else_exit = body_is_exit(e0, e1);
  }
  if (!then_exit && !else_exit) return 0;
  s->cs = c0 - b;
  s->ce = c1 - b;
  s->st = p - b;
  s->en = (else_exit && e1 != NULL ? e1 : t1) - b;
  return 1;
}

static int exit_pc_zero(const char *s, const char *e) {
  const char *p;
  for (p = s; p + 12 < e; p++)
    if (*p == 'E' && strncmp(p, "E_GOTO(", 7) == 0 && p[8] == 'x' && p[9] == ',') return p[11] == '0' && (p[12] == ')' || p[12] == ',');
  return 0;
}

static void counting_prelude(FILE *o, int nsites) {
  fprintf(o,
      "#include <stdio.h>\n"
      "enum { Q2_GK_TAG, Q2_GK_SHAPE, Q2_GK_OVERFLOW, Q2_GK_BOUNDS, Q2_GK_EPOCH, Q2_GK_CALLEE, Q2_GK_CALLRET, Q2_GK_OTHER, Q2_GK_N };\n"
      "static unsigned long long q2_gc[Q2_GK_N];\n"
      "static unsigned long long q2_gl[4];\n"
      "static unsigned long long q2_gci[%d];\n"
      "#define Q2_GC(k_, i_, l_) (q2_gc[k_]++, q2_gl[l_]++, q2_gci[i_]++)\n"
      "__attribute__((destructor)) static void q2_gc_dump(void)\n"
      "{\n"
      "  static const char *const nm[Q2_GK_N] = {\"tag\", \"shape\", \"overflow\", \"bounds\", \"epoch\", \"callee\", \"callret\", \"other\"};\n"
      "  unsigned long long tot = 0, best[24][2];\n"
      "  int k, i, j, nb = 0;\n"
      "  for (k = 0; k < Q2_GK_N; k++) tot += q2_gc[k];\n"
      "  fprintf(stderr, \"aot guardcount guards=%%llu\", tot);\n"
      "  for (k = 0; k < Q2_GK_N; k++) fprintf(stderr, \" %%s=%%llu\", nm[k], q2_gc[k]);\n"
      "  fprintf(stderr, \" | unproven=%%llu claim=%%llu repeat=%%llu entry=%%llu\\n\", q2_gl[0], q2_gl[1], q2_gl[2], q2_gl[3]);\n"
      "  for (i = 1; i < %d; i++) {\n"
      "    if (q2_gci[i] == 0) continue;\n"
      "    for (j = nb; j > 0 && best[j - 1][0] < q2_gci[i]; j--) if (j < 24) { best[j][0] = best[j - 1][0]; best[j][1] = best[j - 1][1]; }\n"
      "    if (j < 24) { best[j][0] = q2_gci[i]; best[j][1] = (unsigned long long)i; if (nb < 24) nb++; }\n"
      "  }\n"
      "  for (j = 0; j < nb; j++) fprintf(stderr, \"aot guardcount-top site=%%llu executed=%%llu\\n\", best[j][1], best[j][0]);\n"
      "}\n",
      nsites + 2, nsites + 2);
}

static void emit_kind_label_counts(const char *what, long total, const long *kinds, const long *labels) {
  int k;
  fprintf(stderr, "guard census: %s=%ld", what, total);
  for (k = 0; k < GK_N; k++) fprintf(stderr, " %s=%ld", guard_kind_names[k], kinds[k]);
  fprintf(stderr, " | unproven=%ld claim=%ld repeat=%ld entry=%ld\n", labels[0], labels[1], labels[2], labels[3]);
}

void qjsaot_number_guards(const char *path) {
  FILE *in = fopen(path, "rb");
  long n, i, nsites = 0, capsites = 1024;
  char *buf = NULL, *o = NULL, *w;
  const char *p, *end;
  FILE *g = NULL, *gs = NULL;
  char gp[4096];
  GSite *sites = NULL;
  unsigned long long *seen = NULL;
  int nseen = 0, id = 0;
  int counting = getenv("QJS_AOT_GUARDCOUNT") != NULL;
  long kinds[GK_N] = {0}, labels[4] = {0};
  long ekinds[GK_N] = {0}, elabels[4] = {0};
  long nuncounted = 0;

  if (in == NULL) return;
  fseek(in, 0, SEEK_END);
  n = ftell(in);
  fseek(in, 0, SEEK_SET);
  buf = malloc((size_t)n + 1);
  if (buf == NULL || fread(buf, 1, (size_t)n, in) != (size_t)n) {
    fclose(in);
    free(buf);
    return;
  }
  fclose(in);
  buf[n] = 0;
  end = buf + n;
  sites = malloc(sizeof(*sites) * (size_t)capsites);
  seen = malloc(sizeof(*seen) * GL_NSEEN);
  o = malloc((size_t)n * 2 + 4096);
  if (sites == NULL || seen == NULL || o == NULL) goto done;

  /* pass 1: find the guards */
  {
    int bol = 1;
    for (p = buf; p < end; p++) {
      char c = *p;
      if (c == '\n') {
        bol = 1;
        continue;
      }
      if (bol) {
        if (c == '#') {
          p = skip_line(p, end) - 1;
          continue;
        }
        if (p == buf || p[-1] == '\n') {
          /* a function head starts a new scope for the repeated-condition label */
          if (c != '/' && c != '}' && c != '{') {
            const char *le = memchr(p, '\n', (size_t)(end - p));
            const char *fp = memchr(p, '(', (size_t)((le ? le : end) - p));
            if (fp != NULL && end - fp > 15 && strncmp(fp, "(JSContext *ctx", 15) == 0) nseen = 0;
          }
        }
      }
      if (c != ' ' && c != '\t') bol = 0;
      if (c == '"' || c == '\'') {
        p++;
        while (p < end && *p != c && *p != '\n') p += *p == '\\' ? 2 : 1;
        continue;
      }
      if (c == '/' && p + 1 < end && p[1] == '*') {
        const char *q = p + 2;
        while (q + 1 < end && !(q[0] == '*' && q[1] == '/')) q++;
        p = q + 1;
        continue;
      }
      if (c == '/' && p + 1 < end && p[1] == '/') {
        p = skip_line(p, end) - 1;
        continue;
      }
      if (c == 'i' && p[1] == 'f' && (p[2] == ' ' || p[2] == '(') && p > buf && !(p[-1] == '_' || p[-1] == '#' || (p[-1] >= 'a' && p[-1] <= 'z') ||
                                                                                   (p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= '0' && p[-1] <= '9'))) {
        GSite s;
        if (parse_if(buf, end, p, &s)) {
          const char *cond = buf + s.cs;
          size_t cl = (size_t)(s.ce - s.cs);
          int at_entry = exit_pc_zero(buf + s.st, buf + s.en);
          s.kind = guard_kind_of(cond, cl);
          s.label = guard_label_of(cond, cl, s.kind, at_entry, seen, &nseen);
          s.gcid = (int)nsites + 1;
          if (nsites == capsites) {
            GSite *ns = realloc(sites, sizeof(*sites) * (size_t)capsites * 2);
            if (ns == NULL) goto done;
            sites = ns;
            capsites *= 2;
          }
          sites[nsites++] = s;
          kinds[s.kind]++;
          labels[s.label]++;
        }
      }
    }
  }

  /* pass 2: number the E_GOTO exits, attribute them to their guard, and wrap the counted conditions */
  snprintf(gp, sizeof(gp), "%s.guards", path);
  g = fopen(gp, "w");
  snprintf(gp, sizeof(gp), "%s.gcsites", path);
  gs = fopen(gp, "w");
  {
    long line = 1, si = 0, wi = 0; /* si: next site whose statement has not started; wi: next site whose condition is not wrapped */
    long stack[64];
    int sp = 0;
    const char *ls = buf;
    w = o;
    for (p = buf, i = 0; p < end;) {
      long off = p - buf;
      if (*p == '\n') {
        line++;
        ls = p + 1;
      }
      while (sp > 0 && off >= sites[stack[sp - 1]].en) sp--;
      while (si < nsites && sites[si].st <= off) {
        if (sp < 64) stack[sp++] = si;
        if (gs != NULL) {
          const char *le = memchr(p, '\n', (size_t)(end - p));
          int tl = (int)((le ? le : end) - p);
          fprintf(gs, "%d %ld %s %s %.*s\n", sites[si].gcid, line, guard_kind_names[sites[si].kind], guard_label_names[sites[si].label], tl > 240 ? 240 : tl, p);
        }
        si++;
      }
      if (counting) {
        /* conditions are wrapped at their start and end; sites are in order of their start */
        while (wi < nsites && sites[wi].cs < off) wi++;
        if (wi < nsites && sites[wi].cs == off) {
          w += sprintf(w, "(Q2_GC(%d, %d, %d), (", sites[wi].kind, sites[wi].gcid, sites[wi].label);
          /* copy the condition itself, then close */
          memcpy(w, p, (size_t)(sites[wi].ce - off));
          w += sites[wi].ce - off;
          for (const char *r = p; r < buf + sites[wi].ce; r++)
            if (*r == '\n') line++, ls = r + 1;
          *w++ = ')';
          *w++ = ')';
          p = buf + sites[wi].ce;
          wi++;
          continue;
        }
      }
      if (*p == 'E' && strncmp(p, "E_GOTO(", 7) == 0 && (p[7] == 'v' || p[7] == 'd' || p[7] == 'o' || p[7] == 'a') && p[8] == 'x' && p[9] == ',') {
        const char *q = memchr(p, ')', (size_t)(end - p > 40 ? 40 : end - p));
        if (q != NULL) {
          const char *le = memchr(ls, '\n', (size_t)(end - ls));
          int tl = (int)((le ? le : end) - ls);
          const char *kn = "uncounted", *ln = "-";
          const char *t = ls;
          while (t < p && (*t == ' ' || *t == '\t')) t++, tl--;
          if (sp > 0) {
            GSite *s = &sites[stack[sp - 1]];
            kn = guard_kind_names[s->kind];
            ln = guard_label_names[s->label];
            ekinds[s->kind]++;
            elabels[s->label]++;
          } else {
            nuncounted++;
          }
          id++;
          memcpy(w, p, (size_t)(q - p));
          w += q - p;
          w += sprintf(w, ", %d", id);
          p = q;
          if (g != NULL) fprintf(g, "%d %ld %s %s %.*s\n", id, line, kn, ln, tl > 240 ? 240 : tl, t);
          continue;
        }
      }
      *w++ = *p++;
    }
  }
  if (id > 0 || nsites > 0) {
    FILE *wf = fopen(path, "wb");
    if (wf != NULL) {
      char *nl = counting ? memchr(o, '\n', (size_t)(w - o)) : NULL;
      if (nl != NULL) {
        /* the generated-by line stays first (ccsplit looks for it) */
        fwrite(o, 1, (size_t)(nl + 1 - o), wf);
        counting_prelude(wf, (int)nsites);
        fwrite(nl + 1, 1, (size_t)(w - nl - 1), wf);
      } else {
        fwrite(o, 1, (size_t)(w - o), wf);
      }
      fclose(wf);
    }
  }
  if (counting || getenv("QJS_AOT_GUARDCENSUS") != NULL) {
    fprintf(stderr, "guard census: exits=%d without-guard=%ld\n", id, nuncounted);
    emit_kind_label_counts("guards", nsites, kinds, labels);
    emit_kind_label_counts("exit-sites-by-guard", id - nuncounted, ekinds, elabels);
  }
done:
  if (g != NULL) fclose(g);
  if (gs != NULL) fclose(gs);
  free(sites);
  free(seen);
  free(buf);
  free(o);
}
