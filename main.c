/*
 *  Netgraph alternative to open-source project udpxy
 *  and non free software known as "relaying"
 */

#include	<stdio.h>
#include	<netgraph.h>
#include	<stdlib.h>
#include <malloc_np.h>
#include	<err.h>
#include	<errno.h>
#include	<sys/types.h>
#include	<sys/socket.h>
#include	<sys/stat.h>
#include	<netgraph.h>
#include	<netgraph/ng_message.h>
#include	<netgraph/ng_socket.h>
#include	<netgraph/ng_ksocket.h>
#include	<netgraph/ng_hub.h>
#include	<netinet/in.h>
#include	<netdb.h>
#include	<string.h>
#include	<strings.h>
#include	<signal.h>
#include	<getopt.h>
#include	<syslog.h>
#include	<unistd.h>
#include	<stdarg.h>
#include	<unistd.h>
#include	<pthread.h>
#include	"ng-r.h"

#define		VERSION	"0.0.10"
#define		LST		64
#define		PIDFILE	"/var/run/mcastng.pid"

#define		MAX_THREADS	1024
#define		CFG_PATH	"/usr/local/etc/mcastng.cfg"
#define		LOGFILE  "/var/log/mcastng.log"
#define		UNNAMED	"<unnamed>"
#define		Log_IDENT	"mcastng"

// Internal Functions
void shut_fanout(void);
void usage(const char *progname);
void signal_handler(int sig);
void exit_nice(void);
void daemonize(void);
int check_and_clear(int srv_num, int cmonsock);
int shut_clients(int srv_num, int cmonsock);
int client_dead(int node, int cmonsock);
int shut_node(char path[NG_PATHSIZ]);
void print_config(void);

// External Functions
extern int mkhub_udp(int srv_num);
extern int drop_mgroup(int srv_num);
extern int config(const char *filename);
extern void *mkserver_http(void);
extern void Log(int log, const char *fmt, ...);

// Global Variables
const char *logfile;
int srv_count, thr;  // Global server counter
int daemonized;
uint32_t client_count = 0; // Global client counter
pthread_t threads[MAX_THREADS], main_thread;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
uint32_t tokens[MAX_SERVERS];

