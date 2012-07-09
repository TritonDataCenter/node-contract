/*
 * Copyright (c) 2012, Joyent, Inc.  All rights reserved.
 */

#include "node_contract.h"

typedef struct ctid_entry {
	ctid_t ce_ctid;
	node_contract_t *ce_contract;
	struct ctid_entry *ce_next;
} ctid_entry_t;

static ctid_entry_t *ctid_head;

node_contract_t *
nc_lookup(ctid_t ctid)
{
	const ctid_entry_t *ep;

	for (ep = ctid_head; ep != NULL; ep = ep->ce_next) {
		if (ep->ce_ctid == ctid)
			return (ep->ce_contract);
	}

	return (NULL);
}

int
nc_add(ctid_t ctid, node_contract_t *cp)
{
	ctid_entry_t *ep = malloc(sizeof (ctid_entry_t));

	if (ep == NULL)
		return (-1);

	ep->ce_ctid = ctid;
	ep->ce_contract = cp;
	ctid_head->ce_next = ctid_head;
	ctid_head = ep;

	return (0);
}

void
nc_del(const node_contract_t *cp)
{
	ctid_entry_t **ep, *np;

	for (ep = &ctid_head; *ep != NULL; ep = &((*ep)->ce_next)) {
		if ((*ep)->ce_contract == cp) {
			np = (*ep)->ce_next;
			free(*ep);
			*ep = np;
			return;
		}
	}
}
