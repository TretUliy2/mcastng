#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netgraph.h>
#include <netgraph/ng_ksocket.h>
#include <pthread.h>
#include <pthread_np.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <syslog.h>

#include "ng-r.h"

#define LST 1024
#define	RUSAGE_SELF	0
#define	SERVSOCK	"-servsock"

// External variables

extern int srv_count;
extern pthread_mutex_t mutex;
extern char http_replay[];
extern u_int32_t tokens[MAX_SERVERS];
// External functions
extern void shut_fanout(void);
extern void exit_nice(void);
extern int add_mgroup(int srv_num);
extern void Log(int log, const char *fmt, ...);

// Structures
struct connect {
	char pth[NG_PATHSIZ];
	int srv_num;
};
// Internal Functions
int handle_client(int csock, int dsock, struct connect);
void send_accept(int srv_csock, int srv_num);
void * mkserver_http(void);
u_int32_t parse_pth(char pth[NG_PATHSIZ]);

/*
 *
 * */

int create_listening_socket(int i, int srv_csock) {

	char path[NG_PATHSIZ], ourhook[NG_HOOKSIZ];
	char name[NG_NODESIZ];
	struct ngm_mkpeer mkp;
	union {
		u_char buf[sizeof(struct ng_ksocket_sockopt) + sizeof(int)];
		struct ng_ksocket_sockopt sockopt;
	} sockopt_buf;
	struct ng_ksocket_sockopt * const sockopt = &sockopt_buf.sockopt;
	int lst, yes;
	uint32_t token;
	const char *basename;
	yes = 1;

	// mkpeer . ksocket listen/stream/tcp
	basename = server_cfg[i].name;

	sprintf(path, ".");
	sprintf(ourhook, "%s%d", "listen", i);
	snprintf(mkp.type, sizeof(mkp.type), "ksocket");
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "%s", ourhook);
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), "inet/stream/tcp");

	if (NgSendMsg(srv_csock, path, NGM_GENERIC_COOKIE, NGM_MKPEER, &mkp,
			sizeof(mkp)) < 0) {
		Log(LOG_ERR, "%s(%d): Creating and connecting node path = %s error: %s",
				__FUNCTION__, i, path, strerror(errno));
		return -1;
	}
	// name    .:listen hub0-servsock
	sprintf(path, ".:%s", ourhook);
	sprintf(name, "%s%s", basename, SERVSOCK);
	if (NgNameNode(srv_csock, path, "%s", name) < 0) {
		Log(LOG_ERR, "%s(%d): Naming Node failed at path: %s : %s",
				__FUNCTION__, i, path, strerror(errno));
		return -1;
	}
	// setsockopt resolve TIME_WAIT problem
	// setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(int)) < 0)
	memset(&sockopt_buf, 0, sizeof(sockopt_buf));

	sockopt->level = SOL_SOCKET;
	sockopt->name = SO_REUSEPORT;
	memcpy(sockopt->value, &yes, sizeof(int));
	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_SETOPT,
			sockopt, sizeof(sockopt_buf)) == -1) {
		Log(LOG_ERR, "%s(%d): Sockopt set failed : %s", __FUNCTION__, i,
				strerror(errno));
		return -1;
	}
	// msg servsock: bind inet/0.0.0.0:8080
	sprintf(path, "%s%s:", server_cfg[i].name, SERVSOCK);

	Log(LOG_NOTICE, "%s(%d): Trying to bind to interface %s:%d", __FUNCTION__,
			i, inet_ntoa(server_cfg[i].dst.sin_addr),
			ntohs(server_cfg[i].dst.sin_port));

	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_BIND,
			&server_cfg[i].dst, sizeof(struct sockaddr_in)) < 0) {
		Log(LOG_ERR, "%s(%d): Can't bind on address: %s:%d err: %s",
				__FUNCTION__, i, inet_ntoa(server_cfg[i].dst.sin_addr),
				ntohs(server_cfg[i].dst.sin_port), strerror(errno));
		return -1;
	}
	//  msg servsock: listen 64
	lst = LST;
	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_LISTEN, &lst,
			sizeof(lst)) < 0) {
		Log(LOG_ERR, "%s(%d): msg servsock: listen 64 failed %s", __FUNCTION__,
				i, strerror(errno));
		return -1;
	}

	Log(LOG_NOTICE, "%s(%d): Starting cycle %d of ACCEPT", __FUNCTION__, i, i);
	// msg servsock: accept
	sprintf(path, "%s%s:", server_cfg[i].name, SERVSOCK);
	// If not first connection - check and clear useless ksock nodes

	token = NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_ACCEPT,
			NULL, 0);
	if ((int) token < 0 && errno != EINPROGRESS && errno != EALREADY) {
		Log(LOG_ERR, "%s(%d): Accept Failed %s", __FUNCTION__, i,
				strerror(errno));
		return -1;
	}
	tokens[i] = token;
	return 1;
}
/* Make http server to serve clients
 * */
