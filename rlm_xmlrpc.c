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
#ifdef HAVE_PTHREAD_H
#endif
/*
 *	Define a structure for our module configuration.
 *
 *	These variables do not need to be in a structure, but it's
 *	a lot cleaner to do so, and a pointer to the structure can
 *	be used as the instance handle.
 */
typedef struct rlm_xmlrpc_client_t {
	xmlrpc_server_info *serverInfoP;
	xmlrpc_env env;
	xmlrpc_client *clientP;
	struct rlm_xmlrpc_client_t *next;
} rlm_xmlrpc_client_t;

typedef struct rlm_xmlrpc_t {
	char *url;
	char *method;
	char *interface;
	char *auth_type;
	char *user;
	char *password;
	int no_ssl_verify_peer;
	int no_ssl_verify_host;
	int xmlrpc_num_socks;
	rlm_xmlrpc_client_t *client;

#ifdef HAVE_PTHREAD_H
	pthread_mutex_t client_mutex;
#endif
} rlm_xmlrpc_t;

void free_instance(rlm_xmlrpc_t * instance)
{
	rlm_xmlrpc_t *inst = instance;

	rlm_xmlrpc_client_t *next;
	rlm_xmlrpc_client_t *cur;

	cur = inst->client;

	while (cur->next) {
		next = cur->next;
		xmlrpc_env_clean(&cur->env);
		xmlrpc_server_info_free(cur->serverInfoP);
		xmlrpc_client_destroy(cur->clientP);
		free(cur);
		cur = next;
	}
	if (cur) {
		xmlrpc_env_clean(&cur->env);
		xmlrpc_server_info_free(cur->serverInfoP);
		xmlrpc_client_destroy(cur->clientP);
		free(cur);
	}

	xmlrpc_client_teardown_global_const();

	pthread_mutex_destroy(&inst->client_mutex);
	free(inst);
}

int check_error_and_free(rlm_xmlrpc_t * instance)
{
	xmlrpc_env *env = &instance->client->env;
	char *error = env->fault_string;
	if (env->fault_occurred) {
		radlog(L_ERR, "rlm_xmlrpc: %s", error);
		DEBUG("rlm_xmlrpc: %s", error);
		free_instance(instance);
		return RLM_MODULE_FAIL;
	}
	return RLM_MODULE_OK;
}

int check_error(xmlrpc_env * env)
{
	char *error = env->fault_string;
	if (env->fault_occurred) {
		radlog(L_ERR, "rlm_xmlrpc: %s", error);
		DEBUG("rlm_xmlrpc: %s", error);
		return RLM_MODULE_FAIL;
	}
	return RLM_MODULE_OK;
}

/*
 * This method returns a pointer to a client. Thread use this method
 * to ensure a proper access to a member of the clients linked list.
 * Mutex is used to grant a round robin access to clients. 
 */
