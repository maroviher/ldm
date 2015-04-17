#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glib.h>
#include <libmount/libmount.h>
#include <libudev.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include "ipc.h"

typedef enum {
	VOLUME,
	MAX,
} VolumeType;

typedef enum {
	NONE      = 0x00,
	OWNER_FIX = 0x01,
	UTF8_FLAG = 0x02,
	MASK      = 0x04,
	FLUSH     = 0x08,
	RO        = 0x10,
} Quirk;

typedef struct {
	int fmask;
	int dmask;
} Mask;

typedef struct {
	VolumeType type;
	char *node;
	struct udev_device *dev;
	char *mp; // The path to the mountpoint
	char *fs; // The name of the filesystem
} Device;

typedef struct {
	char *name;
	Quirk quirks;
} FsQuirk;

#define FSTAB_PATH  "/etc/fstab"
#define MTAB_PATH   "/proc/self/mounts"
#define LOCK_PATH   "/run/ldm.pid"

// Static global structs

static struct libmnt_table *g_fstab;
static struct libmnt_table *g_mtab;
static FILE *g_lockfd;
static int g_running;
static int g_gid, g_uid;
static char *g_mount_path;
static char *g_callback_cmd;
static Mask g_mask;
static GHashTable *g_dev_table;

#define first_nonnull(a,b,c) ((a) ? (a) : ((b) ? (b) : ((c) ? (c) : NULL)))

char *
udev_get_prop (struct udev_device *dev, char *key)
{
	const char *value = udev_device_get_property_value(dev, key);
	return value? (char *)value: NULL;
}

int
udev_prop_true (struct udev_device *dev, char *key)
{
	const char *value = udev_device_get_property_value(dev, key);
	return value && !strcmp(value, "1");
}

// Locking functions

int
lock_create (int pid)
{
	g_lockfd = fopen(LOCK_PATH, "w+");
	if (!g_lockfd)
		return 0;
	fprintf(g_lockfd, "%d", pid);
	fclose(g_lockfd);
	return 1;
}

// Spawn helper

int
spawn_callback (char *action, Device *dev)
{
	int ret;
	pid_t child_pid;
	char *env[5];

	// No callback registered, we're done
	if (!g_callback_cmd)
		return 0;

	env[0] = g_strdup_printf("LDM_ACTION=%s", action);
	env[1] = g_strdup_printf("LDM_NODE=%s", dev->node);
	env[2] = g_strdup_printf("LDM_MOUNTPOINT=%s", dev->mp);
	env[3] = g_strdup_printf("LDM_FS=%s", dev->fs);
	env[4] = NULL;

	if (!env[0] || !env[1] || !env[2] || !env[3]) {
		free(env[0]);
		free(env[1]);
		free(env[2]);
		free(env[3]);

		return 1;
	}

	child_pid = fork();

	if (child_pid < 0)
		return 1;

	if (child_pid > 0) {
		// Wait for the process to return
		wait(&ret);

		free(env[0]);
		free(env[1]);
		free(env[2]);
		free(env[3]);

		// Return the exit code or EXIT_FAILURE if something went wrong
		return WIFEXITED(ret) ? WEXITSTATUS(ret) : EXIT_FAILURE;
	}

	// Drop the root priviledges. Oh and the bass too.
	if (setgid((__gid_t)g_gid) < 0 || setuid((__uid_t)g_uid) < 0) {
		free(env[0]);
		free(env[1]);
		free(env[2]);
		free(env[3]);

		_Exit(EXIT_FAILURE);
	}

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	char * const cmdline[] = {
		"/bin/sh",
		"-c", g_callback_cmd,
		NULL
	};
	execve(cmdline[0], cmdline, env);

	// Should never reach this
	syslog(LOG_ERR, "Could not execute \"%s\"", g_callback_cmd);
	// Die
	_Exit(EXIT_FAILURE);
}

// Convenience function for fstab handling

enum {
	NODE,
	UUID,
	LABEL,
};