void * mkserver_http(void) {
	/*	Goals of this function in ngctl syntax

	 mkpeer . ksocket listen/stream/tcp
	 name	.:listen servsock
	 msg servsock: bind inet/0.0.0.0:8080
	 msg servsock: listen 64
	 msg servsock: accept
	 */

	struct ngm_connect con;
	struct sockaddr_in addr;
	struct ng_mesg *m;
	struct connect connect;
	struct rusage rusage;
	char pth[NG_PATHSIZ];
	char name[NG_NODESIZ];

	int i, srv_csock, srv_dsock;

	union {
		u_char buf[sizeof(struct ng_mesg) + sizeof(struct sockaddr)];
		struct ng_mesg reply;
	} ugetsas;

	m = NULL;

	memset(&ugetsas, 0, sizeof(ugetsas));

	memset(pth, 0, sizeof(pth));
	memset(&con, 0, sizeof(con));

	memset((char *) &addr, 0, sizeof(addr));

	memset(name, 0, sizeof(name));

	memset(tokens, 0, sizeof(tokens));
	// Naming Control socket node
	sprintf(name, "srv-csock-%d", getpid());
	if (NgMkSockNode(name, &srv_csock, &srv_dsock) < 0) {
		Log(LOG_ERR, "%s(): Error Creating ng_socket: %s : %s", __FUNCTION__,
				name, strerror(errno));
		return NULL;
	}
	// For each server in config file we need to create listening socket
	for (i = 0; i < srv_count; i++) {
		create_listening_socket(i, srv_csock);
	}

	for (;;) {
		int found = 0;
		if (NgAllocRecvMsg(srv_csock, &m, pth) < 0) {
			Log(LOG_ERR, "%s(%d): Error receiving response from ksocket %s",
					__FUNCTION__, i, strerror(errno));
			shut_fanout();
			exit_nice();
			return NULL;
		}
		// Trying to find out for which server client connects
		for (i = 0; i < srv_count; i++) {
			if (m->header.token == tokens[i]) {
				found = 1;
				break;
			}
		}

		if (found != 1) {
			Log(LOG_ERR, "%s(%d): token %d not found ", __FUNCTION__, i,
					m->header.token);
			continue;
		}
		// Handling clients
		sprintf(connect.pth, pth, sizeof(pth));
		connect.srv_num = i;

		handle_client(srv_csock, srv_dsock, connect);

		Log(LOG_INFO,
				"%s(%d): We have a new client connection node: %s streaming = %d",
				__FUNCTION__, i, pth, server_cfg[i].streaming);

		if (server_cfg[i].streaming == 0) {
			Log(LOG_NOTICE,
					"%s(%d): no connected clients ADD GROUP MEMBERSHIP needed",
					__FUNCTION__, i);
			if (add_mgroup(i) == 0) {
				pthread_mutex_lock(&mutex);
				server_cfg[i].streaming = 1;
				pthread_mutex_unlock(&mutex);
			} else {
				Log(LOG_ERR,
						"%s(%d): Error has occured while add_mgroup do nothing",
						__FUNCTION__, i);
			}
		}

		send_accept(srv_csock, i);
		free(m);
	}
	return NULL;
}
// We need to translate received in ng answer value from [0000000de]: to int
u_int32_t parse_pth(char pth[NG_PATHSIZ]) {
	u_int32_t i, j;
	char buf[NG_PATHSIZ];
	memset(buf, 0, sizeof(buf));

	for (i = 1, j = 0; i < (strlen(pth) - 2); i++, j++) {
		buf[j] = pth[i];
	}
	return (u_int32_t) strtol(buf, NULL, 16);
}

// Sending accept message to ng_ksocket node for next client be able to connect
void send_accept(int srv_csock, int srv_num) {
	char path[NG_PATHSIZ];
	u_int32_t token;
	memset(path, 0, sizeof(path));
	sprintf(path, "%s%s:", server_cfg[srv_num].name, SERVSOCK);

	token = NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_ACCEPT,
	NULL, 0);
	if ((int) token < 0 && errno != EINPROGRESS && errno != EALREADY) {
		Log(LOG_ERR, "%s(%d): Accept Failed %s", __FUNCTION__, srv_num,
				strerror(errno));
	}
	Log(LOG_NOTICE, "%s(%d): Accept sent  to [%s] new token = %d", __FUNCTION__,
			srv_num, path, token);
	tokens[srv_num] = token;
	Log(LOG_NOTICE, "%s(%d): Accept sent  to [%s] new token = %d", __FUNCTION__,
			srv_num, path, token);
}
/* Function to handle each client connection
 *
 * */
