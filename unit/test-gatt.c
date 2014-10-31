/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Intel Corporation. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <glib.h>

#include "lib/uuid.h"
#include "src/shared/util.h"
#include "src/shared/att.h"
#include "src/shared/gatt-helpers.h"
#include "src/shared/gatt-client.h"

struct test_pdu {
	bool valid;
	const uint8_t *data;
	size_t size;
};

enum context_type {
	ATT,
	CLIENT,
	SERVER
};

struct test_data {
	char *test_name;
	struct test_pdu *pdu_list;
	enum context_type context_type;
	bt_uuid_t *uuid;
};

struct context {
	GMainLoop *main_loop;
	struct bt_gatt_client *client;
	struct bt_att *att;
	guint source;
	guint process;
	int fd;
	unsigned int pdu_offset;
	const struct test_data *data;
};

#define data(args...) ((const unsigned char[]) { args })

#define raw_pdu(args...)					\
	{							\
		.valid = true,					\
		.data = data(args),				\
		.size = sizeof(data(args)),			\
	}

#define define_test(name, function, type, bt_uuid, args...)		\
	do {								\
		const struct test_pdu pdus[] = {			\
			args, { }					\
		};							\
		static struct test_data data;				\
		data.test_name = g_strdup(name);			\
		data.context_type = type;				\
		data.uuid = bt_uuid;					\
		data.pdu_list = g_malloc(sizeof(pdus));			\
		memcpy(data.pdu_list, pdus, sizeof(pdus));		\
		g_test_add_data_func(name, &data, function);		\
	} while (0)

static bt_uuid_t uuid_16 = {
	.type = BT_UUID16,
	.value.u16 = 0x1800
};

static void test_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	g_print("%s%s\n", prefix, str);
}

static void test_free(gconstpointer user_data)
{
	const struct test_data *data = user_data;

	g_free(data->test_name);
	g_free(data->pdu_list);
}

static gboolean context_quit(gpointer user_data)
{
	struct context *context = user_data;

	if (context->process > 0)
		g_source_remove(context->process);

	g_main_loop_quit(context->main_loop);

	return FALSE;
}

static gboolean send_pdu(gpointer user_data)
{
	struct context *context = user_data;
	const struct test_pdu *pdu;
	ssize_t len;

	pdu = &context->data->pdu_list[context->pdu_offset++];

	len = write(context->fd, pdu->data, pdu->size);

	if (g_test_verbose())
		util_hexdump('<', pdu->data, len, test_debug, "GATT: ");

	g_assert_cmpint(len, ==, pdu->size);

	context->process = 0;
	return FALSE;
}

static void context_process(struct context *context)
{
	if (!context->data->pdu_list[context->pdu_offset].valid) {
		context_quit(context);
		return;
	}

	context->process = g_idle_add(send_pdu, context);
}

static gboolean test_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct context *context = user_data;
	const struct test_pdu *pdu;
	unsigned char buf[512];
	ssize_t len;
	int fd;

	pdu = &context->data->pdu_list[context->pdu_offset++];

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP)) {
		context->source = 0;
		g_print("%s: cond %x\n", __func__, cond);
		return FALSE;
	}

	fd = g_io_channel_unix_get_fd(channel);

	len = read(fd, buf, sizeof(buf));

	g_assert(len > 0);

	if (g_test_verbose())
		util_hexdump('>', buf, len, test_debug, "GATT: ");

	g_assert_cmpint(len, ==, pdu->size);

	g_assert(memcmp(buf, pdu->data, pdu->size) == 0);

	context_process(context);

	return TRUE;
}

static void gatt_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	g_print("%s%s\n", prefix, str);
}

static struct context *create_context(uint16_t mtu, gconstpointer data)
{
	struct context *context = g_new0(struct context, 1);
	const struct test_data *test_data = data;
	GIOChannel *channel;
	int err, sv[2];
	struct bt_att *att;

	context->main_loop = g_main_loop_new(NULL, FALSE);
	g_assert(context->main_loop);

