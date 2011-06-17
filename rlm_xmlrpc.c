/*
 * rlm_xmlrpc.c
 *
 * Version:	$0.1$
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 * Copyright 2011  Angelo Compagnucci <angelo.compagnucci@gmail.com>
 */

#include <freeradius/ident.h>
RCSID("$Id$")

#include <freeradius/radiusd.h>
#include <freeradius/modules.h>

#include <xmlrpc-c/base.h>
#include <xmlrpc-c/client.h>

#define NAME "Freeradius rlm_xmlrpc"
#define VERSION "0.1"

/*
 *	Define a structure for our module configuration.
 *
 *	These variables do not need to be in a structure, but it's
 *	a lot cleaner to do so, and a pointer to the structure can
 *	be used as the instance handle.
 */
typedef struct rlm_xmlrpc_t {
	char				*url;
	char				*method;
	int					count;
	xmlrpc_server_info 	* serverInfoP;
	xmlrpc_env 			env;
	xmlrpc_client 		* clientP;
} rlm_xmlrpc_t;

int check_error(void *instance){
	rlm_xmlrpc_t *inst = instance;
	xmlrpc_env env = inst->env;
	if (env.fault_occurred){
		radlog(L_ERR, "rlm_xmlrpc: %s", env.fault_string);
		DEBUG("rlm_xmlrpc: %s", env.fault_string);
		return RLM_MODULE_FAIL;
	 }
	 return RLM_MODULE_OK;
}

/*
 *	A mapping of configuration file names to internal variables.
 *
 *	Note that the string is dynamically allocated, so it MUST
 *	be freed.  When the configuration file parse re-reads the string,
 *	it free's the old one, and strdup's the new one, placing the pointer
 *	to the strdup'd string into 'config.string'.  This gets around
 *	buffer over-flows.
 */
static const CONF_PARSER module_config[] = {
  { "url",  PW_TYPE_STRING_PTR, offsetof(rlm_xmlrpc_t,url), NULL,  NULL},
  { "method",  PW_TYPE_STRING_PTR, offsetof(rlm_xmlrpc_t,method), NULL,  NULL},

  { NULL, -1, 0, NULL, NULL }		/* end the list */
};


/*
 *	Do any per-module initialization that is separate to each
 *	configured instance of the module.  e.g. set up connections
 *	to external databases, read configuration files, set up
 *	dictionary entries, etc.
 *
 *	If configuration information is given in the config section
 *	that must be referenced in later calls, store a handle to it
 *	in *instance otherwise put a null pointer there.
 */
static int xmlrpc_instantiate(CONF_SECTION *conf, void **instance)
{
	rlm_xmlrpc_t *data;
	int error;

	/*
	 *	Set up a storage area for instance data
	 */
	data = rad_malloc(sizeof(*data));
	if (!data) {
		return -1;
	}
	memset(data, 0, sizeof(*data));

	/*
	 *	If the configuration parameters can't be parsed, then
	 *	fail.
	 */
	if (cf_section_parse(conf, data, module_config) < 0) {
		free(data);
		return -1;
	}

	*instance = data;
	
	xmlrpc_env_init(&data->env);
	
	xmlrpc_env env;
	env = data->env;
	
	xmlrpc_client_setup_global_const(&env);
	xmlrpc_client_create(&env, XMLRPC_CLIENT_NO_FLAGS, NAME, VERSION, NULL, 0,
								 &data->clientP);
	error = check_error(data);
	if (error != RLM_MODULE_OK) return error;
	
	data->serverInfoP = xmlrpc_server_info_new(&env, data->url);
	error = check_error(data);
	if (error != RLM_MODULE_OK) return error;
	
	return 0;
}

/*
 *	Write accounting information to this modules database.
 */
static int xmlrpc_accounting(void *instance, REQUEST *request)
{
	rlm_xmlrpc_t *inst = instance;
	VALUE_PAIR *vps = request->packet->vps;
	xmlrpc_env env = inst->env;
	
	xmlrpc_value *array_param;
	xmlrpc_value *array_string;
	xmlrpc_value *resultP;
	 
	char * const methodName = inst->method;
	int error;
	 
	VALUE_PAIR *status_type_pair;
	if ((status_type_pair = pairfind(request->packet->vps, PW_ACCT_STATUS_TYPE)) == NULL) {
		radlog(L_ERR, "rlm_xmlrpc: No Accounting-Status-Type record.");
		return RLM_MODULE_NOOP;
	}
	
	array_param = xmlrpc_array_new(&env);
	error = check_error(inst);
	if (error != RLM_MODULE_OK) return error;
	 
	array_string = xmlrpc_array_new(&env);
	error = check_error(inst);
	if (error != RLM_MODULE_OK) return error;
	 
	VALUE_PAIR *vp = vps;
	for( ; vp; vp = vp->next){
		char buf[1024];
		vp_prints(buf, sizeof(buf), vp);
		xmlrpc_array_append_item(&env, array_string, xmlrpc_string_new(&env, buf));
		int error = check_error(inst);
		if (error != RLM_MODULE_OK) return error;
	}
	
	xmlrpc_array_append_item(&env, array_param, array_string);
	error = check_error(inst);
	if (error != RLM_MODULE_OK) return error;
	 
	xmlrpc_client_call2(&env, inst->clientP, inst->serverInfoP, methodName, 
		array_param, &resultP);
	error = check_error(inst);
	if (error != RLM_MODULE_OK) return error;
	 
	RDEBUG("rlm_xmlrpc: done");

	xmlrpc_DECREF(resultP);
	xmlrpc_DECREF(array_param);
	xmlrpc_DECREF(array_string);

	return RLM_MODULE_OK;
}

/*
 *	Only free memory we allocated.  The strings allocated via
 *	cf_section_parse() do not need to be freed.
 */
static int xmlrpc_detach(void *instance)
{
	rlm_xmlrpc_t *inst = instance;
	
	xmlrpc_env_clean(&inst->env);
	xmlrpc_client_destroy(inst->clientP);
	 xmlrpc_client_teardown_global_const();
	 
	free(inst);
	return 0;
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
module_t rlm_xmlrpc = {
	RLM_MODULE_INIT,
	"xmlrpc",
	RLM_TYPE_THREAD_SAFE,		/* type */
	xmlrpc_instantiate,		/* instantiation */
	xmlrpc_detach,			/* detach */
	{
		NULL,	/* authentication */
		NULL,	/* authorization */
		NULL,	/* preaccounting */
		xmlrpc_accounting,	/* accounting */
		NULL,	/* checksimul */
		NULL,			/* pre-proxy */
		NULL,			/* post-proxy */
		NULL			/* post-auth */
	},
};
