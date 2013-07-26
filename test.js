var contract = require('./lib/index.js');
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
child = child_process.spawn('/bin/bash', ['-c', 'sleep 10 &' ]);
contract.clear_template();
ct = contract.latest();
console.log(util.inspect(ct.status(), null, true));

setTimeout(function () {
	console.log('sending SIGKILL');
	ct.sigsend(9);
}, 2000);

ct.on('pr_empty', function (ev) {
	console.log(util.inspect(ev, null, true));
	console.log('contract ' + ev.ctid + ' has emptied');
	ct.ack(ev.evid);
	console.log(util.inspect(ct.status(), null, true));

	try {
		ct.sigsend(9);
	} catch (ex) {
		console.error('expected error: ', ex);
	}

	ct.abandon();
	ct.removeAllListeners();
	ct = null;
});
