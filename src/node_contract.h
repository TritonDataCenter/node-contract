#ifndef _NODE_CONTRACT_H
#define	_NODE_CONTRACT_H

#include <sys/types.h>
#include <stdarg.h>
#include <libcontract.h>
#include <libnvpair.h>
#include <uv.h>
#include "v8plus/v8plus_glue.h"

#ifdef	__cplusplus
extern "C" {
#endif	/* __cplusplus */

#define	__UNUSED	__attribute__((__unused__))

typedef enum nc_type {
	NCT_PROCESS,
	NCT_DEVICE,
	NCT_MAX
} nc_type_t;

typedef struct nc_descr {
	uint_t ncd_i;
	const char *ncd_str;
} nc_descr_t;

typedef struct nc_typedesc {
	nc_type_t nct_type;
	const char *nct_name;
	const nc_descr_t *nct_events;
	const char *nct_root;
	int (*nct_status_add_to_nvlist)(nvlist_t *, ct_stathdl_t);
	int (*nct_tmpl_setprop)(int, const nvlist_t *);
} nc_typedesc_t;

typedef struct node_contract {
	const nc_typedesc_t *nc_type;
	ctid_t nc_id;
	int nc_ctl_fd;
	int nc_st_fd;
	int nc_ev_fd;
	uv_poll_t nc_uv_poll;
} node_contract_t;

typedef struct contract_mgr {
	int cm_tmpl_fd;
	const nc_typedesc_t *cm_last_type;
	int cm_ev_fds[NCT_MAX];
	uv_poll_t cm_uv_poll[NCT_MAX];
} contract_mgr_t;

extern const nc_typedesc_t *nc_types;
extern const nc_descr_t *nc_ct_states;
extern const nc_descr_t *nc_pr_params;
extern const nc_descr_t *nc_dev_states;
extern const nc_descr_t *nc_ev_flags;

extern const char *nc_descr_strlookup(const nc_descr_t *, uint_t);
extern uint_t nc_descr_ilookup(const nc_descr_t *, const char *);
extern node_contract_t *nc_lookup(ctid_t);
extern int nc_add(ctid_t, node_contract_t *);
extern void nc_del(const node_contract_t *);
extern void handle_events(int);

#ifdef	__cplusplus
}
#endif	/* __cplusplus */

#endif	/* _NODE_CONTRACT_H */
