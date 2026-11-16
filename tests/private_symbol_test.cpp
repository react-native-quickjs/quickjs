/* Copyright (c) Ammar Ahmed. */

// JS_NewPrivateSymbol is how the host attaches native state to an arbitrary
// object (QuickJSRuntime.cpp, nativeState). This pins every way script could
// otherwise see or move that key: each case prints what the engine does and the
// expected text is the contract.

#include <gtest/gtest.h>
#include <quickjs.h>

#include <string>

namespace {

class PrivateSymbol : public testing::Test {
 protected:
  void SetUp() override {
    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    JSValue key = JS_NewPrivateSymbol(ctx, "nativeState");
    atom = JS_ValueToAtom(ctx, key);
    JS_FreeValue(ctx, key);
  }
  void TearDown() override {
    JS_FreeAtom(ctx, atom);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
  }

  // Evaluates `src` (a string-valued expression) after `global.o` holds an
  // object {a:1} that carries the private key with value 7.
  std::string run(const char *src, const char *setup = "globalThis.o = {a: 1};") {
    JSValue init = JS_Eval(ctx, setup, strlen(setup), "setup.js", JS_EVAL_TYPE_GLOBAL);
    EXPECT_FALSE(JS_IsException(init)) << dump();
    JS_FreeValue(ctx, init);
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue o = JS_GetPropertyStr(ctx, g, "o");
    EXPECT_GE(JS_SetProperty(ctx, o, atom, JS_NewInt32(ctx, 7)), 0);
    JS_FreeValue(ctx, o);
    JS_FreeValue(ctx, g);
    JSValue v = JS_Eval(ctx, src, strlen(src), "case.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) return "throws: " + dump();
    const char *s = JS_ToCString(ctx, v);
    std::string out = s ? s : "?";
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return out;
  }
  std::string dump() {
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    std::string out = s ? s : "?";
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
    return out;
  }
  int64_t readNumber(const char *expr) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue o = JS_GetPropertyStr(ctx, g, expr);
    JSValue v = JS_GetProperty(ctx, o, atom);
    int32_t n = -1;
    if (JS_IsNumber(v)) JS_ToInt32(ctx, &n, v);
    else if (JS_IsUndefined(v)) n = -2;
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, o);
    JS_FreeValue(ctx, g);
    return n;
  }

  JSRuntime *rt = nullptr;
  JSContext *ctx = nullptr;
  JSAtom atom = JS_ATOM_NULL;
};

TEST_F(PrivateSymbol, ReadBackFromC) {
  EXPECT_EQ(run("'ok'"), "ok");
  EXPECT_EQ(readNumber("o"), 7);
}

TEST_F(PrivateSymbol, NotListedByAnyKeyApi) {
  EXPECT_EQ(run("Object.getOwnPropertySymbols(o).length + ':' + Reflect.ownKeys(o).join() + ':' + "
                "Object.keys(o).join() + ':' + Object.getOwnPropertyNames(o).join() + ':' + "
                "Object.entries(o).join()"),
            "0:a:a:a:a,1");
  EXPECT_EQ(run("let n = 0; for (const k in o) n++; String(n)"), "1");
  EXPECT_EQ(run("Object.keys(Object.getOwnPropertyDescriptors(o)).join()"), "a");
}

TEST_F(PrivateSymbol, NotSerialisedOrCopied) {
  EXPECT_EQ(run("JSON.stringify(o)"), "{\"a\":1}");
  EXPECT_EQ(run("JSON.stringify({...o})"), "{\"a\":1}");
  run("globalThis.c1 = Object.assign({}, o); globalThis.c2 = {...o};"
      " globalThis.c3 = Object.create(null, Object.getOwnPropertyDescriptors(o));");
  // Object.assign and descriptor copies leave the key behind; object spread
  // copies every own property including this one (the engine's behaviour
  // today, pinned so a merge cannot change it unnoticed).
  EXPECT_EQ(readNumber("c1"), -2);
  EXPECT_EQ(readNumber("c2"), 7);
  EXPECT_EQ(readNumber("c3"), -2);
}

TEST_F(PrivateSymbol, ScriptCannotReachItByName) {
  EXPECT_EQ(run("String(Object.getOwnPropertyNames(o).length) + ':' + "
                "String(Object.getOwnPropertySymbols(o).some(s => String(s).includes('nativeState')))"),
            "1:false");
  EXPECT_EQ(run("String(Object.hasOwn(o, 'nativeState')) + ':' + String(o.nativeState)"), "false:undefined");
}

