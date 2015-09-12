/*  Configuration file parser Functions
 *  Another ugly code following here
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netgraph.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>
#include 	<sys/ioctl.h>
#include	<net/if.h>
#include "ng-r.h"
#include "config.h"

#define		FAIL -1
//
// Global variables

extern int srv_count, daemonized;
// External Function
extern void Log(int log, char *fmt, ...);

char mifglob[IP_LEN];
char basename[MAXLINE];

int cut_comments(char[]);
int parse_global_line(char[]);
int parse_servers_line(char[]);
int config(const char *filename);
int isnumeric(char *str);
int get_if_addr(const char *ifname, struct sockaddr_in *ip);
int parse_src(const char *phrase);
int parse_dst(const char *phrase);

// Main config parse function
int config(const char* filename) {
	int count, global_flag, servers_flag;
	char bufr[MAXLINE];
	FILE *fp;

	daemonized = 0;
	memset(server_cfg, 0, sizeof(server_cfg));
	srv_count = 0;
	count = global_flag = servers_flag = 0;
	fp = fopen(filename, "r");
	if (fp == NULL) {
		fprintf(stderr, "Failed to open config file \"%s\" : %s \n", filename,
				strerror(errno));
		exit(EXIT_FAILURE);
	}
	while (fgets(bufr, MAXLINE, fp) != NULL) {
		// count keeps current line number
		count++;
		if (cut_comments(bufr)) {
			continue;
		}
		if (strcasestr(bufr, "[global]")) {
			global_flag = 1;
			servers_flag = 0;
		} else if (strcasestr(bufr, "[servers]")) {
			servers_flag = 1;
			global_flag = 0;
		} else {
			if (global_flag == 1) {
				parse_global_line(bufr);
			} else if (servers_flag == 1) {
				if (!parse_servers_line(bufr)) {
					fprintf(stderr, "%s: configuration error quiting\n",
							__func__);
					exit(EXIT_FAILURE);
				}
			} else {
				fprintf(stderr,
						"No section defined. You should use one of [global] or [servers]\n");
				exit(EXIT_FAILURE);
			}
		}
	}
	return (1);
}

int cut_comments(char bufr[]) {
// Omit comments
// Uses global variables bufr and tmp
	char *pos;
	char tmp[MAXLINE];

	if (bufr[0] == '#') {
		memset(bufr, 0, MAXLINE * sizeof(char));
		return (1);
	}
	if (bufr[0] == '\n') {
		memset(bufr, 0, MAXLINE * sizeof(char));
		return (1);
	}
// Find # in string and cut out all following by it
	if ((pos = strchr(bufr, '#'))) {
		strcpy(tmp, bufr);
		memset(bufr, 0, MAXLINE * sizeof(char));
		memcpy(bufr, tmp, pos - bufr);
	}
	return (0);
}

// Global section options parsing
int parse_global_line(char bufr[MAXLINE]) {
	// Parse [global] line
	int i, j, nw, state, optflag;
	char tmp[MAXLINE];

	j = nw = optflag = 0;
	state = OUT;
	for (i = 0; i < MAXLINE; i++) {
		if (bufr[i] == ' ' || bufr[i] == '\t' || bufr[i] == '\n') {
			if (state == IN) {
				state = OUT;
				nw++;
				switch (nw) {
				case 1:
					if (strcasestr(tmp, "mifsrc")) {
						optflag = MCAST_SRC_ADDR;
					} else if (strcasestr(tmp, "basename")) {
						optflag = BASENAME;
					}
					break;
				case 2:
					if (!strcasestr(tmp, "=")) {
						fprintf(stderr, "%s: Error: Format is option = value\n",
								__func__);
						return (FAIL);
					}
					break;
				case 3:
					switch (optflag) {
					case MCAST_SRC_ADDR:
						memset(mifglob, 0, sizeof(mifglob));
						memcpy(mifglob, tmp, strlen(tmp));
						break;
					case BASENAME:
						memcpy(basename, tmp, strlen(tmp));
						break;
					default:
						fprintf(stderr,
								"%s: Error:  parameter = %s for unknown option\n",
								__func__, tmp);
						break;
					}
					break;
				default:
					break;
				}
				memset(tmp, 0, sizeof(tmp));
				j = 0;
			}
		} else {
			if (state == OUT) {
				state = IN;
			}
			tmp[j] = bufr[i];
			j++;
		}
	}
	return 0;
}

/* Servers section options parsing
 *  Will fill struct servers
 */