struct libmnt_fs *
table_search_by_str (struct libmnt_table *tbl, int type, char *str)
{
	struct libmnt_fs *fs;

	if (!tbl || !str)
		return NULL;

	switch (type) {
		case NODE:
			fs = mnt_table_find_source(tbl, str, MNT_ITER_FORWARD);
			break;
		case UUID:
			fs = mnt_table_find_tag(tbl, "UUID", str, MNT_ITER_FORWARD);
			break;
		case LABEL:
			fs = mnt_table_find_tag(tbl, "LABEL", str, MNT_ITER_FORWARD);
			break;
		default:
			return NULL;
	}

	return fs;
}

struct libmnt_fs *
table_search_by_dev (struct libmnt_table *tbl, Device *dev)
{
	struct libmnt_fs *fs;

	// Try to match the dev node
	fs = table_search_by_str(tbl, NODE, dev->node);
	if (fs)
		return fs;

	// Try to match the uuid
	fs = table_search_by_str(tbl, UUID, udev_get_prop(dev->dev, "ID_FS_UUID"));
	if (fs)
		return fs;

	// Try to match the label
	fs = table_search_by_str(tbl, LABEL, udev_get_prop(dev->dev, "ID_FS_LABEL"));
	if (fs)
		return fs;

	return NULL;
}

struct libmnt_fs *
table_search_by_udev (struct libmnt_table *tbl, struct udev_device *udev)
{
	struct libmnt_fs *fs;
	char *resolved;

	resolved = mnt_resolve_path(udev_device_get_devnode(udev), NULL);
	// Try to match the resolved dev node
	fs = table_search_by_str(tbl, NODE, resolved);
	free(resolved);

	if (fs)
		return fs;

	// Try to match the uuid
	fs = table_search_by_str(tbl, UUID, udev_get_prop(udev, "ID_FS_UUID"));
	if (fs)
		return fs;

	// Try to match the label
	fs = table_search_by_str(tbl, LABEL, udev_get_prop(udev, "ID_FS_LABEL"));
	if (fs)
		return fs;

	return NULL;
}

int
fstab_has_option (struct udev_device *udev, const char *option)
{
	struct libmnt_fs *fs;

	if (!udev || !option)
		return 0;

	fs = table_search_by_udev(g_fstab, udev);
	if (!fs)
		return 0;

	return mnt_fs_match_options(fs, option);
}

unsigned int
fs_get_quirks (char *fs)
{
	static const FsQuirk fs_table [] = {
		{ "msdos" , OWNER_FIX | UTF8_FLAG },
		{ "umsdos", OWNER_FIX | UTF8_FLAG },
		{ "vfat",   OWNER_FIX | UTF8_FLAG | MASK | FLUSH },
		{ "exfat",  OWNER_FIX },
		{ "ntfs",   OWNER_FIX | UTF8_FLAG | MASK },
		{ "iso9660",OWNER_FIX | UTF8_FLAG | RO },
		{ "udf",    OWNER_FIX },
	};

	for (int i = 0; i < sizeof(fs_table)/sizeof(FsQuirk); i++) {
		if (!strcmp(fs_table[i].name, fs))
			return fs_table[i].quirks;
	}
	return NONE;
}

int
device_find_predicate (char *key, Device *value, char *what)
{
	(void)key;

	// Try to match the resolved node or the mountpoint name
	if (!strcmp(value->node, what))
		return 1;
	if (value->type == VOLUME && !strcmp(value->mp, what))
		return 1;

	return 0;
}
// Path is either the /dev/ node or the mountpoint
Device *
device_search (const char *path)
{
	Device *dev;

	if (!path || !path[0])
		return NULL;

	// This is the fast path, let's just hope it's a /dev/ node
	dev = g_hash_table_lookup(g_dev_table, path);

	if (!dev)
		dev = g_hash_table_find(g_dev_table, (GHRFunc)device_find_predicate, (gpointer)path);

	return dev;
}

