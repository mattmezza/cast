#!/bin/sh
# An explicit opt-in build; ordinary make must never invoke this implicitly.
set -eu
exec python3 "$(dirname "$0")/deps-lgpl.py" "$@"