// Main Program
int main(int argc, char **argv) {
	extern int csock, dsock;
	const char *cfgfile;
	char buf[BUF_LEN], name[BUF_LEN];
	char path[NG_PATHSIZ];
	int cfgflag, debug, nflag, dflag, iuflag, ihtflag, ohtflag, ouflag, iuiflag;
	int ch, err, i;
	int cmonsock, dmonsock;
	char pth[NG_PATHSIZ];

	// Start Logging
	openlog(Log_IDENT, 0, LOG_USER);

	memset(clients_primary, 0, sizeof(clients_primary));
	memset(clients_secondary, 0, sizeof(clients_secondary));

	cfgflag = debug = dflag = iuflag = nflag = ihtflag = ohtflag = ouflag =
			iuiflag = 0;
	// Thread counter set to 0 and zeroing threads array
	memset(threads, 0, sizeof(threads));
	thr = 0;
	logfile = LOGFILE;
	cfgfile = NULL;

	if (argc < 2) {
		usage(argv[0]);
	}

	while ((ch = getopt(argc, argv, "c:dvb?")) != -1) {
		switch (ch) {
		case 'v':
			printf("%s: Version: %s\n", __FUNCTION__, VERSION);
			exit(EXIT_FAILURE);
			break;
		case '?':
			usage(argv[0]);
			break;
		case 'b':
			dflag = 1;
			break;
		case 'd':
			debug = 1;
			break;
		case 'c':
			cfgflag = 1;
			cfgfile = optarg;
			break;
		default:
			usage(argv[0]);
			break;

		}
	}
	argc -= optind;
	argv += optind;
	// Checking if -c command line key defined
	if (cfgflag == 0)
		cfgfile = CFG_PATH;
	if (config(cfgfile) < 0) {
		if (debug == 1) {
			fprintf(stderr, "%s: print config\n", __FUNCTION__);
		}
		fprintf(stderr, "error: parsing config file failed so exit\n");
		exit(EXIT_FAILURE);
	}

	//print_config();

	if (dflag == 1) {
		daemonize();
	}

	memset(buf, 0, sizeof(buf));
	memset(name, 0, sizeof(name));

	sprintf(name, "mcastng%d", getpid());
	if (debug == 1)
		NgSetDebug(4);

	if (debug == 1)
		Log(LOG_DEBUG, "main(): NgSetErrLog done");

	// Handling Ctrl + C and other signals
	signal(SIGTSTP, signal_handler);
	signal(SIGTTIN, signal_handler);
	signal(SIGTTOU, signal_handler);
	signal(SIGSYS, signal_handler);
	signal(SIGTRAP, signal_handler);
	signal(SIGXCPU, signal_handler);
	signal(SIGXFSZ, signal_handler);
	signal(SIGSTOP, signal_handler);
	signal(SIGINT, signal_handler);
	signal(SIGSEGV, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGABRT, signal_handler);
	signal(SIGALRM, signal_handler);
	signal(SIGFPE, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGILL, signal_handler);
	signal(SIGKILL, signal_handler);
	signal(SIGPIPE, signal_handler);
	signal(SIGQUIT, signal_handler);
	signal(SIGUSR1, signal_handler);
	if (debug == 1)
		Log(LOG_DEBUG, "%s: signals done", __FUNCTION__);

	if (NgMkSockNode(name, &csock, &dsock) < 0) {
		Log(LOG_ERR, "%s: Creation of Ngsocket Failed: %s", __FUNCTION__,
				strerror(errno));
		exit(EXIT_FAILURE);
	}

	// First shutdown all hubs just in case it was created previously
	for (i = 0; i < srv_count; i++) {
		sprintf(path, "%s:", server_cfg[i].name);
		if (NgSendMsg(csock, path, NGM_GENERIC_COOKIE, NGM_SHUTDOWN, NULL, 0)
				< 0 && errno != ENOENT) {
			Log(LOG_ERR, "%s: Error shutdowning %s: %s", path, __FUNCTION__,
					strerror(errno));
		}
	}

	for (i = 0; i < srv_count; i++) {
		if (mkhub_udp(i) == 0) {
			Log(LOG_ERR, "%s: mkhub function died : %s", __FUNCTION__,
					strerror(errno));
			exit(EXIT_FAILURE);
		}
	}
	Log(LOG_NOTICE, "%s() All hubs created", __FUNCTION__);
	close(csock);
	close(dsock);

	main_thread = pthread_self();

	// Starting Http servers
	err = pthread_create(&threads[thr], NULL, (void *) mkserver_http, NULL);
	if (err != 0)
		Log(LOG_ERR, "%s: Failed to create thread %d: %s", __FUNCTION__, i,
				strerror(err));

	errno = 0;
	sprintf(pth, "mcastng-mon-%d", getpid());
	/*
	 memset(tmp_clients, 0, sizeof(tmp_clints));
	 */
	// Create separate netgraph socket to do the job
	if (NgMkSockNode(pth, &cmonsock, &dmonsock) < 0) {
		Log(LOG_ERR, "%s: Can`t create Netgraph monsock : %s", __FUNCTION__,
				strerror(errno));
		return 0;
	}

	for (;;) {
		for (i = 0; i < srv_count; i++) {
			check_and_clear(i, cmonsock);
		}
		sleep(10);
	}

	return EXIT_SUCCESS;
}

void exit_nice(void) {
	int err, i;

	for (i = 0; i < thr; i++) {
		err = pthread_cancel(threads[i]);
		if (err != 0) {
			Log(LOG_ERR, "exit_nice(): Error occured while pthread_cancel: %s",
					strerror(err));
		} else {
			Log(LOG_NOTICE, "exit_nice(): Thread thread[%d] closed successful",
					i);
		}
	}
	exit(EXIT_FAILURE);
}

