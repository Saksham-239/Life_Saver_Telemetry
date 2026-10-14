#!/bin/bash
set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

LLVM_PROFDATA=$(which llvm-profdata-18 || which llvm-profdata)
LLVM_COV=$(which llvm-cov-18 || which llvm-cov)

echo "=== Building fuzz_pre_auth with libFuzzer + ASan + UBSan ==="
clang -fsanitize=fuzzer,address,undefined -O2 -g \
    -I shared/include -I shared/src \
    tests/fuzz/fuzz_pre_auth.c shared/src/packet_codec.c \
    -o tests/fuzz/fuzz_pre_auth

echo "=== Building fuzz_post_auth with libFuzzer + ASan + UBSan ==="
clang -fsanitize=fuzzer,address,undefined -O2 -g \
    -I shared/include -I shared/src -I base_station/main/state -I tests/host/stubs \
    tests/fuzz/fuzz_post_auth.c shared/src/packet_codec.c base_station/main/state/base_sm.c tests/host/stubs/logger.c \
    -o tests/fuzz/fuzz_post_auth

echo "=== Building fuzz_post_auth with Source-Based Code Coverage ==="
clang -fsanitize=fuzzer,address,undefined -fprofile-instr-generate -fcoverage-mapping -O2 -g \
    -I shared/include -I shared/src -I base_station/main/state -I tests/host/stubs \
    tests/fuzz/fuzz_post_auth.c shared/src/packet_codec.c base_station/main/state/base_sm.c tests/host/stubs/logger.c \
    -o tests/fuzz/fuzz_post_auth_cov

mkdir -p tests/fuzz/corpus_pre
mkdir -p tests/fuzz/corpus_post

echo "=== Running fuzz_pre_auth Campaign (-jobs=4 -workers=4) ==="
./tests/fuzz/fuzz_pre_auth tests/fuzz/corpus_pre -dict=tests/fuzz/protocol.dict -max_total_time=10 -jobs=4 -workers=4

echo "=== Running fuzz_post_auth Campaign (-jobs=4 -workers=4) ==="
./tests/fuzz/fuzz_post_auth tests/fuzz/corpus_post -dict=tests/fuzz/protocol.dict -max_total_time=10 -jobs=4 -workers=4

echo "=== Generating llvm-cov Per-Function Report for base_sm.c ==="
rm -f tests/fuzz/*.profraw tests/fuzz/post_auth.profdata
LLVM_PROFILE_FILE="tests/fuzz/post_auth_%p.profraw" ./tests/fuzz/fuzz_post_auth_cov tests/fuzz/corpus_post -runs=10000 > /dev/null 2>&1 || true

$LLVM_PROFDATA merge -sparse tests/fuzz/post_auth_*.profraw -o tests/fuzz/post_auth.profdata
$LLVM_COV report tests/fuzz/fuzz_post_auth_cov -instr-profile=tests/fuzz/post_auth.profdata base_station/main/state/base_sm.c -show-functions

rm -f tests/fuzz/*.profraw tests/fuzz/fuzz_pre_auth tests/fuzz/fuzz_post_auth tests/fuzz/fuzz_post_auth_cov
echo "=== Fuzzing Campaign & Coverage Report Complete ==="