int parse_servers_line(char bufr[MAXLINE]) {
	// As this function takes string from outside world
	// We should have global variable srv_count to fill struct servers correctly
	char tmp[MAXLINE];
	char *phrase;
	int nw;

	nw = 0;

	memset(tmp, 0, sizeof(tmp));
	for (phrase = strtok(bufr, " \t"); phrase; phrase = strtok(NULL, " \t")) {
		nw++;
		if (phrase == NULL) {
			fprintf(stderr, "%s: error no words in line\n", __func__);
			return (0);
		}
		if (phrase[0] == '\n') {
			continue;
		}

		sprintf(server_cfg[srv_count].name, "%s%d", basename, srv_count);
		switch (nw) {
		case 1:
			if (!parse_src(phrase))
				return (0);
			break;
		case 2:
			if (!parse_dst(phrase))
				return (0);
			break;
		default:
			fprintf(stderr,
					"%s: error only two words per line permited\n error: %s",
					__func__, bufr);
			return (0);
			break;
		}

	}
	srv_count++;
	return 1;
}
// IS numeric function
int isnumeric(char *str) {
	while (*str) {
		if (!isdigit(*str)) {
			return 0;
		}
		str++;
	}
	return 1;
}

// parse src section
int parse_src(const char *phrase) {
	char *p;
	char string[82];
	char buf[82];

	memset(string, 0, sizeof(string));
	memset(buf, 0, sizeof(buf));
	strcpy(string, phrase);
	strcpy(buf, phrase);

	p = strsep((char **) &phrase, "@");
	if (phrase != NULL) {
		if (!inet_aton(p, &server_cfg[srv_count].mifip)) {
			if (!get_if_addr(p,
					(struct sockaddr_in *) &server_cfg[srv_count].mifip)) {
				fprintf(stderr,
						"%s: error : %s is not either a valid ip address or interface name\n",
						__func__, p);
				return (0);
			}
		}
	} else {
		// we should write correct value to server_cfg[srv_count].mifip here
		if (!inet_aton(mifglob, &server_cfg[srv_count].mifip)) {
			fprintf(stderr, "%s: error bad ip address : %s", __func__,
					mifglob);
			return (0);
		}
		phrase = string;
	}
	// parse ip
	p = strsep((char **) &phrase, ":");
	//fprintf(stderr, "%s: parsed src_ip = %s line = %s\n", __func__, p, buf);
	if (phrase == NULL) {
		fprintf(stderr, "%s: Port not specified for src", __func__);
		return (0);
	}

	server_cfg[srv_count].src.sin_family = AF_INET;

	if (!inet_aton(p, &server_cfg[srv_count].src.sin_addr)) {
		fprintf(stderr, "%s: fatal error: %s is not a valid ip address\n",
				__func__, p);
		return (0);
	}

	/*
	 fprintf(stderr, "%s: server_cfg[%d].src.sin_addr = %s\n", __func__,
	 srv_count, inet_ntoa(server_cfg[srv_count].src.sin_addr));

	 */
	server_cfg[srv_count].src.sin_port = htons(atoi(phrase));
	server_cfg[srv_count].src.sin_len = sizeof(struct sockaddr_in);
	return (1);

}

// parse dst section of server`s line
int parse_dst(const char *phrase) {
	char *p;

	p = strsep((char **) &phrase, ":");
	server_cfg[srv_count].dst.sin_family = AF_INET;
	if (!inet_aton(p, &server_cfg[srv_count].dst.sin_addr)) {
		fprintf(stderr, "%s: fatal error: %s is not a valid ip address\n",
				__func__, p);
		return (0);
	}
	server_cfg[srv_count].dst.sin_port = htons(atoi(phrase));
	server_cfg[srv_count].dst.sin_len = sizeof(struct sockaddr_in);
	return (1);
}

// Function return struct in_addr form char *s[] = "vlan9"
int get_if_addr(const char *ifname, struct sockaddr_in *ip) {
	int fd;
	struct ifreq ifr;

	memset(ip, 0, sizeof(struct sockaddr_in));
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd == -1) {
		fprintf(stderr, "%s: an error has occured while opening fd: %s",
				__func__, strerror(errno));
		return (0);
	}
	/* I want to get an IPv4 IP address */
	ifr.ifr_addr.sa_family = AF_INET;

	/* I want IP address attached to "eth0" */
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

	if (ioctl(fd, SIOCGIFADDR, &ifr) == -1) {
		Log(LOG_ERR,
				"%s: An error has occured while trying get ip address of interface %s : %s",
				__func__, ifname, strerror(errno));
		return 0;
	}

	close(fd);

	/* display result */
	memcpy(ip, &(((struct sockaddr_in *) &ifr.ifr_addr)->sin_addr),
			sizeof(struct sockaddr_in));
	return 1;
}
