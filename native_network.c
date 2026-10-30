/* SPDX-License-Identifier: BSD-3-Clause */
#include "native_network.h"
#include <lwip/tcpip.h>
#include <lwip/netif.h>
#include <lwip/ip6.h>
#include <lwip/sys.h>
#include <unistd.h>

struct ipv6_setup {
	sys_sem_t done;
	const char *address;
	struct netif *selected;
	s8_t index;
	int initialized, result;
};

static void configure(void *arg)
{
	struct ipv6_setup *s = arg;
	struct netif *nf;

	if (!s->initialized) {
		NETIF_FOREACH(nf)
		{
			if (!netif_is_up(nf) ||
			    !(nf->flags & NETIF_FLAG_ETHARP))
				continue;
			/* The pinned uknetdev port does not advertise MLD6.
			 * lwIP with MLD enabled otherwise rejects
			 * solicited-node neighbor discovery packets. virtio
			 * receives multicast; register memberships before
			 * adding addresses.
			 */
#if LWIP_IPV6_MLD
			netif_set_flags(nf, NETIF_FLAG_MLD6);
#endif
			netif_create_ip6_linklocal_address(nf, 1);
			nf->ip6_autoconfig_enabled = 1;
			if (*s->address && !s->selected) {
				ip6_addr_t addr;

				if (!ip6addr_aton(s->address, &addr) ||
				    netif_add_ip6_address(
					nf, &addr, &s->index) != ERR_OK) {
					s->result = -1;
					goto done;
				}
				s->selected = nf;
			}
		}
		s->initialized = 1;
	}
	s->result =
	    !*s->address ||
	    (s->selected &&
	     ip6_addr_isvalid(netif_ip6_addr_state(s->selected, s->index)));
done:
	sys_sem_signal(&s->done);
}

int native_network_init(const char *ipv6_address)
{
	struct ipv6_setup setup = {.address = ipv6_address};
	int result = -1;

	if (sys_sem_new(&setup.done, 0) != ERR_OK)
		return -1;
	for (int n = 0; n < 60; ++n) {
		if (tcpip_callback_with_block(configure, &setup, 1) != ERR_OK)
			break;
		/* A queued callback must finish before its stack-owned argument
		 * is released.
		 */
		sys_arch_sem_wait(&setup.done, 0);
		if (setup.result) {
			result = setup.result > 0 ? 0 : -1;
			break;
		}
		usleep(100000);
	}
	sys_sem_free(&setup.done);
	return result;
}
