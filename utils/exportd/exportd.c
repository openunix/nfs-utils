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
#include "exportfs.h"
#include "export.h"

extern void my_svc_run(void);

struct state_paths etab;
struct state_paths rmtab;

int manage_gids;
int use_ipaddr = -1;

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
	xlog (L_NOTICE, "Caught signal %d, exiting.", sig);
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

	if (!setup_state_path_names(progname, ETAB, ETABTMP, ETABLCK, &etab))
		return 1;
	if (!setup_state_path_names(progname, RMTAB, RMTABTMP, RMTABLCK, &rmtab))
		return 1;

	if (!foreground) 
		xlog_stderr(0);

	daemon_init(foreground);

	set_signals();
	daemon_ready();

	/* Open files now to avoid sharing descriptors among forked processes */
	cache_open();

	/* Process incoming upcalls */
	cache_process_loop();

	xlog(L_ERROR, "%s: process loop terminated unexpectedly. Exiting...\n",
		progname);

	free_state_path_names(&etab);
	free_state_path_names(&rmtab);
	exit(1);
}
