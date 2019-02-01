/* Multicast group management (join/leave) API
 *
 * Copyright (C) 2001-2005  Carsten Schill <carsten@cschill.de>
 * Copyright (C) 2006-2009  Julien BLACHE <jb@jblache.org>
 * Copyright (C) 2009       Todd Hayton <todd.hayton@gmail.com>
 * Copyright (C) 2009-2011  Micha Lenk <micha@debian.org>
 * Copyright (C) 2011-2017  Joachim Nilsson <troglobit@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#ifdef HAVE_LINUX_FILTER_H
#include <linux/filter.h>
#endif

#include "log.h"
#include "ipc.h"
#include "ifvc.h"
#include "queue.h"
#include "socket.h"

struct mgroup {
	LIST_ENTRY(mgroup) link;

	short          inbound;
	struct in_addr source;
	struct in_addr group;
};

/*
 * Track IGMP join, any-source and source specific
 */
LIST_HEAD(, mgroup) mgroup_static_list = LIST_HEAD_INITIALIZER();

/*
 * Failed joins due to no-ip-address on iface yet ...
 */
LIST_HEAD(, mgroup) mgroup_retry_list  = LIST_HEAD_INITIALIZER();

static int mcgroup4_socket = -1;

#ifdef HAVE_LINUX_FILTER_H
/* Extremely simple "drop everything" filter for Linux so we do not get
 * a copy each packet of every routed group we join. */
static struct sock_filter filter[] = {
	{ 0x6, 0, 0, 0x00000000 },
};

static struct sock_fprog fprog = {
	sizeof(filter) / sizeof(filter[0]),
	filter
};
#endif /* HAVE_LINUX_FILTER_H */


static struct iface *match_valid_iface(const char *ifname, struct ifmatch *state)
{
	struct iface *iface = iface_match_by_name(ifname, state);

	if (!iface && !state->match_count)
		smclog(LOG_DEBUG, "unknown interface %s", ifname);

	return iface;
}

static void list_add(struct iface *iface, struct in_addr source, struct in_addr group, int fail)
{
	struct mgroup *entry;

	entry = malloc(sizeof(*entry));
	if (!entry) {
		smclog(LOG_ERR, "Failed adding mgroup to list: %s", strerror(errno));
		return;
	}

	memset(entry, 0, sizeof(*entry));
	entry->inbound = iface->vif;
	entry->source  = source;
	entry->group   = group;

	if (fail)
		LIST_INSERT_HEAD(&mgroup_retry_list, entry, link);
	else
 		LIST_INSERT_HEAD(&mgroup_static_list, entry, link);
}

static void list_rem(struct iface *iface, struct in_addr source, struct in_addr group)
{
	struct mgroup *entry, *tmp;

	LIST_FOREACH_SAFE(entry, &mgroup_static_list, link, tmp) {
		if (entry->inbound       != iface->vif    ||
		    entry->source.s_addr != source.s_addr ||
		    entry->group.s_addr  != group.s_addr)
			continue;

		LIST_REMOVE(entry, link);
		free(entry);
	}
}


static void mcgroup4_init(void)
{
	if (mcgroup4_socket < 0) {
		mcgroup4_socket = socket_create(AF_INET, SOCK_DGRAM, 0, NULL, NULL);
		if (mcgroup4_socket < 0) {
			smclog(LOG_ERR, "failed creating IPv4 mcgroup socket: %s", strerror(errno));
			exit(255);
		}

#ifdef HAVE_LINUX_FILTER_H
		if (setsockopt(mcgroup4_socket, SOL_SOCKET, SO_ATTACH_FILTER, &fprog, sizeof(fprog)) < 0)
			smclog(LOG_WARNING, "failed setting IPv4 socket filter, continuing anyway");
#endif
	}
}

