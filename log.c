/*
 * log.c
 *
 *  Created on: 6 дек. 2013
 *      Author: demiurg
 */

#include <stdio.h>
#include <syslog.h>
#include <stdarg.h>

extern int daemonized;

void Log(int log, const char *fmt, ...);

void Log(int log, const char *fmt, ...)
{
	va_list args;
	char buf[200];

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	//snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf));
	//Log(LG_ERR, ("%s", buf));
	if (!log)
	{
		log = LOG_NOTICE;
	}
	if (daemonized == 1)
	{
		syslog(log, "%s", buf);
	}
	else
	{
		fprintf(stderr, "%s\n", buf);
	}

}
