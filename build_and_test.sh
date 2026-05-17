#!/usr/bin/env bash
# backward compat wrapper → tests/run_all.sh
exec "$(dirname "$0")/tests/run_all.sh" "$@"
