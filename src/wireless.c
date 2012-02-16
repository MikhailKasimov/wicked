/*
 * Routines for handling Wireless devices.
 *
 * Holie cowe, the desygne of thefe Wyreless Extensions is indisputablie baroque!
 *
 * Copyright (C) 2010 Olaf Kirch <okir@suse.de>
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <net/if_arp.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>

#include <wicked/wireless.h>
#include <wicked/socket.h>
#include "socket_priv.h"
#include "netinfo_priv.h"
#include "buffer.h"
#include "kernel.h"
#include "dbus-client.h"
#include "wpa-supplicant.h"
#include "wireless_priv.h"

#ifndef IW_IE_CIPHER_NONE
# define IW_IE_CIPHER_NONE       0
# define IW_IE_CIPHER_WEP40      1
# define IW_IE_CIPHER_TKIP       2
# define IW_IE_CIPHER_WRAP       3
# define IW_IE_CIPHER_CCMP       4
# define IW_IE_CIPHER_WEP104     5
# define IW_IE_KEY_MGMT_NONE     0
# define IW_IE_KEY_MGMT_802_1X   1
# define IW_IE_KEY_MGMT_PSK      2
#endif

static void		ni_wireless_set_assoc_network(ni_wireless_t *, ni_wireless_network_t *);
static void		__ni_wireless_network_destroy(ni_wireless_network_t *net);

static ni_wpa_client_t *wpa_client;

/*
 * Get the dbus client handle for wpa_supplicant
 */
static ni_wpa_client_t *
ni_wpa_client(void)
{
	if (wpa_client == NULL) {
		wpa_client = ni_wpa_client_open();
		if (wpa_client == NULL)
			ni_error("Unable to connect to wpa_supplicant");
	}
	return wpa_client;
}

/*
 * Refresh what we think we know about this interface.
 */
int
ni_wireless_interface_refresh(ni_interface_t *ifp)
{
	ni_wpa_client_t *wpa;
	ni_wpa_interface_t *wif;
	ni_wireless_scan_t *scan;

	if (!(wpa = ni_wpa_client()))
		return -1;

	wif = ni_wpa_interface_bind(wpa, ifp->name);
	if (wif == NULL) {
		ni_error("wpa_supplicant doesn't know interface %s", ifp->name);
		return -1;
	}

	if (ifp->wireless == NULL) {
		ni_wireless_t *wlan;

		wlan = ni_wireless_new();
		ifp->wireless = wlan;
	}

	ifp->wireless->capabilities = wif->capabilities;

	scan = ni_wireless_scan_new();
	if (ni_wpa_interface_retrieve_scan(wpa, wif, scan) >= 0)
		ni_interface_set_wireless_scan(ifp, scan);
	else
		ni_wireless_scan_free(scan);

	return 0;
}

int
__ni_wireless_get_scan_results(ni_netconfig_t *nc, ni_interface_t *ifp)
{
	ni_wireless_scan_t *scan;
	ni_wpa_interface_t *wif;

	if (ifp->link.type != NI_IFTYPE_WIRELESS) {
		ni_error("%s: cannot do wireless scan on this interface", ifp->name);
		return -1;
	}

	if ((scan = ifp->wireless_scan) == NULL || wpa_client == NULL) {
		ni_debug_wireless("no scan object for wireless i/f %s - need to request one first", ifp->name);
		return 0;
	}

	wif = ni_wpa_interface_bind(wpa_client, ifp->name);
	if (wif == NULL) {
		ni_debug_wireless("wpa_supplicant doesn't know interface %s", ifp->name);
		return -1;
	}

	if (ni_wpa_interface_retrieve_scan(wpa_client, wif, scan) < 0) {
		ni_error("ni_wpa_interface_retrieve_scan failed");
		return -1;
	}

	return 0;
}

