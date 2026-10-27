/*
 * mg_test: what MobileGestalt says about this device (the answers iOS builds its idea of the
 * hardware from).   usage: mg_test [KEY...]
 * A key may also be one of the obfuscated names programs ask by; "MGGetProductType" gives the number
 * MGGetProductType() returns (what programs tell the models apart by); "statfs" the sizes of the root and the
 * data volume as the file system gives them (what a capacity is computed from).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

extern CFTypeRef MGCopyAnswer(CFStringRef key);
extern uint64_t MGGetProductType(void);

static const char *const keys[] = {
	"DeviceClass", "DeviceClassNumber", "DeviceName", "ProductType", "HWModelStr", "ModelNumber", "ProductName",
	"marketing-name", "UserAssignedDeviceName", "ComputerName", "SerialNumber", "UniqueDeviceID", "HasBaseband",
	"telephony", "cellular-data", "wifi", "bluetooth", "DeviceSupportsTelephonyOverUSB", "main-screen-scale",
	"main-screen-width", "main-screen-height", "main-screen-class", "ArtworkDeviceSubType", "DeviceColor", "CPUArchitecture",
	"BuildVersion", "ProductVersion", "RegionInfo", "front-facing-camera", "rear-facing-camera", "still-camera",
};

int
main(int argc, char **argv)
{
	int count = argc > 1 ? argc - 1 : (int)(sizeof(keys) / sizeof(keys[0]));

	for (int i = 0; i < count; i++) {
		const char *name = argc > 1 ? argv[i + 1] : keys[i];
		if (strcmp(name, "statfs") == 0) {
			static const char *const paths[] = { "/", "/private/var", "/private/var/mobile" };

			for (unsigned p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
				struct statfs f;

				if (statfs(paths[p], &f) != 0) {
					printf("mg_test: statfs %s: failed\n", paths[p]);
					continue;
				}
				printf("mg_test: statfs %s: %s %s from %s, %llu MB, %llu MB free\n", paths[p], f.f_fstypename,
				    f.f_mntonname, f.f_mntfromname, (unsigned long long)f.f_blocks * f.f_bsize >> 20,
				    (unsigned long long)f.f_bavail * f.f_bsize >> 20);
			}
			continue;
		}
		if (strcmp(name, "MGGetProductType") == 0) {
			printf("mg_test: MGGetProductType() = %#llx\n", (unsigned long long)MGGetProductType());
			continue;
		}
		CFStringRef key = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
		CFTypeRef answer = MGCopyAnswer(key);
		CFStringRef text = answer ? CFCopyDescription(answer) : NULL;
		char line[1024] = "(none)";

		if (text) {
			CFStringGetCString(text, line, sizeof(line), kCFStringEncodingUTF8);
			CFRelease(text);
		}
		printf("mg_test: %s = %s\n", name, line);
		if (answer) {
			CFRelease(answer);
		}
		CFRelease(key);
	}
	return 0;
}
