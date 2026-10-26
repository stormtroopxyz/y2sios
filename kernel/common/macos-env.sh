# macOS host environment for the HackiOS build (build natively on Apple silicon, no Linux).
#
# The project's shell is written for GNU userland (readlink -f, stat -c, nproc, cp -rs,
# gtar --wildcards, sed -i). On macOS that comes from Homebrew coreutils/gnu-tar/gnu-sed,
# whose "gnubin" directories place the GNU tools first on PATH under their usual names.
# This file only sets PATH; it is sourced by xnu-env.sh and common/lib.sh, and can also be
# sourced from your shell profile so `xl`, `qemu/*.sh`, `userland/build.sh` etc. all see it.
#
#   brew install coreutils gnu-tar gnu-sed findutils bison
#
# Deliberately NOT put on PATH: xnu-deps/host/bin -- those shims (a fake xcrun, a no-op
# codesign/plutil, sw_vers) are what the *Linux* build needs to look like macOS, and they
# would shadow the real tools here.

[ -n "$HACKIOS_MACOS_ENV" ] && return 0
HACKIOS_MACOS_ENV=1

# Our own path, whether sourced from bash (BASH_SOURCE) or zsh (${(%):-%x}, evaluated
# lazily so bash never has to parse that syntax).
_hackios_self="${BASH_SOURCE[0]:-}"
if [ -z "$_hackios_self" ] && [ -n "${ZSH_VERSION:-}" ]; then
	eval '_hackios_self=${(%):-%x}'
fi
[ -z "$_hackios_self" ] && _hackios_self="$0"
_hackios_root=$(cd "$(dirname "$_hackios_self")/.." 2>/dev/null && pwd)

_brew=$(command -v brew 2>/dev/null || echo /opt/homebrew/bin/brew)
if [ -x "$_brew" ]; then
	_brewp=$("$_brew" --prefix)
	for _d in coreutils findutils gnu-tar gnu-sed; do
		[ -d "$_brewp/opt/$_d/libexec/gnubin" ] && PATH="$_brewp/opt/$_d/libexec/gnubin:$PATH"
	done
	# XNU's parsers want a modern bison; the system one is 2.3
	if [ -x "$_brewp/opt/bison/bin/bison" ]; then
		export HOST_BISON="$_brewp/opt/bison/bin/bison"
		PATH="$_brewp/opt/bison/bin:$PATH"
	fi
	unset _d _brewp
fi
unset _brew

# The shims macOS still needs (xcrun for the SDK overlay, iig for DriverKit); xnu-deps/
# setup.sh fills the directory with xnu-clang and friends.
if [ -d "$_hackios_root/xnu-deps/host-macos/bin" ]; then
	PATH="$_hackios_root/xnu-deps/host-macos/bin:$PATH"
fi
export PATH

# python.org builds verify TLS against a CA bundle they do not ship; macOS has one.
if [ -z "${SSL_CERT_FILE:-}" ] && [ -f /etc/ssl/cert.pem ]; then
	export SSL_CERT_FILE=/etc/ssl/cert.pem
fi
unset _hackios_self _hackios_root