int
__ni_wireless_request_scan(ni_netconfig_t *nc, ni_interface_t *ifp)
{
	ni_wpa_client_t *wpa;
	ni_wireless_scan_t *scan;
	ni_wpa_interface_t *wif;

	if (ifp->link.type != NI_IFTYPE_WIRELESS) {
		ni_error("%s: cannot do wireless scan on this interface", ifp->name);
		return -1;
	}

	if ((scan = ifp->wireless_scan) == NULL) {
		scan = ni_wireless_scan_new();
		ni_interface_set_wireless_scan(ifp, scan);
	}

	if (!(wpa = ni_wpa_client()))
		return -1;

	wif = ni_wpa_interface_bind(wpa, ifp->name);
	if (wif == NULL) {
		ni_error("wpa_supplicant doesn't know interface %s", ifp->name);
		return -1;
	}

	if (wif->scan.pending) {
		ni_error("wireless scan pending for interface %s", ifp->name);
		return -1;
	}

	if (ni_wpa_interface_request_scan(wpa, wif, scan) < 0) {
		ni_error("ni_wpa_interface_request_scan failed");
		return -1;
	}

	ni_debug_ifconfig("%s: requested wireless scan", ifp->name);
	return 0;
}

/*
 * Request association
 */
int
ni_wireless_associate(ni_interface_t *dev, ni_wireless_network_t *net)
{
	ni_wireless_t *wlan;
	ni_wpa_client_t *wpa;
	ni_wpa_interface_t *wpa_dev;

	if ((wlan = ni_interface_get_wireless(dev)) == NULL) {
		ni_error("%s: no wireless info for device", dev->name);
		return -1;
	}

	if (!(wpa = ni_wpa_client()))
		return -1;

	if (!(wpa_dev = ni_wpa_interface_bind(wpa, dev->name)))
		return -1;

	ni_wireless_set_assoc_network(wlan, net);

	return ni_wpa_interface_associate(wpa_dev, net);
}

/*
 * Callback from wpa_supplicant client whenever the association state changes
 * in a significant way.
 * FIXME: this should really use the ifindex rather than the name
 */
void
ni_wireless_association_changed(const char *ifname, ni_wireless_assoc_state_t new_state)
{
	ni_netconfig_t *nc = ni_global_state_handle(0);
	ni_interface_t *dev;
	ni_wireless_t *wlan;
	ni_event_t ev = -1;

	if (!(dev = ni_interface_by_name(nc, ifname)))
		return;

	if (!(wlan = dev->wireless))
		return;

	if (new_state == wlan->assoc.state)
		return;

	switch (new_state) {
	case NI_WIRELESS_ESTABLISHED:
		ev = NI_EVENT_LINK_ASSOCIATED;
		break;

	case NI_WIRELESS_NOT_ASSOCIATED:
		ev = NI_WIRELESS_NOT_ASSOCIATED;
		break;

	default: ;
	}

	wlan->assoc.state = new_state;
	if (ev != -1)
		__ni_interface_event(nc, dev, ev);
}

/*
 * rtnetlink sent us an RTM_NEWLINK event with IFLA_WIRELESS info
 */
int
__ni_wireless_link_event(ni_netconfig_t *nc, ni_interface_t *ifp, void *data, size_t len)
{
	ni_debug_wireless("%s: ignoring wireless event", ifp->name);
	return 0;
}

/*
 * Helper function to set AP address
 */
static inline void
__ni_wireless_set_ap(ni_hwaddr_t *hwa, const void *ap_addr)
{
	hwa->type = NI_IFTYPE_WIRELESS;
	hwa->len = ETH_ALEN;
	memcpy(hwa->data, ap_addr, ETH_ALEN);
}

typedef struct __ni_kernel_map_t {
	int		kernel_value;
	int		wicked_value;
} __ni_kernel_map_t;

#if 0
static __ni_kernel_map_t	__ni_wireless_mode_map[] = {
	{ IW_MODE_AUTO,		NI_WIRELESS_MODE_AUTO},
	{ IW_MODE_ADHOC,	NI_WIRELESS_MODE_ADHOC},
	{ IW_MODE_INFRA,	NI_WIRELESS_MODE_MANAGED},
	{ IW_MODE_MASTER,	NI_WIRELESS_MODE_MASTER},
	{ IW_MODE_REPEAT,	NI_WIRELESS_MODE_REPEATER},
	{ IW_MODE_SECOND,	NI_WIRELESS_MODE_SECONDARY},
	{ IW_MODE_MONITOR,	NI_WIRELESS_MODE_MONITOR},
	{ -1 }
};
#endif

