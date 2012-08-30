/*
 *	This program is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	Copyright (C) 2011-2012 Luka Perkov <freecwmp@lukaperkov.net>
 */

#include <stdlib.h>
#include <string.h>
#include <libfreecwmp.h>
#include <libubox/uloop.h>

#include "cwmp.h"
#include "external.h"
#include "../freecwmp.h"
#include "../config.h"
#include "../http/http.h"
#include "../xml/xml.h"

static struct cwmp {
	int event_code;
	struct uloop_timeout connection_request_t;
	struct uloop_timeout periodic_inform_t;
	int8_t periodic_inform_enabled;
	int64_t periodic_inform_interval;
	struct uloop_timeout acs_error_t;
	int8_t retry_count;
	bool config_reload;
	struct list_head notifications;
} cwmp;


static void
cwmp_periodic_inform(struct uloop_timeout *timeout)
{
	if (cwmp.periodic_inform_enabled && cwmp.periodic_inform_interval) {
		cwmp.periodic_inform_t.cb = cwmp_periodic_inform;
		uloop_timeout_set(&cwmp.periodic_inform_t, cwmp.periodic_inform_interval * 1000);
		cwmp.event_code = PERIODIC;
	}

	if (cwmp.periodic_inform_enabled)
		cwmp_inform();
}

void
cwmp_init(void)
{
	char *c = NULL;

	cwmp.retry_count = 0;
	cwmp.config_reload = false;
	cwmp.periodic_inform_enabled = 0;
	cwmp.periodic_inform_interval = 0;
	cwmp.event_code = config->local->event;

	cwmp_get_parameter_handler("InternetGatewayDevice.ManagementServer.PeriodicInformInterval", &c);
	if (c) {
		cwmp.periodic_inform_interval = atoi(c);
		cwmp.periodic_inform_t.cb = cwmp_periodic_inform;
		uloop_timeout_set(&cwmp.periodic_inform_t, cwmp.periodic_inform_interval * 1000);
	}
	FREE(c);

	cwmp_get_parameter_handler("InternetGatewayDevice.ManagementServer.PeriodicInformEnable", &c);
	if (c) {
		cwmp.periodic_inform_enabled = atoi(c);
	}
	FREE(c);

	http_server_init();

	INIT_LIST_HEAD(&cwmp.notifications);
}

void
cwmp_exit(void)
{
	http_client_exit();
	xml_exit();
}

int
cwmp_inform(void)
{
	char *msg_in, *msg_out;
	msg_in = msg_out = NULL;

	if (http_client_init()) {
		D("initializing http client failed\n");
		goto error;
	}

	if (xml_prepare_inform_message(&msg_out)) {
		D("xml message creating failed\n");
		goto error;
	}

	if (http_send_message(msg_out, &msg_in)) {
		D("sending http message failed\n");
		goto error;
	}
	
	if (msg_in && xml_parse_inform_response_message(msg_in, &msg_out)) {
		D("parse xml message from ACS failed\n");
		goto error;
	}

	FREE(msg_in);
	FREE(msg_out);

	cwmp.retry_count = 0;

	if (cwmp_handle_messages()) {
		D("handling xml message failed\n");
		goto error;
	}

	cwmp_clear_notifications();
	http_client_exit();
	xml_exit();

	return 0;

error:
	FREE(msg_in);
	FREE(msg_out);

	http_client_exit();
	xml_exit();

	cwmp.acs_error_t.cb = cwmp_inform;
	if (cwmp.retry_count < 100) {
		cwmp.retry_count++;
		uloop_timeout_set(&cwmp.acs_error_t, 10000 * cwmp.retry_count);
	} else {
		/* try every 20 minutes */
		uloop_timeout_set(&cwmp.acs_error_t, 1200000);
	}

	return -1;
}

