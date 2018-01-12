#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if_arp.h>
#include <linux/route.h>
#include <ifaddrs.h>
#include "linux_ppp.h"

#include "triton.h"
#include "iputils.h"
#include "events.h"
#include "ppp.h"
#include "ipdb.h"
#include "log.h"
#include "backup.h"
#include "memdebug.h"

// from /usr/include/linux/ipv6.h
struct in6_ifreq {
        struct in6_addr ifr6_addr;
        __u32           ifr6_prefixlen;
        int             ifr6_ifindex;
};

static int find_hwaddr(struct sockaddr_in *inaddr, struct sockaddr *hwaddr, char *ifname, int ifname_len)
{
	struct ifaddrs *ifaddr, *ifa;
	struct ifreq ifr;
	__u32 ipaddr, addr, mask;
	__u32 bestmask = 0;
	int found = 0;

	if (getifaddrs(&ifaddr) < 0)
		return -1;

	memset(&ifr, 0, sizeof(ifr));
	ipaddr = inaddr->sin_addr.s_addr;
	for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if ((ifa->ifa_flags ^ (IFF_UP | IFF_BROADCAST)) &
		    (IFF_UP | IFF_BROADCAST | IFF_POINTOPOINT | IFF_LOOPBACK | IFF_NOARP))
			continue;

		addr = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
		mask = ((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr;
		if (((ipaddr ^ addr) & mask) != 0)
			continue;

		if (mask >= bestmask) {
			strncpy(ifr.ifr_name, ifa->ifa_name, sizeof(ifr.ifr_name));
			bestmask = mask;
			found = 1;
		}
	}
	freeifaddrs(ifaddr);

	if (!found)
		return 0;

	if (net->sock_ioctl(SIOCGIFHWADDR, &ifr) < 0)
		return -1;

	memcpy(hwaddr, &ifr.ifr_hwaddr, sizeof(struct sockaddr));
	strncpy(ifname, ifr.ifr_name, ifname_len);
	strsep(&ifname, ":");

	return 1;
}

static void devconf(struct ap_session *ses, const char *attr, const char *val)
{
	int fd;
	char fname[PATH_MAX];

	sprintf(fname, "/proc/sys/net/ipv6/conf/%s/%s", ses->ifname, attr);
	fd = open(fname, O_WRONLY);
	if (!fd) {
		log_ppp_error("failed to open '%s': %s\n", fname, strerror(errno));
		return;
	}

	write(fd, val, strlen(val));

	close(fd);
}

void ap_session_ifup(struct ap_session *ses)
{
	if (ses->ifname_rename) {
		if (ap_session_rename(ses, ses->ifname_rename, -1)) {
			ap_session_terminate(ses, TERM_NAS_ERROR, 0);
			return;
		}
		_free(ses->ifname_rename);
		ses->ifname_rename = NULL;
	}

	triton_event_fire(EV_SES_ACCT_START, ses);

	if (ses->stop_time)
		return;

	if (!ses->acct_start) {
		ses->acct_start = 1;
		ap_session_accounting_started(ses);
	}
}

void __export ap_session_accounting_started(struct ap_session *ses)
{
	struct ipv6db_addr_t *a;
	struct ifreq ifr;
	//struct rtentry rt;
	struct in6_ifreq ifr6;
	struct npioctl np;
	struct sockaddr_in addr;
	struct arpreq arpreq;
	struct ppp_t *ppp;
	int ret;

	if (ses->stop_time)
		return;

	if (--ses->acct_start)
		return;

	triton_event_fire(EV_SES_PRE_UP, ses);
	if (ses->stop_time)
		return;

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, ses->ifname);

	if (ses->ctrl->dont_ifcfg) {
		if (net->sock_ioctl(SIOCGIFFLAGS, &ifr))
			log_ppp_error("failed to get interface flags: %s\n", strerror(errno));

		if (!(ifr.ifr_flags & IFF_UP)) {
			ifr.ifr_flags |= IFF_UP;

			if (net->sock_ioctl(SIOCSIFFLAGS, &ifr))
				log_ppp_error("failed to set interface flags: %s\n", strerror(errno));
		}
	} else {
#ifdef USE_BACKUP
		if (!ses->backup || !ses->backup->internal) {
#endif
			if (ses->ipv4) {
				memset(&addr, 0, sizeof(addr));
				addr.sin_family = AF_INET;
				addr.sin_addr.s_addr = ses->ipv4->addr;
				memcpy(&ifr.ifr_addr, &addr, sizeof(addr));

				if (net->sock_ioctl(SIOCSIFADDR, &ifr))
					log_ppp_error("failed to set IPv4 address: %s\n", strerror(errno));

				/*if (ses->ctrl->type == CTRL_TYPE_IPOE) {
					addr.sin_addr.s_addr = 0xffffffff;
					memcpy(&ifr.ifr_netmask, &addr, sizeof(addr));
					if (ioctl(sock_fd, SIOCSIFNETMASK, &ifr))
						log_ppp_error("failed to set IPv4 nask: %s\n", strerror(errno));
				}*/

				addr.sin_addr.s_addr = ses->ipv4->peer_addr;

				/*if (ses->ctrl->type == CTRL_TYPE_IPOE) {
					memset(&rt, 0, sizeof(rt));
					memcpy(&rt.rt_dst, &addr, sizeof(addr));
					rt.rt_flags = RTF_HOST | RTF_UP;
					rt.rt_metric = 1;
					rt.rt_dev = ifr.ifr_name;
					if (ioctl(sock_fd, SIOCADDRT, &rt, sizeof(rt)))
						log_ppp_error("failed to add route: %s\n", strerror(errno));
				} else*/ {
					memcpy(&ifr.ifr_dstaddr, &addr, sizeof(addr));

					if (net->sock_ioctl(SIOCSIFDSTADDR, &ifr))
						log_ppp_error("failed to set peer IPv4 address: %s\n", strerror(errno));
				}

				if (ses->ctrl->proxyarp) {
					memset(&arpreq, 0, sizeof(arpreq));
					arpreq.arp_flags = ATF_PERM | ATF_PUBL;

					addr.sin_addr.s_addr = ses->ipv4->peer_addr;
					memcpy(&arpreq.arp_pa, &addr, sizeof(addr));

					ret = find_hwaddr(&addr, &arpreq.arp_ha, arpreq.arp_dev, sizeof(arpreq.arp_dev));
					if (ret > 0) {
						ret = net->sock_ioctl(SIOCSARP, (caddr_t)&arpreq);
						if (ret == 0)
							ses->proxyarp = strdup(arpreq.arp_dev);
					}
					if (ret < 0)
						log_ppp_error("failed to add proxy arp: %s\n", strerror(errno));
				}
			}

			if (ses->ipv6) {
				net->enter_ns();
				devconf(ses, "accept_ra", "0");
				devconf(ses, "autoconf", "0");
				devconf(ses, "forwarding", "1");
				net->exit_ns();

				memset(&ifr6, 0, sizeof(ifr6));

				if (ses->ctrl->ppp) {
					ifr6.ifr6_addr.s6_addr32[0] = htonl(0xfe800000);
					memcpy(ifr6.ifr6_addr.s6_addr + 8, &ses->ipv6->intf_id, 8);
					ifr6.ifr6_prefixlen = 64;
					ifr6.ifr6_ifindex = ses->ifindex;

					if (net->sock6_ioctl(SIOCSIFADDR, &ifr6))
						log_ppp_error("faild to set LL IPv6 address: %s\n", strerror(errno));
				}

				list_for_each_entry(a, &ses->ipv6->addr_list, entry) {
					a->installed = 0;
					/*if (a->prefix_len < 128) {
						build_ip6_addr(a, ses->ipv6->intf_id, &ifr6.ifr6_addr);
						ifr6.ifr6_prefixlen = a->prefix_len;

						if (ioctl(sock6_fd, SIOCSIFADDR, &ifr6))
							log_ppp_error("failed to add IPv6 address: %s\n", strerror(errno));
					} else
					if (ip6route_add(ses->ifindex, &a->addr, a->prefix_len, 0))
						log_ppp_error("failed to add IPv6 route: %s\n", strerror(errno));*/
				}
			}

			if (net->sock_ioctl(SIOCGIFFLAGS, &ifr))
				log_ppp_error("failed to get interface flags: %s\n", strerror(errno));

			ifr.ifr_flags |= IFF_UP;

			if (net->sock_ioctl(SIOCSIFFLAGS, &ifr))
				log_ppp_error("failed to set interface flags: %s\n", strerror(errno));

			if (ses->ctrl->ppp) {
				ppp = container_of(ses, typeof(*ppp), ses);
				if (ses->ipv4) {
					np.protocol = PPP_IP;
					np.mode = NPMODE_PASS;

					if (net->ppp_ioctl(ppp->unit_fd, PPPIOCSNPMODE, &np))
						log_ppp_error("failed to set NP (IPv4) mode: %s\n", strerror(errno));
				}

				if (ses->ipv6) {
					np.protocol = PPP_IPV6;
					np.mode = NPMODE_PASS;

					if (net->ppp_ioctl(ppp->unit_fd, PPPIOCSNPMODE, &np))
						log_ppp_error("failed to set NP (IPv6) mode: %s\n", strerror(errno));
				}
			}
#ifdef USE_BACKUP
		}
#endif
	}

	ses->ctrl->started(ses);

	triton_event_fire(EV_SES_STARTED, ses);
	triton_event_fire(EV_SES_POST_STARTED, ses);
}

