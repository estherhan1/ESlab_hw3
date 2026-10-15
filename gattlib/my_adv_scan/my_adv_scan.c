/*
 * my_adv_scan.c - HW-Lab3 optional problem 1
 *
 * A BLE scanner written in C on top of GATTLIB (BlueZ D-Bus backend).
 * Based on GATTLIB's examples/advertisement_data, extended to:
 *   - list every device only once (the BlueZ callback fires on each change)
 *   - by default show only devices that advertise a name (-a shows all)
 *   - show RSSI and the advertised service UUIDs
 *   - turn manufacturer ids into company names using the Bluetooth SIG
 *     "company_identifiers.yaml" list (falls back to a small built-in table)
 *   - free the buffers returned by gattlib_get_advertisement_data_from_mac()
 *   - print a per-company summary at the end
 *
 * Usage: sudo ./my_adv_scan [-t seconds] [-a] [-f company_identifiers.yaml]
 */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gattlib.h"

#define DEFAULT_SCAN_SECONDS 15
#define DEFAULT_COMPANY_FILE "/home/pi/company_identifiers.yaml"
#define MAX_DEVICES          512
#define MAX_NAME_LEN         64

/* ---------------- company id -> name ---------------- */

typedef struct {
	uint16_t id;
	char *name;
} company_t;

static company_t *companies;
static size_t company_count;

/* used when the SIG list cannot be loaded */
static const company_t builtin_companies[] = {
	{ 0x0006, "Microsoft" },
	{ 0x000F, "Broadcom Corporation" },
	{ 0x004C, "Apple, Inc." },
	{ 0x0059, "Nordic Semiconductor ASA" },
	{ 0x0075, "Samsung Electronics Co. Ltd." },
	{ 0x00E0, "Google" },
	{ 0x0157, "Anhui Huami Information Technology Co., Ltd." },
	{ 0x038F, "Xiaomi Inc." },
};

/*
 * Parse the "- value: 0x004C" / "name: 'Apple, Inc.'" pairs of the Bluetooth
 * SIG assigned-numbers YAML file. Returns the number of companies loaded.
 */
static size_t load_companies(const char *path) {
	FILE *f = fopen(path, "r");
	char line[256];
	unsigned int value;
	int have_value = 0;
	size_t capacity = 0;

	if (f == NULL) {
		return 0;
	}

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (isspace((unsigned char)*p)) p++;

		if (sscanf(p, "- value: %x", &value) == 1) {
			have_value = 1;
		} else if (have_value && strncmp(p, "name:", 5) == 0) {
			char *name = p + 5;
			size_t len;

			while (isspace((unsigned char)*name)) name++;
			if (*name == '\'') name++;
			len = strcspn(name, "\r\n");
			if (len > 0 && name[len - 1] == '\'') len--;

			if (company_count == capacity) {
				capacity = capacity ? capacity * 2 : 1024;
				companies = realloc(companies, capacity * sizeof(company_t));
				if (companies == NULL) {
					fclose(f);
					return 0;
				}
			}
			companies[company_count].id = (uint16_t)value;
			companies[company_count].name = strndup(name, len);
			company_count++;
			have_value = 0;
		}
	}
	fclose(f);
	return company_count;
}

static const char *company_name(uint16_t id) {
	size_t i;

	for (i = 0; i < company_count; i++) {
		if (companies[i].id == id) return companies[i].name;
	}
	for (i = 0; i < sizeof(builtin_companies) / sizeof(builtin_companies[0]); i++) {
		if (builtin_companies[i].id == id) return builtin_companies[i].name;
	}
	return "unknown";
}

/* ---------------- devices already printed ---------------- */

typedef struct {
	char addr[18];
	int has_company;
	uint16_t company_id;
} seen_device_t;

static seen_device_t seen[MAX_DEVICES];
static size_t seen_count;
static size_t total_callbacks;
static pthread_mutex_t seen_mutex = PTHREAD_MUTEX_INITIALIZER;

static int show_all;
static int scan_seconds = DEFAULT_SCAN_SECONDS;
static const char *company_file = DEFAULT_COMPANY_FILE;

static int already_seen(const char *addr) {
	size_t i;

	for (i = 0; i < seen_count; i++) {
		if (strcmp(seen[i].addr, addr) == 0) return 1;
	}
	return 0;
}

static void free_advertisement(gattlib_advertisement_data_t *adv, size_t adv_count,
		gattlib_manufacturer_data_t *mfr, size_t mfr_count) {
	size_t i;

	for (i = 0; i < adv_count; i++) free(adv[i].data);
	free(adv);
	for (i = 0; i < mfr_count; i++) free(mfr[i].data);
	free(mfr);
}

