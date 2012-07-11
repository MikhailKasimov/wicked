/*
 *	DHCP6 supplicant -- client device
 *
 *	Copyright (C) 2010-2012 Olaf Kirch <okir@suse.de>
 *	Copyright (C) 2012 Marius Tomaschewski <mt@suse.de>
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
 *	You should have received a copy of the GNU General Public License along
 *	with this program; if not, see <http://www.gnu.org/licenses/> or write
 *	to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *	Boston, MA 02110-1301 USA.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/time.h>
#include <net/if_arp.h>
#include <arpa/inet.h>

#include <wicked/util.h>
#include <wicked/netinfo.h>
#include <wicked/logging.h>

#include "dhcp6/dhcp6.h"
#include "dhcp6/device.h"
#include "dhcp6/protocol.h"
#include "dhcp6/duid.h"
#include "dhcp6/fsm.h"

#include "appconfig.h"
#include "socket_priv.h"
#include "util_priv.h"


/*
 * DHCP6 package name and version based on config.h
 */
#ifndef NI_DHCP6_PACKAGE_NAME
#ifndef PACKAGE_NAME
#define	NI_DHCP6_PACKAGE_NAME			"wicked-dhcp6"
#else
#define	NI_DHCP6_PACKAGE_NAME			PACKAGE_NAME "-dhcp6"
#endif
#endif
#ifndef NI_DHCP6_PACKAGE_VERSION
#ifndef PACKAGE_VERSION
#define	NI_DHCP6_PACKAGE_VERSION		"0.0.0"
#else
#define	NI_DHCP6_PACKAGE_VERSION		PACKAGE_VERSION
#endif
#endif

/*
 * Default Vendor enterprise number + data in <name>/<version> format.
 *
 * http://www.iana.org/assignments/enterprise-numbers
 */
#ifndef NI_DHCP6_VENDOR_ENTERPRISE_NUMBER
#define	NI_DHCP6_VENDOR_ENTERPRISE_NUMBER	7075	/* SUSE */
#endif
#ifndef NI_DHCP6_VENDOR_VERSION_STRING
#define NI_DHCP6_VENDOR_VERSION_STRING		NI_DHCP6_PACKAGE_NAME"/"NI_DHCP6_PACKAGE_VERSION
#endif

extern int			ni_dhcp6_load_duid(ni_opaque_t *duid, const char *filename);
extern int			ni_dhcp6_save_duid(const ni_opaque_t *duid, const char *filename);

extern void			ni_dhcp6_fsm_set_timeout_msec(ni_dhcp6_device_t *dev, unsigned long msec);


ni_dhcp6_device_t *		ni_dhcp6_active;

static void			ni_dhcp6_device_close(ni_dhcp6_device_t *);
static void			ni_dhcp6_device_free(ni_dhcp6_device_t *);

static void			ni_dhcp6_device_set_config(ni_dhcp6_device_t *, ni_dhcp6_config_t *);

#if 0
//static int			ni_dhcp6_device_start(ni_dhcp6_device_t *dev);

static int			ni_dhcp6_device_refresh(ni_dhcp6_device_t *dev);
#endif

static ni_bool_t		ni_dhcp6_device_transmit_arm_delay(ni_dhcp6_device_t *);

static ni_bool_t		ni_dhcp6_device_retransmit_advance(ni_dhcp6_device_t *);

static int			ni_dhcp6_init_message(ni_dhcp6_device_t *dev, unsigned int msg_code,
							const ni_addrconf_lease_t *lease);



/*
 * Create and destroy dhcp6 device handles
 */
ni_dhcp6_device_t *
ni_dhcp6_device_new(const char *ifname, const ni_linkinfo_t *link)
{
	ni_dhcp6_device_t *dev, **pos;

	for (pos = &ni_dhcp6_active; (dev = *pos) != NULL; pos = &dev->next)
		;

	dev = xcalloc(1, sizeof(*dev));
	dev->users = 1;

	ni_string_dup(&dev->ifname, ifname);
	dev->link.type		= link->type;
	dev->link.ifindex	= link->ifindex;
	dev->link.ifflags	= link->ifflags;
	ni_string_dup(&dev->link.alias, link->alias);

	dev->link.arp_type	= link->arp_type;
	memcpy(&dev->link.hwaddr, &link->hwaddr, sizeof(dev->link.hwaddr));

	ni_timer_get_time(&dev->start_time);
	dev->fsm.state = NI_DHCP6_STATE_INIT;

	/* append to end of list */
	*pos = dev;

	return dev;
}

ni_dhcp6_device_t *
ni_dhcp6_device_by_index(unsigned int ifindex)
{
	ni_dhcp6_device_t *dev;

	for (dev = ni_dhcp6_active; dev; dev = dev->next) {
		if ((unsigned int)dev->link.ifindex == ifindex)
			return dev;
	}

	return NULL;
}