/*
 Subs to handle signals
 */
void signal_handler(int sig) {
	switch (sig) {
	case SIGINT:
		Log(LOG_INFO, "%s: Caught SIGINT shutting down", __FUNCTION__);
		shut_fanout();
		unlink(PIDFILE);
		exit_nice();
		break;
	case SIGSEGV:
		Log(LOG_INFO, "%s: Caught SIGSEGV shutting down", __FUNCTION__);
		shut_fanout();
		unlink(PIDFILE);
		exit_nice();
		break;
	case SIGTERM:
		Log(LOG_INFO, "%s: Caught SIGTERM shutting down", __FUNCTION__);
		shut_fanout();
		unlink(PIDFILE);
		exit_nice();
		break;
	case SIGUSR1:
		Log(LOG_INFO, "%s: Caught SIGUSR1 calling check_and_clear()",
				__FUNCTION__);
		break;
	default:
		Log(LOG_INFO, "%s: %s signal catched closing all", __FUNCTION__,
				strsignal(sig));
		shut_fanout();
		unlink(PIDFILE);
		exit_nice();
		break;
	}

}

/* Get peer name to find out is this node still connected to client or not
 */
int client_dead(int node, int cmonsock) {
	/* if client node given by idbuf in [0x0112]: format
	 * dead - shut it down and return 1
	 * other situation return 0
	 * */
	uint32_t token;
	char idbuf[NG_NODESIZ];

	memset (idbuf, 0, sizeof(idbuf));
	snprintf(idbuf, sizeof(idbuf), "[%08x]:", node);
	token = NgSendMsg(cmonsock, idbuf, NGM_KSOCKET_COOKIE,
			NGM_KSOCKET_GETPEERNAME, NULL, 0);
	if ((int) token == -1) {
		if (errno == ENOTCONN) {
			syslog(LOG_INFO,
					"%s : Socket not connected, node %s: will be shutdown",
					__FUNCTION__, idbuf);
			shut_node(idbuf);
			return 1;
		} else if (errno == ENOENT) {
			syslog(LOG_NOTICE, "%s (): Node already closed %s", __FUNCTION__,
					idbuf);
			return 1;
		} else {
			syslog(LOG_ERR,
					"%s (): An error has occured while getpeername from node: %s, %s",
					__FUNCTION__,  idbuf, strerror(errno));
			return 0;
		}
	}
	return 0;
}
/* Get peer name to define is ksocket connected to client or not
 */
