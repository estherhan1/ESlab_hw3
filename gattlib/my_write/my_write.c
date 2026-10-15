/*
 * my_write.c - HW-Lab3 optional problem 1
 *
 * BLE central in C using GATTLIB. One program does the whole job:
 *   1. registers a BlueZ pairing agent (iOS asks to pair, see below)
 *   2. scans for the phone by its advertised name
 *   3. connects, writes a text string to a characteristic, reads it back
 *   4. optionally subscribes to the characteristic and prints what arrives
 *
 * Why an agent: after connecting, BlueZ reads every service of the iPhone,
 * including Apple services that need encryption. iOS answers "Insufficient
 * Authentication" and BlueZ starts LE Secure Connections pairing (numeric
 * comparison). Without an agent nobody confirms the passkey and the
 * connection is dropped, so this program confirms it (tap "Pair" on the
 * phone).
 *
 * Why forget old pairings first: once paired, BlueZ knows the iPhone by its
 * public identity address and connects to it over Classic Bluetooth
 * (BR/EDR) instead of LE, so there is no ATT transport ("No ATT transport").
 * It also keeps the D-Bus object path of the random address it first saw,
 * which GATTLIB cannot find ("Cannot find connection"). Removing the old
 * pairing before scanning gives a fresh LE connection every run.
 *
 * Usage:
 *   sudo ./my_write -c <char-uuid> -t "hello, I am Esther!" [-n Blank] [-i 30] [-k]
 *
 *   -n name     advertised name to look for (default Blank)
 *   -c uuid     characteristic to write (needs the WRITE property)
 *   -t text     text to write
 *   -i seconds  afterwards subscribe (BlueZ writes the CCCD) and listen
 *   -k          keep earlier pairings (default: forget them first)
 */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <gio/gio.h>

#include "gattlib.h"

#define SCAN_SECONDS    15
#define WRITE_RETRIES   30   /* seconds to wait for discovery / pairing */
#define MAX_CANDIDATES  4
#define CONNECT_ATTEMPTS 3
#define AGENT_PATH      "/lab3/agent"

static const char *want_name = "Blank";
static const char *char_text;
static const char *text;
static int indicate_seconds;
static int forget_first = 1;

static GDBusConnection *bus;
static char found_addr[18];
static uuid_t char_uuid;
static gattlib_connection_t *connection;

/* the connect callback runs on another thread; ble_task waits on this */
static pthread_mutex_t done_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_cond = PTHREAD_COND_INITIALIZER;
static int done, setup_ok, connect_error;

/* ------------------------------------------------------------------ */
/* Pairing agent (org.bluez.Agent1) that accepts every request         */
/* ------------------------------------------------------------------ */

static const char agent_xml[] =
	"<node><interface name='org.bluez.Agent1'>"
	"<method name='Release'/>"
	"<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
	"<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
	"<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
	"<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
	"<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
	"<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='Cancel'/>"
	"</interface></node>";

static void agent_method_call(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *iface, const gchar *method,
		GVariant *params, GDBusMethodInvocation *inv, gpointer user_data) {
	(void)conn; (void)sender; (void)path; (void)iface; (void)user_data;

	if (strcmp(method, "RequestConfirmation") == 0) {
		const gchar *device;
		guint32 passkey;
		g_variant_get(params, "(&ou)", &device, &passkey);
		printf("Pairing: confirm passkey %06u -> yes (tap Pair on the phone)\n", passkey);
		fflush(stdout);
		g_dbus_method_invocation_return_value(inv, NULL);
	} else if (strcmp(method, "RequestPinCode") == 0) {
		g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", "0000"));
	} else if (strcmp(method, "RequestPasskey") == 0) {
		g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 0));
	} else if (strcmp(method, "Cancel") == 0) {
		printf("Pairing canceled\n");
		g_dbus_method_invocation_return_value(inv, NULL);
	} else {
		/* Release, Display*, RequestAuthorization, AuthorizeService */
		g_dbus_method_invocation_return_value(inv, NULL);
	}
}

static const GDBusInterfaceVTable agent_vtable = { agent_method_call, NULL, NULL, { 0 } };

