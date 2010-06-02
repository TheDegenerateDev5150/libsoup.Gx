/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-session-sync.c
 *
 * Copyright (C) 2000-2003, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define LIBSOUP_I_HAVE_READ_BUG_594377_AND_KNOW_SOUP_PASSWORD_MANAGER_MIGHT_GO_AWAY

#include "soup-address.h"
#include "soup-session-sync.h"
#include "soup-session-private.h"
#include "soup-address.h"
#include "soup-message-private.h"
#include "soup-message-queue.h"
#include "soup-misc.h"
#include "soup-password-manager.h"
#include "soup-proxy-uri-resolver.h"
#include "soup-uri.h"

/**
 * SECTION:soup-session-sync
 * @short_description: Soup session for blocking I/O in multithreaded
 * programs.
 *
 * #SoupSessionSync is an implementation of #SoupSession that uses
 * synchronous I/O, intended for use in multi-threaded programs.
 *
 * You can use #SoupSessionSync from multiple threads concurrently.
 * Eg, you can send a #SoupMessage in one thread, and then while
 * waiting for the response, send another #SoupMessage from another
 * thread. You can also send a message from one thread and then call
 * soup_session_cancel_message() on it from any other thread (although
 * you need to be careful to avoid race conditions, where the message
 * finishes and is then unreffed by the sending thread just before you
 * cancel it).
 *
 * However, the majority of other types and methods in libsoup are not
 * MT-safe. In particular, you <emphasis>cannot</emphasis> modify or
 * examine a #SoupMessage while it is being transmitted by
 * #SoupSessionSync in another thread. Once a message has been handed
 * off to #SoupSessionSync, it can only be manipulated from its signal
 * handler callbacks, until I/O is complete.
 **/

typedef struct {
	GMutex *lock;
	GCond *cond;
} SoupSessionSyncPrivate;
#define SOUP_SESSION_SYNC_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SOUP_TYPE_SESSION_SYNC, SoupSessionSyncPrivate))

static void  queue_message  (SoupSession *session, SoupMessage *msg,
			     SoupSessionCallback callback, gpointer user_data);
static guint send_message   (SoupSession *session, SoupMessage *msg);
static void  cancel_message (SoupSession *session, SoupMessage *msg,
			     guint status_code);
static void  auth_required  (SoupSession *session, SoupMessage *msg,
			     SoupAuth *auth, gboolean retrying);

G_DEFINE_TYPE (SoupSessionSync, soup_session_sync, SOUP_TYPE_SESSION)

static void
soup_session_sync_init (SoupSessionSync *ss)
{
	SoupSessionSyncPrivate *priv = SOUP_SESSION_SYNC_GET_PRIVATE (ss);

	priv->lock = g_mutex_new ();
	priv->cond = g_cond_new ();
}

static void
finalize (GObject *object)
{
	SoupSessionSyncPrivate *priv = SOUP_SESSION_SYNC_GET_PRIVATE (object);

	g_mutex_free (priv->lock);
	g_cond_free (priv->cond);

	G_OBJECT_CLASS (soup_session_sync_parent_class)->finalize (object);
}

static void
soup_session_sync_class_init (SoupSessionSyncClass *session_sync_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (session_sync_class);
	SoupSessionClass *session_class = SOUP_SESSION_CLASS (session_sync_class);

	g_type_class_add_private (session_sync_class, sizeof (SoupSessionSyncPrivate));

	/* virtual method override */
	session_class->queue_message = queue_message;
	session_class->send_message = send_message;
	session_class->cancel_message = cancel_message;
	session_class->auth_required = auth_required;
	object_class->finalize = finalize;
}


/**
 * soup_session_sync_new:
 *
 * Creates an synchronous #SoupSession with the default options.
 *
 * Return value: the new session.
 **/
SoupSession *
soup_session_sync_new (void)
{
	return g_object_new (SOUP_TYPE_SESSION_SYNC, NULL);
}

/**
 * soup_session_sync_new_with_options:
 * @optname1: name of first property to set
 * @...: value of @optname1, followed by additional property/value pairs
 *
 * Creates an synchronous #SoupSession with the specified options.
 *
 * Return value: the new session.
 **/
SoupSession *
soup_session_sync_new_with_options (const char *optname1, ...)
{
	SoupSession *session;
	va_list ap;

	va_start (ap, optname1);
	session = (SoupSession *)g_object_new_valist (SOUP_TYPE_SESSION_SYNC,
						      optname1, ap);
	va_end (ap);

	return session;
}

