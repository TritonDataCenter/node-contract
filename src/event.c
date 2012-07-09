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

#define	MAXINT_STRLEN	32

static const struct {
	uint_t f;
	const char *s;
} ct_flags[] = {
	{ CTE_ACK,	"CTE_ACK" },
	{ CTE_INFO,	"CTE_INFO" },
	{ CTE_NEG,	"CTE_NEG" }
};
static const uint_t n_ct_flags = sizeof (ct_flags) / sizeof (ct_flags[0]);

static const struct {
	uint_t t;
	const char *s;
} ct_evtypes[] = {
	{ CT_EV_NEGEND,		"CT_EV_NEGEND" },
	{ CT_PR_EV_EMPTY,	"CT_PR_EV_EMPTY" },
	{ CT_PR_EV_FORK,	"CT_PR_EV_FORK" },
	{ CT_PR_EV_EXIT,	"CT_PR_EV_EXIT" },
	{ CT_PR_EV_CORE,	"CT_PR_EV_CORE" },
	{ CT_PR_EV_SIGNAL,	"CT_PR_EV_SIGNAL" },
	{ CT_PR_EV_HWERR,	"CT_PR_EV_HWERR" },
	{ CT_DEV_EV_ONLINE,	"CT_DEV_EV_ONLINE" },
	{ CT_DEV_EV_DEGRADED,	"CT_DEV_EV_DEGRADED" }
};
static const uint_t n_ct_evtypes = sizeof (ct_evtypes) / sizeof (ct_evtypes[0]);

static size_t
event_type_name(uint_t type, char *buf, size_t len)
{
	const char *p = NULL;
	uint_t i;

	for (i = 0; i < n_ct_evtypes; i++) {
		if (type == ct_evtypes[i].t) {
			p = ct_evtypes[i].s;
			break;
		}
	}

	if (p != NULL)
		return (snprintf(buf, len, "%s", p));

	return (snprintf(buf, len, "unknown event type %u", type));
}

static size_t
event_strflags(uint_t flags, char *buf, size_t len)
{
	char *p;
	size_t total = 0;
	size_t nleft = len;
	size_t flen;
	uint_t i;

	p = buf;

	for (i = 0; i < n_ct_flags; i++) {
		if (flags & ct_flags[i].f) {
			if (p != buf && nleft > 1) {
				*p++ = ' ';
				nleft--;
				len++;
			}
			flen = strlcpy(p, ct_flags[i].s, nleft);
			if (flen < nleft) {
				p += flen;
				nleft -= flen;
			} else {
				nleft = 0;
			}
			total += flen;
		}
	}

	return (total);
}

static nvlist_t *
ct_ev_to_nvlist(ct_evthdl_t eh, ctid_t ctid, const char *typestr)
{
	char *p;
	nvlist_t *lp;
	ctevid_t evid;
	uint_t flags;
	size_t len;
	int err;

	if ((err = nvlist_alloc(&lp, NV_UNIQUE_NAME, 0)) != 0)
		return (v8plus_nverr(err, NULL));

	evid = ct_event_get_evid(eh);
	flags = ct_event_get_flags(eh);

	p = alloca(MAXINT_STRLEN);
	(void) snprintf(p, MAXINT_STRLEN, "%ld", (long)ctid);
	if ((err = nvlist_add_string(lp, "nce_ctid", p)) != 0) {
		nvlist_free(lp);
		return (v8plus_nverr(err, "nce_ctid"));
	}

	(void) snprintf(p, MAXINT_STRLEN, "%lu", (unsigned long)evid);
	if ((err = nvlist_add_string(lp, "nce_evid", p)) != 0) {
		nvlist_free(lp);
		return (v8plus_nverr(err, "nce_evid"));
	}

	if ((err = nvlist_add_string(lp, "nce_flags", typestr)) != 0) {
		nvlist_free(lp);
		return (v8plus_nverr(err, "nce_flags"));
	}

	len = event_strflags(flags, NULL, 0) + 1;
	p = alloca(len);
	(void) event_strflags(flags, p, len);
	if ((err = nvlist_add_string(lp, "nce_type", p)) != 0) {
		nvlist_free(lp);
		return (v8plus_nverr(err, "nce_type"));
	}

	return (lp);
}

void
handle_events(int fd)
{
	char *typestr;
	node_contract_t *cp;
	ct_evthdl_t eh;
	ctid_t ctid;
	nvlist_t *lp;
	nvlist_t *ap, *rp;
	size_t len;
	uint_t type;
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

		type = ct_event_get_type(eh);
		len = event_type_name(type, NULL, 0) + 1;
		typestr = alloca(len);
		(void) event_type_name(type, typestr, len);

		if ((err = nvlist_alloc(&ap, NV_UNIQUE_NAME, 0)) != 0) {
			v8plus_panic("unable to allocate nvlist for event "
			    "emitter argument list: %s", strerror(err));
		}

		if ((lp = ct_ev_to_nvlist(eh, ctid, typestr)) == NULL) {
			v8plus_panic("unable to construct event: %s",
			    _v8plus_errmsg);
		}

		if ((err = nvlist_add_string(ap, "0", typestr)) != 0) {
			v8plus_panic("unable to add element 0 to event "
			    "emitter argument list: %s", strerror(err));
		}

		if ((err = nvlist_add_nvlist(ap, "1", lp)) != 0) {
			v8plus_panic("unable to add element 1 to event "
			    "emitter argument list: %s", strerror(err));
		}

		rp = v8plus_method_call(cp, "_emit", ap);
		nvlist_free(ap);
		nvlist_free(lp);
		nvlist_free(rp);	/* Tell someone who cares. */
	}

	if (err != EAGAIN) {
		v8plus_panic("unexpected error from ct_event_read: %s",
		    strerror(err));
	}
}
