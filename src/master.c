/*
 * Geoclue
 * master.c - Master process
 *
 * Authors: Iain Holmes <iain@openedhand.com>
 *          Jussi Kukkonen <jku@o-hand.com>
 * Copyright 2007-2008 by Garmin Ltd. or its subsidiaries
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include <config.h>

#include <string.h>
#include <dbus/dbus.h>

#include "main.h"
#include "master.h"
#include "client.h"
#include "master-provider.h"

#ifdef HAVE_NETWORK_MANAGER
#include "connectivity-networkmanager.h"
#else
#ifdef HAVE_CONIC
#include "connectivity-conic.h"
#else
#ifdef HAVE_CONNMAN
#include "connectivity-connman.h"
#endif
#endif
#endif

enum {
	OPTIONS_CHANGED,
	LAST_SIGNAL
};

static guint32 signals[LAST_SIGNAL] = {0, };

G_DEFINE_TYPE (GcMaster, gc_master, G_TYPE_OBJECT);

static GList *providers = NULL;
static GList *clients = NULL;  /* List of active GcMasterClient objects */
static DBusGConnection *master_connection = NULL;  /* Global connection for cleanup */

static void gc_iface_master_create (GcMaster              *master,
				    DBusGMethodInvocation *context);

static void client_destroyed (gpointer data, GObject *old_client);
static DBusHandlerResult name_owner_changed_filter (DBusConnection *connection,
                                                     DBusMessage    *message,
                                                     void           *user_data);

#include "gc-iface-master-glue.h"

static void
add_client (GcMasterClient *client)
{
	clients = g_list_prepend (clients, client);
	g_object_weak_ref (G_OBJECT (client), client_destroyed, NULL);
}

static void
client_destroyed (gpointer data, GObject *old_client)
{
	/* The weak ref callback - client is being finalized */
	clients = g_list_remove (clients, old_client);
	
	/* If this was the last client, unref all providers to save power */
	if (clients == NULL && providers != NULL) {
		GList *l;
		for (l = providers; l; l = l->next) {
			GcMasterProvider *provider = l->data;
			g_object_unref (provider);
		}
		g_list_free (providers);
		providers = NULL;
	}
}

static GcMasterClient *
find_client_by_sender (const char *sender)
{
	GList *l;
	
	for (l = clients; l; l = l->next) {
		GcMasterClient *client = l->data;
		const char *client_sender = gc_master_client_get_sender (client);
		
		if (client_sender && strcmp (client_sender, sender) == 0) {
			return client;
		}
	}
	
	return NULL;
}

