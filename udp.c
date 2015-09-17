/* udp.c Operations with udp sockets
 * Like register in  multicast group
 * drop multicast group membership and so on
 *
 *
 **/

#include	<err.h>
#include	<errno.h>
#include	<string.h>
#include	<strings.h>
#include	<netdb.h>
#include	<netinet/in.h>
#include	<sys/socket.h>
#include	<sys/types.h>
#include	<arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <netgraph.h>
#include <netgraph/ng_socket.h>
#include <netgraph/ng_ksocket.h>
#include <netgraph/ng_message.h>
#include <netinet/igmp.h>
#include "ng-r.h"
//#include <netinet/ip.h>

#define		MSG_LEN	16

// Internal Functions
int mkhub_udp(int srv_num);
int add_mgroup(int srv_num);
int drop_mgroup(int srv_num);

// External Function
void Log(int log, const char *fmt, ...);
int mkhub_udp(int srv_num) {
	/*  Goals of this function in ngctl syntax
	 ngctl -f-<<-SEQ
	 mkpeer . hub tmp tmp
	 name .:tmp fanout
	 mkpeer fanout: ksocket up inet/stream/tcp
	 name fanout:up upstream
	 msg upstream: connect inet/195.64.148.4:9127
	 SEQ

	 send to fanout :
	 "GET / HTTP/1.1\r\nRange: bytes=0-\r\nIcy-MetaData: 1\r\n\r\n"
	 */
	char basename[NG_PATHSIZ];
	char path[NG_PATHSIZ], name[NG_PATHSIZ], tmp[BUF_LEN];
	char src[IP_LEN], dst[IP_LEN];
	struct ngm_mkpeer mkp;
	struct ngm_connect con;
	struct igmp igmp;
	struct ip_mreq ip_mreq;

	union {
		u_char buf[sizeof(struct ng_ksocket_sockopt) + sizeof(int)];
		struct ng_ksocket_sockopt sockopt;
	} sockopt_buf;

	struct ng_ksocket_sockopt * const sockopt = &sockopt_buf.sockopt;
	int one = 1, debug;
	char peerhook[NG_PATHSIZ], ourhook[NG_PATHSIZ];

	// DEBUG
	debug = 1;

	memset(src, 0, sizeof(src));
	memset(dst, 0, sizeof(dst));
	strcpy(src, inet_ntoa(server_cfg[srv_num].src.sin_addr));
	strcpy(dst, inet_ntoa(server_cfg[srv_num].dst.sin_addr));

	memset(basename, 0, sizeof(basename));
	memset(&igmp, 0, sizeof(igmp));
	memset(&ip_mreq, 0, sizeof(ip_mreq));
	memset(&mkp, 0, sizeof(mkp));
	memset(&con, 0, sizeof(con));
	memset(tmp, 0, sizeof(tmp));
	memset(name, 0, sizeof(name));
	memset(path, 0, sizeof(path));

	Log(LOG_NOTICE, "%s(%d): mcast = %s name = %s server = %s:%d", __func__,
			srv_num, src, server_cfg[srv_num].name, dst,
			ntohs(server_cfg[srv_num].dst.sin_port));

	if (debug == 1) {
		Log(LOG_DEBUG, "%s(%d): memsets done", __func__, srv_num);
	}
	// Parse group making ip and port

	memcpy(basename, server_cfg[srv_num].name, sizeof(basename));

	Log(LOG_NOTICE, "%s(%d): group = %s port = %d basename = %s", __func__,
			srv_num, src, ntohs(server_cfg[srv_num].src.sin_port), basename);

	// mkpeer . hub tmp tmp
	sprintf(path, ".");
	sprintf(peerhook, "%s%d", "tmp", srv_num);
	sprintf(ourhook, "%s%d", "tmp", srv_num);
	snprintf(mkp.type, sizeof(mkp.type), "hub");
	sprintf(mkp.ourhook, "%s", ourhook);
	sprintf(mkp.peerhook, "%s", peerhook);

	if (NgSendMsg(csock, path, NGM_GENERIC_COOKIE, NGM_MKPEER, &mkp,
			sizeof(mkp)) < 0) {
		Log(LOG_ERR,
				"%s(%d): mkpeer . hub tmp tmp Creating and connecting node error: %s",
				__func__, srv_num, strerror(errno));
		return 0;
	}
	// name .:tmp fanout
	sprintf(path, ".:%s", ourhook);
	if (NgNameNode(csock, path, "%s", basename) < 0) {
		Log(LOG_ERR, "%s(%d): Naming Node %s failed: %s", __func__, srv_num,
				basename, strerror(errno));
		sprintf(path, "%s:", basename);
		if (errno == EADDRINUSE) {
			if (NgSendMsg(csock, path, NGM_GENERIC_COOKIE, NGM_SHUTDOWN, NULL,
					0) < 0) {
				Log(LOG_INFO, "%s(%d): Error shutdowning %s : %s", __func__,
						srv_num, path, strerror(errno));
			} else {
				Log(LOG_INFO, "%s(%d): node %s shutdowned", __func__,
						srv_num, path);
			}
		} else {
			return 0;
		}
	}
	// mkpeer fanout: ksocket up inet/stream/tcp
	sprintf(path, "%s:", basename);
	snprintf(mkp.type, sizeof(mkp.type), "ksocket");
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "up");
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), "inet/dgram/udp");

	if (NgSendMsg(csock, path, NGM_GENERIC_COOKIE, NGM_MKPEER, &mkp,
			sizeof(mkp)) < 0) {
		Log(LOG_ERR,
				"%s(%d): mkpeer fanout: ksocket up inet/stream/tcp error: %s",
				__func__, srv_num, strerror(errno));
		return 0;
	}
	// name fanout:up upstream
	sprintf(path, "%s:up", basename);
	sprintf(name, "%s-upstream", basename);

	if (NgNameNode(csock, path, "%s", name) < 0) {
		Log(LOG_ERR, "%s(%d): Naming Node %s failed %s", __func__, srv_num,
				path, strerror(errno));
		return 0;
	}
	sprintf(path, "%s-upstream:", basename);
	// setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(int)) < 0)
	memset(&sockopt_buf, 0, sizeof(sockopt_buf));

	sockopt->level = SOL_SOCKET;
	sockopt->name = SO_REUSEADDR;
	memcpy(sockopt->value, &one, sizeof(int));
	if (NgSendMsg(csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_SETOPT, sockopt,
			sizeof(sockopt_buf)) == -1) {
		Log(LOG_ERR, "%s(%d): Sockopt set failed : %s", __func__, srv_num,
				strerror(errno));
		return 0;
	}
	// setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(int)) < 0)
	sockopt->name = SO_REUSEPORT;
	memcpy(sockopt->value, &one, sizeof(int));
	if (NgSendMsg(csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_SETOPT, sockopt,
			sizeof(sockopt_buf)) == -1) {
		Log(LOG_ERR, "%s(%d): Sockopt set failed : %s", __func__, srv_num,
				strerror(errno));
		return 0;
	}
	// msg upstream: bind inet/239.0.3.5:1234
	sprintf(path, "%s-upstream:", basename);

	if (NgSendMsg(csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_BIND,
			(struct sockaddr*) &server_cfg[srv_num].src,
			sizeof(struct sockaddr_in)) < 0) {
		//NgAllocRecvMsg(csock, &m, pth);
		Log(LOG_ERR, "%s(%d): BIND FAILED %s", __func__, srv_num,
				strerror(errno));
		return 0;

	}
	Log(LOG_NOTICE,
			"%s(%d): Created hub for mcast = %s name = %s server = %s:%d",
			__func__, srv_num, src, server_cfg[srv_num].name, dst,
			ntohs(server_cfg[srv_num].dst.sin_port));
	return 1;
}

