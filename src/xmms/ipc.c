/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003-2006 XMMS2 Team
 *
 *  PLUGINS ARE NOT CONSIDERED TO BE DERIVED WORK !!!
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#include <glib.h>

#include "xmms/xmms_log.h"
#include "xmmspriv/xmms_ipc.h"
#include "xmmsc/xmmsc_ipc_msg.h"


/**
  * @defgroup IPC IPC
  * @ingroup XMMSServer
  * @brief IPC functions for XMMS2 Daemon
  * @{
  */



/**
 * The IPC object list
 */
typedef struct xmms_ipc_object_pool_t {
	xmms_object_t *objects[XMMS_IPC_OBJECT_END];
	xmms_object_t *signals[XMMS_IPC_SIGNAL_END];
	xmms_object_t *broadcasts[XMMS_IPC_SIGNAL_END];
} xmms_ipc_object_pool_t;


/**
 * The server IPC object
 */
struct xmms_ipc_St {
	xmms_ipc_transport_t *transport;
	GList *clients;
	GIOChannel *chan;
	GMutex *mutex_lock;
	xmms_object_t **objects;
	xmms_object_t **signals;
	xmms_object_t **broadcasts;
};


/**
 * A IPC client representation.
 */
typedef struct xmms_ipc_client_St {
	GThread *thread;

	GMainLoop *ml;
	GIOChannel *iochan;

	xmms_ipc_transport_t *transport;
	xmms_ipc_msg_t *read_msg;
	xmms_ipc_t *ipc;

	/* this lock protects out_msg, pendingsignals and broadcasts,
	   which can be accessed from other threads than the
	   client-thread */
	GMutex *lock;

	/** Messages waiting to be written */
	GQueue *out_msg;

	guint pendingsignals[XMMS_IPC_SIGNAL_END];
	GList *broadcasts[XMMS_IPC_SIGNAL_END];
} xmms_ipc_client_t;

static GMutex *ipc_servers_lock;
static GList *ipc_servers = NULL;

static GMutex *ipc_object_pool_lock;
static struct xmms_ipc_object_pool_t *ipc_object_pool = NULL;

static void xmms_ipc_client_destroy (xmms_ipc_client_t *client);

static gboolean xmms_ipc_client_msg_write (xmms_ipc_client_t *client, xmms_ipc_msg_t *msg);
static void xmms_ipc_handle_cmd_value (xmms_ipc_msg_t *msg, xmms_object_cmd_value_t *val);

static gboolean
type_and_msg_to_arg (xmms_object_cmd_arg_type_t type, xmms_ipc_msg_t *msg, xmms_object_cmd_arg_t *arg, gint i)
{
	guint len;
	guint size, k;

	switch (type) {
		case XMMS_OBJECT_CMD_ARG_NONE:
			break;
		case XMMS_OBJECT_CMD_ARG_UINT32 :
			if (!xmms_ipc_msg_get_uint32 (msg, &arg->values[i].value.uint32))
				return FALSE;
			break;
		case XMMS_OBJECT_CMD_ARG_INT32 :
			if (!xmms_ipc_msg_get_int32 (msg, &arg->values[i].value.int32))
				return FALSE;
			break;
		case XMMS_OBJECT_CMD_ARG_STRING :
			if (!xmms_ipc_msg_get_string_alloc (msg, &arg->values[i].value.string, &len)) {
				return FALSE;
			}
			break;
		case XMMS_OBJECT_CMD_ARG_STRINGLIST :
			if (!xmms_ipc_msg_get_uint32 (msg, &size)) {
				return FALSE;
			}
			for (k = 0; k < size; k++) {
				gchar *buf;
				if (!xmms_ipc_msg_get_string_alloc (msg, &buf, &len) ||
					!(arg->values[i].value.list = g_list_prepend (arg->values[i].value.list, buf))) {
					GList * list = arg->values[i].value.list;
					while (list) {g_free(list->data); list=g_list_remove(list, list); }
					return FALSE;
				}
			}
			arg->values[i].value.list = g_list_reverse (arg->values[i].value.list);
			break;
		case XMMS_OBJECT_CMD_ARG_COLL :
			if (!xmms_ipc_msg_get_collection_alloc (msg, &arg->values[i].value.coll)) {
				return FALSE;
			}
			break;
		case XMMS_OBJECT_CMD_ARG_BIN :
			{
				GString *bin = g_string_new (NULL);
				if (!xmms_ipc_msg_get_bin_alloc (msg, (unsigned char **)&bin->str, (uint32_t *)&bin->len)) {
					return FALSE;
				}
				arg->values[i].value.bin = bin;
			}
			break;
		default:
			XMMS_DBG ("Unknown value for a caller argument?");
			return FALSE;
			break;
	}
	arg->values[i].type = type;
	return TRUE;
}


