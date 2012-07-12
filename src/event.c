/*
 * Copyright (c) 2012, Joyent, Inc.  All rights reserved.
 */

#include <sys/contract/process.h>
#include <sys/contract/device.h>
#include <libcontract.h>
#include <libnvpair.h>
#include <stdio.h>
#include <string.h>
#include <alloca.h>
#include "node_contract.h"

#define	VP(_n, _t, _v) \
	V8PLUS_TYPE_##_t, #_n, (_v)
#define	VP_V(_n, _t) \
	V8PLUS_TYPE_##_t, #_n

static uint_t ev_failures;

void
handle_events(int fd)
{
	node_contract_t *cp;
	const nc_descr_t *dp;
	ct_evthdl_t eh;
	ctid_t ctid;
	uint_t evtype;
	ctevid_t evid;
	ctevid_t nevid;
	ctid_t newct;
	uint_t flags;
	nvlist_t *ap, *sap, *rp;
	int err;

	while ((err = ct_event_read(fd, &eh)) == 0) {
		ctid = ct_event_get_ctid(eh);
		cp = nc_lookup(ctid);

		/*
		 * This contract has gone away.  This should be possible only
		 * if we've already abandoned it, but it may be possible to
		 * receive events for it after that.  We can't even ack here,
		 * because we don't have the ctl fd for the contract any more.
		 * Just keep going; there's nothing we can do.
		 */
		if (cp == NULL) {
			ct_event_free(eh);
			++ev_failures;
			continue;
		}

		dp = cp->nc_type->nct_events;

		evtype = ct_event_get_type(eh);
		evid = ct_event_get_evid(eh);
		flags = ct_event_get_flags(eh);

		sap = v8plus_obj(
			VP(ctid, NUMBER, (double)ctid),
			VP(evid, STRNUMBER64, (uint64_t)evid),
			VP_V(flags, INL_OBJECT),
			    VP(info, BOOLEAN, (flags & CTE_INFO) != 0),
			    VP(ack, BOOLEAN, (flags & CTE_ACK) != 0),
			    VP(neg, BOOLEAN, (flags & CTE_NEG) != 0),
			    V8PLUS_TYPE_NONE,
			V8PLUS_TYPE_NONE);

		if (sap == NULL) {
			ct_event_free(eh);
			++ev_failures;
			continue;
		}

		if (evtype == CT_EV_NEGEND) {
			(void) ct_event_get_nevid(eh, &nevid);
			(void) ct_event_get_newct(eh, &newct);
			err = v8plus_obj_setprops(sap,
			    VP(nevid, STRNUMBER64, (uint64_t)nevid),
			    VP(newct, NUMBER, (double)newct),
			    V8PLUS_TYPE_NONE);
			if (err != 0) {
				nvlist_free(sap);
				ct_event_free(eh);
				++ev_failures;
				continue;
			}
		}

		ap = v8plus_obj(
		    VP(0, STRING, nc_descr_strlookup(dp, evtype)),
		    VP(1, OBJECT, sap),
		    V8PLUS_TYPE_NONE);

		nvlist_free(sap);
		ct_event_free(eh);

		if (ap == NULL) {
			++ev_failures;
			continue;
		}

		rp = v8plus_method_call(cp, "_emit", ap);
		nvlist_free(ap);
		nvlist_free(rp);
	}

	if (err != EAGAIN) {
		v8plus_panic("unexpected error from ct_event_read: %s",
		    strerror(err));
	}
}

#undef	VP
#undef	VP_V