static int join_leave_ipv4(int sd, int cmd, struct iface *iface, struct in_addr group)
{
	struct ip_mreq mreq;
	int opt, retry = 0;

	if (!cmd) {
		cmd = 'j';
		retry = 1;
	}

	if (cmd == 'j')
		opt = IP_ADD_MEMBERSHIP;
	else
		opt = IP_DROP_MEMBERSHIP;

	mreq.imr_multiaddr.s_addr = group.s_addr;
	mreq.imr_interface.s_addr = iface->inaddr.s_addr;
	if (setsockopt(sd, IPPROTO_IP, opt, &mreq, sizeof(mreq))) {
		char grp[16];

		if (retry)
			return 1;

		inet_ntop(AF_INET, &group, grp, sizeof(grp));
		if (EADDRNOTAVAIL == errno && cmd == 'l')
			smclog(LOG_DEBUG, "failed leave (*,%s), not a member", grp);
		else if (EADDRINUSE == errno && cmd == 'j')
			smclog(LOG_DEBUG, "failed join (*,%s), already member", grp);
		else if (ENODEV == errno && cmd == 'j')
			smclog(LOG_ERR, "failed join (*,%s) on %s, no interface address.", grp, iface);
		else
			smclog(LOG_ERR, "failed %s (*,%s) on iface %s.  Error %d: %s",
			       cmd == 'j' ? "join" : "leave", grp, iface->name, errno, strerror(errno));

		return 1;
	}

	return 0;
}

static int join_leave_ssm_ipv4(int sd, int cmd, struct iface *iface, struct in_addr source, struct in_addr group)
{
	struct ip_mreq_source mreqsrc;
	int opt, retry = 0;

	if (!cmd) {
		cmd = 'j';
		retry = 1;
	}

	if (cmd == 'j')
		opt = IP_ADD_SOURCE_MEMBERSHIP;
	else
		opt = IP_DROP_SOURCE_MEMBERSHIP;

	mreqsrc.imr_multiaddr.s_addr = group.s_addr;
	mreqsrc.imr_sourceaddr.s_addr = source.s_addr;
	mreqsrc.imr_interface.s_addr = iface->inaddr.s_addr;
	if (setsockopt(sd, IPPROTO_IP, opt, &mreqsrc, sizeof(mreqsrc))) {
		char *src, grp[16];

		if (retry)
			return 1;

		src = inet_ntoa(source);
		inet_ntop(AF_INET, &group, grp, sizeof(grp));

		if (EADDRNOTAVAIL == errno && cmd == 'j')
			smclog(LOG_DEBUG, "failed join (%s,%s), already member", src, grp);
		else if (EADDRNOTAVAIL == errno && cmd == 'l')
			smclog(LOG_DEBUG, "failed leave (%s,%s), not a member from that source", src, grp);
		else if (EINVAL == errno && cmd == 'l')
			smclog(LOG_DEBUG, "failed leave (%s,%s), not a member", src, grp);
		else
			smclog(LOG_WARNING, "failed %s (%s,%s) on iface %s.  Error %d: %s",
			       cmd == 'j' ? "join" : "leave", src, grp, iface->name, errno, strerror(errno));

		return 1;
	}

	return 0;
}

static int mcgroup_join_leave_ipv4(int sd, int cmd, const char *ifname, struct in_addr group)
{
	struct ifmatch state;
	struct in_addr any_src;
	struct iface *iface;
	int ret = 0;

	any_src.s_addr = htonl(INADDR_ANY);
	iface_match_init(&state);
	while ((iface = match_valid_iface(ifname, &state))) {
		ret += join_leave_ipv4(sd, cmd, iface, group);

		if (cmd == 'j')
			list_add(iface, any_src, group, ret);
		else
			list_rem(iface, any_src, group);
	}

	if (!state.match_count)
		return 1;
	else
		return ret;
}

static int mcgroup_join_leave_ssm_ipv4(int sd, int cmd, const char *ifname, struct in_addr source, struct in_addr group)
{
#ifndef IP_ADD_SOURCE_MEMBERSHIP
	smclog(LOG_WARNING, "Source specific join/leave not supported, ignoring source %s", inet_ntoa(source));
	return mcgroup_join_leave_ipv4(sd, cmd, ifname, group);
#else
	struct iface *iface;
	struct ifmatch state;
	int ret = 0;

	iface_match_init(&state);
	while ((iface = match_valid_iface(ifname, &state))) {
		ret += join_leave_ssm_ipv4(sd, cmd, iface, source, group);

		if (cmd == 'j')
			list_add(iface, source, group, ret);
		else
			list_rem(iface, source, group);
	}

	if (!state.match_count)
		return 1;
	else
		return ret;
#endif
}

