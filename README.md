# node-contract

This addon provides a Node.js interface to the SunOS contract(4) subsystem.
This documentation assumes that you are familiar with the contract(4),
process(4), and device_contract(4) documentation.

## Quick Start

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
		cookie: '0xdeadbeef`
	};

	contract.set_template(tmpl);
	child = child_process.spawn('/usr/sbin/rpcbind');
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

## Creating Contracts

There are 4 ways to create a new contract visible to your process:

- adopt an existing contract
- observe an existing contract without adopting it
- fetch the latest contract created by a fork(2) or open(2)
- explicitly create a device contract

The latter two mechanisms require that a template be active.

The public interfaces for performing these actions are as follows:

### contract.adopt([Number] ctid)

Adopt the specified contract, as for `ct_ctl_adopt(3contract)`.

### contract.observe([Number] ctid)

Observe, without adopting, the specified contract.  This is analogous to
opening only the event descriptor associated with the contract and watching
it for events as via `ctwatch(1)`.  Contracts created in this manner cannot
be subsequently adopted, abandoned, or otherwise modified.

### contract.set_template([Object] template)

Create and activate a template with the specified attributed.  The template
object's properties correspond to those that may be set by `ct_tmpl_set_*`,
including those properties specific to the contract type.  The `type` field,
which is not such an attribute, is required and must be one of `process` or
`device`.  The template provided completely replaces any existing active
template of the same type.

### contract.create() [ or open, fork, etc. ]

`contract.create()` is analogous to and uses `ct_tmpl_create(3contract)`.
Following such a contract creation, the contract created may be accessed via
a call to `contract.latest()`.  Note that this function is useful only for
device contracts.  One may also create device contracts by setting a
template, then opening a device minor node.  Process contracts can be
created only by setting a template, then performing a `fork(2)` such as via
`child_process` functionality.

### contract.latest()

Returns an object of type `Contract` with methods described below.  The
contract returned represents the most recently created contract.  Note that
if you set a template, perform an action that instantiates a contract, then
set another template and perform another contract-instantiating action
prior to calling `contract.latest()`, the first contract created cannot be
retrieved unless its ctid becomes known through some alternate mechanism.
It is also likely that such a calling sequence will result in resource
leaks.

### contract.clear_template()

Remove any active contract templates.  Following this call, actions that
would instantiate a new contract or add members to an existing contract will
instead behave normally.

## Contract

The `observe()`, `adopt()`, and `latest()` methods return an object of type
Contract, with the following methods:

### Contract.status()

Returns an object with fields corresponding to the attributes accessible via
a `ct_stathdl_t` from `ct_status_read(3contract)`, including those which are
specific to the contract type.  Flags fields are represented as embedded
objects with one boolean property per flag.

### Contract.abandon()

Abandon the contract and attempt to free all resources associated with it.
This is analogous to, and uses, `ct_ctl_abandon(3contract)`.

### Contract.ack([String] evid)

See `ct_ctl_ack(3contract)`.

### Contract.nack([String] evid)

See `ct_ctl_nack(3contract)`.

### Contract.qack([String] evid)

See `ct_ctl_qack(3contract)`.

## Contract Events

Contract objects inherit from Node.js's `events.EventEmitter`; they emit
events whenever the underlying contract generates an informative, critical,
or fatal event.  Within the event callback, critical and fatal events must
be acknowledged.  The event names correspond to those specified by
`contract(4)`, `process(4)`, and `device_contract(4)`, lower-cased with
`CT_` and `EV_` removed; e.g., `pr_empty`.  These event names are also used
when passing event sets within template and status objects.

## Destruction of Contracts

A contract that has been broken, whether as part of a negotiated transition
or because of a fatal asynchronous event, becomes invalid.  It is the
responsibility of the listener(s) to ensure that it can subsequently be
cleaned up by removing all event listeners.  Similarly, a contract that has
been abandoned, even if destroyed by the system, cannot be garbage collected
by the Node.js runtime until the consumer removes all event listeners and
discards all its references to that Contract object.  The effect of invoking
methods other than those to remove event listeners is undefined for
`Contract` objects that have been abandoned or for which all listeners have
been notified of a fatal event.  There is no explicit mechanism to discard
the native `ContractBinding` object itself.

## Implementation Notes

Contract creation is done via the `contract_binding._new()` mechanism, as
follows:

- `contract_binding._new(<ctid>)` observes an existing contract.
- `contract_binding._new(<ctid>, true)` adopts an existing contract.
- `contract_binding._new()` returns the last contract created.

`contract_binding._create()` explicitly creates a device contract from the
active template but does not return it; use `contract_binding._new()`
afterward to obtain the newly-created contract.

`contract_binding._new()` returns a native object of type `ContractBinding`.
This native object, and any other properties of `contract` or `Contract`
whose keys begin with the underscore (`_`) character, should not be read,
replaced, removed, or modified by consumers.

Note that there is currently no support for writing a new contract to
replace one that has been broken via negotiation or an asynchronous device
state change (analogous to `ct_ctl_newct(3contract)`).  Otherwise it should
be possible to access all public functionality of the contract subsystem for
both device and process contracts.

## License

MIT.
