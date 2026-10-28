#!/bin/bash -eu
# build script for OSS-Fuzz (https://github.com/google/oss-fuzz). PREPARED ONLY.
#
# The development library (ROCKETNET_DEV=1: the fuzz targets mint
# tokens with the published development keys) is compiled from the sdk
# sources and the vendored sodium, then each fuzz target links against it.
# Every compile line carries -ffp-contract=off (tools/check_oss_fuzz_fp_contract.cmake
# holds this file to it). XDP: the generated header carries a zero-length
# object, as cmake/EmbedXdp.cmake writes when no BPF clang exists, so the xdp
# server transport refuses at create; io_uring is not built.

GEN=$WORK/generated
OBJ=$WORK/obj
mkdir -p "$GEN" "$OBJ"
printf 'static const unsigned char rocketnet_server_xdp_o[1] = { 0x00 };\nstatic const unsigned int rocketnet_server_xdp_o_len = 0;\n' > "$GEN/rocketnet_server_xdp.h"

DEFS="-DROCKETNET_DEV=1 -DROCKETNET_HISTORY_PACKETS=4096"
INCS="-Isdk -Isodium -Itests -I$GEN"

$CC $CFLAGS -ffp-contract=off -w -Isodium -c sodium/sodium.c -o "$OBJ/sodium.o"

# the library's sources are CMakeLists.txt's ROCKETNET_SOURCE: every sdk/*.cpp
# but rocketnet_tests.cpp, the unit suite's own
for src in sdk/*.cpp; do
    name=$(basename "$src" .cpp)
    [ "$name" = rocketnet_tests ] && continue
    $CXX $CXXFLAGS -ffp-contract=off -std=c++17 $DEFS $INCS -c "$src" -o "$OBJ/$name.o"
done

ar rcs "$OBJ/librocketnet_dev.a" "$OBJ"/*.o

# each target, its seed corpus and its dictionary (fuzz/dict/<target>.dict,
# else the shared fuzz/rocketnet.dict). A target whose source is absent on
# this checkout is skipped.
seeds_for()
{
    case "$1" in
        rocketnet_fuzz)                    echo fuzz/corpus ;;
        rocketnet_fuzz_message)            echo fuzz/corpus_message ;;
        rocketnet_fuzz_connect_token)      echo fuzz/corpus_connect_token ;;
        rocketnet_fuzz_receive_structured) echo fuzz/corpus_receive_structured ;;
        *)                                 echo "fuzz/corpus_${1#rocketnet_fuzz_}" ;;
    esac
}

for target in rocketnet_fuzz rocketnet_fuzz_message rocketnet_fuzz_connect_token rocketnet_fuzz_receive_structured \
              rocketnet_fuzz_server_transport rocketnet_fuzz_client_transport rocketnet_fuzz_session \
              rocketnet_fuzz_token_private rocketnet_fuzz_round_trip rocketnet_fuzz_config; do
    [ -f "fuzz/$target.cpp" ] || { echo "skipping $target: no fuzz/$target.cpp"; continue; }
    $CXX $CXXFLAGS -ffp-contract=off -std=c++17 $DEFS $INCS -c "fuzz/$target.cpp" -o "$OBJ/$target.o"
    $CXX $CXXFLAGS "$OBJ/$target.o" "$OBJ/librocketnet_dev.a" $LIB_FUZZING_ENGINE -lbpf -lxdp -lpthread -o "$OUT/$target"
    seeds=$(seeds_for "$target")
    if [ -d "$seeds" ]; then
        (cd "$seeds" && zip -q -r "$OUT/${target}_seed_corpus.zip" .)
    fi
    if [ -f "fuzz/dict/$target.dict" ]; then
        cp "fuzz/dict/$target.dict" "$OUT/$target.dict"
    else
        cp fuzz/rocketnet.dict "$OUT/$target.dict"
    fi
done