/*
 * Joins the MC group with the address 'group' on the interface 'ifname'.
 * The join is bound to the UDP socket 'sd', so if this socket is
 * closed the membership is dropped.
 *
 * returns: - 0 if the function succeeds
 *          - 1 if parameters are wrong or the join fails
 */
int mcgroup4_join(const char *ifname, struct in_addr source, struct in_addr group)
{
	mcgroup4_init();

	if (!source.s_addr)
		return mcgroup_join_leave_ipv4(mcgroup4_socket, 'j', ifname, group);

	return mcgroup_join_leave_ssm_ipv4(mcgroup4_socket, 'j', ifname, source, group);
}

/*
 * Leaves the MC group with the address 'group' on the interface 'ifname'.
 *
 * returns: - 0 if the function succeeds
 *          - 1 if parameters are wrong or the join fails
 */
int mcgroup4_leave(const char *ifname, struct in_addr source, struct in_addr group)
{
	mcgroup4_init();

	if (!source.s_addr)
		return mcgroup_join_leave_ipv4(mcgroup4_socket, 'l', ifname, group);

	return mcgroup_join_leave_ssm_ipv4(mcgroup4_socket, 'l', ifname, source, group);
}

/*
 * Close IPv4 multicast socket to kernel to leave any joined groups
 */
void mcgroup4_disable(void)
{
	struct mgroup *entry, *tmp;

	if (mcgroup4_socket != -1) {
		socket_close(mcgroup4_socket);
		mcgroup4_socket = -1;
	}

	LIST_FOREACH_SAFE(entry, &mgroup_static_list, link, tmp) {
		LIST_REMOVE(entry, link);
		free(entry);
	}
}

/*
 * Retry join of previously failed joins, called by iface_update()
 */
static void mcgroup4_retry(void)
{
	struct in_addr any_src;
	struct mgroup *entry, *tmp;

	any_src.s_addr = htonl(INADDR_ANY);

	LIST_FOREACH_SAFE(entry, &mgroup_retry_list, link, tmp) {
		struct iface *iface;
		char grp[16];
		int is_any = 0;
		int rc;

		iface = iface_find_by_vif(entry->inbound);
		if (!iface)
			continue;

		if (!memcmp(&entry->source, &any_src, sizeof(any_src))) {
			is_any = 1;
			rc = join_leave_ipv4(mcgroup4_socket, 0, iface, entry->group);
		} else
			rc = join_leave_ssm_ipv4(mcgroup4_socket, 0, iface, entry->source, entry->group);
		if (rc)
			continue;

		inet_ntop(AF_INET, &entry->group, grp, sizeof(grp));
		smclog(LOG_NOTICE, "successful join (%s,%s) on iface %s.",
		       is_any ? "*" : inet_ntoa(entry->source), grp, iface->name,
		       errno, strerror(errno));

		/* Move to list of joined groups */
		LIST_REMOVE(entry, link);
 		LIST_INSERT_HEAD(&mgroup_static_list, entry, link);
	}
}

int mcgroup_refresh(void)
{
	mcgroup4_retry();
	if (LIST_EMPTY(&mgroup_retry_list))
		return 1;

	return 0;
}

#ifdef HAVE_IPV6_MULTICAST_HOST
static int mcgroup6_socket = -1;

static void mcgroup6_init(void)
{
	if (mcgroup6_socket < 0) {
		mcgroup6_socket = socket_create(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, NULL, NULL);
		if (mcgroup6_socket < 0) {
			smclog(LOG_WARNING, "failed creating IPv6 mcgroup socket: %s", strerror(errno));
			return;
		}

#ifdef HAVE_LINUX_FILTER_H
		if (setsockopt(mcgroup6_socket, SOL_SOCKET, SO_ATTACH_FILTER, &fprog, sizeof(fprog)) < 0)
			smclog(LOG_WARNING, "failed setting IPv6 socket filter, continuing anyway");
#endif
	}
}