static DBusHandlerResult
name_owner_changed_filter (DBusConnection *connection,
                           DBusMessage    *message,
                           void           *user_data)
{
	const char *name, *old_owner, *new_owner;
	
	if (!dbus_message_is_signal (message, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	
	if (!dbus_message_get_args (message, NULL,
	                            DBUS_TYPE_STRING, &name,
	                            DBUS_TYPE_STRING, &old_owner,
	                            DBUS_TYPE_STRING, &new_owner,
	                            DBUS_TYPE_INVALID)) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	
	/* Check if a client disconnected (old_owner exists, new_owner is empty) */
	if (old_owner && *old_owner && (!new_owner || !*new_owner)) {
		GcMasterClient *client = find_client_by_sender (name);
		
		if (client) {
			const char *object_path;
			
			g_print ("Client %s disconnected, cleaning up\n", name);
			
			/* Unregister the client object from D-Bus */
			object_path = gc_master_client_get_object_path (client);
			if (object_path && master_connection) {
				dbus_g_connection_unregister_g_object (master_connection, G_OBJECT (client));
			} else if (object_path && !master_connection) {
				g_warning ("Cannot unregister client %s: master_connection is NULL", name);
			}
			
			/* Release our reference. This will destroy the object,
			 * which will trigger our weak ref callback (client_destroyed) to clean up. */
			g_object_unref (client);
		}
	}
	
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

#define GEOCLUE_MASTER_PATH "/org/freedesktop/Geoclue/Master/client"
static void
gc_iface_master_create (GcMaster              *master,
			DBusGMethodInvocation *context)
{
	static guint32 serial = 0;
	GcMasterClient *client;
	char *path;
	char *sender;

	path = g_strdup_printf ("%s%d", GEOCLUE_MASTER_PATH, serial++);
	client = g_object_new (GC_TYPE_MASTER_CLIENT, NULL);
	
	/* Get and store the sender's unique name and object path */
	sender = dbus_g_method_get_sender (context);
	gc_master_client_set_sender (client, sender);
	gc_master_client_set_object_path (client, path);
	g_free (sender);
	
	/* Register the client object on D-Bus */
	dbus_g_connection_register_g_object (master->connection, path,
					     G_OBJECT (client));
	
	/* Track the client with a weak reference */
	add_client (client);
	
	/* Return the object path */
	dbus_g_method_return (context, path);
	g_free (path);
}

static void
gc_master_class_init (GcMasterClass *klass)
{
	dbus_g_object_type_install_info (gc_master_get_type (),
					 &dbus_glib_gc_iface_master_object_info);

	signals[OPTIONS_CHANGED] = g_signal_new ("options-changed",
						  G_TYPE_FROM_CLASS (klass),
						  G_SIGNAL_RUN_FIRST |
						  G_SIGNAL_NO_RECURSE,
						  G_STRUCT_OFFSET (GcMasterClass, options_changed), 
						  NULL, NULL,
						  g_cclosure_marshal_VOID__BOXED,
						  G_TYPE_NONE, 1,
						  G_TYPE_HASH_TABLE);
}

/* Load the provider details out of a keyfile */
static void
gc_master_add_new_provider (GcMaster   *master,
                            const char *filename)
{
	GcMasterProvider *provider;
	
	provider = gc_master_provider_new (filename, 
	                                   master->connectivity);
	
	if (!provider) {
		g_warning ("Loading from %s failed", filename);
		return;
	}
	
	providers = g_list_prepend (providers, provider);
}

/* Scan a directory for .provider files */
#define PROVIDER_EXTENSION ".provider"

static void
gc_master_load_providers (GcMaster *master)
{
	GDir *dir;
	GError *error = NULL;
	const char *filename;

	dir = g_dir_open (GEOCLUE_PROVIDERS_DIR, 0, &error);
	if (dir == NULL) {
		g_warning ("Error opening %s: %s\n", GEOCLUE_PROVIDERS_DIR,
			   error->message);
		g_error_free (error);
		return;
	}

	filename = g_dir_read_name (dir);
	if (!filename) {
		g_print ("No providers found in %s\n", dir);
	} else {
		g_print ("Found providers:\n");
	}
	while (filename) {
		char *fullname, *ext;

		g_print ("  %s\n", filename);
		ext = strrchr (filename, '.');
		if (ext == NULL || strcmp (ext, PROVIDER_EXTENSION) != 0) {
			g_print ("   - Ignored\n");
			filename = g_dir_read_name (dir);
			continue;
		}

		fullname = g_build_filename (GEOCLUE_PROVIDERS_DIR, 
					     filename, NULL);
		gc_master_add_new_provider (master, fullname);
		g_free (fullname);
		
		filename = g_dir_read_name (dir);
	}

	g_dir_close (dir);
}

static void
gc_master_init (GcMaster *master)
{
	GError *error = NULL;
	DBusConnection *dbus_conn;
	
	
	master->connection = dbus_g_bus_get (GEOCLUE_DBUS_BUS, &error);
	if (master->connection == NULL) {
		g_warning ("Could not get %s: %s", GEOCLUE_DBUS_BUS, 
			   error->message);
		g_error_free (error);
		return;
	}
	
	/* Store connection globally for client cleanup */
	master_connection = master->connection;
	
	/* Add filter for NameOwnerChanged signals to detect client disconnections */
	dbus_conn = dbus_g_connection_get_connection (master->connection);
	dbus_connection_add_filter (dbus_conn, name_owner_changed_filter, master, NULL);
	
	/* Subscribe to NameOwnerChanged signals */
	dbus_bus_add_match (dbus_conn,
	                    "type='signal',interface='" DBUS_INTERFACE_DBUS "',member='NameOwnerChanged'",
	                    NULL);
	
	master->connectivity = geoclue_connectivity_new ();

	gc_master_load_providers (master);
}


GList *
gc_master_get_providers (GcInterfaceFlags      iface_type,
                         GeoclueAccuracyLevel  min_accuracy,
                         gboolean              can_update,
                         GeoclueResourceFlags  allowed,
                         GError              **error)
{
	GList *l, *p = NULL;
	
	if (providers == NULL) {
		return NULL;
	}
	
	for (l = providers; l; l = l->next) {
		GcMasterProvider *provider = l->data;
		
		if (gc_master_provider_is_good (provider,
		                                iface_type, 
		                                min_accuracy, 
		                                can_update, 
		                                allowed)) {
			p = g_list_prepend (p, provider);
		}
	}
	
	return p;
}