static guint
tunnel_connect (SoupSession *session, SoupConnection *conn)
{
	SoupMessageQueueItem *item;
	guint status;

	g_object_ref (conn);

	item = soup_session_make_connect_message (session, conn);
	do {
		soup_session_send_queue_item (session, item, NULL);
		if (item->state == SOUP_MESSAGE_RESTARTING) {
			item->state = SOUP_MESSAGE_STARTING;
			soup_message_restarted (item->msg);
		} else {
			item->state = SOUP_MESSAGE_FINISHED;
			soup_message_finished (item->msg);
		}
	} while (item->state == SOUP_MESSAGE_STARTING &&
		 soup_connection_get_state (conn) != SOUP_CONNECTION_DISCONNECTED);

	/* If the message was requeued but its connection was closed,
	 * return TRY_AGAIN to our caller.
	 */
	if (item->state == SOUP_MESSAGE_STARTING)
		status = SOUP_STATUS_TRY_AGAIN;
	else
		status = item->msg->status_code;
	soup_message_queue_item_unref (item);

	if (SOUP_STATUS_IS_SUCCESSFUL (status)) {
		if (!soup_connection_start_ssl (conn))
			status = SOUP_STATUS_SSL_FAILED;
	}

	if (!SOUP_STATUS_IS_SUCCESSFUL (status))
		soup_connection_disconnect (conn);

	g_object_unref (conn);
	return status;
}

static gboolean
wait_for_connection (SoupMessageQueueItem *item)
{
	SoupSession *session = item->session;
	SoupMessage *msg = item->msg;
	SoupSessionSyncPrivate *priv = SOUP_SESSION_SYNC_GET_PRIVATE (session);
	gboolean try_pruning = FALSE;
	SoupProxyURIResolver *proxy_resolver;
	guint status;

	proxy_resolver = (SoupProxyURIResolver *)soup_session_get_feature_for_message (session, SOUP_TYPE_PROXY_URI_RESOLVER, msg);
	if (proxy_resolver && item->state == SOUP_MESSAGE_STARTING) {
		item->state = SOUP_MESSAGE_RESOLVING_PROXY_URI;
		status = soup_proxy_uri_resolver_get_proxy_uri_sync (
			proxy_resolver, soup_message_get_uri (msg),
			item->cancellable, &item->proxy_uri);
		if (!SOUP_STATUS_IS_SUCCESSFUL (status)) {
			if (status != SOUP_STATUS_CANCELLED)
				soup_session_cancel_message (session, msg, status);
			return FALSE;
		}
		item->state = SOUP_MESSAGE_RESOLVED_PROXY_URI;

		if (item->proxy_uri) {
			item->state = SOUP_MESSAGE_RESOLVING_PROXY_ADDRESS;
			item->proxy_addr = soup_address_new (
				item->proxy_uri->host, item->proxy_uri->port);
			status = soup_address_resolve_sync (item->proxy_addr,
							    item->cancellable);
			if (!SOUP_STATUS_IS_SUCCESSFUL (status)) {
				if (status != SOUP_STATUS_CANCELLED)
					soup_session_cancel_message (session, msg, status);
				return FALSE;
			}
		}
	}

	g_mutex_lock (priv->lock);

	soup_session_cleanup_connections (session, FALSE);

 try_again:
	item->state = SOUP_MESSAGE_AWAITING_CONNECTION;
	if (soup_session_get_connection (session, item, &try_pruning)) {
		if (soup_connection_get_state (item->conn) == SOUP_CONNECTION_NEW) {
			item->state = SOUP_MESSAGE_CONNECTING;
			status = soup_connection_connect_sync (item->conn, item->cancellable);

			if (!SOUP_STATUS_IS_SUCCESSFUL (status)) {
				soup_connection_disconnect (item->conn);
				g_object_unref (item->conn);
				item->conn = NULL;
			} else if (item->state == SOUP_MESSAGE_FINISHED) {
				/* Message was cancelled while we were
				 * connecting.
				 */
				soup_connection_disconnect (item->conn);
				g_object_unref (item->conn);
				item->conn = NULL;
			} else if (soup_connection_get_tunnel_addr (item->conn)) {
				item->state = SOUP_MESSAGE_TUNNELING;
				status = tunnel_connect (session, item->conn);
				if (SOUP_STATUS_IS_SUCCESSFUL (status))
					item->state = SOUP_MESSAGE_READY;
				else if (status == SOUP_STATUS_TRY_AGAIN) {
					soup_connection_disconnect (item->conn);
					g_object_unref (item->conn);
					item->conn = NULL;
					goto try_again;
				} else {
					soup_message_set_status (item->msg, status);
					item->state = SOUP_MESSAGE_FINISHED;
				}
                        } else
				item->state = SOUP_MESSAGE_READY;
		} else
			item->state = SOUP_MESSAGE_READY;

		g_mutex_unlock (priv->lock);
		return item->conn != NULL;
	}

	if (try_pruning) {
		try_pruning = FALSE;
		if (soup_session_cleanup_connections (session, TRUE))
			goto try_again;
	}

	/* Wait... */
	g_cond_wait (priv->cond, priv->lock);

	/* See if something bad happened */
	if (item->state == SOUP_MESSAGE_FINISHED) {
		g_mutex_unlock (priv->lock);
		return FALSE;
	}

	goto try_again;
}