	err = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv);
	g_assert(err == 0);

	att = bt_att_new(sv[0]);
	g_assert(att);

	switch (test_data->context_type) {
	case ATT:
		context->att = att;

		bt_gatt_exchange_mtu(context->att, mtu, NULL, NULL, NULL);
		break;
	case CLIENT:
		context->client = bt_gatt_client_new(att, mtu);
		g_assert(context->client);

		if (g_test_verbose())
			bt_gatt_client_set_debug(context->client, gatt_debug,
								"gatt:", NULL);

		bt_att_unref(att);
		break;
	default:
		break;
	}

	channel = g_io_channel_unix_new(sv[1]);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	context->source = g_io_add_watch(channel,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				test_handler, context);
	g_assert(context->source > 0);

	g_io_channel_unref(channel);

	context->fd = sv[1];
	context->data = data;

	return context;
}

static void primary_cb(bool success, uint8_t att_ecode,
						struct bt_gatt_result *result,
						void *user_data)
{
	struct context *context = user_data;

	g_assert(success);

	context_quit(context);
}

static void destroy_context(struct context *context)
{
	if (context->source > 0)
		g_source_remove(context->source);

	bt_gatt_client_unref(context->client);

	if (context->att)
		bt_att_unref(context->att);

	g_main_loop_unref(context->main_loop);

	test_free(context->data);
	g_free(context);
}

static void execute_context(struct context *context)
{
	g_main_loop_run(context->main_loop);

	destroy_context(context);
}

static void test_client(gconstpointer data)
{
	struct context *context = create_context(512, data);

	execute_context(context);
}

static void test_search_primary(gconstpointer data)
{
	struct context *context = create_context(512, data);
	const struct test_data *test_data = data;

	bt_gatt_discover_all_primary_services(context->att, test_data->uuid,
						primary_cb, context, NULL);

	execute_context(context);
}

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/*
	 * Server Configuration
	 *
	 * The test group objective is to verify Generic Attribute Profile
	 * Server Configuration.
	 */
	define_test("/TP/GAC/CL/BV-01-C", test_client, CLIENT, NULL,
				raw_pdu(0x02, 0x00, 0x02));

	/*
	 * Discovery
	 *
	 * The test group objective is to verify Generic Attribute Profile
	 * Discovery of Services and Service Characteristics.
	 */
	define_test("/TP/GAD/CL/BV-01-C", test_search_primary, ATT, NULL,
			raw_pdu(0x02, 0x00, 0x02),
			raw_pdu(0x03, 0x00, 0x02),
			raw_pdu(0x10, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28),
			raw_pdu(0x11, 0x06, 0x10, 0x00, 0x13, 0x00, 0x00, 0x18,
					0x20, 0x00, 0x29, 0x00, 0xb0, 0x68,
					0x30, 0x00, 0x32, 0x00, 0x19, 0x18),
			raw_pdu(0x10, 0x33, 0x00, 0xff, 0xff, 0x00, 0x28),
			raw_pdu(0x11, 0x14, 0x90, 0x00, 0x96, 0x00, 0xef, 0xcd,
					0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
					0x00, 0x00, 0x00, 0x00, 0x85, 0x60,
					0x00, 0x00),
			raw_pdu(0x10, 0x97, 0x00, 0xff, 0xff, 0x00, 0x28),
			raw_pdu(0x01, 0x10, 0x97, 0x00, 0x0a));

	define_test("/TP/GAD/CL/BV-02-C-1", test_search_primary, ATT, &uuid_16,
			raw_pdu(0x02, 0x00, 0x02),
			raw_pdu(0x03, 0x00, 0x02),
			raw_pdu(0x06, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28, 0x00,
					0x18),
			raw_pdu(0x07, 0x01, 0x00, 0x07, 0x00),
			raw_pdu(0x06, 0x08, 0x00, 0xff, 0xff, 0x00, 0x28, 0x00,
					0x18),
			raw_pdu(0x01, 0x06, 0x08, 0x00, 0x0a));

	return g_test_run();
}