static __ni_kernel_map_t	__ni_wireless_cipher_map[] = {
	{ IW_IE_CIPHER_NONE,	NI_WIRELESS_CIPHER_NONE },
	{ IW_IE_CIPHER_WEP40,	NI_WIRELESS_CIPHER_WEP40 },
	{ IW_IE_CIPHER_TKIP,	NI_WIRELESS_CIPHER_TKIP },
	{ IW_IE_CIPHER_CCMP,	NI_WIRELESS_CIPHER_CCMP },
	{ IW_IE_CIPHER_WRAP,	NI_WIRELESS_CIPHER_WRAP },
	{ IW_IE_CIPHER_WEP104,	NI_WIRELESS_CIPHER_WEP104 },
	{ -1 }
};

static __ni_kernel_map_t	__ni_wireless_key_mgmt_map[] = {
	{ IW_IE_KEY_MGMT_NONE,	NI_WIRELESS_KEY_MGMT_NONE },
	{ IW_IE_KEY_MGMT_PSK,	NI_WIRELESS_KEY_MGMT_PSK },
	{ IW_IE_KEY_MGMT_802_1X,NI_WIRELESS_KEY_MGMT_802_1X },
	{ -1 }
};

static int
__ni_kernel_to_wicked(const __ni_kernel_map_t *map, int value)
{
	while (map->wicked_value >= 0) {
		if (map->kernel_value == value)
			return map->wicked_value;
		map++;
	}
	return -1;
}

static ni_intmap_t __ni_wireless_mode_names[] = {
	{ "unknown",		NI_WIRELESS_MODE_UNKNOWN },
	{ "auto",		NI_WIRELESS_MODE_AUTO },
	{ "adhoc",		NI_WIRELESS_MODE_ADHOC },
	{ "managed",		NI_WIRELESS_MODE_MANAGED },
	{ "master",		NI_WIRELESS_MODE_MASTER },
	{ "repeater",		NI_WIRELESS_MODE_REPEATER },
	{ "secondary",		NI_WIRELESS_MODE_SECONDARY },
	{ "monitor",		NI_WIRELESS_MODE_MONITOR },
	{ NULL }
};

const char *
ni_wireless_mode_to_name(ni_wireless_mode_t mode)
{
	return ni_format_int_mapped(mode, __ni_wireless_mode_names);
}

ni_wireless_mode_t
ni_wireless_name_to_mode(const char *string)
{
	unsigned int value;

	if (ni_parse_int_mapped(string, __ni_wireless_mode_names, &value) < 0)
		return NI_WIRELESS_MODE_UNKNOWN;
	return value;
}

static ni_intmap_t __ni_wireless_security_names[] = {
	{ "default",		NI_WIRELESS_SECURITY_DEFAULT },
	{ "open",		NI_WIRELESS_SECURITY_OPEN },
	{ "restricted",		NI_WIRELESS_SECURITY_RESTRICTED },
	{ NULL }
};

const char *
ni_wireless_security_to_name(ni_wireless_security_t mode)
{
	return ni_format_int_mapped(mode, __ni_wireless_security_names);
}

ni_wireless_security_t
ni_wireless_name_to_security(const char *string)
{
	unsigned int value;

	if (ni_parse_int_mapped(string, __ni_wireless_security_names, &value) < 0)
		return NI_WIRELESS_SECURITY_DEFAULT;
	return value;
}

static ni_intmap_t __ni_wireless_auth_mode_names[] = {
	{ "default",		NI_WIRELESS_AUTH_NONE },
	{ "wpa1",		NI_WIRELESS_AUTH_WPA1 },
	{ "wpa2",		NI_WIRELESS_AUTH_WPA2 },
	{ "unknown",		NI_WIRELESS_AUTH_UNKNOWN },
	{ NULL }
};

