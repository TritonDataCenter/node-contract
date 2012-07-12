var contract = require('./lib/contract.js');
var util = require('util');
var child_process = require('child_process');

var ct;
var child;
var tmpl = {
	type: 'process',
	critical: {
		pr_empty: true,
		pr_hwerr: true
	},
	informative: {
		pr_exit: true,
		pr_core: true
	},
	param: {
		noorphan: true
	},
	cookie: '0xdeadbeef'
};

contract.set_template(tmpl);
child = child_process.spawn('/usr/bin/false');
contract.clear_template();
ct = contract.latest();
console.log(util.inspect(ct.status(), null, true));

ct.on('pr_empty', function (ev) {
	console.log(util.inspect(ev, null, true));
	console.log('contract ' + ev.ctid + ' has emptied');
	console.log(util.inspect(ct.status(), null, true));

	ct.abandon();
	ct.removeAllListeners();
	ct = null;
});
