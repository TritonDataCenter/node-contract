#include <sys/ccompile.h>
#include <stdlib.h>
#include <libnvpair.h>
#include <uv.h>
#include <pthread.h>
#include "node_contract.h"

typedef enum nc_ack {
	NCA_ACK = 0,
	NCA_NACK,
	NCA_QACK
} nc_ack_t;

static contract_mgr_t mgr = {
	cm_tmpl_fd: -1
};

static void
node_contract_event_cb(uv_poll_t *upp, int status, int events)
{
	int fd = (int)(uintptr_t)upp->data;

	/* XXX status == -1 => error; we need to emit that (and close?) */

	handle_events(fd);
}

static nvlist_t *
node_contract_ctor(const nvlist_t *ap, void **cpp)
{
	node_contract_t *cp;
	nvpair_t *pp;

	if (nvlist_lookup_nvpair((nvlist_t *)ap, "0", &pp) == 0) {
		if (nvlist_length(ap) > 1 ||
		    v8plus_typeof(pp) != V8PLUS_TYPE_UNDEFINED) {
			return (v8plus_error(V8PLUSERR_EXTRAARG, NULL));
		}
	}

	if ((cp = malloc(sizeof (node_contract_t))) == NULL)
		return (v8plus_error(V8PLUSERR_NOMEM, NULL));

	bzero(cp, sizeof (node_contract_t));
	cp->nc_ctl_fd = -1;
	cp->nc_ev_fd = -1;
	cp->nc_self = pthread_self();

	*cpp = cp;

	return (v8plus_void());
}

static nvlist_t *
node_contract_activate(const nvlist_t *ap)
{
	int flags;
	int fd;
	int err;

	/* XXX decode params */

	mgr.cm_tmpl_fd = open64(CTFS_ROOT "/process/template", O_RDWR);
	if (mgr.cm_tmpl_fd < 0) {
		return (v8plus_error(V8PLUSERR_SYS, "unable to open %s: %s",
		    CTFS_ROOT "/process/template", strerror(errno)));
	}

	if ((flags = fcntl(mgr.cm_tmpl_fd, F_GETFD, 0)) < 0 ||
	    fcntl(mgr.cm_tmpl_fd, F_SETFD, flags | FDCLOEXEC) < 0) {
		err = errno;
		(void) close(mgr.cm_tmpl_fd);
		mgr.cm_tmpl_fd = -1;
		return (v8plus_syserr(err, "unable to set CLOEXEC: %s",
		    strerror(err)));
	}

	/* XXX activate template */

	return (v8plus_void());
}

static nvlist_t *
node_contract_deactivate(const nvlist_t *ap)
{
	int err;

	if ((err = ct_tmpl_clear(mgr.cm_tmpl_fd)) != 0) {
		return (v8plus_syserr(err,
		    "unable to clear active template: %s", strerror(err)));
	}

	(void) close(mgr.cm_tmpl_fd);
	mgr.cm_tmpl_fd = -1;

	return (v8plus_void());
}

static nvlist_t *
node_contract_abandon(void *op, const nvlist_t *ap)
{
	node_contract_t *cp = op;
	int err;

	if (cp->nc_ctl_fd < 0) {
		return (v8plus_error(V8PLUSERR_BADF,
		    "this contract has no control descriptor"));
	}

	if ((err = ct_ctl_abandon(cp->nc_ctl_fd)) != 0) {
		return (v8plus_syserr(err, "failed to abandon contract: %s",
		    strerror(err)));
	}

	if (cp->nc_ev_fd != 0) {
		(void) uv_poll_stop(&nc_uv_poll);
		(void) close(cp->nc_ev_fd);
		cp->nc_ev_fd = -1;
	}

	(void) close(cp->nc_ctl_fd);
	nc_del(cp);

	return (v8plus_void());
}

static nvlist_t *
node_contract_ack_common(void *op, const nvlist_t *ap, nc_ack_t ack)
{
	node_contract_t *cp = op;
	nvpair_t *pp;
	evid_t evid;
	int err;

	if (nvlist_lookup_nvpair((nvlist_t *)ap, "0", &pp) != 0)
		return (v8plus_error(V8PLUSERR_MISSINGARG, NULL));

	if (v8plus_typeof(pp) == V8PLUS_TYPE_STRING) {
		char *v;

		if ((err = nvpair_string_value(pp, &v)) != 0)
			return (v8plus_nverr(err, "0"));

		errno = 0;
		evid = (uint64_t)strtoull(v, NULL, 0);
		if (errno != 0) {
			return (v8plus_error(V8PLUSERR_BADARG,
			    "could not convert '%s' to an event id", v));
		}
	} else {
		return (v8plus_error(V8PLUSERR_BADARG,
		    "argument must be a string event id"));
	}

	if (cp->nc_ctl_fd < 0) {
		return (v8plus_error(V8PLUSERR_BADF,
		    "this contract has no control descriptor"));
	}

	switch (ack) {
	case NCA_ACK:
		err = ct_ctl_ack(cp->nc_ctl_fd, evid);
		break;
	case NCA_NACK:
		err = ct_ctl_nack(cp->nc_ctl_fd, evid);
		break;
	case NCA_QACK:
		err = ct_ctl_qack(cp->nc_ctl_fd, evid);
		break;
	default:
		v8plus_panic("bad ack value %d", ack);
	}

	if (err != 0) {
		return (v8plus_syserr(err, "failed to ack event '%lld': %s",
		    (unsigned long long)evid, strerror(err)));
	}

	return (v8plus_void());
}

static nvlist_t *
node_contract_ack(void *op, const nvlist_t *ap)
{
	return (node_contract_ack_common(op, ap, NCA_ACK));
}

static nvlist_t *
node_contract_nack(void *op, const nvlist_t *ap)
{
	return (node_contract_ack_common(op, ap, NCA_NACK));
}

static nvlist_t *
node_contract_qack(void *op, const nvlist_t *ap)
{
	return (node_contract_ack_common(op, ap, NCA_QACK));
}

/*
 * v8+ boilerplate
 */
const v8plus_c_ctor_f v8plus_ctor = node_contract_ctor;
const char *v8plus_js_factory_name = "_create";
const char *v8plus_js_class_name = "ContractBinding";
const v8plus_method_descr_t v8plus_methods[] = {
	{
		md_name: "_abandon",
		md_c_func: node_contract_abandon
	},
	{
		md_name: "_ack",
		md_c_func: node_contract_ack
	},
	{
		md_name: "_nack",
		md_c_func: node_contract_nack
	},
	{
		md_name: "_qack",
		md_c_func: node_contract_qack
	}
};
const uint_t v8plus_method_count =
    sizeof (v8plus_methods) / sizeof (v8plus_methods[0]);

const v8plus_static_descr_t v8plus_static_methods[] = {
	{
		sd_name: "_activate",
		sd_c_func: node_contract_activate
	},
	{
		sd_name: "_deactivate",
		sd_c_func: node_contract_deactivate
	}
};
const uint_t v8plus_static_method_count =
    sizeof (v8plus_static_methods) / sizeof (v8plus_static_methods[0]);