const char *
ni_wireless_auth_mode_to_name(ni_wireless_auth_mode_t mode)
{
	return ni_format_int_mapped(mode, __ni_wireless_auth_mode_names);
}

ni_wireless_auth_mode_t
ni_wireless_name_to_auth_mode(const char *string)
{
	unsigned int value;

	if (ni_parse_int_mapped(string, __ni_wireless_auth_mode_names, &value) < 0)
		return -1;
	return value;
}

static ni_intmap_t __ni_wireless_auth_algo_names[] = {
	{ "open",		NI_WIRELESS_AUTH_OPEN },
	{ "shared",		NI_WIRELESS_AUTH_SHARED },
	{ "leap",		NI_WIRELESS_AUTH_LEAP },
	{ NULL }
};

const char *
ni_wireless_auth_algo_to_name(ni_wireless_auth_algo_t algo)
{
	return ni_format_int_mapped(algo, __ni_wireless_auth_algo_names);
}

ni_wireless_auth_algo_t
ni_wireless_name_to_auth_algo(const char *string)
{
	unsigned int value;

	if (ni_parse_int_mapped(string, __ni_wireless_auth_algo_names, &value) < 0)
		return -1;
	return value;
}

static ni_intmap_t __ni_wireless_cipher_names[] = {
	{ "none",		NI_WIRELESS_CIPHER_NONE },
	{ "proprietary",	NI_WIRELESS_CIPHER_PROPRIETARY },
	{ "wep40",		NI_WIRELESS_CIPHER_WEP40 },
	{ "tkip",		NI_WIRELESS_CIPHER_TKIP },
	{ "wrap",		NI_WIRELESS_CIPHER_WRAP },
	{ "ccmp",		NI_WIRELESS_CIPHER_CCMP },
	{ "wep104",		NI_WIRELESS_CIPHER_WEP104 },
	{ NULL }
};

const char *
ni_wireless_cipher_to_name(ni_wireless_cipher_t mode)
{
	return ni_format_int_mapped(mode, __ni_wireless_cipher_names);
}

static ni_intmap_t __ni_wireless_key_mgmt_names[] = {
	{ "none",		NI_WIRELESS_KEY_MGMT_NONE },
	{ "proprietary",	NI_WIRELESS_KEY_MGMT_PROPRIETARY },
	{ "wpa-eap",		NI_WIRELESS_KEY_MGMT_EAP },
	{ "wpa-psk",		NI_WIRELESS_KEY_MGMT_PSK },
	{ "ieee802-1x",		NI_WIRELESS_KEY_MGMT_802_1X },
	{ NULL }
};

const char *
ni_wireless_key_management_to_name(ni_wireless_key_mgmt_t mode)
{
	return ni_format_int_mapped(mode, __ni_wireless_key_mgmt_names);
}

static ni_intmap_t __ni_wireless_eap_method_names[] = {
	{ "md5",	NI_WIRELESS_EAP_MD5	},
	{ "tls",	NI_WIRELESS_EAP_TLS	},
	{ "mschapv2",	NI_WIRELESS_EAP_MSCHAPV2},
	{ "peap",	NI_WIRELESS_EAP_PEAP	},
	{ "ttls",	NI_WIRELESS_EAP_TTLS	},
	{ "gtc",	NI_WIRELESS_EAP_GTC	},
	{ "otp",	NI_WIRELESS_EAP_OTP	},
	{ "leap",	NI_WIRELESS_EAP_LEAP	},
	{ "psk",	NI_WIRELESS_EAP_PSK	},
	{ "pax",	NI_WIRELESS_EAP_PAX	},
	{ "sake",	NI_WIRELESS_EAP_SAKE	},
	{ "gpsk",	NI_WIRELESS_EAP_GPSK	},
	{ "wsc",	NI_WIRELESS_EAP_WSC	},
	{ "ikev2",	NI_WIRELESS_EAP_IKEV2	},
	{ "tnc",	NI_WIRELESS_EAP_TNC	},

	{ NULL }
};

