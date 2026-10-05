function (a, b, iterations) {
  var acc = 0;
  var i = 0;
  while (i < iterations) {
    var dx = b.x - a.x;
    var dy = b.y - a.y;
    acc = acc + dx * dx + dy * dy;
    a.x = a.x + dx * 0.001;
    a.y = a.y + dy * 0.001;
    b.x = b.x - dx * 0.001;
    b.y = b.y - dy * 0.001;
    i = i + 1;
  }
  return acc;
}
