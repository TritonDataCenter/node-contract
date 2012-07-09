#include <sys/ccompile.h>
#include <sys/ctfs.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <libnvpair.h>
#include <uv.h>
#include <pthread.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <libcontract_priv.h>
#include "node_contract.h"

typedef enum nc_ack {
	NCA_ACK = 0,
	NCA_NACK,
	NCA_QACK
} nc_ack_t;

static contract_mgr_t mgr = {
	cm_tmpl_fd: -1,
	cm_last_type: NCT_NONE
};

static void
node_contract_event_cb(uv_poll_t *upp, int status __UNUSED, int events __UNUSED)
{
	int fd = (int)(uintptr_t)upp->data;

	/* XXX status == -1 => error; we need to emit that (and close?) */

	handle_events(fd);
}

static node_contract_t *
node_contract_ctor_common(int cfd)
{
	node_contract_t *cp;
	ct_stathdl_t st;
	int err;

	if ((cp = malloc(sizeof (node_contract_t))) == NULL) {
		(void) v8plus_error(V8PLUSERR_NOMEM, NULL);
		return (NULL);
	}

	bzero(cp, sizeof (node_contract_t));
	cp->nc_ctl_fd = cfd;
	cp->nc_ev_fd = -1;
	cp->nc_self = pthread_self();

	if ((err = ct_status_read(cfd, CTD_COMMON, &st)) != 0) {
		free(cp);
		(void) v8plus_syserr(err,
		    "unable to obtain contract status: %s", strerror(err));
		return (NULL);
	}

	cp->nc_id = ct_status_get_id(st);
	ct_status_free(st);

	return (cp);
}

static nvlist_t *
node_contract_ctor_latest(void **cpp)
{
	node_contract_t *cp;
	int cfd;

	if (mgr.cm_last_type == NCT_PROCESS)
		cfd = open64(CTFS_ROOT "/device/latest", O_RDWR);
	else if (mgr.cm_last_type == NCT_DEVICE)
		cfd = open64(CTFS_ROOT "/process/latest", O_RDWR);
	else
		return (v8plus_error(V8PLUSERR_NOTEMPLATE,
		    "no contract template has been activated"));

	if (cfd < 0) {
		return (v8plus_syserr(errno,
		    "unable to open latest contract: %s", strerror(errno)));
	}

	if ((cp = node_contract_ctor_common(cfd)) == NULL) {
		(void) close(cfd);
		return (NULL);
	}

	*cpp = cp;

	return (v8plus_void());
}

static nvlist_t *
node_contract_ctor_adopt(ctid_t ctid, void **cpp)
{
	node_contract_t *cp;
	char buf[PATH_MAX];
	int cfd;
	int err;

	(void) snprintf(buf, sizeof (buf), CTFS_ROOT "/all/%d", (int)ctid);
	cfd = open64(buf, O_RDWR);

	if (cfd < 0) {
		return (v8plus_syserr(errno,
		    "unable to open contract %d ctl handle: %s", (int)ctid,
		    strerror(errno)));
	}

	if ((cp = node_contract_ctor_common(cfd)) == NULL) {
		(void) close(cfd);
		return (NULL);
	}

	if ((err = ct_ctl_adopt(cp->nc_ctl_fd)) != 0) {
	}

	*cpp = cp;

	return (v8plus_void());
}

static nvlist_t *
node_contract_ctor_observe(ctid_t ctid, void **cpp)
{
	return (NULL);
}

static nvlist_t *
node_contract_ctor(const nvlist_t *ap, void **cpp)
{
	ctid_t ctid;
	double d;
	boolean_t b;

	if (v8plus_args(ap, V8PLUS_ARG_F_NOEXTRA,
	    V8PLUS_TYPE_NUMBER, &d,
	    V8PLUS_TYPE_NONE) == 0) {
		ctid = (ctid_t)d;
		return (node_contract_ctor_observe(ctid, cpp));
	}

	if (v8plus_args(ap, V8PLUS_ARG_F_NOEXTRA,
	    V8PLUS_TYPE_NUMBER, &d,
	    V8PLUS_TYPE_BOOLEAN, &b,
	    V8PLUS_TYPE_NONE) == 0) {
		ctid = (ctid_t)d;
		if (b)
			return (node_contract_ctor_adopt(ctid, cpp));
		return (node_contract_ctor_observe(ctid, cpp));
	}

	if (v8plus_args(ap, V8PLUS_ARG_F_NOEXTRA, V8PLUS_TYPE_NONE) == 0)
		return (node_contract_ctor_latest(cpp));

	return (v8plus_error(V8PLUSERR_BADARG,
	    "constructor arguments do not match an acceptable template"));
}

static nvlist_t *
node_contract_set_tmpl(const nvlist_t *ap)
{
	int flags;
	int fd;
	int err;

	/* XXX decode params */

	mgr.cm_tmpl_fd = open64(CTFS_ROOT "/process/template", O_RDWR);
	if (mgr.cm_tmpl_fd < 0) {
		return (v8plus_syserr(errno, "unable to open %s: %s",
		    CTFS_ROOT "/process/template", strerror(errno)));
	}

	if ((flags = fcntl(mgr.cm_tmpl_fd, F_GETFD, 0)) < 0 ||
	    fcntl(mgr.cm_tmpl_fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
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
node_contract_clear_tmpl(const nvlist_t *ap __UNUSED)
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
node_contract_create(const nvlist_t *ap __UNUSED)
{
	int err;
	ctid_t ctid;

	if (mgr.cm_tmpl_fd < 0) {
		return (v8plus_error(V8PLUSERR_NOTEMPLATE,
		    "no contract template is currently active"));
	}

	if ((err = ct_tmpl_create(mgr.cm_tmpl_fd, &ctid)) != 0) {
		return (v8plus_syserr(err, "unable to create contract: %s",
		    strerror(err)));
	}

	return (v8plus_void());
}

static nvlist_t *
node_contract_abandon(void *op, const nvlist_t *ap __UNUSED)
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

	if (cp->nc_ev_fd >= 0) {
		(void) uv_poll_stop(&cp->nc_uv_poll);
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
	ctevid_t evid;
	int err;

	if (nvlist_lookup_nvpair((nvlist_t *)ap, "0", &pp) != 0)
		return (v8plus_error(V8PLUSERR_MISSINGARG, NULL));

	if (v8plus_typeof(pp) == V8PLUS_TYPE_STRING) {
		char *v;

		if ((err = nvpair_value_string(pp, &v)) != 0)
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

static nvlist_t *
node_contract_status(void *op, const nvlist_t *ap)
{
	return (NULL);
}

/*
 * v8+ boilerplate
 */
const v8plus_c_ctor_f v8plus_ctor = node_contract_ctor;
const char *v8plus_js_factory_name = "_new";
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
	},
	{
		md_name: "_status",
		md_c_func: node_contract_status
	}
};
const uint_t v8plus_method_count =
    sizeof (v8plus_methods) / sizeof (v8plus_methods[0]);

const v8plus_static_descr_t v8plus_static_methods[] = {
	{
		sd_name: "_set_template",
		sd_c_func: node_contract_set_tmpl
	},
	{
		sd_name: "_clear_template",
		sd_c_func: node_contract_clear_tmpl
	},
	{
		sd_name: "_create",
		sd_c_func: node_contract_create
	}
};
const uint_t v8plus_static_method_count =
    sizeof (v8plus_static_methods) / sizeof (v8plus_static_methods[0]);
