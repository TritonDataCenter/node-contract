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
	int nc_ctl_fd;
	int nc_ev_fd;
	uv_poll_t nc_uv_poll;
	pthread_t nc_self;
} node_contract_t;

typedef struct contract_mgr {
	int cm_tmpl_fd;
} contract_mgr_t;

extern node_contract_t *nc_lookup(ctid_t);
extern int nc_add(ctid_t, node_contract_t *);
extern void nc_del(const node_contract_t *);
extern void handle_events(int);

#ifdef	__cplusplus
}
#endif	/* __cplusplus */

#endif	/* _NODE_CONTRACT_H */