static void
count_hash (gpointer key, gpointer value, gpointer udata)
{
	gint *i = (gint*)udata;

	if (key && value)
		(*i)++;
}

static void
hash_to_dict (gpointer key, gpointer value, gpointer udata)
{
	gchar *k = key;
	xmms_object_cmd_value_t *v = value;
	xmms_ipc_msg_t *msg = udata;

	if (k && v) {
		xmms_ipc_msg_put_string (msg, k);
		xmms_ipc_handle_cmd_value (msg, v);
	}
	
}

static void
xmms_ipc_do_dict (xmms_ipc_msg_t *msg, GHashTable *table)
{
	gint i = 0;

	g_hash_table_foreach (table, count_hash, &i);

	xmms_ipc_msg_put_uint32 (msg, i);
	g_hash_table_foreach (table, hash_to_dict, msg);
}

static void
xmms_ipc_handle_cmd_value (xmms_ipc_msg_t *msg, xmms_object_cmd_value_t *val)
{
	GList *n;

	xmms_ipc_msg_put_int32 (msg, val->type);

	switch (val->type) {
		case XMMS_OBJECT_CMD_ARG_BIN:
			xmms_ipc_msg_put_bin (msg, (guchar *)val->value.bin->str, val->value.bin->len);
			break;
		case XMMS_OBJECT_CMD_ARG_STRING:
			xmms_ipc_msg_put_string (msg, val->value.string);
			break;
		case XMMS_OBJECT_CMD_ARG_UINT32:
			xmms_ipc_msg_put_uint32 (msg, val->value.uint32);
			break;
		case XMMS_OBJECT_CMD_ARG_INT32:
			xmms_ipc_msg_put_int32 (msg, val->value.int32);
			break;
		case XMMS_OBJECT_CMD_ARG_LIST:
		case XMMS_OBJECT_CMD_ARG_PROPDICT:
			xmms_ipc_msg_put_uint32 (msg, g_list_length (val->value.list));

			for (n = val->value.list; n; n = g_list_next (n)) {
				xmms_object_cmd_value_t *lval = n->data;
				xmms_ipc_handle_cmd_value (msg, lval);
			}
			break;
		case XMMS_OBJECT_CMD_ARG_DICT:
			xmms_ipc_do_dict (msg, val->value.dict);
			break;
		case XMMS_OBJECT_CMD_ARG_COLL :
			xmms_ipc_msg_put_collection (msg, val->value.coll);
			break;
		case XMMS_OBJECT_CMD_ARG_NONE:
			break;
		default:
			xmms_log_error ("Unknown returnvalue: %d, couldn't serialize message", val->type);
			break;
	}
}