void
device_free (Device *dev)
{
	if (!dev)
		return;

	free(dev->node);
	udev_device_unref(dev->dev);

	switch (dev->type) {
		case VOLUME:
			free(dev->mp);
			free(dev->fs);
			break;

		default:
			break;
	}

	free(dev);
}

Device *
device_new (struct udev_device *udev)
{
	const char *dev_node, *dev_fs, *dev_fs_usage;
	Device *dev;

	if (!udev)
		return NULL;

	dev_node = udev_device_get_devnode(udev);
	dev_fs = udev_get_prop(udev, "ID_FS_TYPE");
	dev_fs_usage = udev_get_prop(udev, "ID_FS_USAGE");

#if 0
	fprintf(stderr, "%s (FS_USAGE : %s FS_TYPE : %s)\n", dev_node, dev_fs, dev_fs_usage);
#endif

	if (!dev_fs && !dev_fs_usage)
		return NULL;

	// Avoid empty cd/dvd drives
	if (udev_get_prop(udev, "ID_CDROM") && !udev_prop_true(udev, "ID_CDROM_MEDIA"))
		return NULL;

	if (!strcmp(dev_fs_usage, "filesystem")) {
		dev = calloc(1, sizeof(Device));

		dev->type = VOLUME;
		dev->dev = udev;
		dev->node = mnt_resolve_path(dev_node, NULL);
		dev->fs = strdup(dev_fs);

		udev_device_ref(udev);
	}
	else {
		dev = NULL;
#if 0
		fprintf(stderr, "Skipping %s (ID_FS_USAGE : %s)\n", dev_node, dev_fs_usage);
#endif
	}

	return dev;
}

char *
device_get_mp (Device *dev, const char *base)
{
	char *unique;
	char mp[4096];
	GDir *dir;

	if (!dev || !base)
		return NULL;

	// Use the first non-null field
	unique = first_nonnull(udev_get_prop(dev->dev, "ID_FS_LABEL"),
                         udev_get_prop(dev->dev, "ID_FS_UUID"),
                         udev_get_prop(dev->dev, "ID_SERIAL"));

	if (!unique)
		return NULL;

	if (snprintf(mp, sizeof(mp), "%s/%s", base, unique) < 0)
		return NULL;

	// If the mountpoint we've come up with already exists try to find a good one by appending '_'
	while (g_file_test(mp, G_FILE_TEST_EXISTS)) {
		// We tried hard and failed
		if (strlen(mp) == sizeof(mp) - 2)
				return NULL;

		// Reuse the directory only if it's empty
		dir = g_dir_open(mp, 0, NULL);
		if (dir) {
			// The directory is empty!
			// Note for the reader : 'g_dir_read_name' omits '.' and '..'
			if (!g_dir_read_name(dir))
				break;

			g_dir_close(dir);
		}

		// Directory not empty, append a '_'
		strcat(mp, "_");
	}

	return strdup(mp);;
}

