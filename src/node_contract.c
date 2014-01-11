/*
 * Copyright (c) 2012, Joyent, Inc.  All rights reserved.
 */

#include <sys/ccompile.h>
#include <sys/ctfs.h>
#include <sys/types.h>
#include <sys/debug.h>
#include <sys/param.h>
#include <sys/contract/process.h>
#include <sys/contract/device.h>
#include <fcntl.h>
#include <stdlib.h>
#include <libnvpair.h>
#include <uv.h>
#include <pthread.h>
#include <signal.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <alloca.h>
#include <libcontract.h>
#include "node_contract.h"

typedef enum nc_ack {
	NCA_ACK = 0,
	NCA_NACK,
	NCA_QACK
} nc_ack_t;

static contract_mgr_t mgr = {
	cm_tmpl_fd: -1,
	cm_last_type: NULL,
	cm_ev_fds: { -1, -1 }
};

static uint_t leaked;

const char *
nc_descr_strlookup(const nc_descr_t *dp, uint_t v)
{
	for (; dp != NULL && dp->ncd_str != NULL; dp++) {
		if (v == dp->ncd_i)
			return (dp->ncd_str);
	}

	return ("unknown");
}

uint_t
nc_descr_ilookup(const nc_descr_t *dp, const char *str)
{
	for (; dp != NULL && dp->ncd_str != NULL; dp++) {
		if (strcmp(str, dp->ncd_str) == 0)
			return (dp->ncd_i);
	}

	return (UINT_MAX);
}

static int
close_on_exec(int fd)
{
	int flags;
	int err;

	if ((flags = fcntl(fd, F_GETFD, 0)) < 0 ||
	    fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
		err = errno;
		(void) v8plus_syserr(err, "unable to set CLOEXEC: %s",
		    strerror(err));
		return (-1);
	}

	return (0);
}

static void
node_contract_event_cb(uv_poll_t *upp, int status __UNUSED, int events)
{
	int fd = (int)(uintptr_t)upp->data;

	if (!(events & UV_READABLE))
		return;

	/* XXX status == -1 => error; emit something? */

	handle_events(fd);
}

static void
node_contract_shutdown(node_contract_t *cp)
{
	nc_del(cp);

	if (cp->nc_ctl_fd != -1)
		(void) close(cp->nc_ctl_fd);
	if (cp->nc_st_fd != -1)
		(void) close(cp->nc_st_fd);
	if (cp->nc_ev_fd != -1) {
		uv_poll_stop(&cp->nc_uv_poll);
		(void) close(cp->nc_ev_fd);
	}
}

static void
node_contract_free(node_contract_t *cp)
{
	node_contract_shutdown(cp);
	free(cp);
}

static node_contract_t *
node_contract_ctor_common(int sfd)
{
	node_contract_t *cp;
	ct_stathdl_t st;
	int err;
	const char *typename;
	const nc_typedesc_t *ntp;

	if ((cp = malloc(sizeof (node_contract_t))) == NULL) {
		(void) v8plus_error(V8PLUSERR_NOMEM, NULL);
		return (NULL);
	}

	bzero(cp, sizeof (node_contract_t));
	cp->nc_ctl_fd = -1;
	cp->nc_st_fd = sfd;
	cp->nc_ev_fd = -1;

	if ((err = ct_status_read(sfd, CTD_COMMON, &st)) != 0) {
		(void) v8plus_syserr(err,
		    "unable to obtain contract status: %s", strerror(err));
		node_contract_free(cp);
		return (NULL);
	}

	cp->nc_id = ct_status_get_id(st);
	typename = ct_status_get_type(st);

	for (ntp = nc_types; ntp->nct_name != NULL; ntp++) {
		if (strcmp(ntp->nct_name, typename) == 0)
			cp->nc_type = ntp;
	}
	if (cp->nc_type == NULL) {
		(void) v8plus_throw_exception("Error", "unknown contract type",
		    V8PLUS_TYPE_STRING, "contract_type", typename,
		    V8PLUS_TYPE_NONE);
		node_contract_free(cp);
		return (NULL);
	}
	ct_status_free(st);

	return (cp);
}

static int
node_contract_ctor_post(node_contract_t *cp)
{
	char buf[MAXPATHLEN];

	/*
	 * We can't necessarily control a contract we're only observing, so
	 * don't bother trying.
	 */
	if (cp->nc_ev_fd < 0 && cp->nc_ctl_fd < 0) {
		(void) snprintf(buf, sizeof (buf), "%s/%d/ctl",
		    cp->nc_type->nct_root, (int)cp->nc_id);
		if ((cp->nc_ctl_fd = open(buf, O_WRONLY)) < 0) {
			(void) v8plus_syserr(errno,
			    "unable to open ctl for ct %d: %s", cp->nc_id,
			    strerror(errno));
			node_contract_free(cp);
			return (-1);
		}
	}

	return (0);
}