static int mcgroup_join_leave_ipv6(int sd, int cmd, const char *ifname, struct in6_addr group)
{
	int ret = 0;
	struct ipv6_mreq mreq;
	struct iface *iface;
	struct ifmatch state;

	iface_match_init(&state);
	while ((iface = match_valid_iface(ifname, &state))) {
		mreq.ipv6mr_multiaddr = group;
		mreq.ipv6mr_interface = iface->ifindex;
		if (setsockopt(sd, IPPROTO_IPV6, cmd == 'j' ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP, &mreq, sizeof(mreq))) {
			char buf[INET6_ADDRSTRLEN];

			if (EADDRNOTAVAIL == errno && cmd == 'l')
				smclog(LOG_DEBUG, "failed leaving group, not a member of %s",
				       inet_ntop(AF_INET6, &group, buf, sizeof(buf)));
			else if (EADDRINUSE == errno && cmd == 'j')
				smclog(LOG_DEBUG, "failed joining group, already member of %s",
				       inet_ntop(AF_INET6, &group, buf, sizeof(buf)));
			else
				smclog(LOG_DEBUG, "failed group %s: %s", cmd == 'j' ? "join" : "leave", strerror(errno));
			ret = 1;
		}
	}

	if (!state.match_count)
		return 1;
	else
		return ret;
}

/*
 * Joins the MC group with the address 'group' on the interface 'ifname'.
 * The join is bound to the UDP socket 'sd', so if this socket is
 * closed the membership is dropped.
 *
 * returns: - 0 if the function succeeds
 *          - 1 if parameters are wrong or the join fails
 */
int mcgroup6_join(const char *ifname, struct in6_addr group)
{
	mcgroup6_init();

	return mcgroup_join_leave_ipv6(mcgroup6_socket, 'j', ifname, group);
}

/*
 * Leaves the MC group with the address 'group' on the interface 'ifname'.
 *
 * returns: - 0 if the function succeeds
 *          - 1 if parameters are wrong or the join fails
 */
int mcgroup6_leave(const char *ifname, struct in6_addr group)
{
	mcgroup6_init();

	return mcgroup_join_leave_ipv6(mcgroup6_socket, 'l', ifname, group);
}
#endif /* HAVE_IPV6_MULTICAST_HOST */

/*
 * Close IPv6 multicast socket to kernel to leave any joined groups
 */
void mcgroup6_disable(void)
{
#ifdef HAVE_IPV6_MULTICAST_HOST
	if (mcgroup6_socket != -1) {
		socket_close(mcgroup6_socket);
		mcgroup6_socket = -1;
	}
#endif /* HAVE_IPV6_MULTICAST_HOST */
}

#ifdef ENABLE_CLIENT
/* Write all joined IGMP/MLD groups to client socket */
int mcgroup_show(int sd, int detail)
{
	char buf[256];
	char sg[INET_ADDRSTRLEN * 2 + 5];
	struct mgroup *g;

	(void)detail;
	LIST_FOREACH(g, &mgroup_static_list, link) {
		char src[INET_ADDRSTRLEN] = "*";
		char grp[INET_ADDRSTRLEN];
		struct iface *i;

		i = iface_find_by_vif(g->inbound);

		if (g->source.s_addr != htonl(INADDR_ANY))
			inet_ntop(AF_INET, &g->source, src, sizeof(src));
		inet_ntop(AF_INET, &g->group, grp, sizeof(grp));

		snprintf(sg, sizeof(sg), "(%s, %s)", src, grp);
		snprintf(buf, sizeof(buf), "%-34s %s\n", sg, i->name);

		if (ipc_send(sd, buf, strlen(buf)) < 0) {
			smclog(LOG_ERR, "Failed sending reply to client: %s", strerror(errno));
			return -1;
		}
	}

	return 0;
}
#endif

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
