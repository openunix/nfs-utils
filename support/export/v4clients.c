/*
 * support/export/v4clients.c
 *
 * Montior clients appearing in, and disappearing from, /proc/fs/nfsd/clients
 * and log relevant information.
 */

#include <unistd.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <errno.h>
#include "export.h"

/* search.h declares 'struct entry' and nfs_prot.h
 * does too.  Easiest fix is to trick search.h into
 * calling its struct "struct Entry".
 */
#define entry Entry
#include <search.h>
#undef entry

static int clients_fd = -1;

void v4clients_init(void)
{
	if (clients_fd >= 0)
		return;
	clients_fd = inotify_init1(IN_NONBLOCK);
	if (clients_fd < 0) {
		xlog_err("Unable to initialise v4clients watcher: %s\n",
			 strerror(errno));
		return;
	}
	if (inotify_add_watch(clients_fd, "/proc/fs/nfsd/clients",
			      IN_CREATE | IN_DELETE) < 0) {
		xlog_err("Unable to watch /proc/fs/nfsd/clients: %s\n",
			 strerror(errno));
		close(clients_fd);
		clients_fd = -1;
		return;
	}
}

void v4clients_set_fds(fd_set *fdset)
{
	if (clients_fd >= 0)
		FD_SET(clients_fd, fdset);
}

static void *tree_root;

struct ent {
	unsigned long num;
	char *clientid;
	char *addr;
	int vers;
};

static int ent_cmp(const void *av, const void *bv)
{
	const struct ent *a = av;
	const struct ent *b = bv;

	if (a->num < b->num)
		return -1;
	if (a->num > b->num)
		return 1;
	return 0;
}

static void free_ent(struct ent *ent)
{
	free(ent->clientid);
	free(ent->addr);
	free(ent);
}

static char *dup_line(char *line)
{
	char *ret;
	char *e = strchr(line, '\n');
	if (!e)
		e = line + strlen(line);
	ret = malloc(e - line + 1);
	if (ret) {
		memcpy(ret, line, e - line);
		ret[e-line] = 0;
	}
	return ret;
}

static void add_id(int id)
{
	char buf[2048];
	struct ent **ent;
	struct ent *key;
	char *path;
	FILE *f;

	if (asprintf(&path, "/proc/fs/nfsd/clients/%d/info", id) < 0)
		return;

	f = fopen(path, "r");
	if (!f) {
		free(path);
		return;
	}
	key = calloc(1, sizeof(*key));
	if (!key) {
		fclose(f);
		free(path);
		return;
	}
	key->num = id;
	while (fgets(buf, sizeof(buf), f)) {
		if (strncmp(buf, "clientid: ", 10) == 0)
			key->clientid = dup_line(buf+10);
		if (strncmp(buf, "address: ", 9) == 0)
			key->addr = dup_line(buf+9);
		if (strncmp(buf, "minor version: ", 15) == 0)
			key->vers = atoi(buf+15);
	}
	fclose(f);
	free(path);

	xlog(L_NOTICE, "v4.%d client attached: %s from %s",
	     key->vers, key->clientid, key->addr);

	ent = tsearch(key, &tree_root, ent_cmp);

	if (!ent || *ent != key)
		/* Already existed, or insertion failed */
		free_ent(key);
}

static void del_id(int id)
{
	struct ent key = {.num = id};
	struct ent **e, *ent;

	e = tfind(&key, &tree_root, ent_cmp);
	if (!e || !*e)
		return;
	ent = *e;
	tdelete(ent, &tree_root, ent_cmp);
	xlog(L_NOTICE, "v4.%d client detached: %s from %s",
	     ent->vers, ent->clientid, ent->addr);
	free_ent(ent);
}

int v4clients_process(fd_set *fdset)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	const struct inotify_event *ev;
	ssize_t len;
	char *ptr;

	if (clients_fd < 0 ||
	    !FD_ISSET(clients_fd, fdset))
		return 0;

	while ((len = read(clients_fd, buf, sizeof(buf))) > 0) {
		for (ptr = buf; ptr < buf + len;
		     ptr += sizeof(struct inotify_event) + ev->len) {
			int id;
			ev = (const struct inotify_event *)ptr;

			id = atoi(ev->name);
			if (id <= 0)
				continue;
			if (ev->mask & IN_CREATE)
				add_id(id);
			if (ev->mask & IN_DELETE)
				del_id(id);
		}
	}
	return 1;
}