int check_and_clear(int srv_num, int cmonsock) {
	/*
	 
	 Goal of this function is to find and shutdown nodes that don`t have
	 connected clients (NGM_KSOCKET_GETPEERNAME returns error ENOTCONN),
	 for that specific client, the srv_num variable points on
	 which hub we should examine (server_cfg[srv_num].)

	 */

	// hubXXX - listhooks than for each hook getpeername
	int i;
	uint32_t sec_idx = 0;
	uint32_t c_count;
	client *tmp;
	pthread_mutex_lock(&mutex);
	for (i = 0; i < client_count; i++) {
		if (check_dead(primary[i].node_id)) {
			// Dead node detected
			int srv_num = primary[i].srv_num;
			if (--server_cfg[srv_num].c_count == 0) {
				drop_mgroup(srv_num);
			}
			server_cfg[srv_num].streaming = 0;
		} else {
			secondary[sec_idx] = primary[i];
			sec_idx++;
		}
	}
	pthread_mutex_unlock(&mutex);

	tmp = primary;
	primary = secondary;
	secondary = tmp;

	/*
	union {
		u_char buf[sizeof(struct ng_mesg) + sizeof(struct sockaddr)];
		struct ng_mesg reply;
	} ugetsas;
	struct ng_mesg *resp;
	struct hooklist *hlist;
	struct nodeinfo *ninfo;
	//struct sockaddr_in *ips;
	int i, c_count;
	char pth[NG_PATHSIZ], hub[NG_PATHSIZ];
	char peername[NG_NODESIZ];
	uint32_t token;
	resp = NULL;

	memset(&ugetsas, 0, sizeof(ugetsas));
	memset(pth, 0, sizeof(pth));
	memset(hub, 0, sizeof(hub));

	sprintf(hub, "%s:", server_cfg[srv_num].name);
	// Get hooklist from hub
	token = NgSendMsg(cmonsock, hub, NGM_GENERIC_COOKIE, NGM_LISTHOOKS, NULL,
			0);
	if ((int) token < 0) {
		Log(LOG_ERR, "%s(%d): Filed to get hooklist from node %s: %s",
				__FUNCTION__, srv_num, server_cfg[srv_num].name,
				strerror(errno));
		return 0;
	}

	// Receiving node_list
	do {
		if (resp != NULL) {
			free(resp);
		}

		if (NgAllocRecvMsg(cmonsock, &resp, NULL) < 0) {
			Log(LOG_ERR, "%s(%d): Failed to receive hooklist from hub: %s",
					__FUNCTION__, srv_num, strerror(errno));
			exit_nice();
		}
	} while (resp->header.token != token);

	hlist = (struct hooklist *) resp->data;
	ninfo = (struct nodeinfo *) &hlist->nodeinfo;

	c_count = 0;
	bzero(peername, sizeof(peername));

	if (ninfo->hooks > 0) {
		for (i = 0; (u_int32_t) i < ninfo->hooks; i++) {
			struct nodeinfo * const peer = &hlist->link[i].nodeinfo;
			char idbuf[NG_PATHSIZ];

			if (!*peer->name) {
				snprintf(peername, strlen(UNNAMED) + 1, "%s", UNNAMED);
			} else {
				snprintf(peername, strlen(peer->name) + 1, "%s", peer->name);
			}

			snprintf(idbuf, sizeof(idbuf), "[%08x]:", peer->id);
			if ((strcmp(peer->type, "ksocket") == 0)
					&& (strcmp(peername, UNNAMED) == 0)) {
				c_count++;
				token = NgSendMsg(cmonsock, idbuf, NGM_KSOCKET_COOKIE,
						NGM_KSOCKET_GETPEERNAME, NULL, 0);
				if ((int) token == -1) {
					if (errno == ENOTCONN) {
						Log(LOG_INFO,
								"%s(%d): Socket not connected, node: %s will be shutdown",
								__FUNCTION__, srv_num, pth);
						shut_node(idbuf);
					} else if (errno == ENOENT) {
						Log(LOG_NOTICE, "%s(%d): Node already closed %s",
								__FUNCTION__, srv_num, pth);
					} else {
						Log(LOG_ERR,
								"%s(%d): An error has occured while getpeername from node: %s, %s",
								__FUNCTION__, srv_num, pth, strerror(errno));
					}
				}
			}
		}
		if (c_count == 0 && server_cfg[srv_num].streaming == 1) {
			Log(LOG_NOTICE,
					"%s(%d): no clients found in %s dropping group membership",
					__FUNCTION__, srv_num, hub);
			Log(LOG_NOTICE, "%s(%d): streaming[%d] = %d, c_count = %d",
					__FUNCTION__, srv_num, srv_num,
					server_cfg[srv_num].streaming, c_count);
			drop_mgroup(srv_num);
			Log(LOG_INFO, "%s(%d): trying to lock data mutex", __FUNCTION__,
					srv_num);

			pthread_mutex_lock(&mutex);
			Log(LOG_INFO, "%s(%d): data mutex locked", __FUNCTION__, srv_num);
			server_cfg[srv_num].streaming = 0;
			pthread_mutex_unlock(&mutex);
			Log(LOG_INFO, "%s(%d): data mutex unlocked", __FUNCTION__, srv_num);
		}
	} else {
		Log(LOG_NOTICE,
				"%s(%d): There is no hooks connected to hub it`s strange ",
				__FUNCTION__, srv_num);
	}

	free(resp);
	*/
	return EXIT_SUCCESS;
}
// Shutdown clients
int shut_clients(int srv_num, int cmonsock) {
	union {
		u_char buf[sizeof(struct ng_mesg) + sizeof(struct sockaddr)];
		struct ng_mesg reply;
	} ugetsas;
	struct ng_mesg *resp;
	struct hooklist *hlist;
	struct nodeinfo *ninfo;
	int i;
	char pth[NG_PATHSIZ], hub[NG_PATHSIZ];
	char peername[NG_NODESIZ];
	uint32_t token;
	resp = NULL;

	memset(&ugetsas, 0, sizeof(ugetsas));
	memset(pth, 0, sizeof(pth));
	memset(hub, 0, sizeof(hub));

	bzero(peername, sizeof(peername));
	sprintf(hub, "%s:", server_cfg[srv_num].name);
	// Get hooklist from hub

	token = NgSendMsg(cmonsock, hub, NGM_GENERIC_COOKIE, NGM_LISTHOOKS, NULL,
			0);
	if ((int) token < 0) {
		Log(LOG_ERR, "%s(%d): Filed to get hooklist from node %s: %s",
				__FUNCTION__, srv_num, server_cfg[srv_num].name,
				strerror(errno));
		return EXIT_FAILURE;
	}
	// Receiving node_list
	do {
		if (resp != NULL) {
			free(resp);
		}
		if (NgAllocRecvMsg(cmonsock, &resp, NULL) < 0) {
			Log(LOG_ERR, "%s(%d): Failed to receive hooklist from hub: %s",
					__FUNCTION__, srv_num, strerror(errno));
			return EXIT_FAILURE;
		}
	} while (resp->header.token != token);
	hlist = (struct hooklist *) resp->data;
	ninfo = (struct nodeinfo *) &hlist->nodeinfo;
	if (ninfo->hooks > 0) {
		for (i = 0; (u_int32_t) i < ninfo->hooks; i++) {
			struct nodeinfo * const peer = &hlist->link[i].nodeinfo;
			char idbuf[NG_PATHSIZ];

			if (!*peer->name) {
				//snprintf(peer->name, sizeof(peer->name), "%s", UNNAMED);
				snprintf(peername, strlen(UNNAMED), "%s", UNNAMED);
				Log(LOG_DEBUG, "%s(%d): peername = %s", __FUNCTION__, srv_num,
						peername);
			} else {
				snprintf(peername, strlen(peer->name), "%s", peer->name);
				Log(LOG_DEBUG, "%s(%d): peername = %s", __FUNCTION__, srv_num,
						peername);
			}
			Log(LOG_NOTICE, "%s(%d): number of connected hooks = %d",
					__FUNCTION__, srv_num, ninfo->hooks);
			snprintf(idbuf, sizeof(idbuf), "[%08x]:", peer->id);
			if ((strcmp(peer->type, "ksocket") == 0)
					&& (strcmp(peername, UNNAMED) == 0)) {
				Log(LOG_NOTICE,
						"%s(%d): peer->name = %s peer->type = %s, peer->id = [%08x]:",
						__FUNCTION__, srv_num, peername, peer->type, peer->id);
				shut_node(idbuf);
			}
		}
	}

	free(resp);
	return (EXIT_SUCCESS);
}
// Shutdown hubs
void shut_fanout(void) {
	char path[BUF_LEN];
	int i;
	memset(path, 0, sizeof(path));
	for (i = 0; i < srv_count; i++) {
		shut_clients(i, csock);
		sprintf(path, "%s:", server_cfg[i].name);
		if (NgSendMsg(csock, path, NGM_GENERIC_COOKIE, NGM_SHUTDOWN, NULL, 0)
				< 0) {
			Log(LOG_ERR, "%s(): Error shutdowning %s: %s", __FUNCTION__, path,
					strerror(errno));
			exit_nice();
		}
	}
	//return(0);
}

