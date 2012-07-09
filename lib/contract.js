var util = require('util');
var EventEmitter = require('events').EventEmitter;
var contract = require('./contract');

function
Contract()
{
	var self = this;

	EventEmitter.call(this);

	this._binding = contract._create();
	this._binding._emit = function () {
		var args = Array.prototype.slice.call(arguments);
		self.emit.apply(self, args);
	};

	this._ctid = 0;
	this._active = false;
	this.__defineGetter__('ctid', function () { return (self._ctid); });
}
util.inherits(Contract, EventEmitter);

function
create()
{
	return (new Contract());
}

function
adopt(ctid)
{
	var ct = new Contract();

	ct._binding._adopt(ctid);
	this._ctid = ctid;

	return (ct);
}

Contract.activate = function activate() {
	if (this.ctid !== 0 || this._active === true)
		throw new Error('already associated with an active contract');

	this._binding._activate();
	this._active = true;
};

Contract.deactivate = function deactivate() {
	this._binding._deactivate();
	this._active = false;
};

Contract.abandon = function abandon() {
	this._binding._abandon();
	this._ctid = 0;
};

module.exports = {
	create: create,
	adopt: adopt
};