static nvlist_t *
node_contract_ctor_latest(void **cpp)
{
	node_contract_t *cp;
	char spath[MAXPATHLEN];
	int sfd;

	if (mgr.cm_last_type == NULL) {
		return (v8plus_throw_exception("Error",
		    "no contract template has been activated",
		    V8PLUS_TYPE_NONE));
	}

	(void) snprintf(spath, sizeof (spath), "%s/latest",
	    mgr.cm_last_type->nct_root);
	if ((sfd = open(spath, O_RDONLY)) < 0) {
		return (v8plus_syserr(errno,
		    "unable to open latest contract: %s", strerror(errno)));
	}
	if (close_on_exec(sfd) != 0) {
		(void) close(sfd);
		return (NULL);
	}

	if ((cp = node_contract_ctor_common(sfd)) == NULL) {
		(void) close(sfd);
		return (NULL);
	}

	/*
	 * For contracts created in this manner, we cannot know whether there
	 * are events on our pbundle queue that are not on the contract's
	 * queue.  In order to ensure that we do not lose these events, we
	 * will use the global pbundle watcher for this contract's events.
	 * There is no simple way to open the per-contract event queue and then
	 * stop using the pbundle queue without either losing or duplicating
	 * events, and we need the pbundle queue anyway for newly-created
	 * contracts, so we'll just never use the per-contract event queue.
	 */
	if (node_contract_ctor_post(cp) != 0) {
		node_contract_free(cp);
		return (NULL);
	}

	*cpp = cp;

	return (v8plus_void());
}

static nvlist_t *
node_contract_ctor_adopt(ctid_t ctid, void **cpp)
{
	node_contract_t *cp;
	char buf[MAXPATHLEN];
	int sfd;
	int err;

	(void) snprintf(buf, sizeof (buf), CTFS_ROOT "/all/%d", (int)ctid);
	if ((sfd = open(buf, O_RDONLY)) < 0) {
		return (v8plus_syserr(errno,
		    "unable to open contract %d status handle: %s", (int)ctid,
		    strerror(errno)));
	}
	if (close_on_exec(sfd) != 0) {
		(void) close(sfd);
		return (NULL);
	}

	if ((cp = node_contract_ctor_common(sfd)) == NULL) {
		(void) close(sfd);
		return (NULL);
	}

	(void) snprintf(buf, sizeof (buf), "%s/%d/ctl", cp->nc_type->nct_root,
	    (int)ctid);
	if ((cp->nc_ctl_fd = open(buf, O_WRONLY)) < 0) {
		node_contract_free(cp);
		return (v8plus_syserr(errno,
		    "unable to open contract %d ctl handle: %s", (int)ctid,
		    strerror(errno)));
	}
	if (close_on_exec(cp->nc_ctl_fd) != 0) {
		node_contract_free(cp);
		return (NULL);
	}
	if ((err = ct_ctl_adopt(cp->nc_ctl_fd)) != 0) {
		node_contract_free(cp);
		return (v8plus_syserr(err,
		    "unable to adopt contract %d: %s", (int)ctid,
		    strerror(err)));
	}

	/*
	 * Here, we could use the per-contract event queue in a race-free way.
	 * But there's no reason to, since those events will be on our
	 * pbundle as well.
	 */
	if (node_contract_ctor_post(cp) != 0) {
		node_contract_free(cp);
		return (NULL);
	}

	*cpp = cp;

	return (v8plus_void());
}

static nvlist_t *
node_contract_ctor_observe(ctid_t ctid, void **cpp)
{
	node_contract_t *cp;
	char buf[MAXPATHLEN];
	int sfd;
	int err;

	(void) snprintf(buf, sizeof (buf), CTFS_ROOT "/all/%d", (int)ctid);
	if ((sfd = open(buf, O_RDONLY)) < 0) {
		return (v8plus_syserr(errno,
		    "unable to open contract %d status handle: %s", (int)ctid,
		    strerror(errno)));
	}
	if (close_on_exec(sfd) != 0) {
		(void) close(sfd);
		return (NULL);
	}

	if ((cp = node_contract_ctor_common(sfd)) == NULL) {
		(void) close(sfd);
		return (NULL);
	}

	(void) snprintf(buf, sizeof (buf),
	    CTFS_ROOT "/all/%d/events", (int)ctid);
	if ((cp->nc_ev_fd = open(buf, O_RDONLY | O_NONBLOCK)) < 0) {
		err = errno;
		node_contract_free(cp);
		return (v8plus_syserr(err,
		    "unable to open contract %d event handle: %s", (int)ctid,
		    strerror(err)));
	}
	if (close_on_exec(cp->nc_ev_fd) != 0) {
		node_contract_free(cp);
		return (NULL);
	}

	(void) uv_poll_init(uv_default_loop(), &cp->nc_uv_poll, cp->nc_ev_fd);
	(void) uv_poll_start(&cp->nc_uv_poll, UV_READABLE,
	    node_contract_event_cb);

	if (node_contract_ctor_post(cp) != 0) {
		node_contract_free(cp);
		return (NULL);
	}

	*cpp = cp;

	return (v8plus_void());
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

	if (v8plus_args(ap, V8PLUS_ARG_F_NOEXTRA, V8PLUS_TYPE_NONE) == 0 ||
	    v8plus_args(ap, V8PLUS_ARG_F_NOEXTRA, V8PLUS_TYPE_UNDEFINED,
	    V8PLUS_TYPE_NONE) == 0)
		return (node_contract_ctor_latest(cpp));

	{
		nvpair_t *pp;
		(void) nvlist_lookup_nvpair((nvlist_t *)ap, "0", &pp);
		(void) printf("type %d\n", v8plus_typeof(pp));
	}

	return (v8plus_error(V8PLUSERR_BADARG, "bad constructor arguments"));
}