/*
 * Refcount handling
 */
ni_dhcp6_device_t *
ni_dhcp6_device_get(ni_dhcp6_device_t *dev)
{
	ni_assert(dev->users);
	dev->users++;
	return dev;
}

void
ni_dhcp6_device_put(ni_dhcp6_device_t *dev)
{
	ni_assert(dev->users);
	if (--(dev->users) == 0)
		ni_dhcp6_device_free(dev);
}


/*
 * Cleanup functions
 */
static void
ni_dhcp6_device_close(ni_dhcp6_device_t *dev)
{
	if (dev->sock)
		ni_socket_close(dev->sock);
	dev->sock = NULL;

	if (dev->fsm.timer) {
		ni_warn("%s: timer active for %s", __func__, dev->ifname);
		ni_timer_cancel(dev->fsm.timer);
		dev->fsm.timer = NULL;
	}
}

void
ni_dhcp6_device_stop(ni_dhcp6_device_t *dev)
{
	/* Clear the lease. This will trigger an event to wickedd
	 * with a lease that has state RELEASED. */
#if 0
	ni_dhcp6_fsm_commit_lease(dev, NULL);
#endif
	ni_dhcp6_device_close(dev);

	/* Drop existing config and request */
	ni_dhcp6_device_set_config(dev, NULL);
	ni_dhcp6_device_set_request(dev, NULL);
}

static void
ni_dhcp6_device_free(ni_dhcp6_device_t *dev)
{
	ni_dhcp6_device_t **pos;

	ni_assert(dev->users == 0);
#if 0
	ni_dhcp6_device_drop_buffer(dev);
	ni_dhcp6_device_drop_lease(dev);
	ni_dhcp6_device_drop_best_offer(dev);
#endif
	ni_dhcp6_device_close(dev);

	ni_string_free(&dev->ifname);
	ni_string_free(&dev->link.alias);

	/* Drop existing config and request */
	ni_dhcp6_device_set_config(dev, NULL);
	ni_dhcp6_device_set_request(dev, NULL);

	for (pos = &ni_dhcp6_active; *pos; pos = &(*pos)->next) {
		if (*pos == dev) {
			*pos = dev->next;
			break;
		}
	}
	free(dev);
}


/*
 * Device handle request set helper
 */
void
ni_dhcp6_device_set_request(ni_dhcp6_device_t *dev, ni_dhcp6_request_t *request)
{
	if(dev->request)
		ni_dhcp6_request_free(dev->request);
	dev->request = request;
}

/*
 * Device handle config set helper
 */
static void
__ni_dhcp6_device_config_free(ni_dhcp6_config_t *config)
{
	if (config) {
		ni_string_array_destroy(&config->user_class);
		ni_string_array_destroy(&config->vendor_class.data);
		ni_var_array_destroy(&config->vendor_opts.data);
		ni_string_free(&config->client_id);
		free(config);
	}
}

static void
ni_dhcp6_device_set_config(ni_dhcp6_device_t *dev, ni_dhcp6_config_t *config)
{
	if (dev->config)
		__ni_dhcp6_device_config_free(dev->config);
	dev->config = config;
}


unsigned int
ni_dhcp6_device_uptime(const ni_dhcp6_device_t *dev, unsigned int clamp)
{
	struct timeval now;
	struct timeval delta;
	long           uptime = 0;

	ni_timer_get_time(&now);
	if (timerisset(&dev->start_time) && timercmp(&now, &dev->start_time, >)) {
		timersub(&now, &dev->start_time, &delta);

		/* uptime in hundredths of a second (10^-2 seconds) */
		uptime = (delta.tv_sec * 100 + delta.tv_usec / 10000);
	}

	ni_trace("Uptime is %ld (1/100 sec) => %lu", uptime, (long)((uptime < clamp) ? uptime/100 : clamp));

	return (uptime < clamp) ? uptime : clamp;
}

int
ni_dhcp6_device_iaid(const ni_dhcp6_device_t *dev, uint32_t *iaid)
{
	size_t len, off;
	uint32_t tmp;

	/* FIXME: simple iaid with 4 last byte of the mac */

	*iaid = 0;
	if (dev->link.hwaddr.len > 4) {
		off = dev->link.hwaddr.len - 4;
		memcpy(iaid, dev->link.hwaddr.data + off, sizeof(*iaid));
		return 0;
	}
	if ((len = ni_string_len(dev->ifname))) {
		memcpy(&tmp, dev->ifname, len % sizeof(tmp));
		*iaid ^= tmp;
		*iaid ^= dev->link.ifindex;
		return 0;
	}
	return -1;
}



