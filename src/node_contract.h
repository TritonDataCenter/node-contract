#ifndef _NODE_CONTRACT_H
#define	_NODE_CONTRACT_H

#include <pthread.h>
#include <stdarg.h>
#include <libcontract.h>
#include <libnvpair.h>
#include "v8plus/v8plus_glue.h"

#ifdef	__cplusplus
extern "C" {
#endif	/* __cplusplus */

typedef struct node_contract {
	ctid_t nc_id;
	int nc_tmpl_fd;
	int nc_ctl_fd;
	int nc_ev_fd;
	pthread_t nc_self;
	void *nc_js_cb_cookie;
} node_contract_t;

#ifdef	__cplusplus
}
#endif	/* __cplusplus */

#endif	/* _NODE_CONTRACT_H */