int
cwmp_handle_messages(void)
{
	int8_t status;
	char *msg_in, *msg_out;
	msg_in = msg_out = NULL;

	while (1) {
		FREE(msg_in);

		if (http_send_message(msg_out, &msg_in)) {
			D("sending http message failed\n");
			goto error;
		}

		if (!msg_in)
			break;

		FREE(msg_out);

		if (xml_handle_message(msg_in, &msg_out)) {
			D("xml handling message failed\n");
			goto error;
		}

		if (!msg_out) {
			D("acs response message is empty\n");
			goto error;
		}
	}

	FREE(msg_in);
	FREE(msg_out);

	return 0;

error:
	FREE(msg_in);
	FREE(msg_out);

	return -1;
}

void
cwmp_connection_request(int code)
{
	cwmp.event_code = code;
	cwmp.connection_request_t.cb = cwmp_inform;
	uloop_timeout_set(&cwmp.connection_request_t, 500);
}

void
cwmp_add_notification(char *parameter, char *value)
{
	char *c = NULL;
	cwmp_get_notification_handler(parameter, &c);
	if (!c) return;

	bool uniq = true;
	struct notification *n = NULL;
	struct list_head *l, *p;
	list_for_each(p, &cwmp.notifications) {
		n = list_entry(p, struct notification, list);
		if (!strcmp(n->parameter, parameter)) {
			free(n->value);
			n->value = strdup(value);
			uniq = false;
			break;
		}
	}
				
	if (uniq) {
		n = calloc(1, sizeof(*n) + sizeof(char *) + strlen(parameter) + strlen(value));
		if (!n) return;

		list_add_tail(&n->list, &cwmp.notifications);
		n->parameter = strdup(parameter);
		n->value = strdup(value);
	}

	cwmp.event_code = VALUE_CHANGE;
	if (!strncmp(c, "2", 1)) {
		cwmp_inform();
	}
}

struct list_head *
cwmp_get_notifications()
{
	return &cwmp.notifications;
}

void
cwmp_clear_notifications(void)
{
	struct notification *n, *p;
	list_for_each_entry_safe(n, p, &cwmp.notifications, list) {
		free(n->parameter);
		free(n->value);
		list_del(&n->list);
		free(n);
	}
}

int8_t
cwmp_set_parameter_write_handler(char *name, char *value)
{
	FC_DEVEL_DEBUG("enter");

	int8_t status;

#ifdef DEVEL_DEBUG
	printf("+++ CWMP HANDLE SET PARAMETER +++\n");
	printf("Name  : '%s'\n", name);
	printf("Value : '%s'\n", value);
	printf("--- CWMP HANDLE SET PARAMETER ---\n");
#endif

	if((strcmp(name, "InternetGatewayDevice.ManagementServer.Username")) == 0) {
		cwmp.config_reload = true;
	}

	if((strcmp(name, "InternetGatewayDevice.ManagementServer.Password")) == 0) {
		cwmp.config_reload = true;
	}

	if((strcmp(name, "InternetGatewayDevice.ManagementServer.URL")) == 0) {
		cwmp.event_code = VALUE_CHANGE;
		cwmp.config_reload = true;
	}

	if((strcmp(name, "InternetGatewayDevice.ManagementServer.PeriodicInformEnable")) == 0) {
		cwmp.periodic_inform_enabled = atoi(value);
	}

	if((strcmp(name, "InternetGatewayDevice.ManagementServer.PeriodicInformInterval")) == 0) {
		cwmp.periodic_inform_interval = atoi(value);
		cwmp.periodic_inform_t.cb = cwmp_periodic_inform;
		uloop_timeout_set(&cwmp.periodic_inform_t, cwmp.periodic_inform_interval * 1000);
	}

	status = external_set_action_write("value", name, value);

done:
	FC_DEVEL_DEBUG("exit");
	return status;
}

int8_t
cwmp_set_notification_write_handler(char *name, char *value)
{
	FC_DEVEL_DEBUG("enter");

	int8_t status;

#ifdef DEVEL_DEBUG
	printf("+++ CWMP HANDLE SET NOTIFICATION +++\n");
	printf("Name  : '%s'\n", name);
	printf("Value : '%s'\n", value);
	printf("--- CWMP HANDLE SET NOTIFICATION ---\n");
#endif

	status = external_set_action_write("notification", name, value);

done:
	FC_DEVEL_DEBUG("exit");
	return status;
}

