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
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include "ng-r.h"

#define LST 64
#define	RUSAGE_SELF	0
#define	SERVSOCK	"-servsock"

/* External variables */

extern int srv_count;
extern pthread_mutex_t mutex;
extern uint32_t tokens[MAX_SERVERS], client_count;
extern client *primary;

/* External functions */
extern void shut_fanout(void);
extern void exit_nice(void);
extern int add_mgroup(int srv_num);
extern void Log(int log, const char *fmt, ...);

/*  Structures */
struct connect {
	char pth[NG_PATHSIZ];
	int srv_num;
};
/* Internal Functions */
int handle_client(struct connect);
void send_accept(int srv_num);
void * mkserver_http(void);
int create_listening_socket(int i);
uint32_t parse_pth(char pth[NG_PATHSIZ]);
int set_tos(char path[NG_PATHSIZ]);
int reuse_port(char path[NG_PATHSIZ]);
int no_delay(char path[NG_PATHSIZ]);
int get_client_address(int node_id);
int set_ksocket_sndbuf(char path[NG_PATHSIZ], int bufsiz);
int set_ksocket_rcvbuf(char path[NG_PATHSIZ], int bufsiz);
int get_ksocket_sndbuf(char path[NG_PATHSIZ]);
int set_ksocket_rcvbuf(char path[NG_PATHSIZ], int bufsiz);
int set_ksocket_shut_rd(char path[NG_PATHSIZ]);

int new_handle_client(struct connect connect);
int mkpeer_tee(struct connect connect);
int connect_tee(struct connect connect);
/* Global variables */

static int srv_csock, srv_dsock;
char http_replay[] =
		"HTTP/1.0 200 OK\r\nContent-type: application/octet-stream\r\nCache-Control: no-cache\r\n\r\n";