static void
process_msg (xmms_ipc_client_t *client, xmms_ipc_t *ipc, xmms_ipc_msg_t *msg)
{
	xmms_object_t *object;
	xmms_object_cmd_desc_t *cmd;
	xmms_object_cmd_arg_t arg;
	xmms_ipc_msg_t *retmsg;
	uint32_t objid, cmdid;
	gint i;

	g_return_if_fail (ipc);
	g_return_if_fail (msg);

	objid = xmms_ipc_msg_get_object (msg);
	cmdid = xmms_ipc_msg_get_cmd (msg);

	if (objid == XMMS_IPC_OBJECT_SIGNAL &&
	    cmdid == XMMS_IPC_CMD_SIGNAL) {
		guint signalid;

		if (!xmms_ipc_msg_get_uint32 (msg, &signalid)) {
			xmms_log_error ("No signalid in this msg?!");
			return;
		}

		if (signalid >= XMMS_IPC_SIGNAL_END) {
			xmms_log_error ("Bad signal id (%d)", signalid);
			return;
		}

		g_mutex_lock (client->lock);
		client->pendingsignals[signalid] = xmms_ipc_msg_get_cookie (msg);
		g_mutex_unlock (client->lock);
		return;
	} else if (objid == XMMS_IPC_OBJECT_SIGNAL &&
	           cmdid == XMMS_IPC_CMD_BROADCAST) {
		guint broadcastid;

		if (!xmms_ipc_msg_get_uint32 (msg, &broadcastid)) {
			xmms_log_error ("No broadcastid in this msg?!");
			return;
		}

		if (broadcastid >= XMMS_IPC_SIGNAL_END) {
			xmms_log_error ("Bad broadcast id (%d)", broadcastid);
			return;
		}

		g_mutex_lock (client->lock);
		client->broadcasts[broadcastid] =
			g_list_append (client->broadcasts[broadcastid],
			               GUINT_TO_POINTER (xmms_ipc_msg_get_cookie (msg)));

		g_mutex_unlock (client->lock);
		return;
	}

	if (objid >= XMMS_IPC_OBJECT_END) {
		xmms_log_error ("Bad object id (%d)", objid);
		return;
	}

	g_mutex_lock (ipc_object_pool_lock);
	object = ipc_object_pool->objects[objid];
	g_mutex_unlock (ipc_object_pool_lock);
	if (!object) {
		xmms_log_error ("Object %d was not found!", objid);
		return;
	}

	if (cmdid >= XMMS_IPC_CMD_END) {
		xmms_log_error ("Bad command id (%d)", cmdid);
		return;
	}

	cmd = object->cmds[cmdid];
	if (!cmd) {
		xmms_log_error ("No such cmd %d on object %d", cmdid, objid);
		return;
	}

	xmms_object_cmd_arg_init (&arg);

	for (i = 0; i < XMMS_OBJECT_CMD_MAX_ARGS; i++) {
		if (!type_and_msg_to_arg (cmd->args[i], msg, &arg, i)) {
			xmms_log_error ("Error parsing args");
			retmsg = xmms_ipc_msg_new (objid, XMMS_IPC_CMD_ERROR);
			xmms_ipc_msg_put_string (retmsg, "Corrupt msg");
			goto err;
		}
			
	}

	xmms_object_cmd_call (object, cmdid, &arg);
	if (xmms_error_isok (&arg.error)) {
		retmsg = xmms_ipc_msg_new (objid, XMMS_IPC_CMD_REPLY);
		xmms_ipc_handle_cmd_value (retmsg, arg.retval);
	} else {
		retmsg = xmms_ipc_msg_new (objid, XMMS_IPC_CMD_ERROR);
		xmms_ipc_msg_put_string (retmsg, xmms_error_message_get (&arg.error));
	}

	if (arg.retval)
		xmms_object_cmd_value_free (arg.retval);

err:
	for (i = 0; i < XMMS_OBJECT_CMD_MAX_ARGS; i++) {
		if (arg.values[i].type == XMMS_OBJECT_CMD_ARG_STRING) {
			g_free (arg.values[i].value.string);
		} else if (arg.values[i].type == XMMS_OBJECT_CMD_ARG_STRINGLIST) {
			GList * list = arg.values[i].value.list;
			while (list) {g_free(list->data); list=g_list_delete_link(list, list); }
		} else if (arg.values[i].type == XMMS_OBJECT_CMD_ARG_COLL) {
			xmmsc_coll_unref (arg.values[i].value.coll);
		} else if (arg.values[i].type == XMMS_OBJECT_CMD_ARG_BIN) {
			g_string_free (arg.values[i].value.bin, TRUE);
		}
	}
	xmms_ipc_msg_set_cookie (retmsg, xmms_ipc_msg_get_cookie (msg));
	g_mutex_lock (client->lock);
	xmms_ipc_client_msg_write (client, retmsg);
	g_mutex_unlock (client->lock);
}