int8_t
cwmp_set_action_execute_handler()
{
	FC_DEVEL_DEBUG("enter");
	int8_t status;

	status = external_set_action_execute();

	FC_DEVEL_DEBUG("exit");
	return status;
}

int8_t
cwmp_get_parameter_handler(char *name, char **value)
{
	FC_DEVEL_DEBUG("enter");

	int8_t status;

	status = external_get_action("value", name, value);

#ifdef DEVEL_DEBUG
	printf("+++ CWMP HANDLE GET PARAMETER +++\n");
	printf("Name  : '%s'\n", name);
	printf("Value : '%s'\n", *value);
	printf("--- CWMP HANDLE GET PARAMETER ---\n");
#endif

	FC_DEVEL_DEBUG("exit");
	return status;
}

int8_t
cwmp_get_notification_handler(char *name, char **value)
{
	FC_DEVEL_DEBUG("enter");

	int8_t status;

	status = external_get_action("notification", name, value);

#ifdef DEVEL_DEBUG
	printf("+++ CWMP HANDLE GET NOTIFICATION +++\n");
	printf("Name  : '%s'\n", name);
	printf("Value : '%s'\n", *value);
	printf("+++ CWMP HANDLE GET NOTIFICATION +++\n");
#endif

	FC_DEVEL_DEBUG("exit");
	return status;
}

int8_t
cwmp_download_handler(char *url, char *size)
{
	FC_DEVEL_DEBUG("enter");

	int8_t status;

	status = external_download(url, size);

#ifdef DEVEL_DEBUG
	printf("+++ CWMP HANDLE DOWNLOAD +++\n");
#endif

	FC_DEVEL_DEBUG("exit");
	return status;
}


int8_t
cwmp_reboot_handler(void)
{
	FC_DEVEL_DEBUG("enter");

	int8_t status;

	status = external_simple("reboot");

#ifdef DEVEL_DEBUG
	printf("+++ CWMP HANDLE REBOOT +++\n");
#endif

	FC_DEVEL_DEBUG("exit");
	return status;
}

int8_t
cwmp_factory_reset_handler(void)
{
	FC_DEVEL_DEBUG("enter");

	int8_t status;

	status = external_simple("factory_reset");

#ifdef DEVEL_DEBUG
	printf("+++ CWMP HANDLE FACTORY RESET +++\n");
#endif

	FC_DEVEL_DEBUG("exit");
	return status;
}

int8_t
cwmp_reload_changes(void)
{
	FC_DEVEL_DEBUG("enter");

	int8_t status;

	if (cwmp.config_reload) {
		status = config_load();
		if (status != FC_SUCCESS)
			goto done;
		cwmp.config_reload = 0;
	}

	status = FC_SUCCESS;

done:
	FC_DEVEL_DEBUG("exit");
	return status;
}

char *
cwmp_get_event_code(void)
{
	FC_DEVEL_DEBUG("enter");

	char *cwmp_inform_event_code;
	switch (cwmp.event_code) {
		case BOOT:
			cwmp_inform_event_code = "1 BOOT";
			break;
		case PERIODIC:
			cwmp_inform_event_code = "2 PERIODIC";
			break;
		case SCHEDULED:
			cwmp_inform_event_code = "3 SCHEDULED";
			break;
		case VALUE_CHANGE:
			cwmp_inform_event_code = "4 VALUE CHANGE";
			break;
		case CONNECTION_REQUEST:
			cwmp_inform_event_code = "6 CONNECTION REQUEST";
			break;
		case BOOTSTRAP:
		default:
			cwmp_inform_event_code = "0 BOOTSTRAP";
			break;
	}

	FC_DEVEL_DEBUG("exit");
	return cwmp_inform_event_code;
}

int
cwmp_get_retry_count(void)
{
	FC_DEVEL_DEBUG("enter & exit");
	return cwmp.retry_count;
}