/* Called by GATTLIB for every advertisement (and every change of it). */
static void on_device_found(gattlib_adapter_t *adapter, const char *addr,
		const char *name, void *user_data) {
	gattlib_advertisement_data_t *adv = NULL;
	gattlib_manufacturer_data_t *mfr = NULL;
	size_t adv_count = 0, mfr_count = 0, i, j;
	int16_t rssi = 0;
	char uuid_str[MAX_LEN_UUID_STR + 1];
	seen_device_t *dev;
	(void)user_data;

	pthread_mutex_lock(&seen_mutex);
	total_callbacks++;
	/* Without -a, wait until the device advertises a name: a later callback
	 * for the same address may still carry it. */
	if (already_seen(addr) || seen_count == MAX_DEVICES ||
	    (!show_all && (name == NULL || name[0] == '\0'))) {
		pthread_mutex_unlock(&seen_mutex);
		return;
	}
	dev = &seen[seen_count++];
	snprintf(dev->addr, sizeof(dev->addr), "%s", addr);
	pthread_mutex_unlock(&seen_mutex);

	if (gattlib_get_advertisement_data_from_mac(adapter, addr, &adv, &adv_count,
			&mfr, &mfr_count) != GATTLIB_SUCCESS) {
		adv_count = mfr_count = 0;
		adv = NULL;
		mfr = NULL;
	}
	gattlib_get_rssi_from_mac(adapter, addr, &rssi);

	printf("\n[%zu] %s  '%.*s'  RSSI %d dBm\n", seen_count, addr, MAX_NAME_LEN,
	       name ? name : "(no name)", rssi);

	for (i = 0; i < mfr_count; i++) {
		printf("     Manufacturer 0x%04X  %-30s data:", mfr[i].manufacturer_id,
		       company_name(mfr[i].manufacturer_id));
		for (j = 0; j < mfr[i].data_size; j++) printf(" %02x", mfr[i].data[j]);
		printf("\n");
	}
	if (mfr_count > 0) {
		pthread_mutex_lock(&seen_mutex);
		dev->has_company = 1;
		dev->company_id = mfr[0].manufacturer_id;
		pthread_mutex_unlock(&seen_mutex);
	}

	for (i = 0; i < adv_count; i++) {
		gattlib_uuid_to_string(&adv[i].uuid, uuid_str, sizeof(uuid_str));
		printf("     Service UUID %s\n", uuid_str);
	}
	fflush(stdout);

	free_advertisement(adv, adv_count, mfr, mfr_count);
}

static void print_summary(void) {
	uint16_t ids[MAX_DEVICES];
	size_t counts[MAX_DEVICES];
	size_t kinds = 0, no_mfr = 0, i, k;

	for (i = 0; i < seen_count; i++) {
		if (!seen[i].has_company) {
			no_mfr++;
			continue;
		}
		for (k = 0; k < kinds && ids[k] != seen[i].company_id; k++);
		if (k == kinds) {
			ids[kinds] = seen[i].company_id;
			counts[kinds++] = 0;
		}
		counts[k]++;
	}

	printf("\n========== Summary ==========\n");
	printf("Advertisement callbacks : %zu\n", total_callbacks);
	printf("Devices listed          : %zu%s\n", seen_count,
	       show_all ? "" : " (named only, use -a for all)");
	for (k = 0; k < kinds; k++) {
		printf("  0x%04X %-34s %zu\n", ids[k], company_name(ids[k]), counts[k]);
	}
	if (no_mfr) printf("  %-41s %zu\n", "(no manufacturer data)", no_mfr);
	printf("=============================\n");
}

/* GATTLIB needs its GLib main loop running; the BLE work happens here. */
static void *ble_task(void *arg) {
	gattlib_adapter_t *adapter;
	(void)arg;

	if (gattlib_adapter_open(NULL, &adapter) != GATTLIB_SUCCESS) {
		fprintf(stderr, "Failed to open the Bluetooth adapter.\n");
		return NULL;
	}

	printf("Scanning for %d s ...\n", scan_seconds);
	if (gattlib_adapter_scan_enable_with_filter(adapter,
			NULL,                                 /* no service UUID filter */
			0,                                    /* no RSSI threshold */
			GATTLIB_DISCOVER_FILTER_NOTIFY_CHANGE, /* also report updates */
			on_device_found, scan_seconds, NULL) != GATTLIB_SUCCESS) {
		fprintf(stderr, "Failed to scan.\n");
	}
	gattlib_adapter_scan_disable(adapter);
	gattlib_adapter_close(adapter);

	print_summary();
	return NULL;
}

int main(int argc, char *argv[]) {
	int opt;

	while ((opt = getopt(argc, argv, "t:af:h")) != -1) {
		switch (opt) {
		case 't': scan_seconds = atoi(optarg); break;
		case 'a': show_all = 1; break;
		case 'f': company_file = optarg; break;
		default:
			fprintf(stderr, "Usage: %s [-t seconds] [-a] [-f company_identifiers.yaml]\n", argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}
	if (scan_seconds <= 0) scan_seconds = DEFAULT_SCAN_SECONDS;

	if (load_companies(company_file) > 0) {
		printf("Loaded %zu company identifiers from %s\n", company_count, company_file);
	} else {
		printf("Could not read %s, using the built-in company table\n", company_file);
	}

	if (gattlib_mainloop(ble_task, NULL) != GATTLIB_SUCCESS) {
		fprintf(stderr, "Failed to create the GATTLIB main loop.\n");
		return 1;
	}
	return 0;
}
