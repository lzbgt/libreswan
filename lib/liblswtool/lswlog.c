/*
 * error logging functions
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2001  D. Hugh Redelmeier.
 * Copyright (C) 2007-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2012-2013 Paul Wouters <paul@libreswan.org>
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
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>	/* used only if MSG_NOSIGNAL not defined */
#include <sys/queue.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <libreswan.h>

#include "constants.h"
#include "lswlog.h"

bool log_to_stderr = TRUE;	/* should log go to stderr? */

char *progname = "";
static char *prog_suffix = "";

void tool_init_log(char *name)
{
	progname = name;
	prog_suffix = " "; /* : */

	if (log_to_stderr)
		setbuf(stderr, NULL);
}

void libreswan_vloglog(enum rc_type rc UNUSED, const char *fmt, va_list ap)
{
	char m[LOG_WIDTH];	/* longer messages will be truncated */
	vsnprintf(m, sizeof(m), fmt, ap);

	if (log_to_stderr)
		fprintf(stderr, "%s%s%s\n",
			progname, prog_suffix, m);
}

void lswlog_log_errno(int e, const char *prefix, const char *message, ...)
{
	va_list args;
	char m[LOG_WIDTH];	/* longer messages will be truncated */

	va_start(args, message);
	vsnprintf(m, sizeof(m), message, args);
	va_end(args);

	if (log_to_stderr)
		fprintf(stderr, "%s%s%s%s. Errno %d: %s\n",
			prefix, progname, prog_suffix,
			m, e, strerror(e));
}

void lswlog_dbg_raw(struct lswlog *buf)
{
	fprintf(stderr, "%s\n", buf->array);
}