// Shutdown Single node
int shut_node(char path[NG_PATHSIZ]) {
	char name[NG_PATHSIZ];
	unsigned int i = 0;
	memset(name, 0, sizeof(name));
	while (i < strlen(path)) {
		name[i] = path[i];
		i++;
	}

	if (name[strlen(name) - 1] != ':') {
		sprintf(name, "%s:", name);
	}
	if (NgSendMsg(csock, name, NGM_GENERIC_COOKIE, NGM_SHUTDOWN, NULL, 0) < 0) {
		Log(LOG_INFO, "%s(): Error shutdowning fanout: %s\n", __FUNCTION__,
				strerror(errno));
		return (0);
	}
	return (1);
}
// USAGE Subroutine
void usage(const char *progname) {
	printf(
			"\
IPTV http/multicast relay, version %s\n\
Powered by someone.\n\
Usage: \n\
\n\
%s [OPTIONS]\n\
\n\
Keys are: \n\
-c - path to config file \n\
-b - run in background \n\
-d - debug mode \n\n\
Example:\n\n\
%s -c %s -b\n\n",
			VERSION, progname, progname, CFG_PATH);
	exit(EXIT_FAILURE);
}
// Daemonize Function
void daemonize(void) {

	pid_t pid, sid;
	FILE *fp;
	const char *pidfile;

	pidfile = PIDFILE;
	pid = fork();

	if (pid < 0) {
		fprintf(stderr, "%s(): Fork Filed: %s\n", __FUNCTION__,
				strerror(errno));
		exit(EXIT_FAILURE);
	}
	// If parent process - close
	if (pid > 0) {
		//Log(LOG_NOTICE, "%s(): Closing parent", __FUNCTION__);
		exit(EXIT_SUCCESS);
	}
	umask(0);
	// Create SID for the child process
	sid = setsid();
	if (sid < 0) {
		fprintf(stderr, "%s(): setsid failed: %s\n", __FUNCTION__,
				strerror(errno));
		exit(EXIT_FAILURE);
	}
	if ((fp = fopen(pidfile, "w")) == NULL) {
		Log(LOG_ERR, "%s(): Can`t write pid file: %s", __FUNCTION__,
				strerror(errno));
	} else {
		fprintf(fp, "%d", getpid());
		fclose(fp);
	}
	if ((chdir("/")) < 0) {
		fprintf(stderr, "%s(): chdir failed: %s\n", __FUNCTION__,
				strerror(errno));
		exit(EXIT_FAILURE);
	}

	/*
	 Log(LOG_NOTICE, "%s(): Starting relaying-ng pid = %d sid = %d",
	 __FUNCTION__, getpid(), sid);
	 */

	/* close out the standart file descriptors */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	daemonized = 1;
	Log(LOG_NOTICE, "%s(): Daemon Started", __FUNCTION__);

}
// Helper function you can use:

void print_config(void) {
	int i;
	char src_ip[IP_LEN], dst_ip[IP_LEN];

	for (i = 0; i < srv_count; i++) {
		memset(src_ip, 0, sizeof(src_ip));
		memset(dst_ip, 0, sizeof(dst_ip));

		strcpy(src_ip, inet_ntoa(server_cfg[i].src.sin_addr));
		strcpy(dst_ip, inet_ntoa(server_cfg[i].dst.sin_addr));

		Log(LOG_DEBUG,
				"%s: server_cfg[%d] src.ip = %s src.port = %d dst.ip = %s dst_port = %d",
				__FUNCTION__, i, src_ip, ntohs(server_cfg[i].src.sin_port),
				dst_ip, ntohs(server_cfg[i].dst.sin_port));
	}
}