void __export ap_session_ifdown(struct ap_session *ses)
{
	struct ifreq ifr;
	struct sockaddr_in addr;
	struct in6_ifreq ifr6;
	struct ipv6db_addr_t *a;
	struct arpreq arpreq;

	if (ses->ifindex == -1)
		return;

	if (!ses->ctrl->dont_ifcfg) {
		strcpy(ifr.ifr_name, ses->ifname);
		ifr.ifr_flags = 0;
		net->sock_ioctl(SIOCSIFFLAGS, &ifr);
	}

	if (ses->ipv4) {
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		memcpy(&ifr.ifr_addr,&addr,sizeof(addr));
		net->sock_ioctl(SIOCSIFADDR, &ifr);

		if (ses->proxyarp) {
			memset(&arpreq, 0, sizeof(arpreq));
			arpreq.arp_flags = ATF_PERM | ATF_PUBL;

			addr.sin_addr.s_addr = ses->ipv4->peer_addr;
			memcpy(&arpreq.arp_pa, &addr, sizeof(addr));
			strncpy(arpreq.arp_dev, ses->proxyarp, sizeof(arpreq.arp_dev));

			free(ses->proxyarp);
			ses->proxyarp = NULL;

			if (net->sock_ioctl(SIOCDARP, (caddr_t)&arpreq))
				log_ppp_error("failed to delete proxy-arp: %s\n", strerror(errno));
		}
	}

	if (ses->ipv6) {
		memset(&ifr6, 0, sizeof(ifr6));
		ifr6.ifr6_ifindex = ses->ifindex;

		if (ses->ctrl->ppp) {
			ifr6.ifr6_addr.s6_addr32[0] = htonl(0xfe800000);
			*(uint64_t *)(ifr6.ifr6_addr.s6_addr + 8) = ses->ipv6->intf_id;
			ifr6.ifr6_prefixlen = 64;
			net->sock6_ioctl(SIOCDIFADDR, &ifr6);
		}

		list_for_each_entry(a, &ses->ipv6->addr_list, entry) {
			if (!a->installed)
				continue;
			if (a->prefix_len > 64)
				ip6route_del(ses->ifindex, &a->addr, a->prefix_len);
			else {
				struct in6_addr addr;
				memcpy(addr.s6_addr, &a->addr, 8);
				memcpy(addr.s6_addr + 8, &ses->ipv6->intf_id, 8);
				ip6addr_del(ses->ifindex, &addr, a->prefix_len);
			}
		}
	}
}

