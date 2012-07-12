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
	params: {
		noorphan: true
	}
};

contract.set_template(tmpl);
child = child_process.spawn('/usr/bin/false');
contract.clear_template();
ct = contract.latest();
ct.on('pr_empty', function (ev) {
	console.log(util.inspect(ev, null, true));
	console.log('contract ' + ev.nce_ctid + ' empty');
	ct.abandon();
	ct.removeAllListeners();
	ct = null;
});
