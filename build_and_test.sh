#!/usr/bin/env bash
# backward compat wrapper → tests/build_and_test.sh
exec "$(dirname "$0")/tests/build_and_test.sh" "$@"