#if 0
static int
ni_dhcp6_device_start(ni_dhcp6_device_t *dev)
{
	ni_dhcp6_device_drop_lease(dev);
	ni_dhcp6_device_drop_buffer(dev);
	dev->failed = 0;

	if (ni_dhcp6_fsm_discover(dev) < 0) {
		ni_error("unable to initiate discovery");
		return -1;
	}

	return 0;
}
#endif



void
ni_dhcp6_restart(void)
{
	ni_dhcp6_device_t *dev;

	for (dev = ni_dhcp6_active; dev; dev = dev->next) {
		if (dev->request) {
			ni_trace("restarting acquire %s on dev %s",
				(dev->request->info_only ? "info" : "lease"),
				dev->ifname);
			ni_dhcp6_acquire(dev, dev->request);
		}
	}
}

/*
 * Refresh the device info prior to taking any actions
 */
static int
ni_dhcp6_device_refresh(ni_dhcp6_device_t *dev)
{
	ni_netconfig_t *nc;
	ni_netdev_t *ifp;
	ni_address_t *addr;
	int rv = -1;

	nc = ni_global_state_handle(0);
	if(!nc || !(ifp = ni_netdev_by_index(nc, dev->link.ifindex)))
		return -1;

	for(addr = ifp->addrs; addr; addr = addr->next) {
		if(addr->family != AF_INET6)
			continue;

		/* FIXME: ignore tentative and dad-failed addresses */

		if (IN6_IS_ADDR_LINKLOCAL(&addr->local_addr.six.sin6_addr)) {
			ni_trace("Found link-local address %s",
					ni_address_print(&addr->local_addr));
			memcpy(&dev->client_addr, &addr->local_addr, sizeof(dev->client_addr));
			rv = 0;
			break;
		}
	}

	return rv;
}

static ni_bool_t
ni_dhcp6_device_transmit_arm_delay(ni_dhcp6_device_t *dev)
{
	dev->tx_counter = 0;

	if ((dev->tx_delay = dev->retrans.params.delay)) {
		long jitter;

		jitter = ni_dhcp6_timeout_jitter(dev->retrans.params.max_jitter,
						 dev->retrans.params.max_jitter);

		dev->tx_delay += jitter;

		ni_trace("transmit arm delay %ld +/- %ld", dev->tx_delay, jitter);

		if (dev->tx_delay) {
			ni_dhcp6_fsm_set_timeout_msec(dev, dev->tx_delay);
			return TRUE;
		}
	}
	return FALSE;
}

void
ni_dhcp6_device_retransmit_arm(ni_dhcp6_device_t *dev)
{
	unsigned long timeout;

	dev->tx_delay = 0;	/* initial transmit is done here */

	ni_timer_get_time(&dev->retrans.start);

	/*
	 ** rfc3315: 17.1.2. Transmission of Solicit Messages
	 ** [...]
	 ** the first RT MUST be selected to be strictly greater than
	 ** IRT by choosing RAND to be strictly greater than 0.
	 ** [...]
	 */
	if (!(dev->tx_counter > 1) && dev->retrans.params.pos_jitter) {
		timeout = ni_dhcp6_timeout_arm_msec(&dev->retrans.deadline,
					dev->retrans.params.timeout,
					0,
					dev->retrans.params.max_jitter);
	} else {
		timeout = ni_dhcp6_timeout_arm_msec(&dev->retrans.deadline,
					dev->retrans.params.timeout,
					dev->retrans.params.max_jitter,
					dev->retrans.params.max_jitter);
	}
	dev->retrans.timeout = timeout;

	ni_trace("retransmit timeout: %lu", dev->retrans.timeout);

	if (dev->retrans.params.max_duration) {
		/*
		 * rfc3315#section-14
		 * "[...]
		 * MRD specifies an upper bound on the length of time a client may
		 * retransmit a message. Unless MRD is zero, the message exchange
		 * fails once MRD seconds have elapsed since the client first
		 * transmitted the message.
		 * [...]"
		 */
		ni_trace("retransmit duration deadline: %lu", dev->retrans.params.max_duration);
		ni_dhcp6_fsm_set_timeout_msec(dev, dev->retrans.params.max_duration);
	}

	ni_trace("retransmit start at %ld.%ld, rt deadline: %ld.%ld [timeout=%lu]",
			dev->retrans.start.tv_sec, dev->retrans.start.tv_usec,
			dev->retrans.deadline.tv_sec,dev->retrans.deadline.tv_usec,
			timeout);
}

void
ni_dhcp6_device_retransmit_disarm(ni_dhcp6_device_t *dev)
{
	memset(&dev->retrans, 0, sizeof(dev->retrans));
}