static gboolean
xmms_ipc_client_read_cb (GIOChannel *iochan,
                         GIOCondition cond,
                         gpointer data)
{
	xmms_ipc_client_t *client = data;
	bool disconnect = FALSE;

	g_return_val_if_fail (client, FALSE);

	if (!(cond & G_IO_IN)) {
		xmms_log_error ("Client got error/hup, maybe connection died?");
		g_main_loop_quit (client->ml);
		return FALSE;
	} else {
		while (TRUE) {
			if (!client->read_msg) {
				client->read_msg = xmms_ipc_msg_alloc ();
			}

			if (xmms_ipc_msg_read_transport (client->read_msg, client->transport, &disconnect)) {
				xmms_ipc_msg_t *msg = client->read_msg;
				client->read_msg = NULL;
				process_msg (client, client->ipc, msg);
				xmms_ipc_msg_destroy (msg);
			} else {
				break;
			}
		}
	}

	if (disconnect) {
		if (client->read_msg) {
			xmms_ipc_msg_destroy (client->read_msg);
			client->read_msg = NULL;
		}
		XMMS_DBG ("disconnect was true!");
		g_main_loop_quit (client->ml);
		return FALSE;
	}

	return TRUE;
}

static gboolean
xmms_ipc_client_write_cb (GIOChannel *iochan,
                          GIOCondition cond,
                          gpointer data)
{
	xmms_ipc_client_t *client = data;
	bool disconnect = FALSE;

	g_return_val_if_fail (client, FALSE);

	while (TRUE) {
		xmms_ipc_msg_t *msg;

		g_mutex_lock (client->lock);
		msg = g_queue_peek_head (client->out_msg);
		g_mutex_unlock (client->lock);

		if (!msg)
			break;

		if (!xmms_ipc_msg_write_transport (msg,
		                                   client->transport,
		                                   &disconnect)) {
			if (disconnect) {
				break;
			} else {
				/* try sending again later */
				return TRUE;
			}
		}

		g_mutex_lock (client->lock);
		g_queue_pop_head (client->out_msg);
		g_mutex_unlock (client->lock);

		xmms_ipc_msg_destroy (msg);
	}

	return FALSE;
}

static gpointer
xmms_ipc_client_thread (gpointer data)
{
	xmms_ipc_client_t *client = data;
	GSource *source;

	source = g_io_create_watch (client->iochan, G_IO_IN | G_IO_ERR | G_IO_HUP);
	g_source_set_callback (source,
	                       (GSourceFunc) xmms_ipc_client_read_cb,
	                       (gpointer) client,
	                       NULL);
	g_source_attach (source, g_main_loop_get_context (client->ml));
	g_source_unref (source);

	g_main_loop_run (client->ml);

	xmms_ipc_client_destroy (client);

	return NULL;
}

static xmms_ipc_client_t *
xmms_ipc_client_new (xmms_ipc_t *ipc, xmms_ipc_transport_t *transport)
{
	xmms_ipc_client_t *client;
	GMainContext *context;
	int fd;

	g_return_val_if_fail (transport, NULL);

	client = g_new0 (xmms_ipc_client_t, 1);

	context = g_main_context_new ();
	client->ml = g_main_loop_new (context, FALSE);
	g_main_context_unref (context);
	
	fd = xmms_ipc_transport_fd_get (transport);
	client->iochan = g_io_channel_unix_new (fd);
	g_return_val_if_fail (client->iochan, NULL);

	client->transport = transport;
	client->ipc = ipc;
	client->out_msg = g_queue_new ();
	client->lock = g_mutex_new ();
	client->thread = g_thread_create (xmms_ipc_client_thread, client, FALSE, NULL);
	
	return client;
}

static void
xmms_ipc_client_destroy (xmms_ipc_client_t *client)
{
	guint i;

	XMMS_DBG ("Destroying client!");

	g_main_loop_unref (client->ml);
	g_io_channel_unref (client->iochan);

	if(client->ipc) {
		g_mutex_lock (client->ipc->mutex_lock);
		client->ipc->clients = g_list_remove (client->ipc->clients, client);
		g_mutex_unlock (client->ipc->mutex_lock);
	}

	xmms_ipc_transport_destroy (client->transport);

	g_mutex_lock (client->lock);
	while (!g_queue_is_empty (client->out_msg)) {
		xmms_ipc_msg_t *msg = g_queue_pop_head (client->out_msg);
		xmms_ipc_msg_destroy (msg);
	}

	g_queue_free (client->out_msg);

	for (i = 0; i < XMMS_IPC_SIGNAL_END; i++) {
		g_list_free (client->broadcasts[i]);
	}

	g_mutex_unlock (client->lock);
	g_mutex_free (client->lock);
	g_free (client);
}