static void
process_queue_item (SoupMessageQueueItem *item)
{
	SoupSessionSyncPrivate *priv = SOUP_SESSION_SYNC_GET_PRIVATE (item->session);

	item->state = SOUP_MESSAGE_STARTING;
	do {
		if (!wait_for_connection (item))
			break;

		if (item->state == SOUP_MESSAGE_READY) {
			item->state = SOUP_MESSAGE_RUNNING;
			soup_session_send_queue_item (item->session, item, NULL);
		}

		if (item->state == SOUP_MESSAGE_RESTARTING) {
			item->state = SOUP_MESSAGE_STARTING;
			soup_message_restarted (item->msg);
		} else {
			item->state = SOUP_MESSAGE_FINISHED;
			soup_message_finished (item->msg);
		}
		g_cond_broadcast (priv->cond);
	} while (item->state != SOUP_MESSAGE_FINISHED);
}

static gboolean
queue_message_callback (gpointer data)
{
	SoupMessageQueueItem *item = data;

	item->callback (item->session, item->msg, item->callback_data);
	g_object_unref (item->session);
	soup_message_queue_item_unref (item);
	return FALSE;
}

static gpointer
queue_message_thread (gpointer data)
{
	SoupMessageQueueItem *item = data;

	process_queue_item (item);
	if (item->callback) {
		soup_add_completion (soup_session_get_async_context (item->session),
				     queue_message_callback, item);
	} else {
		g_object_unref (item->session);
		soup_message_queue_item_unref (item);
	}

	return NULL;
}

static void
queue_message (SoupSession *session, SoupMessage *msg,
	       SoupSessionCallback callback, gpointer user_data)
{
	SoupMessageQueueItem *item;

	SOUP_SESSION_CLASS (soup_session_sync_parent_class)->
		queue_message (g_object_ref (session), msg, callback, user_data);

	item = soup_message_queue_lookup (soup_session_get_queue (session), msg);
	g_return_if_fail (item != NULL);

	g_thread_create (queue_message_thread, item, FALSE, NULL);
}

static guint
send_message (SoupSession *session, SoupMessage *msg)
{
	SoupMessageQueueItem *item;
	guint status;

	SOUP_SESSION_CLASS (soup_session_sync_parent_class)->queue_message (session, msg, NULL, NULL);

	item = soup_message_queue_lookup (soup_session_get_queue (session), msg);
	g_return_val_if_fail (item != NULL, SOUP_STATUS_MALFORMED);

	process_queue_item (item);
	status = msg->status_code;
	soup_message_queue_item_unref (item);
	return status;
}

static void
cancel_message (SoupSession *session, SoupMessage *msg, guint status_code)
{
	SoupSessionSyncPrivate *priv = SOUP_SESSION_SYNC_GET_PRIVATE (session);

	SOUP_SESSION_CLASS (soup_session_sync_parent_class)->cancel_message (session, msg, status_code);
	g_cond_broadcast (priv->cond);
}

static void
auth_required (SoupSession *session, SoupMessage *msg,
	       SoupAuth *auth, gboolean retrying)
{
	SoupSessionFeature *password_manager;

	password_manager = soup_session_get_feature_for_message (
		session, SOUP_TYPE_PASSWORD_MANAGER, msg);
	if (password_manager) {
		soup_password_manager_get_passwords_sync (
			SOUP_PASSWORD_MANAGER (password_manager),
			msg, auth, NULL); /* FIXME cancellable */
	}

	SOUP_SESSION_CLASS (soup_session_sync_parent_class)->
		auth_required (session, msg, auth, retrying);
}
