#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "xstat.h"

#ifdef HAVE_FSTATAT

int xlstat(const char *pathname, struct stat *statbuf)
{
	return fstatat(AT_FDCWD, pathname, statbuf, AT_NO_AUTOMOUNT |
			AT_SYMLINK_NOFOLLOW);
}

int xstat(const char *pathname, struct stat *statbuf)
{
	return fstatat(AT_FDCWD, pathname, statbuf, AT_NO_AUTOMOUNT);
}

#else

int xlstat(const char *pathname, struct stat *statbuf)
{
	return lstat(pathname, statbuf);
}

int xstat(const char *pathname, struct stat *statbuf)
{
	return stat(pathname, statbuf);
}
#endif
