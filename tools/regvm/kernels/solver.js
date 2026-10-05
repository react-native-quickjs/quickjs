function (iterations) {
  var a = 1.5, b = 2.25, c = 0.75, d = 3.125, e = 0.5;
  var acc = 0;
  var i = 0;
  while (i < iterations) {
    var x = a * b + c;
    var y = x * d - e;
    var z = y / (a + b);
    acc = acc + x + y + z;
    a = a + 0.01;
    b = b - 0.005;
    c = c + x * 0.001;
    if (acc > 1e12) acc = acc - 1e12;
    i = i + 1;
  }
  return acc;
}
