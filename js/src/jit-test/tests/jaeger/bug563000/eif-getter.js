var g = newGlobal();
var dbg = new g.Debugger(this);

this.__defineGetter__("someProperty", function () { evalInFrame(1, "x = 'success'"); });
function caller(obj) {
  var x = "failure";
  obj.someProperty;
  return x;
}
assertEq(caller(this), "success");
