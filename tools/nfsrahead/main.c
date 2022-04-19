#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <libmount/libmount.h>
#include <sys/sysmacros.h>

#ifndef MOUNTINFO_PATH
#define MOUNTINFO_PATH "/proc/self/mountinfo"
#endif

/* Device information from the system */
struct device_info {
	char *device_number;
	dev_t dev;
	char *mountpoint;
	char *fstype;
};

/* Convert a string in the format n:m to a device number */
static dev_t dev_from_arg(const char *device_number)
{
	char *s = strdup(device_number), *p;
	char *maj_s, *min_s;
	unsigned int maj, min;
	dev_t dev;

	maj_s = p = s;
	for ( ; *p != ':'; p++)
		;

	*p = '\0';
	min_s = p + 1;

	maj = strtol(maj_s, NULL, 10);
	min = strtol(min_s, NULL, 10);

	dev = makedev(maj, min);

	free(s);
	return dev;
}

#define sfree(ptr) if (ptr) free(ptr)

/* device_info maintenance */
static void init_device_info(struct device_info *di, const char *device_number)
{
	di->device_number = strdup(device_number);
	di->dev = dev_from_arg(device_number);
	di->mountpoint = NULL;
	di->fstype = NULL;
}


static void free_device_info(struct device_info *di)
{
	sfree(di->mountpoint);
	sfree(di->fstype);
	sfree(di->device_number);
}

static int get_mountinfo(const char *device_number, struct device_info *device_info, const char *mountinfo_path)
{
	int ret = 0;
	struct libmnt_table *mnttbl;
	struct libmnt_fs *fs;
	char *target;

	init_device_info(device_info, device_number);

	mnttbl = mnt_new_table();

	if ((ret = mnt_table_parse_file(mnttbl, mountinfo_path)) < 0)
		goto out_free_tbl;

	if ((fs = mnt_table_find_devno(mnttbl, device_info->dev, MNT_ITER_FORWARD)) == NULL) {
		ret = ENOENT;
		goto out_free_tbl;
	}

	if ((target = (char *)mnt_fs_get_target(fs)) == NULL) {
		ret = ENOENT;
		goto out_free_fs;
	}

	device_info->mountpoint = strdup(target);
	target = (char *)mnt_fs_get_fstype(fs);
	if (target)
		device_info->fstype = strdup(target);

out_free_fs:
	mnt_free_fs(fs);
out_free_tbl:
	mnt_free_table(mnttbl);
	free(device_info->device_number);
	device_info->device_number = NULL;
	return ret;
}

static int get_device_info(const char *device_number, struct device_info *device_info)
{
	int ret = ENOENT;
	for (int retry_count = 0; retry_count < 10 && ret != 0; retry_count++)
		ret = get_mountinfo(device_number, device_info, MOUNTINFO_PATH);

	return ret;
}

int main(int argc, char **argv)
{
	int ret = 0;
	struct device_info device;
	unsigned int readahead = 128;

	if (argc != 2) {
		return -EINVAL;
	}

	if ((ret = get_device_info(argv[1], &device)) != 0) {
		goto out;
	}

	if (strncmp("nfs", device.fstype, 3) != 0) {
		ret = -EINVAL;
		goto out;
	}

	printf("%d\n", readahead);

out:
	free_device_info(&device);
	return ret;
}
