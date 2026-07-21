#!/usr/bin/env bash
# Emit Arch Linux makepkg.conf CFLAGS/LDFLAGS.
# /etc/makepkg.conf is plain shell assigning CFLAGS=... LDFLAGS=...
set -euo pipefail

conf=/etc/makepkg.conf
if [[ ! -f "$conf" ]]; then
    echo "arch flags: $conf missing; building with toolchain defaults" >&2
    exit 0
fi

# Source in a subshell; makepkg.conf may reference $CARCH / $CHOST set above.
eval "$(
    # shellcheck disable=SC1090
    set +u
    # shellcheck source=/dev/null
    source "$conf"
    printf 'cflags=%q\n' "${CFLAGS-}"
    printf 'ldflags=%q\n' "${LDFLAGS-}"
)"

[[ -n "${cflags:-}" ]] && printf 'export CFLAGS=%q\n' "$cflags"
[[ -n "${ldflags:-}" ]] && printf 'export LDFLAGS=%q\n' "$ldflags"
