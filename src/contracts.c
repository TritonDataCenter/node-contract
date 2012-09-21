/*
 * Copyright (c) 2012, Joyent, Inc.  All rights reserved.
 */

#include <sys/debug.h>
#include "node_contract.h"

static node_contract_t *ctid_head;

node_contract_t *
nc_lookup(ctid_t ctid)
{
	node_contract_t *cp;

	for (cp = ctid_head; cp != NULL; cp = cp->nc_next) {
		if (cp->nc_id == ctid)
			return (cp);
	}

	return (NULL);
}

void
nc_add(node_contract_t *cp)
{
	VERIFY(cp->nc_next == NULL);

	cp->nc_next = ctid_head;
	ctid_head = cp;
}

void
nc_del(node_contract_t *cp)
{
	node_contract_t **ep, *np;

	for (ep = &ctid_head; *ep != NULL; ep = &((*ep)->nc_next)) {
		if (*ep == cp) {
			np = (*ep)->nc_next;
			(*ep)->nc_next = NULL;
			*ep = np;
			return;
		}
	}

	VERIFY("deletion of nonexistent contract entry" == NULL);
}
