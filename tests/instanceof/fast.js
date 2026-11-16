function A(){} function B(){} B.prototype = Object.create(A.prototype);
var a = new A, b = new B, r = [];
r.push(a instanceof A, b instanceof A, a instanceof B, b instanceof B, 1 instanceof A, null instanceof A);
var px = new Proxy(b, {}); r.push(px instanceof A, px instanceof B);
var o = Object.create(px); r.push(o instanceof A, o instanceof B);
function C(){}; C.prototype = 5; try { a instanceof C; r.push("noerr") } catch (e) { r.push(e instanceof TypeError) }
var D = new Proxy(function(){}, {get(t,k){ return k==="prototype" ? A.prototype : t[k] }}); r.push(a instanceof D);
var E = function(){}; Object.defineProperty(E, Symbol.hasInstance, {value(){ return 42 }}); r.push(a instanceof E);
var F = A.bind(null); r.push(a instanceof F);
var G = class {}; r.push(new G instanceof G, a instanceof G);
function* H(){} r.push(a instanceof H);
print(r.join());
var r2 = [];
function F2(){} var f2 = new F2;
r2.push(f2 instanceof F2);
Object.setPrototypeOf(F2, {[Symbol.hasInstance]() { return false; }});
r2.push(f2 instanceof F2);
function F3(){} var f3 = new F3;
Object.setPrototypeOf(F3, {[Symbol.hasInstance]() { return true; }});
r2.push(f3 instanceof F3, ({}) instanceof F3);
function SA(){} Object.defineProperty(SA, Symbol.hasInstance, {value(x){ return x === 7 }});
class SB extends SA {}
r2.push(new SB instanceof SB, 7 instanceof SB, new SA instanceof SA);
class SC {} r2.push(new SC instanceof SC);
print(r2.join());