/**
 * Gets called when the config property "core.ipcsocket" has changed.
 */
void
on_config_ipcsocket_change (xmms_object_t *object, gconstpointer data, gpointer udata)
{
	xmms_ipc_shutdown();
	XMMS_DBG("Shuttind down ipc server threads through config property \"core.ipcsocket\" change.");
	xmms_ipc_setup_server((gchar *)data);
}

/**
 * Put a message in the queue awaiting to be sent to the client.
 * Should hold client->lock.
 */
static gboolean
xmms_ipc_client_msg_write (xmms_ipc_client_t *client, xmms_ipc_msg_t *msg)
{
	gboolean queue_empty;

	g_return_val_if_fail (client, FALSE);
	g_return_val_if_fail (msg, FALSE);

	queue_empty = g_queue_is_empty (client->out_msg);
	g_queue_push_tail (client->out_msg, msg);

	/* If there's no write in progress, add a new callback */
	if (queue_empty) {
		GMainContext *context = g_main_loop_get_context (client->ml);
		GSource *source = g_io_create_watch (client->iochan, G_IO_OUT);

		g_source_set_callback (source,
		                       (GSourceFunc) xmms_ipc_client_write_cb,
		                       (gpointer) client,
		                       NULL);
		g_source_attach (source, context);
		g_source_unref (source);

		g_main_context_wakeup (context);
	}

	return TRUE;
}

static gboolean
xmms_ipc_source_accept (GIOChannel *chan, GIOCondition cond, gpointer data)
{
	xmms_ipc_t *ipc = (xmms_ipc_t *) data;
	xmms_ipc_transport_t *transport;
	xmms_ipc_client_t *client;

	if (!(cond & G_IO_IN)) {
		xmms_log_error ("IPC listener got error/hup");
		return FALSE;
	}

	XMMS_DBG ("Client connected");
	transport = xmms_ipc_server_accept (ipc->transport);
	if (!transport) {
		xmms_log_error ("accept returned null!");
		return TRUE;
	}

	client = xmms_ipc_client_new (ipc, transport);
	if (!client) {
		xmms_ipc_transport_destroy (transport);
		return TRUE;
	}

	g_mutex_lock (ipc->mutex_lock);
	ipc->clients = g_list_append (ipc->clients, client);
	g_mutex_unlock (ipc->mutex_lock);

	return TRUE;
}

/**
 * Enable IPC
 */
gboolean
xmms_ipc_setup_server_internaly (xmms_ipc_t *ipc)
{
	g_mutex_lock (ipc->mutex_lock);
	ipc->chan = g_io_channel_unix_new (xmms_ipc_transport_fd_get (ipc->transport));
	g_io_add_watch (ipc->chan, G_IO_IN | G_IO_HUP | G_IO_ERR,
	                xmms_ipc_source_accept, ipc);
	g_mutex_unlock (ipc->mutex_lock);
	return TRUE;
}

/**
 * Checks if someone is waiting for #signalid
 */
gboolean
xmms_ipc_has_pending (guint signalid)
{
	GList *c, *s;
	xmms_ipc_t *ipc;

	g_mutex_lock (ipc_servers_lock);

	for(s = ipc_servers; s; s = g_list_next (s))
	{
		ipc = s->data;
		g_mutex_lock (ipc->mutex_lock);
		for (c = ipc->clients; c; c = g_list_next (c)) {
			xmms_ipc_client_t *cli = c->data;
			g_mutex_lock (cli->lock);
			if (cli->pendingsignals[signalid]) {
				g_mutex_unlock (cli->lock);
				g_mutex_unlock (ipc->mutex_lock);
				g_mutex_unlock (ipc_servers_lock);
				return TRUE;
			}
			g_mutex_unlock (cli->lock);
		}
		g_mutex_unlock (ipc->mutex_lock);
	}

	g_mutex_unlock (ipc_servers_lock);
	return FALSE;
}

