#!/bin/sh
# The per-commit big-endian leg of ci.yml, run on hetzner2: s390x cross build,
# bpfeb XDP object plus its negative control, ELF proof, wire check, byte
# cases and XDP filter host cases under qemu-s390x, plus every case in
# tests/test_unit_xdp_frame.cpp (the VLAN codec cases among them).
# Usage: sh be-s390x.sh <commit> <worktree> <logdir>
C=$1; T=$2; L=$3
mkdir -p $L
git -C ~/bench/xdp-vlan-gates worktree remove --force $T 2>/dev/null
git -C ~/bench/xdp-vlan-gates worktree prune
git -C ~/bench/xdp-vlan-gates worktree add -q --detach $T $C || exit 1
cd $T || exit 1
echo "tree=$(git log --oneline -1)"
export PKG_CONFIG_LIBDIR=/usr/lib/s390x-linux-gnu/pkgconfig:/usr/share/pkgconfig
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=s390x \
  -DCMAKE_C_COMPILER=s390x-linux-gnu-gcc -DCMAKE_CXX_COMPILER=s390x-linux-gnu-g++ \
  -DCMAKE_LIBRARY_ARCHITECTURE=s390x-linux-gnu -DCMAKE_FIND_ROOT_PATH=/usr/lib/s390x-linux-gnu \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
  "-DCMAKE_EXE_LINKER_FLAGS=-L/usr/lib/s390x-linux-gnu -Wl,-rpath-link,/usr/lib/s390x-linux-gnu" \
  "-DCMAKE_SHARED_LINKER_FLAGS=-L/usr/lib/s390x-linux-gnu -Wl,-rpath-link,/usr/lib/s390x-linux-gnu" \
  > $L/configure.log 2>&1; echo be-s390x-config=$?
cmake --build build --target rocketnet test test_sodium --parallel 4 > $L/build.log 2>&1
echo be-s390x-build=$? errors=$(grep -c "error:" $L/build.log) warnings=$(grep -c "warning:" $L/build.log)
unset PKG_CONFIG_LIBDIR
clang -D__linux__ -D__BPF__ -ffreestanding -ffp-contract=off -O2 -g -I/usr/include/s390x-linux-gnu -Isdk \
  -target bpfeb -c sdk/xdp/server_xdp.c -o build/server_xdp_bpfeb.o > $L/bpfeb.log 2>&1
cmake -DOBJECT=build/server_xdp_bpfeb.o -DHEADER=build/server_xdp_bpfeb.h -DBYTE_ORDER=BIG_ENDIAN \
  -P cmake/CheckXdpObjectByteOrder.cmake >> $L/bpfeb.log 2>&1; echo bpfeb-object=$?
clang -D__linux__ -D__BPF__ -ffreestanding -ffp-contract=off -O2 -I/usr/include/x86_64-linux-gnu -Isdk \
  -target bpf -c sdk/xdp/server_xdp.c -o build/server_xdp_build_machine.o >> $L/bpfeb.log 2>&1
cmake -DOBJECT=build/server_xdp_build_machine.o -DHEADER=build/server_xdp_build_machine.h \
  -DBYTE_ORDER=BIG_ENDIAN -P cmake/CheckXdpObjectByteOrder.cmake >> $L/bpfeb.log 2>&1
echo "bpfeb-negative-control rc=$? (must be nonzero)"
mkdir -p bin
s390x-linux-gnu-g++ -std=c++11 -Wall -ffp-contract=off -Isdk tools/bigendian/wire_check.cpp build/librocketnet.a \
  -L/usr/lib/s390x-linux-gnu -Wl,-rpath-link,/usr/lib/s390x-linux-gnu -lbpf -lxdp -lpthread -o bin/wire_check \
  > $L/wire-build.log 2>&1; echo wire-check-build=$?
for b in bin/test bin/wire_check bin/test_sodium build/generated/server_xdp.o; do
  echo "elf $b: $(readelf -h $b | grep -E 'Data|Machine' | tr -s ' ' | tr '\n' ' ')"
done
export QEMU_LD_PREFIX=/
export LD_LIBRARY_PATH=/usr/lib/s390x-linux-gnu:/usr/s390x-linux-gnu/lib
qemu-s390x ./bin/wire_check > $L/wire.log 2>&1; echo "wire-check=$? :: $(tail -1 $L/wire.log)"
qemu-s390x ./bin/test_sodium > $L/sodium.log 2>&1; echo "test-sodium=$? :: $(tail -1 $L/sodium.log)"
bash tools/bigendian/run_byte_cases.sh bin/test qemu-s390x > $L/byte-cases.log 2>&1; echo "byte-cases=$? :: $(tail -1 $L/byte-cases.log)"
n=0; red=0
for name in $(sed -n 's/^REGISTER_UNIT_TEST( *\(test_[a-z0-9_]*\) *);.*/\1/p' tests/test_unit_xdp_frame.cpp); do
  n=$((n + 1))
  ROCKETNET_TEST_SUITE_CAPTURE=1 qemu-s390x ./bin/test --run $name > $L/frame-$name.log 2>&1; rc=$?
  [ $rc != 0 ] && red=$((red + 1))
  echo "frame-case rc=$rc $name"
done
echo "frame-cases run=$n red=$red"
cd / && git -C ~/bench/xdp-vlan-gates worktree remove --force $T
echo BE-S390X-DONE
