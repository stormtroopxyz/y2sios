/*
 * Apple's HFS+ (apple-oss-distributions/hfs, tag hfs-627.100.6, core/) built into this kernel
 * rather than loaded as a kext (PLANS/09-standalone.md, P6): iOS's data volume, /private/var, on
 * the phone's flash. This header is the kext's prefix header (core/kext-config.h), for this port:
 * no content protection (it needs AppleKeyStore, which is not here), no quotas,
 * no secluded rename.
 */
#ifndef _hfs_config_
#define _hfs_config_

#define HFS_COMPRESSION         1       /* (its code assumes it; xnu has decmpfs) */
#define FIFO                    1
#define QUOTA                   0
#define CONFIG_PROTECT          0
#define CONFIG_SECLUDED_RENAME  0

#endif /* _hfs_config_ */