int add_mgroup(int srv_num) {

	/*
	 setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

	 */
	union {
		u_char buf[sizeof(struct ng_ksocket_sockopt) + sizeof(struct ip_mreq)];
		struct ng_ksocket_sockopt sockopt;
	} sockopt_buf;
	struct ng_ksocket_sockopt * const sockopt = &sockopt_buf.sockopt;
	struct ip_mreq ip_mreq;
	char servsock[NG_PATHSIZ];
	char src[IP_LEN];

	memset(src, 0, sizeof(src));
	strcpy(src, inet_ntoa(server_cfg[srv_num].src.sin_addr));

	sprintf(servsock, "%s-upstream:", server_cfg[srv_num].name);

	memset(&sockopt_buf, 0, sizeof(sockopt_buf));
	memset(&ip_mreq, 0, sizeof(ip_mreq));

	sockopt->level = IPPROTO_IP;
	sockopt->name = IP_ADD_MEMBERSHIP;
	ip_mreq.imr_multiaddr.s_addr = server_cfg[srv_num].src.sin_addr.s_addr;
	ip_mreq.imr_interface.s_addr = server_cfg[srv_num].mifip.s_addr;
	memcpy(sockopt->value, &ip_mreq, sizeof(ip_mreq));

	if (NgSendMsg(csock, servsock, NGM_KSOCKET_COOKIE, NGM_KSOCKET_SETOPT,
			sockopt, sizeof(sockopt_buf)) < 0) {
		Log(LOG_ERR, "%s(%d): Failed ADD MEMBERSHIP %s to %s: %s", __func__,
				srv_num, servsock, src, strerror(errno));
		Log(LOG_ERR,
				"%s:%d %s(%d): ip_mreq.imr_multiaddr.s_addr = %s ip_mreq.imr_interface.s_addr=%s",
				__FILE__, __LINE__, __func__, srv_num, src,
				inet_ntoa(server_cfg[srv_num].mifip));
		return EXIT_FAILURE;
	}
	Log(LOG_NOTICE, "%s:%d %s(%d): Register in mgroup = %s success", 
            __FILE__, __LINE__, __func__, srv_num, src);
	return EXIT_SUCCESS;
}