const char *
ni_wireless_eap_method_to_name(ni_wireless_eap_method_t mode)
{
	return ni_format_int_mapped(mode, __ni_wireless_eap_method_names);
}

ni_wireless_eap_method_t
ni_wireless_name_to_eap_method(const char *string)
{
	unsigned int value;

	if (ni_parse_int_mapped(string, __ni_wireless_eap_method_names, &value) < 0)
		return -1;
	return value;
}

/*
 * Key index is 1-based.
 */
static inline void
__ni_wireless_set_key_index(unsigned int *key_index, unsigned int value)
{
	value &= IW_ENCODE_INDEX;
	*key_index = (value > 1)? value - 1 : 0;
}

/*
 * Process information elements
 */
static inline int
__ni_wireless_process_ie_cipher(ni_buffer_t *bp, const unsigned char *wpa_oui, ni_wireless_cipher_t *result)
{
	unsigned char buffer[4];

	if (ni_buffer_get(bp, buffer, 4) < 0)
		return -1;

	*result = NI_WIRELESS_CIPHER_PROPRIETARY;
	if (memcmp(buffer, wpa_oui, 3) == 0) {
		int mapped = __ni_kernel_to_wicked(__ni_wireless_cipher_map, buffer[3]);
		if (mapped >= 0)
			*result = mapped;
	}

	return 0;
}

static inline int
__ni_wireless_process_ie_key_mgmt(ni_buffer_t *bp, const unsigned char *wpa_oui, ni_wireless_key_mgmt_t *result)
{
	unsigned char buffer[4];

	if (ni_buffer_get(bp, buffer, 4) < 0)
		return -1;

	*result = NI_WIRELESS_KEY_MGMT_PROPRIETARY;
	if (memcmp(buffer, wpa_oui, 3) == 0) {
		int mapped = __ni_kernel_to_wicked(__ni_wireless_key_mgmt_map, buffer[3]);
		if (mapped >= 0)
			*result = mapped;
	}

	return 0;
}

static inline int
__ni_buffer_get_le16(ni_buffer_t *bp)
{
	unsigned char temp[2];

	if (ni_buffer_get(bp, temp, 2) < 0)
		return -1;
	return temp[0] | (temp[1] << 8);
}

static inline int
__ni_wireless_process_wpa_common(ni_wireless_network_t *net, ni_buffer_t *bp,
			ni_wireless_auth_mode_t auth_mode, const unsigned char *wpa_oui)
{
	ni_wireless_auth_info_t *auth;
	int version, count;

	if ((version = __ni_buffer_get_le16(bp)) < 0)
		return -1;

	auth = ni_wireless_auth_info_new(auth_mode, version);
	ni_wireless_auth_info_array_append(&net->scan_info.supported_auth_modes, auth);

	/* Everything else is optional, so failure to get sufficient
	 * data from the buffer is non-terminal. */
	if (__ni_wireless_process_ie_cipher(bp, wpa_oui, &auth->group_cipher) < 0)
		return 0;

	/* Array of pairwise ciphers */
	if ((count = __ni_buffer_get_le16(bp)) < 0)
		return 0;

	/* Clear default list of pairwise ciphers */
	auth->pairwise_ciphers = 0;
	while (count--) {
		ni_wireless_cipher_t cipher;

		if (__ni_wireless_process_ie_cipher(bp, wpa_oui, &cipher) < 0)
			return -1;
		ni_wireless_auth_info_add_pairwise_cipher(auth, cipher);
	}

	/* Array of auth suites */
	if ((count = __ni_buffer_get_le16(bp)) < 0)
		return 0;

	while (count--) {
		ni_wireless_key_mgmt_t algo;

		if (__ni_wireless_process_ie_key_mgmt(bp, wpa_oui, &algo) < 0)
			return -1;
		ni_wireless_auth_info_add_key_management(auth, algo);
	}

	return 0;
}