static ni_bool_t
ni_dhcp6_device_retransmit_advance(ni_dhcp6_device_t *dev)
{
	if (dev->retrans.params.max_retransmits) {
		/*
		 * rfc3315#section-14
		 * "[...]
		 * MRC specifies an upper bound on the number of times a client may
		 * retransmit a message.  Unless MRC is zero, the message exchange
		 * fails once the client has transmitted the message MRC times.
		 * [...]"
		 *
		 * Hmm... max transmits (1 + retransmits) or retransmits ??
		 * MRC 0 means no limit, I'm using MDC 1 to transmit once...
		 */
		if (dev->tx_counter >= dev->retrans.params.max_retransmits)
			return FALSE;
	}

	if (timerisset(&dev->retrans.deadline)) {
		/*
		 * rfc3315#section-14
		 * "[...]
		 * RT for each subsequent message transmission is based on the previous
		 * value of RT:
		 *
		 * 	RT = 2*RTprev + RAND*RTprev
		 *
		 * MRT specifies an upper bound on the value of RT (disregarding the
		 * randomization added by the use of RAND).  If MRT has a value of 0,
		 * there is no upper limit on the value of RT.  Otherwise:
		 *
		 * 	if (RT > MRT)
		 * 		RT = MRT + RAND*MRT
		 * [...]"
		 */
		dev->retrans.timeout *= 2;

		if(dev->retrans.params.max_timeout) {
			if(dev->retrans.timeout > dev->retrans.params.max_timeout)
				dev->retrans.timeout = dev->retrans.params.max_timeout;
		}

		dev->retrans.timeout = ni_dhcp6_timeout_arm_msec(
						&dev->retrans.deadline,
						dev->retrans.timeout,
						dev->retrans.params.max_jitter,
						dev->retrans.params.max_jitter);

		return TRUE;
	}
	return FALSE;
}

int
ni_dhcp6_device_retransmit(ni_dhcp6_device_t *dev)
{
	int rv;

	if (!ni_dhcp6_device_retransmit_advance(dev)) {
		ni_dhcp6_device_retransmit_disarm(dev);
		return -1;
	}

        if (dev->config->info_only) {
        	if ((rv = ni_dhcp6_build_message(dev, NI_DHCP6_INFO_REQUEST, NULL, &dev->message)) != 0)
        		return rv;
        }
#if 0
        else if(have_valid_lease) {
        	if ((rv = ni_dhcp6_build_message(dev, NI_DHCP6_CONFIRM, dev->lease, &dev->message)) != 0)
        	        		return rv;
        }
#endif
        else {
        	if ((rv = ni_dhcp6_build_message(dev, NI_DHCP6_SOLICIT, dev->lease, &dev->message)) != 0)
        	        		return rv;
        }

	if ((rv = ni_dhcp6_device_transmit(dev)) != 0)
		return rv;

	ni_trace("retransmitted, deadline: %ld.%ld",
		dev->retrans.deadline.tv_sec,dev->retrans.deadline.tv_usec);

	return -1;
}

void
ni_dhcp6_generate_duid(ni_opaque_t *duid, ni_dhcp6_device_t *dev)
{
	ni_netconfig_t *nc;
	ni_netdev_t *ifp;
	ni_uuid_t uuid;

	if (dev->link.hwaddr.len) {
		if(ni_duid_init_llt(duid, dev->link.arp_type,
				dev->link.hwaddr.data, dev->link.hwaddr.len))
			return;
	}

	nc = ni_global_state_handle(0);

	for (ifp = ni_netconfig_devlist(nc); ifp; ifp = ifp->next) {
		switch(ifp->link.arp_type) {
		case ARPHRD_ETHER:
		case ARPHRD_IEEE802:
		case ARPHRD_INFINIBAND:
			if (dev->link.hwaddr.len) {
				if(ni_duid_init_llt(duid, dev->link.arp_type,
						dev->link.hwaddr.data, dev->link.hwaddr.len))
					return;
			}
		break;
		}
	}

	/*
	 * TODO:
	 * 1) MAC based uuid duid, see
	 *    http://tools.ietf.org/html/rfc4122#section-4.1.6
	 * 2) There should be some system unique uuid at least on x86_64
	 */
	memset(&uuid, 0, sizeof(uuid));
	ni_uuid_generate(&uuid);
	ni_duid_init_uuid(duid, &uuid);
}

ni_bool_t
ni_dhcp6_init_duid(ni_opaque_t *duid, ni_dhcp6_device_t *dev, const char *preferred)
{
	ni_bool_t save = TRUE;

	if (preferred) {
		ni_duid_parse_hex(duid, preferred);
	}
	if (duid->len == 0) {
		ni_dhcp6_config_default_duid(duid);
	}

	if (duid->len == 0) {
		if( ni_dhcp6_load_duid(duid, NULL) == 0)
			save = FALSE;
	}
	if (duid->len == 0) {
		ni_dhcp6_generate_duid(duid, dev);
	}

	if (duid->len && save) {
		(void)ni_dhcp6_save_duid(duid, NULL);
	}
	return (duid->len > 0);
}

