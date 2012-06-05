#include <sys/ccompile.h>
#include <stdlib.h>
#include <libnvpair.h>
#include "node_contract.h"

static nvlist_t *
node_contract_ctor(const nvlist_t *ap, void **cpp)
{
	node_contract_t *cp = malloc(sizeof (node_contract_t));
	double adopt_arg;
	ctid_t adopt_ct = -1;

	if (cp == NULL)
		return (v8plus_error(V8PLUSERR_NOMEM, NULL));

	(void) fprintf(stderr, "contract ctor\n");
	nvlist_print(stderr, (nvlist_t *)ap);

	if (nvlist_lookup_double((nvlist_t *)ap, "0", &adopt_arg) == 0)
		adopt_ct = (ctid_t)adopt_arg;

	cp->nc_id = adopt_ct == -1 ? 42 : adopt_ct;
	*cpp = cp;

	return (v8plus_void());
}

static nvlist_t *
node_contract_activate(void *op, const nvlist_t *ap)
{
	node_contract_t *cp = op;

	(void) fprintf(stderr, "activate %d\n", (int)cp->nc_id);
	nvlist_print(stderr, (nvlist_t *)ap);
	return (v8plus_void());
}

static nvlist_t *
node_contract_deactivate(void *op, const nvlist_t *ap)
{
	node_contract_t *cp = op;
	nvlist_t *rlp;
	nvlist_t *olp;
	int err;

	(void) fprintf(stderr, "deactivate %d\n", (int)cp->nc_id);
	nvlist_print(stderr, (nvlist_t *)ap);

	if ((err = nvlist_alloc(&olp, NV_UNIQUE_NAME, 0)) != 0)
		return (v8plus_nverr(err, NULL));
	if ((err = nvlist_alloc(&rlp, NV_UNIQUE_NAME, 0)) != 0)
		return (v8plus_nverr(err, NULL));
	if ((err = nvlist_add_string(olp, "hello", "world")) != 0)
		return (v8plus_nverr(err, "hello"));
	if ((err = nvlist_add_nvlist(rlp, "res", olp)) != 0)
		return (v8plus_nverr(err, "res"));

	return (rlp);
}

static nvlist_t *
node_contract_abandon(void *op, const nvlist_t *ap)
{
	node_contract_t *cp = op;

	(void) fprintf(stderr, "abandon %d\n", (int)cp->nc_id);
	nvlist_print(stderr, (nvlist_t *)ap);
	return (v8plus_error(V8PLUSERR_NOMEM, NULL));
/*	return (v8plus_void()); */
}

/*
 * v8+ boilerplate
 */
const v8plus_c_ctor_f v8plus_ctor = node_contract_ctor;
const char *v8plus_js_factory_name = "create";
const char *v8plus_js_class_name = "Contract";
const v8plus_method_descr_t v8plus_methods[] = {
	{
		md_name: "_activate",
		md_c_func: node_contract_activate
	},
	{
		md_name: "_deactivate",
		md_c_func: node_contract_deactivate
	},
	{
		md_name: "abandon",
		md_c_func: node_contract_abandon
	}
};
const uint_t v8plus_method_count =
    sizeof (v8plus_methods) / sizeof (v8plus_methods[0]);