static void
node_contract_dtor(void *op)
{
	node_contract_t *cp = op;

	if (cp->nc_refcnt != 0)
		++leaked;

	free(cp);
}

static int
nc_pr_tmpl_setprop(int fd, const nvlist_t *lp)
{
	const nc_typedesc_t *ntp = &nc_types[NCT_PROCESS];
	double d;
	char *s;
	boolean_t b;
	int err;
	nvlist_t *sp;
	const nc_descr_t *dp;

	if (nvlist_lookup_double((nvlist_t *)lp, "transfer", &d) == 0) {
		ctid_t ctid = (ctid_t)d;

		if ((err = ct_pr_tmpl_set_transfer(fd, ctid)) != 0) {
			(void) v8plus_syserr(err,
			    "unable to set template property transfer: %s",
			    strerror(err));
			return (-1);
		}
	}

	if (nvlist_lookup_nvlist((nvlist_t *)lp, "fatal", &sp) == 0) {
		uint_t evset = 0;

		for (dp = ntp->nct_events; dp->ncd_str != NULL; dp++) {
			if (nvlist_lookup_boolean_value(sp,
			    dp->ncd_str, &b) == 0 && b)
				evset |= dp->ncd_i;
		}
		if ((err = ct_pr_tmpl_set_fatal(fd, evset)) != 0) {
			(void) v8plus_syserr(err,
			    "unable to set template property fatal: %s",
			    strerror(err));
			return (-1);
		}
	}

	if (nvlist_lookup_nvlist((nvlist_t *)lp, "param", &sp) == 0) {
		uint_t param = 0;

		for (dp = nc_pr_params; dp->ncd_str != NULL; dp++) {
			if (nvlist_lookup_boolean_value(sp,
			    dp->ncd_str, &b) == 0 && b)
				param |= dp->ncd_i;
		}
		if ((err = ct_pr_tmpl_set_param(fd, param)) != 0) {
			(void) v8plus_syserr(err,
			    "unable to set template property param: %s",
			    strerror(err));
			return (-1);
		}
	}

	if (nvlist_lookup_string((nvlist_t *)lp, "svc_fmri", &s) == 0) {
		if ((err = ct_pr_tmpl_set_svc_fmri(fd, s)) != 0) {
			(void) v8plus_syserr(err,
			    "unable to set template property svc_fmri: %s",
			    strerror(err));
			return (-1);
		}
	}

	if (nvlist_lookup_string((nvlist_t *)lp, "svc_aux", &s) == 0) {
		if ((err = ct_pr_tmpl_set_svc_aux(fd, s)) != 0) {
			(void) v8plus_syserr(err,
			    "unable to set template property svc_aux: %s",
			    strerror(err));
			return (-1);
		}
	}

	return (0);
}

static int
nc_dev_tmpl_setprop(int fd, const nvlist_t *lp)
{
	char *s;
	boolean_t b;
	nvlist_t *sp;
	int err;
	const nc_descr_t *dp;

	if (nvlist_lookup_nvlist((nvlist_t *)lp, "dev_aset", &sp) == 0) {
		uint_t aset = 0;

		for (dp = nc_dev_states; dp->ncd_str != NULL; dp++) {
			if (nvlist_lookup_boolean_value(sp,
			    dp->ncd_str, &b) == 0 && b)
				aset |= dp->ncd_i;
		}
		if ((err = ct_dev_tmpl_set_aset(fd, aset)) != 0) {
			(void) v8plus_syserr(err,
			    "unable to set template property aset: %s",
			    strerror(err));
			return (-1);
		}
	}

	if (nvlist_lookup_string((nvlist_t *)lp, "dev_minor", &s) == 0) {
		if ((err = ct_dev_tmpl_set_minor(fd, s)) != 0) {
			(void) v8plus_syserr(err,
			    "unable to set template property minor: %s",
			    strerror(err));
			return (-1);
		}
	}

	if (nvlist_lookup_boolean_value((nvlist_t *)lp, "dev_noneg", &b) == 0) {
		if (b)
			err = ct_dev_tmpl_set_noneg(fd);
		else
			err = ct_dev_tmpl_clear_noneg(fd);
		if (err != 0) {
			(void) v8plus_syserr(err,
			    "unable to set/clear template property noneg: %s",
			    strerror(err));
			return (-1);
		}
	}

	return (0);
}

