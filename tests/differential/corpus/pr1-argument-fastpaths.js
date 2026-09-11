function out(name, value) {
  print(name + ':' + String(value));
}

function mapped(a, b) {
  a = a + 1;
  return [arguments.length, arguments[0], arguments[1]];
}
out('mapped', mapped(4, 5, 6).join(','));

function forwarded(a, b) {
  return Math.max.apply(null, arguments);
}
out('forwarded', forwarded(3, 8, 2));

function missing(a, b) {
  return arguments.length + ':' + String(arguments[1]);
}
out('missing', missing(1));
out('excess', missing(1, 2, 3));

function closure(a) {
  return function () { return a + arguments.length; };
}
out('closure', closure(7)(1, 2));

function evaluated(a) {
  return eval('arguments[0]') + a;
}
out('eval', evaluated(6));

async function async_value(a) { return arguments.length + a; }
async_value(5).then(function (v) { out('async', v); });

function* generator_value(a) { yield arguments.length + a; }
out('generator', generator_value(9).next().value);