int
device_mount (Device *dev)
{
	char *mp;
	unsigned int fs_quirks;
	char opt_fmt[256] = {0};
	struct libmnt_context *ctx;
	struct libmnt_fs *fstab;

	if (!dev)
		return 0;

	fstab = table_search_by_dev(g_fstab, dev);

	if (fstab && mnt_fs_get_target(fstab))
		mp = strdup(mnt_fs_get_target(fstab));
	else
		mp = device_get_mp(dev, g_mount_path);

	if (!mp)
		return 0;

	if (mkdir(mp, 775) < 0) {
		syslog(LOG_ERR, "Could not mkdir() the folder at %s (%s)", mp, strerror(errno));
		return 0;
	}

	// Set 'mp' as the mountpoint for the device
	dev->mp = mp;

	fs_quirks = fs_get_quirks(dev->fs);

	// Apply the needed quirks
	if (fs_quirks != NONE) {
		char *p = opt_fmt;
		// Microsoft filesystems and filesystems used on optical
		// discs require the gid and uid to be passed as mount
		// arguments to allow the user to read and write, while
		// posix filesystems just need a chown after being mounted
		if (fs_quirks & OWNER_FIX)
			p += sprintf(p, "uid=%i,gid=%i,", g_uid, g_gid);
		if (fs_quirks & UTF8_FLAG)
			p += sprintf(p, "utf8,");
		if (fs_quirks & FLUSH)
			p += sprintf(p, "flush,");
		if (fs_quirks & MASK)
			p += sprintf(p, "dmask=%04o,fmask=%04o,", g_mask.dmask, g_mask.fmask);

		*p = '\0';
	}

	ctx = mnt_new_context();

#if 0
	fprintf(stderr, "fs   : %s\n", dev->fs);
	fprintf(stderr, "node : %s\n", dev->node);
	fprintf(stderr, "mp   : %s\n", dev->mp);
	fprintf(stderr, "opt  : %s\n", opt_fmt);
#endif

	mnt_context_set_fstype(ctx, dev->fs);
	mnt_context_set_source(ctx, dev->node);
	mnt_context_set_target(ctx, dev->mp);
	mnt_context_set_options(ctx, opt_fmt);

	if (fs_quirks & RO)
		mnt_context_set_mflags(ctx, MS_RDONLY);

	if (mnt_context_mount(ctx)) {
		syslog(LOG_ERR, "Error while mounting %s (%s)", dev->node, strerror(errno));
		mnt_free_context(ctx);
		rmdir(dev->mp);
		return 0;
	}

	mnt_free_context(ctx);

	if (!(fs_quirks & OWNER_FIX)) {
		if (chown(dev->mp, (__uid_t)g_uid, (__gid_t)g_gid) < 0) {
			syslog(LOG_ERR, "Cannot chown %s", dev->mp);
			return 0;
		}
	}

	(void)spawn_callback ("mount", dev);

	return 1;
}

int
device_unmount (Device *dev)
{
	struct libmnt_context *ctx;

	if (!dev)
		return 0;

	// Unmount the device if it is actually mounted
	if (table_search_by_dev(g_mtab, dev)) {
		ctx = mnt_new_context();
		mnt_context_set_target(ctx, dev->node);
		if (mnt_context_umount(ctx)) {
			syslog(LOG_ERR, "Error while unmounting %s (%s)", dev->node, strerror(errno));
			mnt_free_context(ctx);
			return 0;
		}
		mnt_free_context(ctx);
	}

	rmdir(dev->mp);

	(void)spawn_callback ("unmount", dev);

	return 1;
}

// udev action callbacks

void
on_udev_add (struct udev_device *udev)
{
	Device *dev;
	const char *dev_node;

	dev_node = udev_device_get_devnode(udev);

	// libmount < 2.21 doesn't support '+noauto', using 'noauto' instead
	const char *noauto_opt = mnt_parse_version_string(LIBMOUNT_VERSION) < 2210 ? "noauto" : "+noauto";
	if (fstab_has_option(udev, noauto_opt))
		return;

	dev = device_new(udev);
	if (!dev) {
		fprintf(stderr, "device_new()\n");
		return;
	}

	if (!device_mount(dev)) {
		fprintf(stderr, "device_mount()\n");
		return;
	}

	g_hash_table_insert(g_dev_table, (char *)dev_node, dev);
}

void
on_udev_remove (struct udev_device *udev)
{
	Device *dev;
	const char *dev_node;

	dev_node = udev_device_get_devnode(udev);

	dev = g_hash_table_lookup(g_dev_table, dev_node);
	if (!dev)
		return;

	if (!device_unmount(dev)) {
		fprintf(stderr, "device_unmount()");
		return;
	}

	g_hash_table_remove(g_dev_table, dev_node);
}

