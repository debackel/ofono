/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014  Philip Paeps. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
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

#include <errno.h>
#include <stdlib.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };

struct ublox_data {
	GAtChat *modem;
	GAtChat *aux;
};

static void ublox_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int ublox_probe(struct ofono_modem *modem)
{
	struct ublox_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct ublox_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void ublox_remove(struct ofono_modem *modem)
{
	struct ublox_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);
	g_at_chat_unref(data->aux);
	g_at_chat_unref(data->modem);
	g_free(data);
}

static GAtChat *open_device(struct ofono_modem *modem,
				const char *key, char *debug)
{
	const char *device;
	GAtSyntax *syntax;
	GIOChannel *channel;
	GAtChat *chat;

	device = ofono_modem_get_string(modem, key);
	if (device == NULL)
		return NULL;

	DBG("%s %s", key, device);

	channel = g_at_tty_open(device, NULL);
	if (channel == NULL)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);

	g_io_channel_unref(channel);

	if (chat == NULL)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, ublox_debug, debug);

	return chat;
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ublox_data * data = ofono_modem_get_data(modem);

	DBG("ok %d", ok);

	if (!ok) {
		g_at_chat_unref(data->aux);
		data->aux = NULL;
		g_at_chat_unref(data->modem);
		data->modem = NULL;
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	ofono_modem_set_powered(modem, TRUE);
}

static int ublox_enable(struct ofono_modem *modem)
{
	struct ublox_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->modem = open_device(modem, "Modem", "Modem: ");
	if (data->modem == NULL)
		return -EINVAL;

	data->aux = open_device(modem, "Aux", "Aux: ");
	if (data->aux == NULL) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;
		return -EIO;
	}

	g_at_chat_set_slave(data->modem, data->aux);

	g_at_chat_send(data->modem, "ATE0 +CMEE=1", none_prefix,
					NULL, NULL, NULL);
	g_at_chat_send(data->aux, "ATE0 +CMEE=1", none_prefix,
					NULL, NULL, NULL);

	g_at_chat_send(data->aux, "AT+CFUN=4", none_prefix,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ublox_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->aux);
	data->aux = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int ublox_disable(struct ofono_modem *modem)
{
	struct ublox_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_cancel_all(data->modem);
	g_at_chat_unregister_all(data->modem);
	g_at_chat_unref(data->modem);
	data->modem = NULL;

	g_at_chat_cancel_all(data->aux);
	g_at_chat_unregister_all(data->aux);

	g_at_chat_send(data->aux, "AT+CFUN=0", none_prefix,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void set_online_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void ublox_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct ublox_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (g_at_chat_send(data->aux, command, none_prefix, set_online_cb,
					cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void ublox_pre_sim(struct ofono_modem *modem)
{
	struct ublox_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->aux);
	sim = ofono_sim_create(modem, OFONO_VENDOR_UBLOX, "atmodem",
					data->aux);

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void ublox_post_sim(struct ofono_modem *modem)
{
	struct ublox_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	gprs = ofono_gprs_create(modem, OFONO_VENDOR_UBLOX, "atmodem",
					data->aux);
	gc = ofono_gprs_context_create(modem, OFONO_VENDOR_UBLOX, "atmodem",
					data->modem);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);
}

static void ublox_post_online(struct ofono_modem *modem)
{
	struct ublox_data *data = ofono_modem_get_data(modem);

	ofono_netreg_create(modem, 0, "atmodem", data->aux);
}

static struct ofono_modem_driver ublox_driver = {
	.name		= "ublox",
	.probe		= ublox_probe,
	.remove		= ublox_remove,
	.enable		= ublox_enable,
	.disable	= ublox_disable,
	.set_online	= ublox_set_online,
	.pre_sim	= ublox_pre_sim,
	.post_sim	= ublox_post_sim,
	.post_online	= ublox_post_online,
};

static int ublox_init(void)
{
	return ofono_modem_driver_register(&ublox_driver);
}

static void ublox_exit(void)
{
	ofono_modem_driver_unregister(&ublox_driver);
}

OFONO_PLUGIN_DEFINE(ublox, "u-blox modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, ublox_init, ublox_exit)