static int register_agent(void) {
	GError *error = NULL;
	GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(agent_xml, &error);
	GVariant *reply;

	if (node == NULL) goto fail;
	if (!g_dbus_connection_register_object(bus, AGENT_PATH, node->interfaces[0],
			&agent_vtable, NULL, NULL, &error)) goto fail;

	reply = g_dbus_connection_call_sync(bus, "org.bluez", "/org/bluez",
			"org.bluez.AgentManager1", "RegisterAgent",
			g_variant_new("(os)", AGENT_PATH, "DisplayYesNo"),
			NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (reply == NULL) goto fail;
	g_variant_unref(reply);

	reply = g_dbus_connection_call_sync(bus, "org.bluez", "/org/bluez",
			"org.bluez.AgentManager1", "RequestDefaultAgent",
			g_variant_new("(o)", AGENT_PATH),
			NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (reply == NULL) goto fail;
	g_variant_unref(reply);

	g_dbus_node_info_unref(node);
	printf("Pairing agent registered\n");
	return 0;

fail:
	fprintf(stderr, "Cannot register pairing agent: %s\n", error ? error->message : "?");
	g_clear_error(&error);
	if (node) g_dbus_node_info_unref(node);
	return -1;
}

/* ------------------------------------------------------------------ */
/* BlueZ object lookup                                                 */
/* ------------------------------------------------------------------ */

/*
 * Collect the BlueZ Device1 objects whose Address (or Name, when name is
 * given) matches, and return the address each object path is named by,
 * e.g. /org/bluez/hci0/dev_50_90_36_5F_AF_DE -> "50:90:36:5F:AF:DE".
 */
static int find_device_paths(const char *addr, const char *name,
		char out[][18], int max) {
	GError *error = NULL;
	GVariant *reply, *objects, *ifaces;
	GVariantIter iter;
	const gchar *path;
	int n = 0;

	reply = g_dbus_connection_call_sync(bus, "org.bluez", "/",
			"org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
			NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
			-1, NULL, &error);
	if (reply == NULL) {
		fprintf(stderr, "GetManagedObjects failed: %s\n", error->message);
		g_error_free(error);
		return 0;
	}

	objects = g_variant_get_child_value(reply, 0);
	g_variant_iter_init(&iter, objects);
	while (n < max && g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
		GVariant *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", NULL);
		const char *tail = strstr(path, "/dev_");
		const gchar *value = NULL;

		if (dev && tail && strlen(tail) == 22) {
			int match = 0;
			if (addr && g_variant_lookup(dev, "Address", "&s", &value))
				match = strcasecmp(value, addr) == 0;
			if (!match && name && g_variant_lookup(dev, "Name", "&s", &value))
				match = strcmp(value, name) == 0;
			if (match) {
				int i;
				for (i = 0; i < 17; i++) out[n][i] = tail[5 + i] == '_' ? ':' : tail[5 + i];
				out[n][17] = '\0';
				n++;
			}
		}
		if (dev) g_variant_unref(dev);
		g_variant_unref(ifaces);
	}
	g_variant_unref(objects);
	g_variant_unref(reply);
	return n;
}

/* Remove paired devices (and cached ones advertising our name) so the
 * phone is found again as a new LE device. */
static void forget_phone(void) {
	GVariant *reply, *objects, *ifaces;
	GVariantIter iter;
	const gchar *path;

	reply = g_dbus_connection_call_sync(bus, "org.bluez", "/",
			"org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
			NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
			-1, NULL, NULL);
	if (reply == NULL) return;

	objects = g_variant_get_child_value(reply, 0);
	g_variant_iter_init(&iter, objects);
	while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
		GVariant *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", NULL);
		gboolean paired = FALSE;
		const gchar *name = NULL, *addr = "?";

		if (dev) {
			g_variant_lookup(dev, "Paired", "b", &paired);
			g_variant_lookup(dev, "Name", "&s", &name);
			g_variant_lookup(dev, "Address", "&s", &addr);
			if (paired || (name && strcmp(name, want_name) == 0)) {
				GVariant *r = g_dbus_connection_call_sync(bus, "org.bluez",
						"/org/bluez/hci0", "org.bluez.Adapter1", "RemoveDevice",
						g_variant_new("(o)", path), NULL, G_DBUS_CALL_FLAGS_NONE,
						-1, NULL, NULL);
				if (r) {
					printf("Forgot old %s %s\n", paired ? "pairing" : "entry", addr);
					g_variant_unref(r);
				}
			}
			g_variant_unref(dev);
		}
		g_variant_unref(ifaces);
	}
	g_variant_unref(objects);
	g_variant_unref(reply);
}

/* ------------------------------------------------------------------ */
/* GATTLIB part                                                        */
/* ------------------------------------------------------------------ */

static void print_value(const char *label, const uint8_t *data, size_t len) {
	size_t i;

	printf("%s %zu bytes: ", label, len);
	for (i = 0; i < len; i++) printf("%02x", data[i]);
	printf("  \"");
	for (i = 0; i < len; i++) putchar(isprint(data[i]) ? data[i] : '.');
	printf("\"\n");
	fflush(stdout);
}

static void on_device_found(gattlib_adapter_t *adapter, const char *addr,
		const char *name, void *user_data) {
	(void)user_data;

	if (found_addr[0] == '\0' && name && strcmp(name, want_name) == 0) {
		snprintf(found_addr, sizeof(found_addr), "%s", addr);
		printf("Found '%s' at %s\n", name, addr);
		gattlib_adapter_scan_disable(adapter);   /* stop scanning early */
	}
}

static void on_received(const uuid_t *uuid, const uint8_t *data,
		size_t len, void *user_data) {
	(void)uuid;
	(void)user_data;
	print_value("<<< received", data, len);
}

static void finish(int ok, int error) {
	pthread_mutex_lock(&done_mutex);
	setup_ok = ok;
	connect_error = error;
	done = 1;
	pthread_cond_signal(&done_cond);
	pthread_mutex_unlock(&done_mutex);
}

static void on_connected(gattlib_adapter_t *adapter, const char *dst,
		gattlib_connection_t *conn, int error, void *user_data) {
	void *value = NULL;
	size_t value_len = 0;
	int ret = GATTLIB_NOT_FOUND, i;
	(void)adapter;
	(void)user_data;

	if (error != GATTLIB_SUCCESS || conn == NULL) {
		finish(0, error);
		return;
	}
	connection = conn;
	printf("Connected to %s\n", dst);

	/* ---- write: the characteristic value ----
	 * Right after connecting the write can fail for a while: BlueZ is still
	 * exporting the characteristics, or iOS refuses it (ATT error 0x0e)
	 * until pairing has finished, so retry once a second. */
	for (i = 0; i < WRITE_RETRIES; i++) {
		ret = gattlib_write_char_by_uuid(conn, &char_uuid, text, strlen(text));
		if (ret == GATTLIB_SUCCESS) break;
		if (i == 0) printf("Waiting for service discovery / pairing ...\n");
		sleep(1);
	}
	if (ret != GATTLIB_SUCCESS) {
		fprintf(stderr, "Write failed (error %d). Does the characteristic have WRITE?\n", ret);
		finish(0, ret);
		return;
	}
	print_value("Wrote", (const uint8_t *)text, strlen(text));

	/* ---- read back ---- */
	if (gattlib_read_char_by_uuid(conn, &char_uuid, &value, &value_len) == GATTLIB_SUCCESS) {
		print_value("Read back", value, value_len);
		gattlib_characteristic_free_value(value);
	} else {
		printf("Read back failed (characteristic may not allow READ)\n");
	}

	/* ---- subscribe: BlueZ writes the CCCD for us ----
	 * gattlib_indication_start() ends up in BlueZ's D-Bus StartNotify, and
	 * BlueZ picks the CCCD value itself: 0x0001 (notification) when the
	 * characteristic supports NOTIFY, 0x0002 (indication) only when it
	 * supports INDICATE alone. */
	if (indicate_seconds > 0) {
		gattlib_register_indication(conn, on_received, NULL);
		ret = gattlib_indication_start(conn, &char_uuid);
		if (ret != GATTLIB_SUCCESS) {
			fprintf(stderr, "Failed to subscribe (error %d)\n", ret);
			finish(0, ret);
			return;
		}
		printf("Subscribed (BlueZ wrote the CCCD: 0x0001 if NOTIFY is supported, "
		       "else 0x0002), listening %d s ...\n", indicate_seconds);
	}
	finish(1, GATTLIB_SUCCESS);
}

static void *ble_task(void *arg) {
	gattlib_adapter_t *adapter = NULL;
	char candidates[MAX_CANDIDATES][18];
	int n, i, attempt, ok = 0;
	(void)arg;

	if (gattlib_adapter_open(NULL, &adapter) != GATTLIB_SUCCESS) {
		fprintf(stderr, "Failed to open the Bluetooth adapter\n");
		return NULL;
	}

	printf("Scanning up to %d s for '%s' ...\n", SCAN_SECONDS, want_name);
	gattlib_adapter_scan_enable(adapter, on_device_found, SCAN_SECONDS, NULL);
	if (found_addr[0] == '\0') {
		fprintf(stderr, "'%s' not found. Is the phone app in the foreground?\n", want_name);
		goto out;
	}

	/* the object path may be named by another (random) address */
	n = find_device_paths(found_addr, NULL, candidates, MAX_CANDIDATES);
	if (n == 0) {
		snprintf(candidates[0], sizeof(candidates[0]), "%s", found_addr);
		n = 1;
	}

	/* a connection attempt can be aborted (le-connection-abort-by-local),
	 * e.g. when the Pi's Bluetooth chip glitches, so try a few times */
	for (attempt = 1; attempt <= CONNECT_ATTEMPTS && !ok; attempt++) {
		if (attempt > 1) {
			printf("Retrying connection (%d/%d) ...\n", attempt, CONNECT_ATTEMPTS);
			sleep(2);
		}
		for (i = 0; i < n && !ok; i++) {
			pthread_mutex_lock(&done_mutex);
			done = 0;
			pthread_mutex_unlock(&done_mutex);

			if (strcasecmp(candidates[i], found_addr) != 0)
				printf("Using BlueZ object of %s for %s\n", candidates[i], found_addr);
			if (gattlib_connect(adapter, candidates[i], GATTLIB_CONNECTION_OPTIONS_NONE,
					on_connected, NULL) != GATTLIB_SUCCESS && i + 1 < n)
				continue;   /* this object is unknown to GATTLIB, try the next */

			pthread_mutex_lock(&done_mutex);
			while (!done) pthread_cond_wait(&done_cond, &done_mutex);
			ok = setup_ok;
			pthread_mutex_unlock(&done_mutex);
		}
		/* only a failed connection is worth retrying */
		if (!ok && connection != NULL) break;
	}
	if (!ok) {
		fprintf(stderr, "Failed (error %d)\n", connect_error);
		goto out;
	}

	if (indicate_seconds > 0) sleep(indicate_seconds);

out:
	if (connection) {
		gattlib_disconnect(connection, true);
		printf("Disconnected\n");
	}
	gattlib_adapter_close(adapter);
	return NULL;
}

static void usage(const char *prog) {
	fprintf(stderr, "Usage: %s -c <char-uuid> -t <text> [-n name] [-i seconds] [-k]\n", prog);
}

int main(int argc, char *argv[]) {
	GError *error = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "n:c:t:i:kh")) != -1) {
		switch (opt) {
		case 'n': want_name = optarg; break;
		case 'c': char_text = optarg; break;
		case 't': text = optarg; break;
		case 'i': indicate_seconds = atoi(optarg); break;
		case 'k': forget_first = 0; break;
		default: usage(argv[0]); return opt == 'h' ? 0 : 1;
		}
	}
	if (char_text == NULL || text == NULL) {
		usage(argv[0]);
		return 1;
	}
	if (gattlib_string_to_uuid(char_text, strlen(char_text) + 1, &char_uuid) != GATTLIB_SUCCESS) {
		fprintf(stderr, "Invalid UUID: %s\n", char_text);
		return 1;
	}

	bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (bus == NULL) {
		fprintf(stderr, "Cannot connect to the system D-Bus: %s\n", error->message);
		return 1;
	}
	/* registered before gattlib_mainloop(), so the agent's D-Bus calls are
	 * dispatched by the GLib main loop GATTLIB runs */
	if (register_agent() != 0) return 1;
	if (forget_first) forget_phone();

	if (gattlib_mainloop(ble_task, NULL) != GATTLIB_SUCCESS) {
		fprintf(stderr, "Failed to create the GATTLIB main loop\n");
		return 1;
	}
	return 0;
}