void
on_udev_change (struct udev_device *udev)
{
	Device *dev;
	const char *dev_node;

	dev_node = udev_device_get_devnode(udev);

	dev = g_hash_table_lookup(g_dev_table, dev_node);
	// Unmount the old media
	if (dev && table_search_by_dev(g_mtab, dev)) {
		if (device_unmount(dev))
			g_hash_table_remove(g_dev_table, dev_node);
	}

	if (!strcmp(udev_get_prop(udev, "ID_TYPE"), "cd") &&
			NULL !=  udev_get_prop(udev, "ID_CDROM_MEDIA")) {
			device_mount(dev);
	}
}

void
on_mtab_change (void)
{
	struct libmnt_table *new_tab;
	struct libmnt_tabdiff *diff;
	struct libmnt_fs *old, *new;
	struct libmnt_iter *it;
	Device *dev;
	int change_type;

	new_tab = mnt_new_table_from_file(MTAB_PATH);
	if (!new_tab) {
		fprintf(stderr, "Could not parse %s\n", MTAB_PATH);
		return;
	}

	diff = mnt_new_tabdiff();
	if (!diff) {
		fprintf(stderr, "Could not diff the mtab\n");
		mnt_free_table(new_tab);
		return;
	}

	if (mnt_diff_tables(diff, g_mtab, new_tab) < 0) {
		fprintf(stderr, "Could not diff the mtab\n");
		mnt_free_table(new_tab);
		mnt_free_tabdiff(diff);
		return;
	}

	it = mnt_new_iter(MNT_ITER_BACKWARD);

	while (!mnt_tabdiff_next_change(diff, it, &new, &old, &change_type)) {
		switch (change_type) {
			case MNT_TABDIFF_UMOUNT:
				dev = device_search(mnt_fs_get_source(new));

				if (dev) {
					const char *ht_key = udev_device_get_devnode(dev->dev);

					g_hash_table_remove(g_dev_table, ht_key);
				}

				break;

			case MNT_TABDIFF_REMOUNT:
			case MNT_TABDIFF_MOVE:
				dev = device_search(mnt_fs_get_source(old));

				// Disown the device if it has been remounted
				if (dev) {
					const char *ht_key = udev_device_get_devnode(dev->dev);

					g_hash_table_remove(g_dev_table, ht_key);
				}

				break;
		}
	}

	mnt_free_iter(it);
	mnt_free_tabdiff(diff);

	// We're done diffing, replace the old table
	mnt_free_table(g_mtab);
	g_mtab = new_tab;
}

void
mount_plugged_devices (struct udev *udev)
{
	struct udev_enumerate *udev_enum;
	struct udev_list_entry *devices;
	struct udev_list_entry *entry;
	struct udev_device *dev;
	const char *path;

	udev_enum = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(udev_enum, "block");
	udev_enumerate_scan_devices(udev_enum);
	devices = udev_enumerate_get_list_entry(udev_enum);

	udev_list_entry_foreach(entry, devices) {
		path = udev_list_entry_get_name(entry);
		dev = udev_device_new_from_syspath(udev, path);

		if (!table_search_by_udev(g_mtab, dev))
			on_udev_add(dev);

		udev_device_unref(dev);
	}
	udev_enumerate_unref(udev_enum);
}

void
sig_handler (int signal)
{
	if (signal == SIGINT || signal == SIGTERM || signal == SIGHUP)
		g_running = 0;
}

int
daemonize (void)
{
	pid_t child_pid;

	child_pid = fork();

	if (child_pid < 0)
		return 0;

	if (child_pid > 0)
		exit(EXIT_SUCCESS);

	if (chdir("/") < 0) {
		perror("chdir");
		return 0;
	}

	umask(022);

	if (setsid() < 0) {
		perror("setsid");
		return 0;
	}

	// Close std* descriptors
	close(0);
	close(1);
	close(2);

	return 1;
}

