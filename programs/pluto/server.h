/* get-next-event loop
 * Copyright (C) 1998-2001,2013  D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2012-2013 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013 Florian Weimer <fweimer@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#ifndef _SERVER_H
#define _SERVER_H

#include <sys/queue.h>
#include <event2/event.h>	/* from libevent devel */
#include <event2/event_struct.h>
#include "timer.h"

extern char *pluto_vendorid;

extern int ctl_fd;                      /* file descriptor of control (whack) socket */
extern struct sockaddr_un ctl_addr;     /* address of control (whack) socket */

extern int info_fd;                     /* file descriptor of control (info) socket */
extern struct sockaddr_un info_addr;    /* address of control (info) socket */

extern err_t init_ctl_socket(void);
extern void delete_ctl_socket(void);

extern bool listening;  /* should we pay attention to IKE messages? */
extern enum ddos_mode pluto_ddos_mode; /* auto-detect or manual? */
extern enum seccomp_mode pluto_seccomp_mode;
extern unsigned int pluto_max_halfopen; /* Max allowed half-open IKE SA's before refusing */
extern unsigned int pluto_ddos_threshold; /* Max incoming IKE before activating DCOOKIES */
extern deltatime_t pluto_shunt_lifetime; /* lifetime before we cleanup bare shunts (for OE) */
extern unsigned int pluto_sock_bufsize; /* pluto IKE socket buffer */
extern bool pluto_sock_errqueue; /* Enable MSG_ERRQUEUE on IKE socket */

/* interface: a terminal point for IKE traffic, IPsec transport mode
 * and IPsec tunnels.
 * Essentially:
 * - an IP device (eg. eth1), and
 * - its partner, an ipsec device (eg. ipsec0), and
 * - their shared IP address (eg. 10.7.3.2)
 * Note: the port for IKE is always implicitly UDP/pluto_port.
 *
 * The iface is a unique IP address on a system. It may be used
 * by multiple port numbers. In general, two conns have the same
 * interface if they have the same iface_port->iface_alias.
 */
struct iface_dev {
	LIST_ENTRY(iface_dev) id_entry;
	int id_count;
	char *id_vname; /* virtual (ipsec) device name */
	char *id_rname; /* real device name */
#ifdef USE_NIC_OFFLOAD
	bool id_nic_offload;
#endif
};

struct iface_port {
	struct iface_dev   *ip_dev;
	u_int16_t port;         /* host byte order */
	ip_address ip_addr;     /* interface IP address */
	int fd;                 /* file descriptor of socket for IKE UDP messages */
	struct iface_port *next;
	bool ike_float;
	enum { IFN_ADD, IFN_KEEP, IFN_DELETE } change;
	struct pluto_event *pev;
};

extern struct iface_port  *interfaces;   /* public interfaces */
extern enum pluto_ddos_mode ddos_mode;
extern bool pluto_drop_oppo_null;

extern bool use_interface(const char *rifn);
extern void find_ifaces(void);
extern void show_ifaces_status(void);
extern void free_ifaces(void);
extern void show_debug_status(void);
extern void show_fips_status(void);
extern void call_server(void);
extern void init_event_base(void);
typedef void event_callback_routine(evutil_socket_t, const short, void *);
extern struct pluto_event *pluto_event_add(evutil_socket_t fd, short events,
		                event_callback_fn cb, void *arg,
				const struct timeval *delay, char *name);
extern void delete_pluto_event(struct pluto_event **evp);
extern void link_pluto_event_list(struct pluto_event *e);
extern void free_pluto_event_list(void);
bool ev_before(struct pluto_event *pev, deltatime_t delay);
extern void set_pluto_busy(bool busy);
extern void set_whack_pluto_ddos(enum ddos_mode mode);
extern bool should_fragment_ike_msg(struct state *st, size_t len,
				    bool resending);

struct packet_byte_stream;	/* forward decl of tag */
extern void record_outbound_ike_msg(struct state *st, struct packet_byte_stream *pbs, const char *what);
extern bool send_ike_msg(struct state *st, const char *where);
extern bool record_and_send_ike_msg(struct state *st, struct packet_byte_stream *pbs, const char *what);
extern bool send_ike_msg_without_recording(struct state *st, struct packet_byte_stream *pbs, const char *where);
extern bool resend_ike_v1_msg(struct state *st, const char *where);
extern bool send_keepalive(struct state *st, const char *where);
extern struct event_base *get_pluto_event_base(void);

#endif /* _SERVER_H */
