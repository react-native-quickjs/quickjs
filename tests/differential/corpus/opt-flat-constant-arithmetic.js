// Differential corpus for pass 5's arithmetic on the constants the pass can
// name exactly: int32, Boolean and null.
//
// WHY THIS FILE.  A Boolean is 1 or 0 and null is 0, so `true + 1` is 2 and
// `null - 1` is -1 -- but the three cases at the edges are what a fold gets
// wrong: `undefined` is NaN and not a number this pass models, a product with
// a negative and a zero is minus zero, and a sum or product outside int32 is a
// double. Each of those has to keep its instructions, and the printed values
// are what says so.

function show(k, v) { print(k + '=' + v); }

show('true+1', true + 1);
show('false*2', false * 2);
show('true*true', true * true);
show('null+1', null + 1);
show('null-1', null - 1);
show('true-false', true - false);
show('false-false', false - false);

/* not numbers this pass models: every operation on undefined is NaN */
show('undef+0', undefined + 0);
show('undef-0', undefined - 0);
show('undef*0', undefined * 0);
show('null+undef', null + undefined);

/* minus zero: 0 * -3 is -0, and 1 / -0 is -Infinity, not Infinity */
show('neg0', 1 / (0 * -3));
show('neg0b', 1 / (null * -1));
show('neg0c', 1 / (false * -2));
show('pos0', 1 / (0 * 3));
show('pos0b', 1 / (false * 2));
show('pos0c', 1 / (null * 0));

/* outside int32 the result is a double, and this pass does not model those */
show('overflow', 2147483647 + 1);
show('overflow2', 50000 * 50000);
show('overflow3', -2147483648 - 1);

/* and the same in a loop, where the folding has to keep the branch and the
   arithmetic in step */
function counted(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    t += true + 1;
    t += false * 2;
    t += null - 1;
    t += (undefined + 0) & 0;
  }
  return t;
}
show('counted', counted(3));

/* the shifts: a Boolean and null shifted are exact, the count is masked to
   five bits, and an unsigned shift above INT32_MAX is not an int32 constant */
show('1<<2', 1 << 2);
show('true<<1', true << 1);
show('false<<1', false << 1);
show('null>>1', null >> 1);
show('undefined>>1', undefined >> 1);
show('8>>>2', 8 >>> 2);
show('masked', 1 << 32);
show('masked2', 1 << 33);
show('-1>>>0', -1 >>> 0);
show('negShift', -8 >> 1);
show('negShiftU', -8 >>> 1);
show('shiftMany', 1 << 31);

/* division and remainder, in the cases that are whole: a fraction, a zero
   divisor and a minus zero all have to keep their instructions */
show('6/2', 6 / 2);
show('7/2', 7 / 2);
show('7%3', 7 % 3);
show('-6/2', -6 / 2);
show('-7%3', -7 % 3);
show('7%-3', 7 % -3);
show('1/0', 1 / 0);
show('0/0', 0 / 0);
show('7%0', 7 % 0);
show('0/-2', 1 / (0 / -2));
show('-6%3', 1 / (-6 % 3));
show('6%3', 1 / (6 % 3));
show('true/1', true / 1);
show('null%2', null % 2);
show('undefined/1', undefined / 1);
show('30/5+1', (30 / 5) + 1);