/* Make http server to serve clients
 * main function in this file
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
	//struct rusage rusage;
	char pth[NG_PATHSIZ];
	char name[NG_PATHSIZ];

	int i;

	m = NULL;

	memset(pth, 0, sizeof(pth));
	memset(&con, 0, sizeof(con));
	memset((char *) &addr, 0, sizeof(addr));
	memset(name, 0, sizeof(name));
	memset(tokens, 0, sizeof(tokens));
	// Naming Control socket node
	sprintf(name, "srv-csock-%d", getpid());
	if (NgMkSockNode(name, &srv_csock, &srv_dsock) < 0) {
		Log(LOG_ERR, "%s:%d %s(): Error Creating ng_socket: %s : %s",
		__FILE__, __LINE__, __func__, name, strerror(errno));
		return NULL;
	}
	// For each server in config file we need to create listening socket
	for (i = 0; i < srv_count; i++) {
		create_listening_socket(i);
	}
	// Serve clients
	for (;;) {
		int found = 0;
		if (NgAllocRecvMsg(srv_csock, &m, pth) < 0) {
			Log(LOG_ERR,
					"%s:%d %s(%d): Error receiving response from ksocket %s",
					__FILE__, __LINE__, __func__, i, strerror(errno));
			shut_fanout();
			exit_nice();
			return NULL;
		}
		// Trying to find out for which server client connects
		Log(LOG_INFO, "%s:%d %s() Received message from <%s>",
		__FILE__, __LINE__, __func__, pth);
		for (i = 0; i < srv_count; i++) {
			if (m->header.token == tokens[i]) {
				found = 1;
				break;
			}
		}

		if (found != 1) {
			Log(LOG_ERR, "%s:%d %s(%d): token %d not found ",
			__FILE__, __LINE__, __func__, i, m->header.token);
			continue;
		}
		// Handling clients
		sprintf(connect.pth, pth, sizeof(pth));
		connect.srv_num = i;

		handle_client(connect);

		Log(LOG_INFO,
				"%s:%d %s(%d): We have a new client connection node: %s streaming = %d",
				__FILE__, __LINE__, __func__, i, pth, server_cfg[i].streaming);
		// Change shared data structures

		pthread_mutex_lock(&mutex);
		primary[client_count].node_id = parse_pth(pth);
		primary[client_count].srv_num = i;
		get_client_address(primary[client_count].node_id);
		client_count++;
		pthread_mutex_unlock(&mutex);

		if (server_cfg[i].streaming == 0) {
			Log(LOG_NOTICE,
					"%s:%d %s(%d): no connected clients ADD GROUP MEMBERSHIP needed",
					__FILE__, __LINE__, __func__, i);
			if (add_mgroup(i) == 0) {
				pthread_mutex_lock(&mutex);
				server_cfg[i].streaming = 1;
				pthread_mutex_unlock(&mutex);
			} else {
				Log(LOG_ERR,
						"%s:%d %s(%d): Error has occured while add_mgroup do nothing",
						__FILE__, __LINE__, __func__, i);
			}
		}

		send_accept(i);
		free(m);
	}
	return NULL;
}

int set_tos(char path[NG_PATHSIZ]) {
	int tos;
	union {
		u_char buf[sizeof(struct ng_ksocket_sockopt) + sizeof(int)];
		struct ng_ksocket_sockopt sockopt;
	} sockopt_buf;
	struct ng_ksocket_sockopt * const sockopt = &sockopt_buf.sockopt;
	//struct ng_ksocket_sockopt *sockopt_resp = malloc(sizeof(struct ng_ksocket_sockopt) + sizeof(int));
	//struct ng_mesg *m;

	// set dscp value 32 for socket
	sockopt->level = IPPROTO_IP;
	sockopt->name = IP_TOS;
	tos = IPTOS_DSCP_CS4;

	memcpy(sockopt->value, &tos, sizeof(tos));
	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_SETOPT,
			sockopt, sizeof(sockopt_buf)) == -1) {
		Log(LOG_ERR, "%s:%d %s(): Sockopt set failed : %s",
		__FILE__, __LINE__, __func__, strerror(errno));
		return 0;
	}
	return 1;
}

int reuse_port(char path[NG_PATHSIZ]) {
	/* REUSE_ADDR and REUSE_PORT actually */
	union {
		u_char buf[sizeof(struct ng_ksocket_sockopt) + sizeof(int)];
		struct ng_ksocket_sockopt sockopt;
	} sockopt_buf;
	struct ng_ksocket_sockopt * const sockopt = &sockopt_buf.sockopt;
	int one = 1;

	memset(&sockopt_buf, 0, sizeof(sockopt_buf));

	sockopt->level = SOL_SOCKET;
	sockopt->name = SO_REUSEADDR;
	memcpy(sockopt->value, &one, sizeof(int));
	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_SETOPT,
			sockopt, sizeof(sockopt_buf)) == -1) {
		Log(LOG_ERR, "%s:%d %s(): Sockopt set failed : %s",
		__FILE__, __LINE__, __func__, strerror(errno));
		return -1;
	}
	sockopt->name = SO_REUSEPORT;
	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_SETOPT,
			sockopt, sizeof(sockopt_buf)) == -1) {
		Log(LOG_ERR, "%s:%d %s(): Sockopt set failed : %s",
		__FILE__, __LINE__, __func__, strerror(errno));
		return -1;
	}
	return 1;
}

int no_delay(char path[NG_PATHSIZ]) {
	/* Set tcp sockopt NO_DELAY for ksocknode */
	union {
		u_char buf[sizeof(struct ng_ksocket_sockopt) + sizeof(int)];
		struct ng_ksocket_sockopt sockopt;
	} sockopt_buf;
	struct ng_ksocket_sockopt * const sockopt = &sockopt_buf.sockopt;
	int one = 1;

	memset(&sockopt_buf, 0, sizeof(sockopt_buf));

	sockopt->level = IPPROTO_TCP;
	sockopt->name = TCP_NODELAY;
	memcpy(sockopt->value, &one, sizeof(int));
	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_SETOPT,
			sockopt, sizeof(sockopt_buf)) == -1) {
		Log(LOG_ERR, "%s(): Sockopt set failed : %s", __func__,
				strerror(errno));
		return -1;
	}
	return 1;
}