static inline int
__ni_wireless_process_wpa1(ni_wireless_network_t *net, void *ptr, size_t len)
{
	static unsigned char wpa1_oui[] = {0x00, 0x50, 0xf2};
	unsigned char buffer[3];
	ni_buffer_t data;

	ni_buffer_init_reader(&data, ptr, len);
	if (ni_buffer_get(&data, buffer, 3) < 0)
		return -1;

	if (memcmp(buffer, wpa1_oui, 3)) {
		ni_debug_ifconfig("skipping non-WPA1 IE (OUI=%02x:%02x:%02x)",
				buffer[0], buffer[1], buffer[0]);
		return 0;
	}

	if (ni_buffer_get(&data, buffer, 1) < 0)
		return -1;
	if (buffer[0] != 0x01)
		return 0;

	return __ni_wireless_process_wpa_common(net, &data, NI_WIRELESS_AUTH_WPA1, wpa1_oui);
}

static inline int
__ni_wireless_process_wpa2(ni_wireless_network_t *net, void *ptr, size_t len)
{
	static unsigned char wpa2_oui[] = {0x00, 0x0f, 0xac};
	ni_buffer_t data;

	ni_buffer_init_reader(&data, ptr, len);
	return __ni_wireless_process_wpa_common(net, &data, NI_WIRELESS_AUTH_WPA2, wpa2_oui);
}

int
__ni_wireless_process_ie(ni_wireless_network_t *net, void *ptr, size_t len)
{
	ni_buffer_t data;

	ni_buffer_init_reader(&data, ptr, len);
	while (ni_buffer_count(&data) >= 2) {
		unsigned char type, len;
		int rv = -1;

		if (ni_buffer_get(&data, &type, 1) < 0
		 || ni_buffer_get(&data, &len, 1) < 0)
			goto format_error;

		if (ni_buffer_count(&data) < len)
			goto format_error;
		ptr = ni_buffer_head(&data);
		data.head += len;

		switch (type) {
		case 0xdd:
			rv = __ni_wireless_process_wpa1(net, ptr, len);
			break;

		case 0x30:
			rv = __ni_wireless_process_wpa2(net, ptr, len);
			break;

		default:
			ni_debug_wireless("Skipping unsupported Informaton Element 0x%02x", type);
			continue;
		}
		if (rv < 0)
			return -1;
	}
	return 0;

format_error:
	ni_error("error processing wireless Information Elements");
	return -1;
}

/*
 * Extract information from wireless scan result
 */
/*
 * Wireless interface config
 */
ni_wireless_t *
ni_wireless_new(void)
{
	return xcalloc(1, sizeof(ni_wireless_t));
}

void
ni_wireless_free(ni_wireless_t *wireless)
{
	ni_wireless_set_assoc_network(wireless, NULL);
	free(wireless);
}

void
ni_wireless_set_assoc_network(ni_wireless_t *wireless, ni_wireless_network_t *net)
{
	if (wireless->assoc.network)
		ni_wireless_network_put(wireless->assoc.network);
	wireless->assoc.network = net? ni_wireless_network_get(net) : NULL;
}

/*
 * Wireless scan objects
 */
ni_wireless_scan_t *
ni_wireless_scan_new(void)
{
	ni_wireless_scan_t *scan;

	scan = xcalloc(1, sizeof(ni_wireless_scan_t));
	scan->max_age = NI_WIRELESS_SCAN_MAX_AGE;
	scan->timestamp = time(NULL);
	scan->lifetime = 60;

	return scan;
}

void
ni_wireless_scan_free(ni_wireless_scan_t *scan)
{
	ni_wireless_network_array_destroy(&scan->networks);
}

/*
 * Wireless network objects
 */
ni_wireless_network_t *
ni_wireless_network_new(void)
{
	ni_wireless_network_t *net;

	net = xcalloc(1, sizeof(ni_wireless_network_t));
	net->refcount = 1;
	return net;
}

