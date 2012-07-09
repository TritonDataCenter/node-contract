# node-contract

This addon provides a Node.js interface to the SunOS contract(4) subsystem.
This documentation assumes that you are familiar with the contract(4),
process(4), and device_contract(4) documentation.

## Quick Start

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
	child = child_process.spawn('/usr/sbin/rpcbind');
	contract.clear_template();
	ct = contract.last();
	ct.on('CT_PR_EV_EMPTY', function (ev) {
		console.log('contract ' + ct.ctid + ' empty');
		ct.abandon();
		ct.removeAllListeners();
	});

## Creating Contracts

There are 4 ways to create a new contract visible to your process:

- adopt an existing contract
- observe an existing contract without adopting it
- fetch the latest contract created by a fork(2) or open(2)
- explicitly create a device contract

The latter two mechanisms require that a template be active.

The public interfaces for performing these actions are as follows:

contract.observe([Number] ctid)
contract.adopt([Number] ctid)
contract.set_template([Object] template)
contract.create() [ or open, fork, etc. ]
contract.last()
contract.clear_template()

The observe(), adopt(), and last() methods return an object of type
Contract, with the following methods:

Contract.status()
Contract.abandon()

## Contract Events

This object is also an EventEmitter; it emits events whenever the underlying
contract generates an informative, critical, or fatal event.  Within the
event handler, critical and fatal events must be acknowledged.  The
following three methods exist for this purpose:

Contract.ack()
Contract.nack()
Contract.qack()

These have the same meaning as their libcontract(3lib) counterparts.

## Destruction of Contracts

A contract that has been broken, whether as part of a negotiated transition
or because of a fatal asnychronous event, becomes invalid.  It is the
responsibility of the listener(s) to ensure that it can subsequently be
cleaned up by removing all event listeners.  Similarly, a contract that has
been abandoned, even if destroyed by the system, cannot be garbage collected
by the Node.js runtime until the consumer removes all event listeners and
discards all its references to that Contract object.  The effect of invoking
methods other than those to remove event listeners is undefined for Contract
objects that have been abandoned or for which all listeners have been
notified of a fatal event.  There is no explicit mechanism to discard the
native ContractBinding object.

## Implementation

Contract creation is done via the contract_binding._new() mechanism, as
follows:

contract_binding._new(<ctid>) observes an existing contract.
contract_binding._new(<ctid>, true) adopts an existing contract.
contract_binding._new() returns the last contract created.

contract_binding._create() explicitly creates a device contract from the
active template but does not return it; use contract_binding._new()
afterward to obtain the newly-created contract.

contract_binding._new returns a native object of type ContractBinding.  

Note that there is currently no support for writing a new contract to
replace one that has been broken via negotiation or an asynchronous device
state change (analogous to ct_ctl_newct(3contract)).  Otherwise it should be
possible to access all public functionality of the contract subsystem for
both device and process contracts.

## License

MIT.