int create_listening_socket(int i) {
	/* Creating listening socket for single server */
	char path[NG_PATHSIZ], ourhook[NG_HOOKSIZ];
	char name[NG_NODESIZ];
	struct ngm_mkpeer mkp;
	int lst;
	uint32_t token;
	const char *basename;

	// mkpeer . ksocket listen/stream/tcp
	basename = server_cfg[i].name;

	sprintf(path, ".");
	sprintf(ourhook, "%s%d", "listen", i);
	snprintf(mkp.type, sizeof(mkp.type), "ksocket");
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "%s", ourhook);
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), "inet/stream/tcp");

	if (NgSendMsg(srv_csock, path, NGM_GENERIC_COOKIE, NGM_MKPEER, &mkp,
			sizeof(mkp)) < 0) {
		Log(LOG_ERR,
				"%s:%d %s(%d): Creating and connecting node path = %s error: %s",
				__FILE__, __LINE__, __func__, i, path, strerror(errno));
		return -1;
	}
	// name    .:listen hub0-servsock
	sprintf(path, ".:%s", ourhook);
	sprintf(name, "%s%s", basename, SERVSOCK);
	if (NgNameNode(srv_csock, path, "%s", name) < 0) {
		Log(LOG_ERR, "%s:%d %s(%d): Naming Node failed at path: %s : %s",
		__FILE__, __LINE__, __func__, i, path, strerror(errno));
		return -1;
	}

	// msg servsock: bind inet/0.0.0.0:8080
	sprintf(path, "%s%s:", server_cfg[i].name, SERVSOCK);

	Log(LOG_NOTICE, "%s:%d %s(%d): Trying to bind to interface %s:%d",
	__FILE__, __LINE__, __func__, i, inet_ntoa(server_cfg[i].dst.sin_addr),
			ntohs(server_cfg[i].dst.sin_port));

	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_BIND,
			&server_cfg[i].dst, sizeof(struct sockaddr_in)) < 0) {
		Log(LOG_ERR, "%s:%d %s(%d): Can't bind on address: %s:%d err: %s",
		__FILE__, __LINE__, __func__, i, inet_ntoa(server_cfg[i].dst.sin_addr),
				ntohs(server_cfg[i].dst.sin_port), strerror(errno));
		return -1;
	}
	// Setting REUSE_PORT
	reuse_port(path);
	// Setting tos value
	no_delay(path);
	set_ksocket_sndbuf(path, 256 * 1024);
	set_ksocket_rcvbuf(path, 256 * 1024);
	//  msg servsock: listen 64
	lst = LST;
	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_LISTEN, &lst,
			sizeof(lst)) < 0) {
		Log(LOG_ERR, "%s:%d %s(%d): msg servsock: listen 64 failed %s",
		__FILE__, __LINE__, __func__, i, strerror(errno));
		return -1;
	}

	Log(LOG_NOTICE, "%s:%d %s(%d): Starting cycle %d of ACCEPT", __FILE__,
			__LINE__, __func__, i, i);
	// msg servsock: accept
	sprintf(path, "%s%s:", server_cfg[i].name, SERVSOCK);
	// If not first connection - check and clear useless ksock nodes

	token = NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_ACCEPT,
			NULL, 0);
	if ((int) token < 0 && errno != EINPROGRESS && errno != EALREADY) {
		Log(LOG_ERR, "%s(%d): Accept Failed %s", __func__, i, strerror(errno));
		return -1;
	}
	tokens[i] = token;
	return 1;
}