void
ni_wireless_network_set_key(ni_wireless_network_t *net, const unsigned char *key_data, size_t key_len)
{
	if (net->encode.key_data) {
		memset(net->encode.key_data, 0, net->encode.key_len);
		free(net->encode.key_data);
		net->encode.key_data = NULL;
		net->encode.key_len = 0;
	}

	if (key_len) {
		net->encode.key_data = malloc(key_len);
		net->encode.key_len = key_len;
		memcpy(net->encode.key_data, key_data, key_len);
	}
}

void
__ni_wireless_network_destroy(ni_wireless_network_t *net)
{
	ni_assert(net->refcount == 0);
	ni_wireless_network_set_key(net, NULL, 0);
	ni_wireless_auth_info_array_destroy(&net->scan_info.supported_auth_modes);
	memset(net, 0, sizeof(*net));
}

void
ni_wireless_network_free(ni_wireless_network_t *net)
{
	__ni_wireless_network_destroy(net);
	free(net);
}

/*
 * Wireless network arrays
 */
void
ni_wireless_network_array_init(ni_wireless_network_array_t *array)
{
	memset(array, 0, sizeof(*array));
}

void
ni_wireless_network_array_append(ni_wireless_network_array_t *array, ni_wireless_network_t *net)
{
	array->data = realloc(array->data, (array->count + 1) * sizeof(ni_wireless_network_t *));
	array->data[array->count++] = ni_wireless_network_get(net);
}

void
ni_wireless_network_array_destroy(ni_wireless_network_array_t *array)
{
	unsigned int i;

	for (i = 0; i < array->count; ++i)
		ni_wireless_network_put(array->data[i]);
	free(array->data);
	memset(array, 0, sizeof(*array));
}

/*
 * Wireless auth info
 */
ni_wireless_auth_info_t *
ni_wireless_auth_info_new(ni_wireless_auth_mode_t mode, unsigned int version)
{
	ni_wireless_auth_info_t *auth;

	auth = xcalloc(1, sizeof(*auth));
	auth->mode = mode;
	auth->version = version;
	auth->group_cipher = NI_WIRELESS_CIPHER_TKIP;
	auth->pairwise_ciphers = (1 << NI_WIRELESS_CIPHER_TKIP);

	return auth;
}

void
ni_wireless_auth_info_add_pairwise_cipher(ni_wireless_auth_info_t *auth, ni_wireless_cipher_t cipher)
{
	auth->pairwise_ciphers |= (1 << cipher);
}

void
ni_wireless_auth_info_add_key_management(ni_wireless_auth_info_t *auth, ni_wireless_key_mgmt_t algo)
{
	auth->keymgmt_algos |= 1 << algo;
}

void
ni_wireless_auth_info_free(ni_wireless_auth_info_t *auth)
{
	free(auth);
}

void
ni_wireless_auth_info_array_init(ni_wireless_auth_info_array_t *array)
{
	memset(array, 0, sizeof(*array));
}

void
ni_wireless_auth_info_array_append(ni_wireless_auth_info_array_t *array, ni_wireless_auth_info_t *auth)
{
	array->data = realloc(array->data, (array->count + 1) * sizeof(auth));
	array->data[array->count++] = auth;
}

void
ni_wireless_auth_info_array_destroy(ni_wireless_auth_info_array_t *array)
{
	unsigned int i;

	for (i = 0; i < array->count; ++i)
		ni_wireless_auth_info_free(array->data[i]);
	memset(array, 0, sizeof(*array));
}

/*
 * Helper function to print and parse an SSID
 * Non-printable characters and anything fishy is represented
 * as \\ooo octal escape characters
 */
const char *
ni_wireless_print_ssid(const ni_wireless_ssid_t *ssid)
{
	static char result[4 * sizeof(ssid->data) + 1];
	unsigned int i, j;

	ni_assert(ssid->len <= sizeof(ssid->data));

	for (i = j = 0; i < ssid->len; ++i) {
		unsigned char cc = ssid->data[i];

		if (isalnum(cc) || cc == '-' || cc == '_' || cc == ' ') {
			result[j++] = cc;
		} else {
			sprintf(result + j, "\\%03o", cc);
			j += 4;
		}
	}
	result[j] = '\0';

	return result;
}

