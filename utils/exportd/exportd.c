/*
 * Copyright (C) 2021 Red Hat <nfs@redhat.com>
 *
 * support/exportd/exportd.c
 *
 * Routines used to support NFSv4 exports
 *
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stddef.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <getopt.h>

#include "nfslib.h"
#include "conffile.h"


static struct option longopts[] =
{
	{ "foreground", 0, 0, 'F' },
	{ "debug", 1, 0, 'd' },
	{ "help", 0, 0, 'h' },
	{ NULL, 0, 0, 0 }
};

/*
 * Signal handlers.
 */
inline static void set_signals(void);

static void 
killer (int sig)
{
	xlog (L_NOTICE, "Caught signal %d, un-registering and exiting.", sig);
	exit(0);
}
static void
sig_hup (int UNUSED(sig))
{
	/* don't exit on SIGHUP */
	xlog (L_NOTICE, "Received SIGHUP... Ignoring.\n");
	return;
}
inline static void 
set_signals(void) 
{
	struct sigaction sa;

	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPIPE, &sa, NULL);
	/* WARNING: the following works on Linux and SysV, but not BSD! */
	sigaction(SIGCHLD, &sa, NULL);

	sa.sa_handler = killer;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	sa.sa_handler = sig_hup;
	sigaction(SIGHUP, &sa, NULL);
}
static void
usage(const char *prog, int n)
{
	fprintf(stderr,
		"Usage: %s [-f|--foreground] [-h|--help] [-d kind|--debug kind]\n", prog);
	exit(n);
}

int
main(int argc, char **argv)
{
	char *progname;
	int	foreground = 0;
	int	 c;

	/* Set the basename */
	if ((progname = strrchr(argv[0], '/')) != NULL)
		progname++;
	else
		progname = argv[0];

	/* Initialize logging. */
	xlog_open(progname);

	conf_init_file(NFS_CONFFILE);
	xlog_set_debug(progname);

	while ((c = getopt_long(argc, argv, "d:fh", longopts, NULL)) != EOF) {
		switch (c) {
		case 'd':
			xlog_sconfig(optarg, 1);
			break;
		case 'f':
			foreground++;
			break;
		case 'h':
			usage(progname, 0);
			break;
		case '?':
		default:
			usage(progname, 1);
		}

	}

	if (!foreground) 
		xlog_stderr(0);

	daemon_init(foreground);

	set_signals();
	
	daemon_ready();
}