int handle_client(int srv_csock, int srv_dsock, struct connect connect) {
	/*
	 * mkpeer . tee l2r left2right
	 * connect l2r [1d]: left ksockhook
	 * write l2r -f http.reply
	 * connect l2r fanout: right 0x1d
	 * shutdown l2r
	 * msg servsock: accept
	 *
	 */
	// Creating
	// Connection accepted nodeid in pth
	// connect fanout: [1d]: client-x client-x
	struct ngm_connect con;
	struct ngm_mkpeer mkp;
	char base_name[NG_PATHSIZ];
	char pth[NG_PATHSIZ], path[NG_PATHSIZ];
	char ourhook[NG_PATHSIZ], peerhook[NG_PATHSIZ];

	u_char tmp[200]; // Buffer for http replay
	int srv_num;

	memset(tmp, 0, sizeof(tmp));
	memset(base_name, 0, sizeof(base_name));
	srv_num = connect.srv_num;

	memcpy(pth, connect.pth, sizeof(pth));
	memcpy(base_name, server_cfg[srv_num].name, sizeof(base_name));
	/* mkpeer . tee l2r left2right  */
	sprintf(path, ".");
	sprintf(ourhook, "l2r");
	sprintf(peerhook, "left2right");

	sprintf(mkp.type, "%s", "tee");
	sprintf(mkp.ourhook, "%s", ourhook);
	sprintf(mkp.peerhook, "%s", peerhook);

	if (NgSendMsg(srv_csock, path, NGM_GENERIC_COOKIE, NGM_MKPEER, &mkp,
			sizeof(mkp)) < 0) {
		Log(LOG_ERR,
				"%s(%d): mkpeer %s tee %s %s Creating and connecting node error: %s",
				__FUNCTION__, srv_num, path, ourhook, peerhook,
				strerror(errno));
		return 0;
	}
	/* connect l2r [1d]: left ksockhook */
	sprintf(path, "%s", "l2r");
	sprintf(con.path, "%s", pth);
	sprintf(con.ourhook, "left");
	sprintf(con.peerhook, "ksockhook");

	if (NgSendMsg(srv_csock, path, NGM_GENERIC_COOKIE, NGM_CONNECT, &con,
			sizeof(con)) < 0 && errno != EISCONN) {
		Log(LOG_ERR, "%s(%d): connect %s %s %s %s : %s", __FUNCTION__, srv_num,
				path, con.path, con.ourhook, con.peerhook, strerror(errno));
		return 0;
	}
	/*
	 * write l2r -f http.reply
	 * Sending http replay to client through ng_tee
	 * */
	memcpy(tmp, http_replay, strlen(http_replay));
	sprintf(ourhook, "l2r");
	if (NgSendData(srv_dsock, ourhook, tmp, strlen((const char *) tmp)) < 0) {
		Log(LOG_ERR, "%s(%d): Error sending a message to %s: %s", __FUNCTION__,
				srv_num, ourhook, strerror(errno));
		shut_fanout();
		return 0;
	}

	/* connect l2r fanout: right 0x1d
	 *
	 */
	sprintf(con.path, "%s:", base_name);
	sprintf(con.ourhook, "right");
	sprintf(con.peerhook, "0x%s", connect.pth);
	// Our hook still = "l2r"
	sprintf(path, "%s", ourhook);

	if (NgSendMsg(srv_csock, path, NGM_GENERIC_COOKIE, NGM_CONNECT, &con,
			sizeof(con)) < 0 && errno != EISCONN) {
		Log(LOG_ERR, "%s(%d): connect %s %s %s %s : %s", __FUNCTION__, srv_num,
				path, con.path, con.ourhook, con.peerhook, strerror(errno));
		return 0;
	}
	/* shutdown l2r */
	sprintf(path, "%s", ourhook);
	if (NgSendMsg(srv_csock, path, NGM_GENERIC_COOKIE, NGM_SHUTDOWN, NULL, 0)
			< 0) {
		Log(LOG_ERR, "%s(%d): Failed to shutdown %s: %s", __FUNCTION__, srv_num,
				path, strerror(errno));
		return (EXIT_FAILURE);
	}
	return (EXIT_SUCCESS);
}