static int
nc_generic_tmpl_setprop(int fd, const nvlist_t *lp, const nc_typedesc_t *ntp)
{
	char *s;
	boolean_t b;
	nvlist_t *sp;
	int err;
	const nc_descr_t *dp;

	if (nvlist_lookup_nvlist((nvlist_t *)lp, "critical", &sp) == 0) {
		uint_t evset = 0;

		for (dp = ntp->nct_events; dp->ncd_str != NULL; dp++) {
			if (nvlist_lookup_boolean_value(sp,
			    dp->ncd_str, &b) == 0 && b)
				evset |= dp->ncd_i;
		}
		if ((err = ct_tmpl_set_critical(fd, evset)) != 0) {
			(void) v8plus_syserr(err,
			    "unable to set template property critical: %s",
			    strerror(err));
			return (-1);
		}
	}

	if (nvlist_lookup_nvlist((nvlist_t *)lp, "informative", &sp) == 0) {
		uint_t evset = 0;

		for (dp = ntp->nct_events; dp->ncd_str != NULL; dp++) {
			if (nvlist_lookup_boolean_value(sp,
			    dp->ncd_str, &b) == 0 && b)
				evset |= dp->ncd_i;
		}
		if ((err = ct_tmpl_set_informative(fd, evset)) != 0) {
			(void) v8plus_syserr(err,
			    "unable to set template property informative: %s",
			    strerror(err));
			return (-1);
		}
	}

	if (nvlist_lookup_string((nvlist_t *)lp, "cookie", &s) == 0) {
		uint64_t cv;

		errno = 0;
		cv = strtoull(s, NULL, 0);
		if (errno != 0) {
			(void) v8plus_syserr(errno,
			    "unable to parse template property cookie: %s",
			    strerror(errno));
			return (-1);
		}
		if ((err = ct_tmpl_set_cookie(fd, cv)) != 0) {
			(void) v8plus_syserr(err,
			    "unable to set template property cookie: %s",
			    strerror(err));
			return (-1);
		}
	}

	return (0);
}

static nvlist_t *
node_contract_set_tmpl(const nvlist_t *ap)
{
	const nc_typedesc_t *ntp;
	nvlist_t *params;
	char *typename;
	char buf[MAXPATHLEN];
	int err;

	if (v8plus_args(ap, V8PLUS_ARG_F_NOEXTRA,
	    V8PLUS_TYPE_OBJECT, &params,
	    V8PLUS_TYPE_NONE) != 0)
		return (NULL);

	if (nvlist_lookup_string(params, "type", &typename) != 0) {
		return (v8plus_error(V8PLUSERR_MISSINGARG,
		    "parameter property 'type' is required"));
	}

	for (ntp = nc_types; ntp != NULL; ntp++) {
		if (strcmp(ntp->nct_name, typename) == 0)
			break;
	}
	if (ntp == NULL) {
		return (v8plus_error(V8PLUSERR_BADARG,
		    "contract type '%s' is unknown", typename));
	}

	if (mgr.cm_tmpl_fd >= 0) {
		(void) ct_tmpl_clear(mgr.cm_tmpl_fd);
		(void) close(mgr.cm_tmpl_fd);
	}

	mgr.cm_tmpl_fd = open(CTFS_ROOT "/process/template", O_RDWR);
	if (mgr.cm_tmpl_fd < 0) {
		return (v8plus_syserr(errno, "unable to open %s: %s",
		    CTFS_ROOT "/process/template", strerror(errno)));
	}
	if (close_on_exec(mgr.cm_tmpl_fd) != 0) {
		(void) close(mgr.cm_tmpl_fd);
		mgr.cm_tmpl_fd = -1;
		return (NULL);
	}

	if (nc_generic_tmpl_setprop(mgr.cm_tmpl_fd, params, ntp) != 0) {
		(void) close(mgr.cm_tmpl_fd);
		mgr.cm_tmpl_fd = -1;
		return (NULL);
	}

	if (ntp->nct_tmpl_setprop(mgr.cm_tmpl_fd, params) != 0) {
		(void) close(mgr.cm_tmpl_fd);
		mgr.cm_tmpl_fd = -1;
		return (NULL);
	}

	if (mgr.cm_ev_fds[ntp->nct_type] < 0) {
		(void) snprintf(buf, sizeof (buf), "%s/pbundle", ntp->nct_root);
		if ((mgr.cm_ev_fds[ntp->nct_type] =
		    open(buf, O_RDONLY | O_NONBLOCK)) < 0) {
			err = errno;
			(void) close(mgr.cm_tmpl_fd);
			mgr.cm_tmpl_fd = -1;
			return (v8plus_syserr(err,
			    "unable to open contract pbundle event handle: %s",
			    strerror(err)));
		}
		if (close_on_exec(mgr.cm_ev_fds[ntp->nct_type]) != 0) {
			(void) close(mgr.cm_ev_fds[ntp->nct_type]);
			mgr.cm_ev_fds[ntp->nct_type] = -1;
			(void) close(mgr.cm_tmpl_fd);
			mgr.cm_tmpl_fd = -1;
			return (NULL);
		}

		(void) uv_poll_init(uv_default_loop(),
		    &mgr.cm_uv_poll[ntp->nct_type],
		    mgr.cm_ev_fds[ntp->nct_type]);
		mgr.cm_uv_poll[ntp->nct_type].data =
		    (void *)(uintptr_t)mgr.cm_ev_fds[ntp->nct_type];
		(void) uv_poll_start(&mgr.cm_uv_poll[ntp->nct_type],
		    UV_READABLE, node_contract_event_cb);
	}

	if ((err = ct_tmpl_activate(mgr.cm_tmpl_fd)) != 0) {
		(void) close(mgr.cm_tmpl_fd);
		mgr.cm_tmpl_fd = -1;
		return (v8plus_syserr(err, "unable to activate template: %s",
		    strerror(err)));
	}

	mgr.cm_last_type = ntp;

	return (v8plus_void());
}

