var contract = require('contract.js');
var child_process = require('child_process.js');

var ct;
var child;
var tmpl = {
	type: 'process',
	critical: [ 'CT_PR_EV_EMPTY', 'CT_PR_EV_HWERR' ],
	informative: [ 'CT_PR_EV_EXIT', 'CT_PR_EV_CORE' ],
	params: [ 'CT_PR_NOORPHAN' ]
};

contract.set_template(tmpl);
child = child_process.spawn('/usr/bin/false');
contract.clear_template();
ct = contract.last();
ct.on('CT_PR_EV_EMPTY', function (ev) {
	console.log('contract ' + ct.ctid + ' empty');
	ct.abandon();
	ct.removeAllListeners();
});