int
ipc_serve (int client)
{
	char msg_buffer[4096];
	ssize_t r;

	if (client < 0)
		return 0;

	r = read(client, msg_buffer, sizeof(msg_buffer));
	if (r < 0) {
		perror("read");
		return 0;
	}
	msg_buffer[r] = '\0';

	if (r < 1) {
		syslog(LOG_WARNING, "Malformed ipc command of length %zu", r);
		return 0;
	}

	switch (msg_buffer[0]) {
		case 'R': { // Remove a mounted device
			// Resolve the user-provided path
			char *res = realpath(msg_buffer + 1, NULL);

			if (!res) {
				perror("realpath");
				ipc_sendf(client, "-");
				return 0;
			}

			Device *dev = device_search(res);
			free(res);

			int ok = 0;
			// We don't have to check whether the device is mounted here since device_unmount takes care
			// of stale device entries
			if (dev) {
				const char *ht_key = udev_device_get_devnode(dev->dev);

				if (device_unmount(dev)) {
					g_hash_table_remove(g_dev_table, ht_key);
					ok = 1;
				}
			}

			// Send the response back
			ipc_sendf(client, "%c", ok? '+': '-');

			return 1;
		}

		case 'L': { // List the mounted devices
			GHashTableIter it;
			char *node;
			Device *dev;

			g_hash_table_iter_init(&it, g_dev_table);

			while (g_hash_table_iter_next(&it, (gpointer)&node, (gpointer)&dev)) {
				// Print the volume information like this
				// v <node> <fs> <mountpoint>
				if (dev->type == VOLUME) {
					ipc_sendf(client, "v \"%s\" \"%s\" \"%s\"\n", dev->node, dev->fs, dev->mp);
				}
			}

			// Send an empty line to mark the end of the list
			ipc_sendf(client, "\n");

			return 1;
		}
	}

	syslog(LOG_WARNING, "Unrecognized ipc command %c", msg_buffer[0]);

	return 0;
}

void device_clear_list () {
	GHashTableIter it;
	Device *dev;

	g_hash_table_iter_init(&it, g_dev_table);
	while (g_hash_table_iter_next(&it, NULL, (gpointer)&dev)) {
		device_unmount(dev);
	}
	g_hash_table_destroy(g_dev_table);
}

int
parse_mask (char *args, int *mask)
{
	unsigned long tmp;

	tmp = 0;

	if (args[0] != '0') {
		// format : rwxrwxrwx
		if (strlen(args) != 9)
			return 0;

		for (int i = 0; i < 9; i++) {
			if (!strchr("rwx-", args[i])) {
				fprintf(stderr, "Stray '%c' character in the mask", args[i]);
				return 0;
			}
			tmp <<= 1;
			tmp |= (args[i] != '-')? 1: 0;
		}
	}
	else {
		// format : 0000
		if (strlen(args) != 4)
			return 0;

		errno = 0;
		tmp = strtoul(args, NULL, 8);
		perror("strtoul");
		if (errno)
			return 0;
	}

	*mask = tmp;

	return 1;
}

