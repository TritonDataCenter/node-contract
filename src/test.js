var contract = require('./contract');
var util = require('util');

var test = {
	foo: 3,
	bar: "true",
	baz: true,
	quux: "blargh string",
	quuux: { inner: -1 },
	mumble: [ 4, 5, 6 ]
};

var c = contract.create(17);
c._activate(test);
var result = c._deactivate();
console.log(util.inspect(result, true, null));
c.abandon();
