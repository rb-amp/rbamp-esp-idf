#!/usr/bin/env bash
# Host unit test of the pure STD decode helpers (no ESP-IDF, no hardware).
set -e
cd "$(dirname "$0")"
CC="${CC:-gcc}"
OUT="${TMPDIR:-/tmp}/rbamp_test_std_decode"
$CC -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
    -I stubs -I ../../include -I ../../src \
    test_std_decode.c ../../src/rbamp_std_decode.c -lm -o "$OUT"
"$OUT"
# Whole-component host simulation: rbamp.c + rbamp_energy.c against a fake
# register image (test_sim_read_all.c implements the IDF calls rbamp.c makes).
SIM="${TMPDIR:-/tmp}/rbamp_test_sim"
$CC -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format \
    -I stubs -I ../../include -I ../../src \
    test_sim_read_all.c sim_module.c ../../src/rbamp.c ../../src/rbamp_energy.c ../../src/rbamp_std_decode.c -lm -o "$SIM"
"$SIM"
$CC -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format \
    -I stubs -I ../../include -I ../../src \
    test_sim_sct.c sim_module.c ../../src/rbamp.c ../../src/rbamp_energy.c ../../src/rbamp_std_decode.c -lm -o "$SIM"_sct
"$SIM"_sct
./check_bench.sh