TEST_F(PrivateSymbol, ProxyTrapsAreCalledForThePrivateKey) {
  // The engine treats a private symbol as an ordinary key as far as a Proxy
  // goes: set, get, has and delete from C each reach the handler (6 trap calls
  // in all, which include the descriptor lookups the set and delete perform).
  EXPECT_EQ(run("globalThis.log = [];"
                "const h = new Proxy({}, {get(t, k) { return (...a) => { log.push(k); return Reflect[k](...a); }; }});"
                "globalThis.p = new Proxy({a: 1}, h);"
                "log.length = 0; String(log.length)"),
            "0");
  // after C reads and writes the key through the proxy:
  {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue p = JS_GetPropertyStr(ctx, g, "p");
    int set = JS_SetProperty(ctx, p, atom, JS_NewInt32(ctx, 9));
    JSValue v = JS_GetProperty(ctx, p, atom);
    int32_t n = -1;
    JS_ToInt32(ctx, &n, v);
    int has = JS_HasProperty(ctx, p, atom);
    int del = JS_DeleteProperty(ctx, p, atom, 0);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, p);
    JS_FreeValue(ctx, g);
    EXPECT_EQ(std::to_string(set) + ":" + std::to_string(n) + ":" + std::to_string(has) + ":" +
                  std::to_string(del),
              "1:9:1:1");
  }
  EXPECT_EQ(run("String(log.length)", ""), "6");
}

TEST_F(PrivateSymbol, FrozenAndNonExtensibleObjects) {
  // Attaching the key to an object script has frozen or made non-extensible
  // fails with a TypeError, like any other new property.
  run("globalThis.f = Object.freeze({a: 1}); globalThis.n = Object.preventExtensions({a: 1});");
  JSValue g = JS_GetGlobalObject(ctx);
  std::string out;
  for (const char *name : {"f", "n"}) {
    JSValue o = JS_GetPropertyStr(ctx, g, name);
    int r = JS_SetProperty(ctx, o, atom, JS_NewInt32(ctx, 5));
    if (r < 0) JS_FreeValue(ctx, JS_GetException(ctx));
    out += std::string(name) + "=" + std::to_string(r) + ",";
    JS_FreeValue(ctx, o);
  }
  JS_FreeValue(ctx, g);
  EXPECT_EQ(out, "f=-1,n=-1,");
}

TEST_F(PrivateSymbol, DoesNotSurviveSerialisation) {
  // JS_WriteObject / JS_ReadObject of a holder drops the private key (the same
  // for structuredClone-style copies built on it).
  JSValue g = JS_GetGlobalObject(ctx);
  run("'x'");
  JSValue o = JS_GetPropertyStr(ctx, g, "o");
  size_t len = 0;
  uint8_t *buf = JS_WriteObject(ctx, &len, o, JS_WRITE_OBJ_REFERENCE);
  ASSERT_NE(buf, nullptr) << dump();
  JSValue back = JS_ReadObject(ctx, buf, len, JS_READ_OBJ_REFERENCE);
  js_free(ctx, buf);
  ASSERT_FALSE(JS_IsException(back)) << dump();
  JSValue pv = JS_GetProperty(ctx, back, atom);
  JSValue av = JS_GetPropertyStr(ctx, back, "a");
  int32_t a = -1;
  JS_ToInt32(ctx, &a, av);
  EXPECT_TRUE(JS_IsUndefined(pv));
  EXPECT_EQ(a, 1);
  JS_FreeValue(ctx, pv);
  JS_FreeValue(ctx, av);
  JS_FreeValue(ctx, back);
  JS_FreeValue(ctx, o);
  JS_FreeValue(ctx, g);
}

TEST_F(PrivateSymbol, TwoSymbolsWithTheSameDescriptionAreDistinct) {
  JSValue k2 = JS_NewPrivateSymbol(ctx, "nativeState");
  JSAtom a2 = JS_ValueToAtom(ctx, k2);
  JS_FreeValue(ctx, k2);
  EXPECT_NE(a2, atom);
  JS_FreeAtom(ctx, a2);
}

}  // namespace