static void
xmms_ipc_signal_cb (xmms_object_t *object, gconstpointer arg, gpointer userdata)
{
	GList *c, *s;
	guint signalid = GPOINTER_TO_UINT (userdata);
	xmms_ipc_t *ipc;
	xmms_ipc_msg_t *msg;

	g_mutex_lock (ipc_servers_lock);

	for(s = ipc_servers; s && s->data; s = g_list_next (s))
	{
		ipc = s->data;
		g_mutex_lock (ipc->mutex_lock);
		for (c = ipc->clients; c; c = g_list_next (c)) {
			xmms_ipc_client_t *cli = c->data;
			g_mutex_lock (cli->lock);
			if (cli->pendingsignals[signalid]) {
				msg = xmms_ipc_msg_new (XMMS_IPC_OBJECT_SIGNAL, XMMS_IPC_CMD_SIGNAL);
				xmms_ipc_msg_set_cookie (msg, cli->pendingsignals[signalid]);
				xmms_ipc_handle_cmd_value (msg, ((xmms_object_cmd_arg_t*)arg)->retval);
				xmms_ipc_client_msg_write (cli, msg);
				cli->pendingsignals[signalid] = 0;
			}
			g_mutex_unlock (cli->lock);
		}
		g_mutex_unlock (ipc->mutex_lock);
	}

	g_mutex_unlock (ipc_servers_lock);

}

static void
xmms_ipc_broadcast_cb (xmms_object_t *object, gconstpointer arg, gpointer userdata)
{
	GList *c, *s;
	guint broadcastid = GPOINTER_TO_UINT (userdata);
	xmms_ipc_t *ipc;
	xmms_ipc_msg_t *msg = NULL;
	GList *l;

	g_mutex_lock (ipc_servers_lock);

	for(s = ipc_servers; s && s->data; s = g_list_next (s))
	{
		ipc = s->data;
		g_mutex_lock (ipc->mutex_lock);
		for (c = ipc->clients; c; c = g_list_next (c)) {
			xmms_ipc_client_t *cli = c->data;

			g_mutex_lock (cli->lock);
			for (l = cli->broadcasts[broadcastid]; l; l = g_list_next (l)) {
				msg = xmms_ipc_msg_new (XMMS_IPC_OBJECT_SIGNAL, XMMS_IPC_CMD_BROADCAST);
				xmms_ipc_msg_set_cookie (msg, GPOINTER_TO_UINT (l->data));
				xmms_ipc_handle_cmd_value (msg, ((xmms_object_cmd_arg_t*)arg)->retval);
				xmms_ipc_client_msg_write (cli, msg);
			}
			g_mutex_unlock (cli->lock);
		}
		g_mutex_unlock (ipc->mutex_lock);
	}
	g_mutex_unlock (ipc_servers_lock);
}

/**
 * Register a broadcast signal.
 */
void
xmms_ipc_broadcast_register (xmms_object_t *object, xmms_ipc_signals_t signalid)
{
	g_return_if_fail (object);
	g_mutex_lock (ipc_object_pool_lock);
	
	ipc_object_pool->broadcasts[signalid] = object;
	xmms_object_connect (object, signalid, xmms_ipc_broadcast_cb, GUINT_TO_POINTER (signalid));

	g_mutex_unlock (ipc_object_pool_lock);
}

/**
 * Unregister a broadcast signal.
 */
void
xmms_ipc_broadcast_unregister (xmms_ipc_signals_t signalid)
{
	xmms_object_t *obj;

	g_mutex_lock (ipc_object_pool_lock);
	obj = ipc_object_pool->broadcasts[signalid];
	if (obj) {
		xmms_object_disconnect (obj, signalid, xmms_ipc_broadcast_cb);
		ipc_object_pool->broadcasts[signalid] = NULL;
	}
	g_mutex_unlock (ipc_object_pool_lock);
}

/**
 * Register a signal
 */
void
xmms_ipc_signal_register (xmms_object_t *object, xmms_ipc_signals_t signalid)
{
	g_return_if_fail (object);

	g_mutex_lock (ipc_object_pool_lock);
	ipc_object_pool->signals[signalid] = object;
	xmms_object_connect (object, signalid, xmms_ipc_signal_cb, GUINT_TO_POINTER (signalid));
	g_mutex_unlock (ipc_object_pool_lock);
}

/**
 * Unregister a signal
 */
