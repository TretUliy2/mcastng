//
#include	<err.h>
#include	<errno.h>
#include    <string.h>
#include    <strings.h>
#include    <netdb.h>
#include    <netinet/in.h>
#include	<sys/socket.h>
#include	<sys/types.h>
#include	<arpa/inet.h>

#define	BUF_LEN	1024

#ifndef MAX_CLIENTS
#define		MAX_CLIENTS 3000
#endif

#define		MAX_SERVERS	1024
#define		IP_LEN	16
// 255.255.255.255
#define		PORT_LEN	6
#define     SND_BUF_SIZE (256*1024)

int csock, dsock;

struct server_cfg {
	struct sockaddr_in src;
	struct sockaddr_in dst;
	struct in_addr mifip;
	unsigned int streaming;
	char name[NG_NODESIZ];
	u_int c_count;
} server_cfg[MAX_SERVERS];

typedef struct clients {
	u_int node_id;
	u_int srv_num;
	struct sockaddr_in addr;
} client;