/*
 DROP MULTICAST GROUP MEMBERSHIP
 */
int drop_mgroup(int srv_num) {

	/*
	 setsockopt(s, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));

	 */
	union {
		u_char buf[sizeof(struct ng_ksocket_sockopt) + sizeof(struct ip_mreq)];
		struct ng_ksocket_sockopt sockopt;
	} sockopt_buf;
	struct ng_ksocket_sockopt * const sockopt = &sockopt_buf.sockopt;
	struct ip_mreq ip_mreq;
	char servsock[NG_PATHSIZ];
	char src[IP_LEN], dst[IP_LEN];

	memset(src, 0, sizeof(src));
	memset(dst, 0, sizeof(src));

	strcpy(src, inet_ntoa(server_cfg[srv_num].src.sin_addr));
	strcpy(dst, inet_ntoa(server_cfg[srv_num].dst.sin_addr));

	Log(LOG_DEBUG, "%s:%d %s(%d): server_cfg[%d] src_ip = %s dst_ip = %s",
			__FILE__, __LINE__, __func__, srv_num, srv_num, src, dst);

	sprintf(servsock, "%s-upstream:", server_cfg[srv_num].name);
	memset(&sockopt_buf, 0, sizeof(sockopt_buf));
	memset(&ip_mreq, 0, sizeof(ip_mreq));

	sockopt->level = IPPROTO_IP;
	sockopt->name = IP_DROP_MEMBERSHIP;
	ip_mreq.imr_multiaddr.s_addr = server_cfg[srv_num].src.sin_addr.s_addr;
	ip_mreq.imr_interface.s_addr = server_cfg[srv_num].mifip.s_addr;
	memcpy(sockopt->value, &ip_mreq, sizeof(ip_mreq));

	if (NgSendMsg(csock, servsock, NGM_KSOCKET_COOKIE, NGM_KSOCKET_SETOPT,
			sockopt, sizeof(sockopt_buf)) < 0) {

		Log(LOG_ERR, "%s:%d %s(%d): Failed DROP MEMBERSHIP to %s: %s", __FILE__, __LINE__, __func__,
				srv_num, servsock, strerror(errno));

		Log(LOG_ERR,
				"%s:%d %s(%d): ip_mreq.imr_multiaddr.s_addr = %s ip_mreq.imr_interface.s_addr = %s",
				__FILE__, __LINE__, __func__, srv_num, src,
				inet_ntoa(server_cfg[srv_num].mifip));

		return EXIT_FAILURE;
	}

	Log(LOG_NOTICE, "%s:%d %s(%d): DROP MEMBERSHIP FOR GROUP = %s success",
			__FILE__, __LINE__, __func__, srv_num, src);

	return EXIT_SUCCESS;
}