/*
 * Process a request to reconfigure the device (ie rebind a lease, or discover
 * a new lease).
 */
int
ni_dhcp6_acquire(ni_dhcp6_device_t *dev, const ni_dhcp6_request_t *info)
{
	ni_dhcp6_config_t *config;
        int rv;

        if ((rv = ni_dhcp6_device_refresh(dev)) < 0) {
		ni_error("%s: unable to refresh interface", dev->ifname);
                return rv;
        }

	config = xcalloc(1, sizeof(*config));
	config->uuid = info->uuid;
	config->update = info->update;

	config->info_only = info->info_only;
	config->rapid_commit = info->rapid_commit;

        /*
         * Make sure we have a DUID for client-id
         */
	if(!ni_dhcp6_init_duid(&config->client_duid, dev, info->clientid)) {
		ni_error("Unable to find usable or generate client duid");
		return -1;
	}

	if (info->hostname) {
		strncpy(config->hostname, info->hostname, sizeof(config->hostname) - 1);
	}

	/* TODO: get from req info */
	ni_dhcp6_config_vendor_class(&config->vendor_class.en, &config->vendor_class.data);
	ni_dhcp6_config_vendor_opts(&config->vendor_opts.en, &config->vendor_opts.data);

	ni_dhcp6_device_set_config(dev, config);

        if (config->info_only) {
        	if ((rv = ni_dhcp6_init_message(dev, NI_DHCP6_INFO_REQUEST, NULL)) != 0)
        		return rv;
        }
#if 0
        else if(have_valid_lease) {
        	if ((rv = ni_dhcp6_init_message(dev, NI_DHCP6_CONFIRM, NULL)) != 0)
        	        		return rv;
        }
#endif
        else {
        	if ((rv = ni_dhcp6_init_message(dev, NI_DHCP6_SOLICIT, NULL)) != 0)
        	        		return rv;
        }

	if (ni_dhcp6_device_transmit_arm_delay(dev))
		return 0;

	if ((rv = ni_dhcp6_device_transmit(dev)) != 0)
		return rv;

	ni_dhcp6_device_retransmit_arm(dev);

	ni_trace("transmitted, retrans deadline: %ld.%ld",
		dev->retrans.deadline.tv_sec,dev->retrans.deadline.tv_usec);

	return rv;

#if 0
	config->max_lease_time = ni_dhcp6_config_max_lease_time();
	if (config->max_lease_time == 0)
		config->max_lease_time = ~0U;
	if (info->lease_time && info->lease_time < config->max_lease_time)
		config->max_lease_time = info->lease_time;

	if (info->hostname)
		strncpy(config->hostname, info->hostname, sizeof(config->hostname) - 1);

	if (info->clientid) {
		strncpy(config->client_id, info->clientid, sizeof(config->client_id)-1);
		ni_dhcp6_parse_client_id(&config->raw_client_id, dev->link.type, info->clientid);
	} else {
		/* Set client ID from interface hwaddr */
		strncpy(config->client_id, ni_link_address_print(&dev->system.hwaddr), sizeof(config->client_id)-1);
		ni_dhcp6_set_client_id(&config->raw_client_id, &dev->system.hwaddr);
	}

	if ((classid = info->vendor_class) == NULL)
		classid = ni_dhcp6_config_vendor_class();
	if (classid)
		strncpy(config->classid, classid, sizeof(config->classid) - 1);

	config->flags = DHCP6_DO_ARP | DHCP6_DO_CSR | DHCP6_DO_MSCSR;
	config->flags |= ni_dhcp6_do_bits(info->update);

	if (ni_debug & NI_TRACE_DHCP6) {
		ni_trace("Received request:");
		ni_trace("  acquire-timeout %u", config->request_timeout);
		ni_trace("  lease-time      %u", config->max_lease_time);
		ni_trace("  hostname        %s", config->hostname[0]? config->hostname : "<none>");
		ni_trace("  vendor-class    %s", config->classid[0]? config->classid : "<none>");
		ni_trace("  client-id       %s", ni_print_hex(config->raw_client_id.data, config->raw_client_id.len));
		ni_trace("  uuid            %s", ni_print_hex(config->uuid.octets, 16));
		ni_trace("  flags           %s", __ni_dhcp6_print_flags(config->flags));
	}

	if (dev->config)
		free(dev->config);
	dev->config = config;

#if 0
	/* FIXME: This cores for now */
	/* If we're asked to reclaim an existing lease, try to load it. */
	if (info->reuse_unexpired && ni_dhcp6_fsm_recover_lease(dev, info) >= 0)
		return 0;
#endif

	if (dev->lease) {
		if (!ni_addrconf_lease_is_valid(dev->lease)
		 || (info->hostname && !ni_string_eq(info->hostname, dev->lease->hostname))
		 || (info->clientid && !ni_string_eq(info->clientid, dev->lease->dhcp6.client_id))) {
			ni_debug_dhcp6("%s: lease doesn't match request", dev->ifname);
			ni_dhcp6_device_drop_lease(dev);
			dev->notify = 1;
		}
	}

	/* Go back to INIT state to force a rediscovery */
	dev->fsm.state = NI_DHCP6_STATE_INIT;
	ni_dhcp6_device_start(dev);
	return 1;
#endif
}


