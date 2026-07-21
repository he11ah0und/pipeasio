#!/usr/bin/env bash
# Emit Fedora RPM optflags for the outer harness to eval.
# Requires: redhat-rpm-config, rpm, gcc (so %{optflags} expands fully).
set -euo pipefail

command -v rpm >/dev/null || {
    echo "fedora flags: rpm not found (install redhat-rpm-config)" >&2
    exit 1
}

cflags="$(rpm -E '%{optflags}')"
ldflags="$(rpm -E '%{build_ldflags}')"

printf 'export CFLAGS=%q\n' "$cflags"
printf 'export LDFLAGS=%q\n' "$ldflags"