static nvlist_t *
node_contract_clear_tmpl(const nvlist_t *ap __UNUSED)
{
	int err;

	if (mgr.cm_tmpl_fd < 0)
		return (v8plus_void());

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
		return (v8plus_throw_exception("Error",
		    "no contract template has been activated",
		    V8PLUS_TYPE_NONE));
	}

	if ((err = ct_tmpl_create(mgr.cm_tmpl_fd, &ctid)) != 0) {
		return (v8plus_syserr(err, "unable to create contract: %s",
		    strerror(err)));
	}

	/*
	 * The caller is expected to use latest() here, so we discard the id.
	 */

	return (v8plus_void());
}

static nvlist_t *
node_contract_abandon(void *op, const nvlist_t *ap __UNUSED)
{
	node_contract_t *cp = op;
	int err;

	if (cp->nc_ctl_fd < 0) {
		return (v8plus_throw_exception("Error",
		    "this contract has no control descriptor",
		    V8PLUS_TYPE_NONE));
	}

	if ((err = ct_ctl_abandon(cp->nc_ctl_fd)) != 0) {
		return (v8plus_syserr(err, "failed to abandon contract: %s",
		    strerror(err)));
	}

	return (v8plus_void());
}

static nvlist_t *
node_contract_ack_common(void *op, const nvlist_t *ap, nc_ack_t ack)
{
	node_contract_t *cp = op;
	nvpair_t *pp;
	ctevid_t evid;
	int err;

	if (v8plus_args(ap, V8PLUS_ARG_F_NOEXTRA,
	    V8PLUS_TYPE_STRNUMBER64, &evid,
	    V8PLUS_TYPE_NONE) != 0)
		return (NULL);

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
		return (v8plus_throw_exception("Error",
		    "this contract has no control descriptor",
		    V8PLUS_TYPE_NONE));
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
node_contract_sigsend(void *op, const nvlist_t *ap)
{
	node_contract_t *cp = op;
	double dsigno;
	char errbuf[64];

	if (cp->nc_type->nct_type != NCT_PROCESS)
		return (v8plus_error(V8PLUSERR_BADARG,
		    "not a process contract"));

	if (v8plus_args(ap, V8PLUS_ARG_F_NOEXTRA,
	    V8PLUS_TYPE_NUMBER, &dsigno, V8PLUS_TYPE_NONE) != 0)
		return (NULL);

	if (sigsend(P_CTID, cp->nc_id, (int)dsigno) != 0) {
		(void) snprintf(errbuf, sizeof (errbuf),
		    "sigsend: %s", strerror(errno));
		return (v8plus_throw_errno_exception(errno, "sigsend", NULL,
		    NULL, V8PLUS_TYPE_NONE));
	}

	return (v8plus_void());
}

#define	VP(_n, _t, _v) \
	V8PLUS_TYPE_##_t, #_n, (_v)

static int
nc_pr_status_add_to_nvlist(nvlist_t *lp, ct_stathdl_t st)
{
	const nc_typedesc_t *ntp = &nc_types[NCT_PROCESS];
	nvlist_t *sp;
	const nc_descr_t *dp;
	uint_t param;
	uint_t fatal;
	pid_t *pids;
	uint_t npids = 0;
	ctid_t *cts;
	uint_t ncts = 0;
	char *fmri = NULL;
	char *aux = NULL;
	ctid_t svc_ctid = 0;
	char *creator = NULL;
	uint_t i;
	char buf[32];
	int err;

	VERIFY(ct_pr_status_get_param(st, &param) == 0);
	VERIFY(ct_pr_status_get_fatal(st, &fatal) == 0);
	(void) ct_pr_status_get_members(st, &pids, &npids);
	(void) ct_pr_status_get_contracts(st, &cts, &ncts);
	(void) ct_pr_status_get_svc_fmri(st, &fmri);
	(void) ct_pr_status_get_svc_aux(st, &aux);
	(void) ct_pr_status_get_svc_ctid(st, &svc_ctid);
	(void) ct_pr_status_get_svc_creator(st, &creator);

	if ((sp = v8plus_obj(V8PLUS_TYPE_NONE)) == NULL)
		return (-1);
	for (dp = nc_pr_params; dp->ncd_str != NULL; dp++) {
		if (v8plus_obj_setprops(sp,
		    V8PLUS_TYPE_BOOLEAN, dp->ncd_str,
		    (param & dp->ncd_i) != 0,
		    V8PLUS_TYPE_NONE) != 0) {
			return (-1);
		}
	}
	err = v8plus_obj_setprops(lp,
	    V8PLUS_TYPE_OBJECT, "pr_param", sp,
	    V8PLUS_TYPE_NONE);
	nvlist_free(sp);
	if (err != 0)
		return (-1);

	if ((sp = v8plus_obj(V8PLUS_TYPE_NONE)) == NULL)
		return (-1);
	for (dp = ntp->nct_events; dp->ncd_str != NULL; dp++) {
		if (v8plus_obj_setprops(sp,
		    V8PLUS_TYPE_BOOLEAN, dp->ncd_str,
		    (fatal & dp->ncd_i) != 0,
		    V8PLUS_TYPE_NONE) != 0) {
			return (-1);
		}
	}
	err = v8plus_obj_setprops(lp,
	    V8PLUS_TYPE_OBJECT, "pr_fatal", sp,
	    V8PLUS_TYPE_NONE);
	nvlist_free(sp);
	if (err != 0)
		return (-1);

	if (npids > 0) {
		if ((sp = v8plus_obj(V8PLUS_TYPE_NONE)) == NULL)
			return (-1);
		for (i = 0; i < npids; i++) {
			(void) snprintf(buf, sizeof (buf), "%u", i);
			if (v8plus_obj_setprops(sp,
			    V8PLUS_TYPE_NUMBER, buf, (double)pids[i],
			    V8PLUS_TYPE_NONE) != 0) {
				nvlist_free(sp);
				return (-1);
			}
		}
		err = v8plus_obj_setprops(lp,
		    V8PLUS_TYPE_OBJECT, "pr_members", sp,
		    V8PLUS_TYPE_NONE);
		nvlist_free(sp);
		if (err != 0)
			return (-1);
	}

	if (ncts > 0) {
		if ((sp = v8plus_obj(V8PLUS_TYPE_NONE)) == NULL)
			return (-1);
		for (i = 0; i < ncts; i++) {
			(void) snprintf(buf, sizeof (buf), "%u", i);
			if (v8plus_obj_setprops(sp,
			    V8PLUS_TYPE_NUMBER, buf, (double)cts[i],
			    V8PLUS_TYPE_NONE) != 0) {
				nvlist_free(sp);
				return (-1);
			}
		}
		err = v8plus_obj_setprops(lp,
		    V8PLUS_TYPE_OBJECT, "pr_contracts", sp,
		    V8PLUS_TYPE_NONE);
		nvlist_free(sp);
		if (err != 0)
			return (-1);
	}

	if (fmri != NULL && v8plus_obj_setprops(lp,
	    V8PLUS_TYPE_STRING, "pr_svc_fmri", fmri,
	    V8PLUS_TYPE_NONE) != 0)
		return (-1);
	if (aux != NULL && v8plus_obj_setprops(lp,
	    V8PLUS_TYPE_STRING, "pr_svc_aux", aux,
	    V8PLUS_TYPE_NONE) != 0)
		return (-1);
	if (svc_ctid != 0 && v8plus_obj_setprops(lp,
	    V8PLUS_TYPE_NUMBER, "pr_svc_ctid", (double)svc_ctid,
	    V8PLUS_TYPE_NONE) != 0)
		return (-1);
	if (creator != NULL && v8plus_obj_setprops(lp,
	    V8PLUS_TYPE_STRING, "pr_svc_creator", creator,
	    V8PLUS_TYPE_NONE) != 0)
		return (-1);

	return (0);
}

static int
nc_dev_status_add_to_nvlist(nvlist_t *lp, ct_stathdl_t st)
{
	const nc_descr_t *sdp;
	nvlist_t *sp;
	uint_t state;
	uint_t aset;
	char *minor;
	uint_t noneg;
	int err;

	VERIFY(ct_dev_status_get_dev_state(st, &state) == 0);
	VERIFY(ct_dev_status_get_aset(st, &aset) == 0);
	VERIFY(ct_dev_status_get_minor(st, &minor) == 0);
	VERIFY(ct_dev_status_get_noneg(st, &noneg) == 0);

	if (v8plus_obj_setprops(lp,
	    V8PLUS_TYPE_STRING, "dev_state",
	    nc_descr_strlookup(nc_dev_states, state),
	    V8PLUS_TYPE_NONE) != 0)
		return (-1);

	if ((sp = v8plus_obj(V8PLUS_TYPE_NONE)) == NULL)
		return (-1);
	for (sdp = nc_dev_states; sdp->ncd_str != NULL; sdp++) {
		if (v8plus_obj_setprops(sp,
		    V8PLUS_TYPE_BOOLEAN, sdp->ncd_str,
		    (aset & sdp->ncd_i) != 0,
		    V8PLUS_TYPE_NONE) != 0) {
			return (-1);
		}
	}
	err = v8plus_obj_setprops(lp,
	    V8PLUS_TYPE_OBJECT, "dev_aset", sp,
	    V8PLUS_TYPE_NONE);
	nvlist_free(sp);
	if (err != 0)
		return (-1);

	if (v8plus_obj_setprops(lp,
	    V8PLUS_TYPE_STRING, "dev_minor", minor,
	    V8PLUS_TYPE_NONE) != 0)
		return (-1);

	if (v8plus_obj_setprops(lp,
	    V8PLUS_TYPE_BOOLEAN, "dev_noneg", (noneg != 0),
	    V8PLUS_TYPE_NONE) != 0)
		return (-1);

	return (0);
}

static nvlist_t *
nc_status_to_nvlist(ct_stathdl_t st)
{
	const char *typename;
	const nc_typedesc_t *ntp;
	const nc_descr_t *dp;
	nvlist_t *rp;
	nvlist_t *srp;
	uint_t inf;
	uint_t crit;
	int err;

	typename = ct_status_get_type(st);
	for (ntp = nc_types; ntp->nct_name != NULL; ntp++) {
		if (strcmp(ntp->nct_name, typename) == 0)
			break;
	}
	if (ntp == NULL) {
		return (v8plus_throw_exception("Error",
		    "unknown contract type",
		    V8PLUS_TYPE_STRING, "contract_type", typename,
		    V8PLUS_TYPE_NONE));
	}

	inf = ct_status_get_informative(st);
	crit = ct_status_get_critical(st);

	rp = v8plus_obj(
	    VP(ctid, NUMBER, (double)ct_status_get_id(st)),
	    VP(zoneid, NUMBER, (double)ct_status_get_zoneid(st)),
	    VP(type, STRING, typename),
	    VP(state, STRING,
		nc_descr_strlookup(nc_ct_states, ct_status_get_state(st))),
	    VP(holder, NUMBER, (double)ct_status_get_holder(st)),
	    VP(nevents, NUMBER, (double)ct_status_get_nevents(st)),
	    VP(ntime, NUMBER, (double)ct_status_get_ntime(st)),
	    VP(qtime, NUMBER, (double)ct_status_get_qtime(st)),
	    VP(nevid, STRNUMBER64, ct_status_get_nevid(st)),
	    VP(cookie, STRNUMBER64, ct_status_get_cookie(st)),
	    V8PLUS_TYPE_NONE);

	if (rp == NULL)
		return (NULL);

	if ((srp = v8plus_obj(V8PLUS_TYPE_NONE)) == NULL) {
		nvlist_free(rp);
		return (NULL);
	}
	for (dp = ntp->nct_events; dp->ncd_str != NULL; dp++) {
		if (v8plus_obj_setprops(srp,
		    V8PLUS_TYPE_BOOLEAN, dp->ncd_str,
		    (inf & dp->ncd_i) != 0,
		    V8PLUS_TYPE_NONE) != 0) {
			nvlist_free(rp);
			return (NULL);
		}
	}
	err = v8plus_obj_setprops(rp,
	    V8PLUS_TYPE_OBJECT, "informative", srp,
	    V8PLUS_TYPE_NONE);
	nvlist_free(srp);
	if (err != 0) {
		nvlist_free(rp);
		return (NULL);
	}

	if ((srp = v8plus_obj(V8PLUS_TYPE_NONE)) == NULL) {
		nvlist_free(rp);
		return (NULL);
	}
	for (dp = ntp->nct_events; dp->ncd_str != NULL; dp++) {
		if (v8plus_obj_setprops(srp,
		    V8PLUS_TYPE_BOOLEAN, dp->ncd_str,
		    (crit & dp->ncd_i) != 0,
		    V8PLUS_TYPE_NONE) != 0) {
			nvlist_free(rp);
			return (NULL);
		}
	}
	err = v8plus_obj_setprops(rp,
	    V8PLUS_TYPE_OBJECT, "critical", srp,
	    V8PLUS_TYPE_NONE);
	nvlist_free(srp);
	if (err != 0) {
		nvlist_free(rp);
		return (NULL);
	}

	if (ntp->nct_status_add_to_nvlist(rp, st) != 0) {
		nvlist_free(rp);
		return (NULL);
	}

	return (rp);
}

#undef VP	/* a little late */

static nvlist_t *
node_contract_status(void *op, const nvlist_t *ap)
{
	node_contract_t *cp = op;
	nvlist_t *rp, *lp;
	ct_stathdl_t st;
	int err;

	if (v8plus_args(ap, V8PLUS_ARG_F_NOEXTRA, V8PLUS_TYPE_NONE) != 0) {
		return (v8plus_error(V8PLUSERR_EXTRAARG,
		    "the status method accepts no arguments"));
	}

	if ((err = ct_status_read(cp->nc_st_fd, CTD_ALL, &st)) != 0) {
		return (v8plus_syserr(err, "unable to read status: %s",
		    strerror(err)));
	}

	if ((err = nvlist_alloc(&rp, NV_UNIQUE_NAME, 0)) != 0) {
		ct_status_free(st);
		return (v8plus_nverr(err, NULL));
	}

	lp = nc_status_to_nvlist(st);
	ct_status_free(st);

	if (lp == NULL)
		return (NULL);

	rp = v8plus_obj(
	    V8PLUS_TYPE_OBJECT, "res", lp,
	    V8PLUS_TYPE_NONE);

	nvlist_free(lp);

	return (rp);
}

static nvlist_t *
node_contract_hold(void *op, const nvlist_t *ap __UNUSED)
{
	node_contract_t *cp = op;

	if (++cp->nc_refcnt == 1) {
		nc_add(cp);
		v8plus_obj_hold(cp);
	}

	return (v8plus_void());
}

static nvlist_t *
node_contract_rele(void *op, const nvlist_t *ap __UNUSED)
{
	node_contract_t *cp = op;

	if (--cp->nc_refcnt == 0) {
		node_contract_shutdown(cp);
		v8plus_obj_rele(cp);
	}

	return (v8plus_void());
}

/*
 * libcontract constant lookup tables
 */
static const nc_descr_t nc_pr_events[] = {
	{ CT_EV_NEGEND,		"negend" },
	{ CT_PR_EV_EMPTY,	"pr_empty" },
	{ CT_PR_EV_FORK,	"pr_fork" },
	{ CT_PR_EV_EXIT,	"pr_exit" },
	{ CT_PR_EV_CORE,	"pr_core" },
	{ CT_PR_EV_SIGNAL,	"pr_signal" },
	{ CT_PR_EV_HWERR,	"pr_hwerr" },
	{ 0,			NULL }
};

static const nc_descr_t nc_dev_events[] = {
	{ CT_EV_NEGEND,		"negend" },
	{ CT_DEV_EV_ONLINE,	"dev_online" },
	{ CT_DEV_EV_DEGRADED,	"dev_degraded" },
	{ CT_DEV_EV_OFFLINE,	"dev_offline" },
	{ 0,			NULL }
};

static const nc_descr_t _nc_pr_params[] = {
	{ CT_PR_INHERIT,	"inherit" },
	{ CT_PR_NOORPHAN,	"noorphan" },
	{ CT_PR_PGRPONLY,	"pgrponly" },
	{ CT_PR_REGENT,		"regent" },
	{ 0,			NULL }
};
const nc_descr_t *nc_pr_params = _nc_pr_params;

static const nc_descr_t _nc_dev_states[] = {
	{ CT_DEV_EV_ONLINE,	"online" },
	{ CT_DEV_EV_DEGRADED,	"degraded" },
	{ CT_DEV_EV_OFFLINE,	"offline" },
	{ 0,			NULL }
};
const nc_descr_t *nc_dev_states = _nc_dev_states;

static const nc_typedesc_t _nc_types[] = {
	{
		nct_type: NCT_PROCESS,
		nct_name: "process",
		nct_events: nc_pr_events,
		nct_root: CTFS_ROOT "/process",
		nct_status_add_to_nvlist: nc_pr_status_add_to_nvlist,
		nct_tmpl_setprop: nc_pr_tmpl_setprop
	},
	{
		nct_type: NCT_DEVICE,
		nct_name: "device",
		nct_events: nc_dev_events,
		nct_root: CTFS_ROOT "/device",
		nct_status_add_to_nvlist: nc_dev_status_add_to_nvlist,
		nct_tmpl_setprop: nc_dev_tmpl_setprop
	},
	{
		nct_type: NCT_MAX,
		nct_name: NULL,
		nct_events: NULL,
		nct_root: NULL,
		nct_status_add_to_nvlist: NULL,
		nct_tmpl_setprop: NULL
	}
};
const nc_typedesc_t *nc_types = _nc_types;

static const nc_descr_t _nc_ct_states[] = {
	{ CTS_OWNED,		"owned" },
	{ CTS_INHERITED,	"inherited" },
	{ CTS_ORPHAN,		"orphan" },
	{ CTS_DEAD,		"dead" },
	{ 0,			NULL }
};
const nc_descr_t *nc_ct_states = _nc_ct_states;

static const nc_descr_t _nc_ev_flags[] = {
	{ CTE_INFO,		"info" },
	{ CTE_ACK,		"ack" },
	{ CTE_NEG,		"neg" },
	{ 0,			NULL }
};
const nc_descr_t *nc_ev_flags = _nc_ev_flags;

/*
 * v8+ boilerplate
 */
const v8plus_c_ctor_f v8plus_ctor = node_contract_ctor;
const v8plus_c_dtor_f v8plus_dtor = node_contract_dtor;
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
		md_name: "_rele",
		md_c_func: node_contract_rele
	},
	{
		md_name: "_hold",
		md_c_func: node_contract_hold
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
		md_name: "_sigsend",
		md_c_func: node_contract_sigsend
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
