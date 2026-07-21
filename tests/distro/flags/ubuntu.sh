#!/usr/bin/env bash
# Emit Ubuntu/Debian dpkg-buildflags with full hardening.
# Requires: dpkg-dev
set -euo pipefail

command -v dpkg-buildflags >/dev/null || {
    echo "ubuntu flags: dpkg-buildflags not found (install dpkg-dev)" >&2
    exit 1
}

export DEB_BUILD_MAINT_OPTIONS=hardening=+all
cflags="$(dpkg-buildflags --get CFLAGS 2>/dev/null)"
ldflags="$(dpkg-buildflags --get LDFLAGS 2>/dev/null)"
# Fold CPPFLAGS into CFLAGS (-Wdate-time, -D_FORTIFY_SOURCE, etc.).
cppflags="$(dpkg-buildflags --get CPPFLAGS 2>/dev/null)"

printf 'export CFLAGS=%q\n' "${cflags} ${cppflags}"
printf 'export LDFLAGS=%q\n' "$ldflags"
