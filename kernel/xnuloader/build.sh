#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"

# On macOS the system clang cannot target a PE/COFF AArch64 image, and LLVM 23 no longer
# bundles the linker: Homebrew LLVM provides clang, the separate `lld` formula provides
# lld-link. On Linux clang/lld-link are used as they are.
#   brew install llvm lld
CC=${CC:-}
LD=${LD:-}
if [ "$(uname -s)" = Darwin ]; then
	LLVM=$(brew --prefix llvm 2>/dev/null || true)
	if [ -z "$CC" ] && [ -x "$LLVM/bin/clang" ]; then
		CC="$LLVM/bin/clang"
	fi
	if [ -z "$LD" ]; then
		if [ -x "$(brew --prefix lld 2>/dev/null)/bin/lld-link" ]; then
			LD="$(brew --prefix lld)/bin/lld-link"
		else
			LD=lld-link
		fi
	fi
fi
CC=${CC:-clang}
LD=${LD:-lld-link}

"$CC" --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter ${GIC_V2:+-DGIC_V2=1} -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
"$LD" /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /out:loader.efi loader.obj
ls -la loader.efi