#if 0
/*
 * Translate a bitmap of NI_ADDRCONF_UPDATE_* flags into a bitmap of
 * DHCP6_DO_* masks
 */
static unsigned int
ni_dhcp6_do_bits(unsigned int update_flags)
{
	static unsigned int	do_mask[32] = {
	[NI_ADDRCONF_UPDATE_HOSTNAME]		= DHCP6_DO_HOSTNAME,
	[NI_ADDRCONF_UPDATE_RESOLVER]		= DHCP6_DO_RESOLVER,
	[NI_ADDRCONF_UPDATE_NIS]		= DHCP6_DO_NIS,
	[NI_ADDRCONF_UPDATE_NTP]		= DHCP6_DO_NTP,
	[NI_ADDRCONF_UPDATE_DEFAULT_ROUTE]	= DHCP6_DO_GATEWAY,
	};
	unsigned int bit, result = 0;

	for (bit = 0; bit < 32; ++bit) {
		if (update_flags & (1 << bit))
			result |= do_mask[bit];
	}
	return result;
}

static const char *
__ni_dhcp6_print_flags(unsigned int flags)
{
	static ni_intmap_t flag_names[] = {
	{ "arp",		DHCP6_DO_ARP		},
	{ "csr",		DHCP6_DO_CSR		},
	{ "mscsr",		DHCP6_DO_MSCSR		},
	{ "hostname",		DHCP6_DO_HOSTNAME	},
	{ "resolver",		DHCP6_DO_RESOLVER	},
	{ "nis",		DHCP6_DO_NIS		},
	{ "ntp",		DHCP6_DO_NTP		},
	{ "gateway",		DHCP6_DO_GATEWAY		},
	{ NULL }
	};
	static char buffer[1024];
	char *pos = buffer;
	unsigned int mask;

	*pos = '\0';
	for (mask = 1; mask != 0; mask <<= 1) {
		const char *name;

		if ((flags & mask) == 0)
			continue;
		if (!(name = ni_format_int_mapped(mask, flag_names)))
			continue;
		snprintf(pos, buffer + sizeof(buffer) - pos, "%s%s",
				(pos == buffer)? "" : ", ",
				name);
		pos += strlen(pos);
	}
	if (buffer[0] == '\0')
		return "<none>";

	return buffer;
}
#endif

/*
 * Process a request to unconfigure the device (ie drop the lease).
 */
int
ni_dhcp6_release(ni_dhcp6_device_t *dev, const ni_uuid_t *lease_uuid)
{
#if 0
	int rv;

	if (dev->lease == NULL) {
		ni_error("%s(%s): no lease set", __func__, dev->ifname);
		return -NI_ERROR_ADDRCONF_NO_LEASE;
	}

	if (lease_uuid) {
		/* FIXME: We should check the provided uuid against the
		 * lease's uuid, and refuse the call if it doesn't match
		 */
	}

	/* We just send out a singe RELEASE without waiting for the
	 * server's reply. We just keep our fingers crossed that it's
	 * getting out. If it doesn't, it's rather likely the network
	 * is hosed anyway, so there's little point in delaying. */
	if ((rv = ni_dhcp6_fsm_release(dev)) < 0)
		return rv;
	ni_dhcp6_device_stop(dev);
#endif
	return 0;
}

/*
 * Handle link up/down events
 */
void
ni_dhcp6_device_event(ni_dhcp6_device_t *dev, ni_event_t event)
{
	switch (event) {
	case NI_EVENT_LINK_DOWN:
		ni_debug_dhcp("received link down event");
		// ni_dhcp6_fsm_link_down(dev);
		break;

	case NI_EVENT_LINK_UP:
		ni_debug_dhcp("received link up event");
		// ni_dhcp6_fsm_link_up(dev);
		break;

	default:
		break;
	}
}

