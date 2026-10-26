/*
 * bluetool_stub: com.apple.BlueTool for the S20+ (PLANS/06-wireless.md, B3b).
 *
 * iOS's bluetoothd does not talk to the Bluetooth chip until BlueTool has booted it: it sends
 * {kMsgId: "RunBuiltinScript", kMsgArgs: {script: boot|init|deepsleep, btaddress}} to the Mach service
 * com.apple.BlueTool and goes on when the reply says {result: 0}. Apple's BlueTool knows Apple's chips and carries
 * their firmware; on the BCM4375 it would fail, or worse, load an iPhone's patchram. Here the kernel has already
 * powered, patched and configured the chip (bsd/dev/redwood_exynos_bt.c), so this answers every script with
 * success and the chip that is there, and every RunCommand with nothing. Mobile, like Apple's.
 */
#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t object);

extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary, _xpc_type_error;
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags);
extern void xpc_connection_set_event_handler(xpc_connection_t connection, xpc_handler_t handler);
extern void xpc_connection_resume(xpc_connection_t connection);
extern void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
extern xpc_type_t xpc_get_type(xpc_object_t object);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t dictionary, const char *key);
extern const char *xpc_dictionary_get_string(xpc_object_t dictionary, const char *key);
extern xpc_connection_t xpc_dictionary_get_remote_connection(xpc_object_t dictionary);
extern void xpc_dictionary_set_int64(xpc_object_t dictionary, const char *key, int64_t value);
extern void xpc_dictionary_set_string(xpc_object_t dictionary, const char *key, const char *string);
extern xpc_object_t xpc_retain(xpc_object_t object);
extern void xpc_release(xpc_object_t object);

#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)
#define BROADCOM 15

static void
answer(xpc_object_t message)
{
	const char *id = xpc_dictionary_get_string(message, "kMsgId");
	xpc_object_t args = xpc_dictionary_get_value(message, "kMsgArgs");
	const char *script = args != NULL && xpc_get_type(args) == &_xpc_type_dictionary ?
	    xpc_dictionary_get_string(args, "script") : NULL;
	const char *command = args != NULL && xpc_get_type(args) == &_xpc_type_dictionary ?
	    xpc_dictionary_get_string(args, "command") : NULL;
	xpc_object_t reply = xpc_dictionary_create_reply(message);

	printf("bluetool_stub: %s%s%s%s%s: answered with success\n", id ? id : "(no kMsgId)", script ? " script " : "",
	    script ? script : "", command ? " command " : "", command ? command : "");
	fflush(stdout);
	if (reply == NULL) {
		return;
	}
	xpc_dictionary_set_int64(reply, "result", 0);
	if (id != NULL && strcmp(id, "RunCommand") == 0) {
		xpc_dictionary_set_string(reply, "output", "");
	} else {
		xpc_dictionary_set_int64(reply, "manufacturer", BROADCOM);
		xpc_dictionary_set_string(reply, "firmware", "BCM4375B1 bcm4375B1-semco.hcd (the kernel's)");
	}
	xpc_connection_send_message(xpc_dictionary_get_remote_connection(message), reply);
	xpc_release(reply);
}

int
main(void)
{
	xpc_connection_t listener = xpc_connection_create_mach_service("com.apple.BlueTool", dispatch_get_main_queue(),
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);

	if (listener == NULL) {
		printf("bluetool_stub: com.apple.BlueTool could not be listened on\n");
		return 1;
	}
	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != &_xpc_type_connection) {
			return;
		}
		xpc_retain(peer);
		xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
			if (xpc_get_type(message) == &_xpc_type_dictionary) {
				answer(message);
			} else if (xpc_get_type(message) == &_xpc_type_error) {
				xpc_release(peer);
			}
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
	printf("bluetool_stub: com.apple.BlueTool is here (the kernel boots the chip)\n");
	fflush(stdout);
	dispatch_main();
}