int
main (int argc, char *argv[])
{
	struct udev *udev;
	struct udev_monitor *monitor;
	struct udev_device *device;
	const char *action;
	struct pollfd	pollfd[4];  // udev / inotify watch / mtab / fifo
	char *resolved;
	int	opt;
	int	daemon;
	int	ino_fd, ipc_fd, fstab_fd, mtab_fd;
	struct inotify_event event;

	ino_fd = ipc_fd = fstab_fd = mtab_fd = -1;
	daemon = 0;
	g_uid	= -1;
	g_gid	= -1;
	g_callback_cmd = NULL;

	g_mask.fmask = 0133;
	g_mask.dmask = 0022;

	while ((opt = getopt(argc, argv, "hdg:u:p:c:m:")) != -1) {
		switch (opt) {
			case 'd':
				daemon = 1;
				break;
			case 'g':
				g_gid = (int)strtoul(optarg, NULL, 10);
				break;
			case 'u':
				g_uid = (int)strtoul(optarg, NULL, 10);
				break;
			case 'm':
				{
					char *sep = strchr(optarg, ',');

					if (!sep) {
						if (!parse_mask(optarg, &g_mask.fmask)) {
							fprintf(stderr, "Invalid mask specified!\n");
							return EXIT_FAILURE;
						}
						// The user specified a single mask, use that as umask
						g_mask.dmask = g_mask.fmask;
					}
					else {
						*sep++ = '\0';
						// The user specified two distinct masks
						if (!parse_mask(optarg, &g_mask.fmask) || !parse_mask(sep, &g_mask.dmask)) {
							fprintf(stderr, "Invalid mask specified!\n");
							return EXIT_FAILURE;
						}
					}
				}
				break;
			case 'p':
				g_mount_path = strdup(optarg);
				break;
			case 'c':
				g_callback_cmd = strdup(optarg);
				break;
			case 'h':
				printf("ldm "VERSION_STR"\n");
				printf("2011-2015 (C) The Lemon Man\n");
				printf("%s [-d | -r | -g | -u | -p | -c | -m | -h]\n", argv[0]);
				printf("\t-d Run ldm as a daemon\n");
				printf("\t-g Specify the gid\n");
				printf("\t-u Specify the uid\n");
				printf("\t-m Specify the umask or the fmask/dmask\n");
				printf("\t-p Specify where to mount the devices\n");
				printf("\t-c Specify the path to the script executed after mount/unmount events\n");
				printf("\t-h Show this help\n");
				// Falltrough
			default:
				return EXIT_SUCCESS;
		}
	}

	if (getuid() != 0) {
		fprintf(stderr, "You have to run this program as root!\n");
		return EXIT_FAILURE;
	}

	if (g_file_test(LOCK_PATH, G_FILE_TEST_EXISTS)) {
		fprintf(stderr, "ldm is already running!\n");
		return EXIT_SUCCESS;
	}

	if (g_uid < 0 || g_gid < 0) {
		fprintf(stderr, "You must supply your gid/uid!\n");
		return EXIT_FAILURE;
	}

	if (g_callback_cmd && !g_file_test(g_callback_cmd, G_FILE_TEST_IS_EXECUTABLE)) {
		fprintf(stderr, "The callback script isn't executable!\n");

		free(g_callback_cmd);
		g_callback_cmd = NULL;
	}

	if (!g_mount_path)
		g_mount_path = strdup("/mnt");

	// Resolve the mount point path before using it
	resolved = realpath(g_mount_path, NULL);
	if (!resolved) {
		perror("realpath()");
		return EXIT_FAILURE;
	}
	free(g_mount_path);
	g_mount_path = resolved;

	// Check anyways
	if (!g_file_test(g_mount_path, G_FILE_TEST_IS_DIR)) {
		fprintf(stderr, "The path %s doesn't name a folder or doesn't exist!\n", g_mount_path);

		free(g_callback_cmd);
		free(g_mount_path);

		return EXIT_FAILURE;
	}

	ino_fd = inotify_init();

	if (ino_fd < 0) {
		perror("inotify_init");

		free(g_callback_cmd);
		free(g_mount_path);

		return EXIT_FAILURE;
	}

	// Create the ipc socket
	umask(0);

	if (daemon && !daemonize()) {
		fprintf(stderr, "Could not spawn the daemon!\n");
		return EXIT_FAILURE;
	}

	lock_create(getpid());

	openlog("ldm", LOG_CONS, LOG_DAEMON);

	signal(SIGTERM, sig_handler);
	signal(SIGINT , sig_handler);
	signal(SIGHUP , sig_handler);

	syslog(LOG_INFO, "ldm "VERSION_STR);

	// Create the udev struct/monitor
	udev = udev_new();
	monitor = udev_monitor_new_from_netlink(udev, "udev");

	if (!monitor) {
		syslog(LOG_ERR, "Cannot create a new monitor");
		goto cleanup;
	}
	if (udev_monitor_enable_receiving(monitor)) {
		syslog(LOG_ERR, "Cannot enable receiving");
		goto cleanup;
	}
	if (udev_monitor_filter_add_match_subsystem_devtype(monitor, "block", NULL)) {
		syslog(LOG_ERR, "Cannot set the filter");
		goto cleanup;
	}

	// Create the hashtable holding the mounted devices
	g_dev_table = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)device_free);

	// Load the tables
	g_fstab = mnt_new_table_from_file(FSTAB_PATH);
	g_mtab = mnt_new_table_from_file(MTAB_PATH);

	if (!g_fstab || !g_mtab) {
		fprintf(stderr, "Could not parse the fstab/mtab\n");
		goto cleanup;
	}

	mount_plugged_devices(udev);

	mnt_free_table(g_mtab);
	g_mtab = mnt_new_table_from_file(MTAB_PATH);

	// Setup the fd to poll
	fstab_fd = inotify_add_watch(ino_fd, FSTAB_PATH, IN_CLOSE_WRITE);
	if (fstab_fd < 0) {
		perror("inotify_add_watch");
		goto cleanup;
	}

	mtab_fd = open(MTAB_PATH, O_RDONLY | O_NONBLOCK);
	if (mtab_fd < 0) {
		perror("open");
		goto cleanup;
	}

	ipc_fd = ipc_init(1);
	if (ipc_fd < 0)
		goto cleanup;

	if (listen(ipc_fd, 1) < 0) {
		perror("listen");
		goto cleanup;
	}

	// Register all the events
	pollfd[0].fd = udev_monitor_get_fd(monitor);
	pollfd[0].events = POLLIN;

	pollfd[1].fd = ino_fd;
	pollfd[1].events = POLLIN;

	pollfd[2].fd = mtab_fd;
	pollfd[2].events = 0;

	pollfd[3].fd = ipc_fd;
	pollfd[3].events = POLLIN;

	syslog(LOG_INFO, "Entering the main loop");

	g_running = 1;

	while (g_running) {
		if (poll(pollfd, 4, -1) < 1)
			continue;

		// Incoming message on udev socket
		if (pollfd[0].revents & POLLIN) {
			device = udev_monitor_receive_device(monitor);

			if (!device)
				continue;

			action = udev_device_get_action(device);

			if (!strcmp(action, "add")) {
				on_udev_add(device);
			}
			else if (!strcmp(action, "remove")) {
				on_udev_remove(device);
			}
			else if (!strcmp(action, "change")) {
				on_udev_change(device);
			}

			udev_device_unref(device);
		}
		// Incoming message on inotify socket
		if (pollfd[1].revents & POLLIN) {
			read(ino_fd, &event, sizeof(struct inotify_event));

			mnt_free_table(g_fstab);
			g_fstab = mnt_new_table_from_file(MTAB_PATH);
		}
		// mtab change
		if (pollfd[2].revents & POLLERR) {
			read(mtab_fd, &event, sizeof(struct inotify_event));

			on_mtab_change();
		}
		// client connection to the ipc socket
		if (pollfd[3].revents & POLLIN) {
			int client;

			client = accept(ipc_fd, NULL, NULL);
			if (client < 0) {
				perror("accept");
				continue;
			}

			if (!ipc_serve(client))
				syslog(LOG_ERR, "Could not serve a client due to an error");

			close(client);
		}
	}

cleanup:

	device_clear_list ();

	free(g_callback_cmd);
	free(g_mount_path);

	// Do the cleanup
	inotify_rm_watch(ino_fd, fstab_fd);

	ipc_deinit (ipc_fd);

	close(ino_fd);
	close(mtab_fd);

	unlink(LOCK_PATH);

	udev_monitor_unref(monitor);
	udev_unref(udev);

	mnt_free_table(g_fstab);
	mnt_free_table(g_mtab);

	syslog(LOG_INFO, "Terminating...");

	return EXIT_SUCCESS;
}