void
ni_dhcp6_device_alloc_buffer(ni_dhcp6_device_t *dev)
{
	if (dev->message.size < NI_DHCP6_WBUF_SIZE) {
		ni_buffer_ensure_tailroom(&dev->message, NI_DHCP6_WBUF_SIZE);
	}
	ni_buffer_clear(&dev->message);
}

void
ni_dhcp6_device_drop_buffer(ni_dhcp6_device_t *dev)
{
	ni_buffer_destroy(&dev->message);
}

#if 0
static void ni_dhcp6_device_retrans_init()
{
	uint32_t ni_dhcp6_xid;

}
#endif

static ni_bool_t
ni_dhcp6_device_can_send_unicast(ni_dhcp6_device_t *dev, unsigned int msg_code, const ni_addrconf_lease_t *lease)
{
	(void)dev;
	(void)msg_code;
	(void)lease;
#if 0
	/*
	 * We can send messages by unicast only if:
	 *
	 * - it is a Request, Renew, Release or Decline message
	 */
	switch(msg_code) {
		case NI_DHCP6_RENEW:
		case NI_DHCP6_REQUEST:
		case NI_DHCP6_RELEASE:
		case NI_DHCP6_DECLINE:
		break;
		default:
			return FALSE;
		break;
	}

	/*
	 * - client has received a unicast option from server
	 *   [the lease contains the server address then]
	 */
	if(lease == NULL ||
		IN6_IS_ADDR_UNSPECIFIED(&lease->dhcp6.server_unicast) ||
		IN6_IS_ADDR_MULTICAST(&lease->dhcp6.server_unicast) ||
		IN6_IS_ADDR_LOOPBACK(&lease->dhcp6.server_unicast))
		return FALSE;

	/*
	 * - TODO: client has a source address of sufficient scope
	 *   to reach the server directly
	 */

	/* - TODO: initial message only, do not use for retransmits
	 *   because it adds more problem than this optimization is
	 *   worth (just waste of time when the server is down, has
	 *   been replaced by other one, ...).
	 *   In multicast mode, all relays and (directly attached)
	 *   servers will be reached and the destination server for
	 *   these messages is selected by server identifier (DUID);
	 *   other servers will drop the message.
	 */
#endif
	return FALSE;
}

static int
ni_dhcp6_init_message(ni_dhcp6_device_t *dev, unsigned int msg_code, const ni_addrconf_lease_t *lease)
{
	int rv;

	if (ni_dhcp6_socket_open(dev) < 0) {
		ni_error("%s: unable to open DHCP6 socket", dev->ifname);
		goto transient_failure;
	}

	/* Assign a new XID to this message */
	while (dev->dhcp6.xid == 0) {
		dev->dhcp6.xid = random() & NI_DHCP6_XID_MASK;
	}

	/* Allocate an empty buffer */
	ni_dhcp6_device_alloc_buffer(dev);

	ni_trace("%s: building %s with xid 0x%x", dev->ifname,
		ni_dhcp6_message_name(msg_code), dev->dhcp6.xid);

	rv = ni_dhcp6_build_message(dev, msg_code, lease, &dev->message);
	if (rv < 0) {
		ni_error("%s: unable to build %s message", dev->ifname,
			ni_dhcp6_message_name(msg_code));
		return -1;
	}

	memset(&dev->server_addr, 0, sizeof(dev->server_addr));
	dev->server_addr.six.sin6_family = AF_INET6;
	dev->server_addr.six.sin6_port = htons(NI_DHCP6_SERVER_PORT);
	dev->server_addr.six.sin6_scope_id = dev->link.ifindex;

	if(ni_dhcp6_device_can_send_unicast(dev, msg_code, lease)) {
		memcpy(&dev->server_addr.six.sin6_addr, &lease->dhcp6.server_unicast,
			sizeof(dev->server_addr.six.sin6_addr));
	} else if(inet_pton(AF_INET6, NI_DHCP6_ALL_RAGENTS,
				&dev->server_addr.six.sin6_addr) != 1) {
		ni_error("%s: Unable to prepare DHCP6 destination address", dev->ifname);
		return -1;
	}

	if(!ni_dhcp6_set_message_timing(msg_code, &dev->retrans.params))
		return -1;

	return 0;

transient_failure:
	/* We ran into a transient problem, such as being unable to open
	 * a raw socket. We should schedule a "short" timeout after which
	 * we should re-try the operation. */
	/* FIXME: Not done yet. */
	return 1;
}

#if 0
int
ni_dhcp6_device_send_message(ni_dhcp6_device_t *dev, unsigned int msg_code, const ni_addrconf_lease_t *lease)
{
	int rv;

	if ((rv = ni_dhcp6_device_init_message(dev, msg_code, lease)) != 0)
		return rv;

	return ni_dhcp6_device_transmit(dev);
}
#endif

