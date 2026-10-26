/*
 * Apple's HFS+ in this kernel (bsd/hfs: core/ of apple-oss-distributions/hfs, hfs-627.100.6):
 * what its kext start routine (core/hfs_iokit.cpp, com_apple_filesystems_hfs::start) does, done
 * at boot instead -- the file system registered with the VFS as "hfs" -- and the IOKit helpers it
 * calls, as this port needs them (PLANS/09-standalone.md, P6: /private/var on the flash).
 */
#include "kext-config.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <kern/startup.h>
#include <libkern/libkern.h>
#include "hfs_iokit.h"
#include "hfs.h"
#include "hfs_cnode.h"

extern int ml_get_max_cpu_number(void);

extern struct vfsops hfs_vfsops;
extern struct vnodeopv_desc hfs_vnodeop_opv_desc;
extern struct vnodeopv_desc hfs_specop_opv_desc;
#if FIFO
extern struct vnodeopv_desc hfs_fifoop_opv_desc;
#endif

static vfstable_t hfs_vfs_handle;

/* (core/hfs_iokit.cpp's globals) assertions do not panic; a corrupt volume is an error, not a panic */
#include "hfs_dbg.h"
#ifndef panic_on_assert
bool panic_on_assert;
#endif
#if DEBUG
bool hfs_corruption_panics = false;
#endif

int
hfs_is_ejectable(__unused const char *cdev_name)
{
	return 0;
}

void
hfs_iterate_media_with_content(__unused const char *content_uuid_cstring,
    __unused int (*func)(const char *bsd_name, const char *uuid_str, void *arg), __unused void *arg)
{
}

kern_return_t
hfs_get_platform_serial_number(char *serial_number_str, uint32_t len)
{
	if (len > 0) {
		serial_number_str[0] = 0;
	}
	return KERN_FAILURE;
}

void redwood_hfs_init(void);

void
redwood_hfs_init(void)
{
	struct vnodeopv_desc *ops[] = {
		&hfs_vnodeop_opv_desc,
		&hfs_specop_opv_desc,
#if FIFO
		&hfs_fifoop_opv_desc,
#endif
	};
	struct vfs_fsentry vfe = {
		.vfe_vfsops = &hfs_vfsops,
		.vfe_vopcnt = sizeof(ops) / sizeof(ops[0]),
		.vfe_opvdescs = ops,
		.vfe_fsname = "hfs",
		.vfe_flags = VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBLREADDIR_EXTENDED | VFS_TBL64BITREADY |
	    VFS_TBLVNOP_PAGEOUTV2 | VFS_TBLVNOP_PAGEINV2 | VFS_TBLTHREADSAFE | VFS_TBLCANMOUNTROOT |
	    VFS_TBLNATIVEXATTR,
	};
	int ret;

	if (hfs_vfs_handle != NULL) {
		return;
	}
	ret = vfs_fsadd(&vfe, &hfs_vfs_handle);
	if (ret != 0) {
		printf("hfs: vfs_fsadd failed: %d: HFS+ NOT registered\n", ret);
		hfs_vfs_handle = NULL;
		return;
	}
	/* the rest of com_apple_filesystems_hfs::start: its zones, its sysctls, its origin caches */
	hfs_init_zones();
	hfs_sysctl_register();
	{
		uint32_t num_cpus = (uint32_t)ml_get_max_cpu_number() + 1;

		if ((2 * num_cpus) > MAX_CACHED_ORIGINS_DEFAULT) {
			_hfs_max_origins = 2 * num_cpus;
			_hfs_max_file_origins = 2 * num_cpus;
		} else if ((2 * num_cpus) > MAX_CACHED_FILE_ORIGINS_DEFAULT) {
			_hfs_max_file_origins = 2 * num_cpus;
		}
	}
	printf("hfs: Apple's HFS+ (hfs-627.100.6) registered\n");
}