int get_client_address(int node_id) {
	uint32_t token;
	char idbuf[NG_PATHSIZ];
	struct ng_mesg *resp;
	struct sockaddr_in *peername;

	memset(idbuf, 0, sizeof(idbuf));
	snprintf(idbuf, sizeof(idbuf), "[%08x]:", node_id);
	token = NgSendMsg(srv_csock, idbuf, NGM_KSOCKET_COOKIE,
			NGM_KSOCKET_GETPEERNAME, NULL, 0);
	if ((int) token == -1) {
		if (errno == ENOTCONN) {
			Log(LOG_INFO, "%s:%d %s : Socket not connected, node %s",
			__FILE__, __LINE__, __func__, idbuf);
			return 1;
		} else if (errno == ENOENT) {
			Log(LOG_NOTICE, "%s:%d %s(): Node already closed %s",
			__FILE__, __LINE__, __func__, idbuf);
			return 1;
		} else {
			Log(LOG_ERR,
					"%s (): An error has occured while getpeername from node: %s, %s",
					__FILE__, __LINE__, __func__, idbuf, strerror(errno));
			return 0;
		}
	}
	if (NgAllocRecvMsg(srv_csock, &resp, NULL) < 0) {
		return 0;
	}

	peername = (struct sockaddr_in *) resp->data;
	primary[client_count].addr.sin_family = peername->sin_family;
	primary[client_count].addr.sin_len = peername->sin_len;
	primary[client_count].addr.sin_addr = peername->sin_addr;
	primary[client_count].addr.sin_port = peername->sin_port;
	Log(LOG_NOTICE, "%s:%d %s(): serving new client connection from %s:%d",
	__FILE__, __LINE__, __func__, inet_ntoa(peername->sin_addr),
			ntohs(peername->sin_port));
	free(resp);
	return 1;
}

// We need to translate received in ng answer value from [0000000de]: to int
uint32_t parse_pth(char pth[NG_PATHSIZ]) {
	uint32_t i, j;
	char buf[NG_PATHSIZ];
	memset(buf, 0, sizeof(buf));

	for (i = 1, j = 0; i < (strlen(pth) - 2); i++, j++) {
		buf[j] = pth[i];
	}
	return (uint32_t) strtol(buf, NULL, 16);
}

// Sending accept message to ng_ksocket node for next client be able to connect
void send_accept(int srv_num) {
	char path[NG_PATHSIZ];
	uint32_t token;
	memset(path, 0, sizeof(path));
	sprintf(path, "%s%s:", server_cfg[srv_num].name, SERVSOCK);

	token = NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_ACCEPT,
			NULL, 0);
	if ((int) token < 0 && errno != EINPROGRESS && errno != EALREADY) {
		Log(LOG_ERR, "%s:%d %s(%d): Accept Failed %s",
		__FILE__, __LINE__, __func__, srv_num, strerror(errno));
	}
	tokens[srv_num] = token;
	Log(LOG_NOTICE, "%s:%d %s(%d): Accept sent  to [%s] new token = %d",
	__FILE__, __LINE__, __func__, srv_num, path, token);
}
/* New moduled function to handle client connection
 * 
 * */
int new_handle_client(struct connect connect) {

	return 1;
}

/* Function to handle each client connection
 *
 * */
int handle_client(struct connect connect) {
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
	char path[NG_PATHSIZ];
	char ourhook[NG_HOOKSIZ];
	int srv_num = connect.srv_num;

	/* mkpeer . tee l2r left2right  */
	mkpeer_tee(connect);
	/* connect l2r [1d]: left ksockhook */
	connect_tee(connect);
	/*
	 * write l2r -f http.reply
	 * Sending http replay to client through ng_tee
	 * */
	snprintf(ourhook, sizeof(ourhook), "l2r");
	if (NgSendData(srv_dsock, ourhook, http_replay, strlen(http_replay)) < 0) {
		Log(LOG_ERR, "%s:%d %s(%d): Error sending a message to %s: %s",
		__FILE__, __LINE__, __func__, srv_num, ourhook, strerror(errno));
		shut_fanout();
		return 0;
	}
	set_tos(connect.pth);
	set_ksocket_shut_rd(connect.pth);

	/* connect l2r fanout: right 0x1d
	 *
	 */
	Log(LOG_INFO, "%s:%d connect.pth = %s", __FILE__, __LINE__, connect.pth);
	snprintf(con.path, sizeof(con.path), "%s:",
			server_cfg[connect.srv_num].name);
	snprintf(con.ourhook, sizeof(con.ourhook), "right");
	snprintf(con.peerhook, sizeof(con.peerhook), "0x%s", connect.pth);
	// Our hook still = "l2r"
	snprintf(path, sizeof(path), "%s", "l2r");

	if (NgSendMsg(srv_csock, path, NGM_GENERIC_COOKIE, NGM_CONNECT, &con,
			sizeof(con)) < 0 && errno != EISCONN) {
		Log(LOG_ERR, "%s:%d %s(%d): connect %s %s %s %s : %s",
		__FILE__, __LINE__, __func__, srv_num, path, con.path, con.ourhook,
				con.peerhook, strerror(errno));
		return 0;
	}
	/* shutdown l2r */
	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "l2r");
	if (NgSendMsg(srv_csock, path, NGM_GENERIC_COOKIE, NGM_SHUTDOWN, NULL, 0)
			< 0) {
		Log(LOG_ERR, "%s:%d %s(%d): Failed to shutdown %s: %s",
		__FILE__, __LINE__, __func__, srv_num, path, strerror(errno));
		return (EXIT_FAILURE);
	}
	pthread_mutex_lock(&mutex);
	server_cfg[connect.srv_num].c_count++;
	pthread_mutex_unlock(&mutex);
	return (EXIT_SUCCESS);
}
/*
 *
 * */
