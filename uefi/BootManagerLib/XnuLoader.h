#ifndef _XNU_LOADER_H_
#define _XNU_LOADER_H_

//
// The payload is embedded inside the UEFI FD image itself, in the unused tail
// of the 2 MB "UEFI FD" region (files end at ~0x78f70, the FD is 0x200000).
// The stock boot shim copies the whole FD there, so the payload arrives with it.
//
#define XNU_PAYLOAD_BASE      0xE8100000ULL   /* FD base + 0x100000 */
#define XNU_PAYLOAD_MAX_SIZE  0x00100000ULL   /* 1 MB: 0x100000..0x200000 of the FD */
#define XNU_PAYLOAD_MAGIC     0x50554E58 // "XNUP"

typedef struct {
  UINT32 Magic;
  UINT32 Size;
} XNU_PAYLOAD_HEADER;

// The header is followed by entries; an empty name ends the list.
typedef struct {
  CHAR8  Name[16];
  UINT32 Size;
  UINT32 Reserved;
  // UINT8 Data[Size], padded to 16 bytes
} XNU_PAYLOAD_ENTRY;

// Raw GPT partition that receives the text log (read back from Android).
#define XNU_LOG_PARTITION  L"logdump"

#define XNU_MARKER_MAGIC  0x584E5521 // "XNU!"

VOID
XnuLoaderMain ();

#endif /* _XNU_LOADER_H_ */
