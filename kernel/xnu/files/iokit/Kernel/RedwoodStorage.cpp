/*
 * The phone's flash as iOS sizes it: a service named by "BSD Name" disk0 with the whole device's
 * size in "Size" (kIOMediaSizeKey).
 *
 * MobileGestalt's NANDSize() (iOS 16.5, libMobileGestalt.dylib 0x19c52ce28) is
 * IOServiceGetMatchingService(IOBSDNameMatching(0, "disk0")), then IORegistryEntryCreateCFProperty
 * (kIOMediaSizeKey) read as an SInt64; TotalDiskCapacity and with it Settings' capacity come from it.
 * There is no IOMedia here (the storage is bsd/dev/redwood_ufs.c's devices), so it failed:
 * "TotalDiskCapacity = -1" and the 2 GB of the system volume shown (27 October, mg_test DiskUsage).
 *
 * Only the two properties are published. The service is not an IOMedia, so nothing that looks for
 * disks (DiskArbitration, the file systems) takes it for one.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

extern "C" void RedwoodStorageReport(unsigned long long bytes);

class RedwoodStorage : public IOService
{
	OSDeclareDefaultStructors(RedwoodStorage);
};

OSDefineMetaClassAndStructors(RedwoodStorage, IOService);

/* Once, from bsd/dev/redwood_ufs.c, with the sum of the storage's logical units. */
void
RedwoodStorageReport(unsigned long long bytes)
{
	static RedwoodStorage * storage;
	IOService * platform = IOService::getPlatform();
	RedwoodStorage * made;
	OSNumber * size;

	if (storage != NULL || bytes == 0 || platform == NULL || (made = OSTypeAlloc(RedwoodStorage)) == NULL) {
		return;
	}
	if (!made->init() || (size = OSNumber::withNumber(bytes, 64)) == NULL) {
		made->release();
		return;
	}
	made->setProperty("BSD Name", "disk0");
	made->setProperty("Size", size);            /* kIOMediaSizeKey */
	size->release();
	if (!made->attach(platform)) {
		made->release();
		return;
	}
	made->setName("RedwoodStorage");
	if (!made->start(platform)) {
		made->detach(platform);
		made->release();
		return;
	}
	made->registerService();
	storage = made;
	IOLog("RedwoodStorage: disk0, %llu bytes (%llu GB)\n", bytes, bytes / 1000000000ULL);
}