int mkpeer_tee(struct connect connect) {
	char path[NG_PATHSIZ];
	struct ngm_mkpeer mkp;

	memset(path, 0, sizeof(path));
	/* mkpeer . tee l2r left2right  */
	snprintf(path, sizeof(path), ".");
	snprintf(mkp.type, sizeof(mkp.type), "%s", "tee");
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "%s", "l2r");
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), "%s", "left2right");
	/*
	 if (NgNameNode(srv_csock, pth, "client%d-%d", srv_num, server_cfg[srv_num].c_count) < 0 ) {
	 Log(LOG_ERR, "%s:%d Error naming node %s - %s", __FILE__, __LINE__, pth, strerror(errno));
	 }
	 */
	if (NgSendMsg(srv_csock, path, NGM_GENERIC_COOKIE, NGM_MKPEER, &mkp,
			sizeof(mkp)) < 0) {
		Log(LOG_ERR,
				"%s:%d mkpeer %s tee %s %s Creating and connecting node error: %s",
				__FILE__, __LINE__, connect.srv_num, path, mkp.ourhook,
				mkp.peerhook, strerror(errno));
		return 0;
	}
	return 1;
}

int connect_tee(struct connect connect) {
	/* connect l2r [1d]: left ksockhook */
	char path[NG_PATHSIZ];
	struct ngm_connect con;

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path), "%s", "l2r");
	snprintf(con.path, sizeof(con.path), "%s", connect.pth);
	snprintf(con.ourhook, sizeof(con.ourhook), "left");
	snprintf(con.peerhook, sizeof(con.peerhook), "ksockhook");

	if (NgSendMsg(srv_csock, path, NGM_GENERIC_COOKIE, NGM_CONNECT, &con,
			sizeof(con)) < 0 && errno != EISCONN) {
		Log(LOG_ERR, "%s:%d %s(%d): connect %s %s %s %s : %s",
		__FILE__, __LINE__, __func__, connect.srv_num, path, con.path,
				con.ourhook, con.peerhook, strerror(errno));
		return 0;
	}

	return 1;
}

int get_ksocket_sndbuf(char path[NG_PATHSIZ]) {
	struct ng_ksocket_sockopt *getopt = malloc(
			sizeof(struct ng_ksocket_sockopt) + sizeof(int));

	memset(getopt, 0, sizeof(struct ng_ksocket_sockopt) + sizeof(int));

	getopt->level = SOL_SOCKET;
	getopt->name = SO_SNDBUF;

	NgSetDebug(3);
	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_GETOPT,
			getopt, sizeof(*getopt)) < 0) {
		Log(LOG_ERR, "%s:%d %s() Error getting sockopt %s", __FILE__, __LINE__,
				__func__, strerror(errno));
	} else {
		struct ng_mesg *resp;
		size_t *value;

		if (NgAllocRecvMsg(srv_csock, &resp, 0) < 0) {
			Log(LOG_ERR,
					"%s:%d %s() Error while trying to get message from getsockopt: %s",
					__FILE__, __LINE__, __func__, strerror(errno));
			return -1;
		}
		value = (size_t *) getopt->value;
		Log(LOG_INFO, "%s:%d %s() current snd_buf_size = %d", __FILE__,
				__LINE__, __func__, *value);
		struct ng_ksocket_sockopt *skopt;
		skopt = (struct ng_ksocket_sockopt *) resp->data;
		Log(LOG_INFO, "received sockopt len = %d",
				resp->header.arglen - sizeof(struct ng_ksocket_sockopt));
		value = (size_t *) skopt->value;
		Log(LOG_INFO, "%s:%d %s() current snd_buf_size_in_resp = %lu", __FILE__,
				__LINE__, __func__, *value);
		free(getopt);
		free(resp);
	}
	NgSetDebug(0);
	return 1;
}
/*
 *
 * */