void
xmms_ipc_signal_unregister (xmms_ipc_signals_t signalid)
{
	xmms_object_t *obj;

	g_mutex_lock (ipc_object_pool_lock);
	obj = ipc_object_pool->signals[signalid];
	if (obj) {
		xmms_object_disconnect (obj, signalid, xmms_ipc_signal_cb);
		ipc_object_pool->signals[signalid] = NULL;
	}
	g_mutex_unlock (ipc_object_pool_lock);
}

/**
 * Register a object to the IPC core. This needs to be done if you
 * want to send commands to that object from the client.
 */
void
xmms_ipc_object_register (xmms_ipc_objects_t objectid, xmms_object_t *object)
{
	g_mutex_lock (ipc_object_pool_lock);
	ipc_object_pool->objects[objectid] = object;
	g_mutex_unlock (ipc_object_pool_lock);
}

/**
 * Remove a object from the IPC core.
 */
void
xmms_ipc_object_unregister (xmms_ipc_objects_t objectid)
{
	g_mutex_lock (ipc_object_pool_lock);
	ipc_object_pool->objects[objectid] = NULL;
	g_mutex_unlock (ipc_object_pool_lock);
}

/**
 * Initialize IPC
 */
xmms_ipc_t *
xmms_ipc_init (void)
{
	ipc_servers_lock = g_mutex_new ();
	ipc_object_pool_lock = g_mutex_new ();
	ipc_object_pool = g_new0 (xmms_ipc_object_pool_t, 1);
	return NULL;
}

/**
 * Shutdown a IPC Server
 */
void
xmms_ipc_shutdown_server(xmms_ipc_t *ipc)
{
	GList *c;
	xmms_ipc_client_t *co;
	if(!ipc) return;
	
	g_mutex_lock (ipc->mutex_lock);
	g_io_channel_unref (ipc->chan);
	xmms_ipc_transport_destroy (ipc->transport);
	
	for(c = ipc->clients; c; c = g_list_next(c)) {
		co = c->data;
		if(!co) continue;
		co->ipc = NULL;
	}
	
	g_list_free(ipc->clients);
	g_mutex_unlock (ipc->mutex_lock);
	g_mutex_free(ipc->mutex_lock);
	
	g_free(ipc);

}


/**
 * Disable IPC
 */
void
xmms_ipc_shutdown (void)
{
	GList *s = ipc_servers;
	xmms_ipc_t *ipc;
	
	g_mutex_lock (ipc_servers_lock);
	while(s)
	{
		ipc = s->data;
		s = g_list_next(s);
		ipc_servers = g_list_remove(ipc_servers, ipc);
		xmms_ipc_shutdown_server (ipc);
	}
	g_mutex_unlock (ipc_servers_lock);
	
}

/**
 * Start the server
 */
gboolean
xmms_ipc_setup_server (const gchar *path)
{
	xmms_ipc_transport_t *transport;
	xmms_ipc_t *ipc;
	gchar **split;
	gint i = 0, num_init = 0;
	g_return_val_if_fail (path, FALSE);

	split = g_strsplit (path, ";", 0);
	
	for(i = 0; split && split[i]; i++) {
		ipc = g_new0 (xmms_ipc_t, 1);
		if(!ipc) {
			XMMS_DBG ("No IPC server initialized.");
			continue;
		}

		transport = xmms_ipc_server_init (split[i]);
		if (!transport) {
			g_free (ipc);
			xmms_log_error ("Couldn't setup IPC listening on '%s'.", split[i]);
			continue;
		}


		ipc->mutex_lock = g_mutex_new ();
		ipc->transport = transport;
		ipc->signals = ipc_object_pool->signals;
		ipc->broadcasts = ipc_object_pool->broadcasts;
		ipc->objects = ipc_object_pool->objects;

		xmms_ipc_setup_server_internaly (ipc);
		xmms_log_error ("IPC listening on '%s'.", split[i]);

		g_mutex_lock (ipc_servers_lock);
		ipc_servers = g_list_prepend (ipc_servers, ipc);
		g_mutex_unlock (ipc_servers_lock);

		num_init++;
	}

	g_strfreev (split);


	/* If there is less than one socket, there is sth. wrong. */
	if (num_init < 1)
		return FALSE;

	XMMS_DBG ("IPC setup done.");
	return TRUE;
}

/** @} */