int __export ap_session_rename(struct ap_session *ses, const char *ifname, int len)
{
	struct ifreq ifr;
	int i, r, up = 0;
	struct ap_net *ns = NULL;
	char ns_name[256];

	if (len == -1)
		len = strlen(ifname);

	for (i = 0; i < len; i++) {
		if (ifname[i] == '/') {
			memcpy(ns_name, ifname, i);
			ns_name[i] = 0;

			ns = ap_net_open_ns(ns_name);
			if (!ns)
				return -1;

			ifname += i + 1;
			len -= i + 1;
			break;
		}
	}

	if (len >= IFNAMSIZ) {
		log_ppp_error("cannot rename interface (name is too long)\n");
		return -1;
	}

	if (len) {
		strcpy(ifr.ifr_name, ses->ifname);
		memcpy(ifr.ifr_newname, ifname, len);
		ifr.ifr_newname[len] = 0;

		r = net->sock_ioctl(SIOCSIFNAME, &ifr);
		if (r < 0 && errno == EBUSY) {
			net->sock_ioctl(SIOCGIFFLAGS, &ifr);
			ifr.ifr_flags &= ~IFF_UP;
			net->sock_ioctl(SIOCSIFFLAGS, &ifr);

			memcpy(ifr.ifr_newname, ifname, len);
			ifr.ifr_newname[len] = 0;
			r = net->sock_ioctl(SIOCSIFNAME, &ifr);

			up = 1;
		}

		if (r < 0) {
			if (!ses->ifname_rename)
				ses->ifname_rename = _strdup(ifr.ifr_newname);
			else
				log_ppp_warn("interface rename to %s failed: %s\n", ifr.ifr_newname, strerror(errno));
		} else {
			/* required since 2.6.27 */
			if (strchr(ifr.ifr_newname, '%')) {
				ifr.ifr_ifindex = ses->ifindex;
				r = net->sock_ioctl(SIOCGIFNAME, &ifr);
				if (r < 0) {
					log_ppp_error("failed to get new interface name: %s\n", strerror(errno));
					return -1;
				}
				len = strnlen(ifr.ifr_name, IFNAMSIZ);
				if (len >= IFNAMSIZ) {
					log_ppp_error("cannot rename interface (name is too long)\n");
					return -1;
				}
				ifr.ifr_name[len] = 0;
				ifname = ifr.ifr_name;
			} else
				ifname = ifr.ifr_newname;

			log_ppp_info2("rename interface to '%s'\n", ifname);
			memcpy(ses->ifname, ifname, len);
			ses->ifname[len] = 0;
		}
	}

	if (ns) {
		if (net->move_link(ns, ses->ifindex)) {
			log_ppp_error("failed to attach namespace\n");
			ns->release(ns);
			return -1;
		}
		ses->net = ns;
		net = ns;
		log_ppp_info2("move to namespace %s\n", ns->name);
	}

	if (up) {
		strcpy(ifr.ifr_name, ses->ifname);
		ifr.ifr_flags |= IFF_UP;
		net->sock_ioctl(SIOCSIFFLAGS, &ifr);
	}

	return 0;
}

