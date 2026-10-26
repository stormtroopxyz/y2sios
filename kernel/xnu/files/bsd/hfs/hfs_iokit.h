/*
 * What the HFS kext's IOKit part (core/hfs_iokit.cpp) gives the rest of it, in C here: this port has
 * no ejectable media, no content-protection keys and no platform serial number to give.
 * bsd/hfs/hfs_redwood.c.
 */
#ifndef hfs_iokit_h
#define hfs_iokit_h

#include <sys/cdefs.h>
#include <mach/kern_return.h>

__BEGIN_DECLS

int hfs_is_ejectable(const char *cdev_name);
void hfs_iterate_media_with_content(const char *content_uuid_cstring,
    int (*func)(const char *bsd_name, const char *uuid_str, void *arg), void *arg);
kern_return_t hfs_get_platform_serial_number(char *serial_number_str, uint32_t len);

__END_DECLS

#endif /* hfs_iokit_h */
