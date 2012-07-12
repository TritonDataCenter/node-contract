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

void
handle_events(int fd)
{
	node_contract_t *cp;
	const nc_descr_t *edp;
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
			continue;
		}

		edp = cp->nc_type->nct_events;

		evtype = ct_event_get_type(eh);
		evid = ct_event_get_evid(eh);
		flags = ct_event_get_flags(eh);

		ap = v8plus_obj(
		    VP(0, STRING, nc_descr_strlookup(edp, evtype)),
		    VP_V(1, OBJECT),
			VP(nce_ctid, NUMBER, (double)ctid),
			VP(nce_evid, STRNUMBER64, (uint64_t)evid),
			VP_V(nce_flags, OBJECT),
			    VP(INFO, BOOLEAN, (flags & CTE_INFO) != 0),
			    VP(ACK, BOOLEAN, (flags & CTE_ACK) != 0),
			    VP(NEG, BOOLEAN, (flags & CTE_NEG) != 0),
			    V8PLUS_TYPE_NONE,
			V8PLUS_TYPE_NONE,
		    V8PLUS_TYPE_NONE);

		if (evtype == CT_EV_NEGEND &&
		    nvlist_lookup_nvlist(ap, "0", &sap) == 0) {
			(void) ct_event_get_nevid(eh, &nevid);
			(void) ct_event_get_newct(eh, &newct);
			err = v8plus_obj_setprops(sap,
			    VP(nce_nevid, STRNUMBER64, (uint64_t)nevid),
			    VP(nce_newct, NUMBER, (double)newct),
			    V8PLUS_TYPE_NONE);
			if (err != 0) {
				nvlist_free(ap);
				ap = NULL;
			}
		}

		if (ap != NULL) {
			rp = v8plus_method_call(cp, "_emit", ap);
			nvlist_free(ap);
			nvlist_free(rp);
		}
	}

	if (err != EAGAIN) {
		v8plus_panic("unexpected error from ct_event_read: %s",
		    strerror(err));
	}
}

#undef	VP
#undef	VP_V