rlm_xmlrpc_client_t *get_client(rlm_xmlrpc_t * instance)
{
	rlm_xmlrpc_client_t *client;

#ifdef HAVE_PTHREAD_H
	pthread_mutex_lock(&instance->client_mutex);
#endif

	client = instance->client;
	instance->client = instance->client->next;

#ifdef HAVE_PTHREAD_H
	pthread_mutex_unlock(&instance->client_mutex);
#endif
	return client;
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
	{"url", PW_TYPE_STRING_PTR, offsetof(rlm_xmlrpc_t, url), NULL, NULL},
	{"method", PW_TYPE_STRING_PTR, offsetof(rlm_xmlrpc_t, method), NULL, NULL},
	{"interface", PW_TYPE_STRING_PTR, offsetof(rlm_xmlrpc_t, interface), NULL, "lo"},
	{"no_ssl_verify_peer", PW_TYPE_BOOLEAN, offsetof(rlm_xmlrpc_t, no_ssl_verify_peer), NULL, "no"},
	{"no_ssl_verify_host", PW_TYPE_BOOLEAN, offsetof(rlm_xmlrpc_t, no_ssl_verify_host), NULL, "no"},
	{"xmlrpc_num_socks", PW_TYPE_INTEGER, offsetof(rlm_xmlrpc_t, xmlrpc_num_socks), NULL, "5"},
	{"auth_type", PW_TYPE_STRING_PTR, offsetof(rlm_xmlrpc_t, auth_type), NULL, "none"},
	{"user", PW_TYPE_STRING_PTR, offsetof(rlm_xmlrpc_t, user), NULL, NULL},
	{"password", PW_TYPE_STRING_PTR, offsetof(rlm_xmlrpc_t, password), NULL, NULL},

	{NULL, -1, 0, NULL, NULL}	/* end the list */
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
static int xmlrpc_instantiate(CONF_SECTION * conf, void **instance)
{
	rlm_xmlrpc_t *data;
	rlm_xmlrpc_client_t *first_client;
	rlm_xmlrpc_client_t *client;

	xmlrpc_env env;

	struct xmlrpc_clientparms clientParms;
	struct xmlrpc_curl_xportparms curlParms;

	int i, error;
	void (*do_auth) ();

	/*
	 *      Set up a storage area for instance data
	 */
	data = rad_malloc(sizeof(*data));
	if (!data) {
		return -1;
	}
	memset(data, 0, sizeof(*data));

	/*
	 *      If the configuration parameters can't be parsed, then
	 *      fail.
	 */
	if (cf_section_parse(conf, data, module_config) < 0) {
		free(data);
		return -1;
	}

	*instance = data;

#ifdef HAVE_PTHREAD_H
	pthread_mutex_init(&data->client_mutex, NULL);
#endif

	/*
	 * network_interface parameter cannot be omitted because the
	 * XMLRPC_CXPSIZE macro calcs the size from the first parameter
	 * to the last modified.
	 * Unfortunately is this the order. 
	 */
	curlParms.network_interface = data->interface;
	curlParms.no_ssl_verifypeer = data->no_ssl_verify_peer;
	curlParms.no_ssl_verifyhost = data->no_ssl_verify_host;

	clientParms.transport = "curl";
	clientParms.transportparmsP = &curlParms;
	clientParms.transportparm_size = XMLRPC_CXPSIZE(no_ssl_verifyhost);

	/*
	 * Choosing method authentication
	 */
	if (strcmp(data->auth_type, "auth_basic") == 0) {
		do_auth = xmlrpc_server_info_allow_auth_basic;
	} else if (strcmp(data->auth_type, "auth_digest") == 0) {
		do_auth = xmlrpc_server_info_allow_auth_digest;
	} else if (strcmp(data->auth_type, "auth_negotiate") == 0) {
		do_auth = xmlrpc_server_info_allow_auth_negotiate;
	} else if (strcmp(data->auth_type, "auth_ntlm") == 0) {
		do_auth = xmlrpc_server_info_allow_auth_ntlm;
	}

	/*
	 * Clients are created into a circular linked list.
	 * Into this cycle we setup clients and server information objects.
	 * Server information contains a method to do html authentication. 
	 */
	for (i = 0; i < data->xmlrpc_num_socks; i++) {
		client = rad_malloc(sizeof(*client));
		if (!client) {
			return -1;
		}
		memset(client, 0, sizeof(*client));

		env = client->env;
		xmlrpc_env_init(&env);

		if (i == 0) {
			data->client = client;
			first_client = client;

			xmlrpc_client_setup_global_const(&env);
		} else {
			data->client->next = client;
			data->client = data->client->next;
		}

		xmlrpc_client_create(&env, XMLRPC_CLIENT_NO_FLAGS, NAME,
				     VERSION, &clientParms,
				     XMLRPC_CPSIZE(transportparm_size), &client->clientP);

		error = check_error_and_free(data);
		if (error != RLM_MODULE_OK)
			return error;

		client->serverInfoP = xmlrpc_server_info_new(&env, data->url);
		error = check_error_and_free(data);
		if (error != RLM_MODULE_OK)
			return error;

		if (strcmp(data->auth_type, "none") != 0) {
			xmlrpc_server_info_set_user(&env, client->serverInfoP,
						    data->user, data->password);
			error = check_error_and_free(data);
			if (error != RLM_MODULE_OK)
				return error;

			do_auth(&env, client->serverInfoP);
			error = check_error_and_free(data);
			if (error != RLM_MODULE_OK)
				return error;

			radlog(L_INFO, "\trlm_xmlrpc: client #%d logged in as %s", i, data->user);
		}

		radlog(L_INFO, "\trlm_xmlrpc: client #%d initialized", i);

	}

	/*
	 * closing the circular linked list. data->client helds a pointer
	 * to the last client used by a thread.
	 */

	data->client->next = first_client;
	data->client = data->client->next;

	return 0;
}

/*
 *	Write accounting information to this modules database.
 */
static int xmlrpc_accounting(void *instance, REQUEST * request)
{
	rlm_xmlrpc_t *inst = instance;
	rlm_xmlrpc_client_t *client;
	VALUE_PAIR *vps = request->packet->vps;

	xmlrpc_value *array_param;
	xmlrpc_value *array_string;
	xmlrpc_value *resultP;

	char *const methodName = inst->method;
	int error;

	VALUE_PAIR *status_type_pair;
	if ((status_type_pair = pairfind(request->packet->vps, PW_ACCT_STATUS_TYPE)) == NULL) {
		radlog(L_ERR, "rlm_xmlrpc: No Accounting-Status-Type record.");
		return RLM_MODULE_NOOP;
	}

	client = get_client(instance);

	/*
	 * Xmlrpc wants method params in an array, so we build an array
	 * with a pointer in index 0. This pointer contains a sub array
	 * filled whith strings each one for packet attributes.
	 */
	array_param = xmlrpc_array_new(&client->env);
	error = check_error(&client->env);
	if (error != RLM_MODULE_OK)
		return error;

	array_string = xmlrpc_array_new(&client->env);
	error = check_error(&client->env);
	if (error != RLM_MODULE_OK)
		return error;

	/*
	 * The array of strings is built whit vp_prints
	 */
	VALUE_PAIR *vp = vps;
	for (; vp; vp = vp->next) {
		char buf[1024];
		vp_prints(buf, sizeof(buf), vp);
		xmlrpc_array_append_item(&client->env, array_string,
					 xmlrpc_string_new(&client->env, buf));
		int error = check_error(&client->env);
		if (error != RLM_MODULE_OK)
			return error;
	}

	xmlrpc_array_append_item(&client->env, array_param, array_string);
	error = check_error(&client->env);
	if (error != RLM_MODULE_OK)
		return error;

	xmlrpc_client_call2(&client->env, client->clientP, client->serverInfoP,
			    methodName, array_param, &resultP);
	error = check_error(&client->env);
	if (error != RLM_MODULE_OK)
		return error;

	/*
	 * We don't check for method return value. If an accounting packet is
	 * dispatched without errors, it should be processed by server 
	 * without any further notification.
	 */

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
	free_instance(instance);
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
	RLM_TYPE_THREAD_SAFE,	/* type */
	xmlrpc_instantiate,	/* instantiation */
	xmlrpc_detach,		/* detach */
	{
	 NULL,			/* authentication */
	 NULL,			/* authorization */
	 NULL,			/* preaccounting */
	 xmlrpc_accounting,	/* accounting */
	 NULL,			/* checksimul */
	 NULL,			/* pre-proxy */
	 NULL,			/* post-proxy */
	 NULL			/* post-auth */
	 }
	,
};
