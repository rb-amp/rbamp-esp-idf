#!/usr/bin/env bash
# Host syntax/API check of every bench program in libs/esp_idf/bench against the
# public header (no ESP-IDF, no hardware): catches bench/API drift such as a
# changed prototype arity before a HIL window. Compiles only; nothing is linked.
set -e
cd "$(dirname "$0")"
CC="${CC:-gcc}"
rc=0
for f in ../../../../bench/*.c; do
    if $CC -std=gnu11 -fsyntax-only -Wall -Wextra -Werror -Wno-unused-parameter \
          -I stubs -I ../../include "$f"; then
        echo "bench OK   $(basename "$f")"
    else
        echo "bench FAIL $(basename "$f")"; rc=1
    fi
done
exit $rc