int
ni_dhcp6_device_transmit(ni_dhcp6_device_t *dev)
{
	ni_dhcp6_packet_header_t *header;
	int flags = 0;
	ssize_t rv = -1;
	size_t cnt;

	/* sanity check: verify we have at least the message type byte */
	if (!ni_buffer_count(&dev->message)) {
		ni_error("Cannot send empty DHCPv6 message packet");
		return rv;
	}

	/* peek message code only */
	header = ni_buffer_head(&dev->message);

	ni_trace("%s: sending %s with xid 0x%x to %s using socket #%d",
		dev->ifname, ni_dhcp6_message_name(header->type),
		dev->dhcp6.xid, ni_address_print(&dev->server_addr), dev->sock->__fd);

	if(IN6_IS_ADDR_MULTICAST(&dev->server_addr.six.sin6_addr) ||
	   IN6_IS_ADDR_LINKLOCAL(&dev->server_addr.six.sin6_addr)) {
		flags = MSG_DONTROUTE;
	}

	cnt = ni_buffer_count(&dev->message);
	rv = sendto(dev->sock->__fd, ni_buffer_head(&dev->message),
			ni_buffer_count(&dev->message), flags,
			&dev->server_addr.sa, sizeof(dev->server_addr.six));
	if(rv < 0) {
		ni_error("unable to send dhcp packet: %m");
		return -1;
	} else {
		ni_trace("message with %zu of %zd bytes sent", rv, cnt);

		// clear buffer
	}
	return (size_t)rv == (ssize_t)ni_buffer_count(&dev->message) ? 0 : -1;
}

/*
 * Functions for accessing various global DHCP configuration options
 */
const char *
ni_dhcp6_config_default_duid(ni_opaque_t *duid)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;

	if (ni_string_empty(dhconf->default_duid))
		return NULL;

	if (duid && ni_duid_parse_hex(duid, dhconf->default_duid) == 0)
		return NULL;

	return dhconf->default_duid;
}

int
ni_dhcp6_config_user_class(ni_string_array_t *user_class_data)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;

	ni_string_array_copy(user_class_data, &dhconf->user_class_data);
	return 0;
}

int
ni_dhcp6_config_vendor_class(unsigned int *vclass_en, ni_string_array_t *vclass_data)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;

	if ((*vclass_en = dhconf->vendor_class_en) != 0) {
		ni_string_array_copy(vclass_data, &dhconf->vendor_class_data);
	} else {
		*vclass_en = NI_DHCP6_VENDOR_ENTERPRISE_NUMBER;
		ni_string_array_destroy(vclass_data);
		ni_string_array_append(vclass_data, NI_DHCP6_VENDOR_VERSION_STRING);
	}
	return 0;
}

int
ni_dhcp6_config_vendor_opts(unsigned int *vopts_en, ni_var_array_t *vopts_data)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;

	ni_var_array_destroy(vopts_data);
	if ((*vopts_en = dhconf->vendor_opts_en) != 0) {
		const ni_var_array_t *nva;
		unsigned int i;

		nva = &dhconf->vendor_opts_data;
		for (i = 0; i < nva->count; ++i) {
			if (ni_string_empty(nva->data[i].name))
				continue;
			ni_var_array_set(vopts_data, nva->data[i].name, nva->data[i].value);
		}
	}
	return 0;
}

int
ni_dhcp6_config_ignore_server(struct in6_addr addr)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;
	char        abuf[INET6_ADDRSTRLEN];
	const char *astr = inet_ntop(AF_INET, &addr, abuf, sizeof(abuf));

	// Hmm ... better another way around using IN6_ARE_ADDR_EQUAL(a,b)
	return (ni_string_array_index(&dhconf->ignore_servers, astr) >= 0);
}

int
ni_dhcp6_config_have_server_preference(void)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;
	return dhconf->num_preferred_servers != 0;
}

int
ni_dhcp6_config_server_preference(struct in6_addr *addr, ni_opaque_t *duid)
{
	const struct ni_config_dhcp6 *dhconf = &ni_global.config->addrconf.dhcp6;
	const ni_server_preference_t *pref = dhconf->preferred_server;
	unsigned int i;

	for (i = 0; i < dhconf->num_preferred_servers; ++i, ++pref) {
		if (duid && pref->serverid.len > 0) {
			if(duid->len == pref->serverid.len &&
			   !memcmp(duid->data, pref->serverid.data, pref->serverid.len))
				return pref->weight;
		}
		if (addr && pref->address.ss_family == AF_INET6) {
			if(IN6_ARE_ADDR_EQUAL(addr, &pref->address.six.sin6_addr))
				return pref->weight;
		}
	}
	return 0;
}

unsigned int
ni_dhcp6_config_max_lease_time(void)
{
	return ni_global.config->addrconf.dhcp6.lease_time;
}