int set_ksocket_sndbuf(char path[NG_PATHSIZ], int bufsiz) {
	union {
		u_char buf[sizeof(struct ng_ksocket_sockopt) + sizeof(int)];
		struct ng_ksocket_sockopt sockopt;
	} sockopt_buf;
	struct ng_ksocket_sockopt * const sockopt = &sockopt_buf.sockopt;

	Log(LOG_INFO, "BEFORE");
	get_ksocket_sndbuf(path);
	memset(&sockopt_buf, 0, sizeof(sockopt_buf));
	sockopt->level = SOL_SOCKET;
	sockopt->name = SO_SNDBUF;

	memcpy(sockopt->value, &bufsiz, sizeof(bufsiz));

	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_SETOPT,
			sockopt, sizeof(sockopt_buf)) < 0) {
		Log(LOG_ERR, "%s:%d %s() Error sending message %s",
		__FILE__, __LINE__, __func__, strerror(errno));
		return -1;
	}

	Log(LOG_INFO, "AFTER");
	get_ksocket_sndbuf(path);
	return 1;
}
/*
 *
 *
 * */
int set_ksocket_rcvbuf(char path[NG_PATHSIZ], int bufsiz) {
	union {
		u_char buf[sizeof(struct ng_ksocket_sockopt) + sizeof(int)];
		struct ng_ksocket_sockopt sockopt;
	} sockopt_buf;

	struct ng_ksocket_sockopt * const sockopt = &sockopt_buf.sockopt;
	struct ng_ksocket_sockopt *getopt = malloc(
			sizeof(struct ng_ksocket_sockopt) + sizeof(int));
	getopt->level = SOL_SOCKET;
	getopt->name = SO_RCVBUF;
	memset(getopt, 0, sizeof(struct ng_ksocket_sockopt) + sizeof(int));
	Log(LOG_INFO, "%s:%d %s() sizeof(*getopt) = %d", __FILE__, __LINE__,
			__func__, sizeof(*getopt));
	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_GETOPT,
			getopt, sizeof(*getopt)) < 0) {
		Log(LOG_ERR, "%s:%d %s() Error getting sockopt %s", __FILE__, __LINE__,
				__func__, strerror(errno));
	} else {
		Log(LOG_INFO, "%s:%d %s() current snd_buf_size = %d", __FILE__,
				__LINE__, __func__, getopt->value);
		free(getopt);
	}

	memset(&sockopt_buf, 0, sizeof(sockopt_buf));
	sockopt->level = SOL_SOCKET;
	sockopt->name = SO_RCVBUF;

	memcpy(sockopt->value, &bufsiz, sizeof(bufsiz));

	if (NgSendMsg(srv_csock, path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_SETOPT,
			sockopt, sizeof(sockopt_buf)) < 0) {
		Log(LOG_ERR, "%s:%d %s() Error sending message %s",
		__FILE__, __LINE__, __func__, strerror(errno));
		return -1;
	}
	return 1;
}

/*
 int set_ksocket_shut_rd( char path[NG_PATHSIZ] ) {

 if ( NgSendMsg( srv_csock,  path, NGM_KSOCKET_COOKIE, NGM_KSOCKET_SHUTDOWN, SHUT_RD, sizeof(int)) < 0) {
 Log(LOG_ERR, "%s:%d %s Error shuting down socket %s",
 __FILE__, __LINE__, __func__, strerror(errno));
 return -1;
 }
 return 1;
 }
 */
