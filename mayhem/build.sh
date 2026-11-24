#!/usr/bin/env bash
# mayhem/build.sh — Qucs (schematic GUI tool, CMake + Qt5).
#  1) sanitized build of upstream's schematic library (libqucsschematic.so + QucsFunctions) in
#     mayhem/build-fuzz, with $SANITIZER_FLAGS, SanCov (-fsanitize=fuzzer-no-link) and DWARF 3;
#  2) the fuzz harnesses (one per CLI operation) + their -standalone reproducers;
#  3) a normal (unsanitized) build of qucs.real in mayhem/build-test for mayhem/test.sh.
# Qt comes from Debian's -dev packages (uninstrumented, like any apt library). Offline-safe:
# nothing is fetched.
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
export CC CXX
cd "$SRC"

FUZZ_BUILD="$SRC/mayhem/build-fuzz"
TEST_BUILD="$SRC/mayhem/build-test"
OUT="$SRC/mayhem/out"
mkdir -p "$OUT"

# UBSan pointer-overflow is relaxed: the diagram code computes `ptr - 2` on a null point array for
# every diagram without data (e.g. tabdiagram.cpp `py = g->cPointsY - 2`), which the batch tool always
# hits because no dataset exists, so every print input would stop there. ASan (which catches any real
# dereference of such a pointer) and the rest of UBSan stay halting.
UB_RELAX=""
case "$SANITIZER_FLAGS" in *undefined*) UB_RELAX="-fno-sanitize=pointer-overflow" ;; esac
FUZZ_CFLAGS="$SANITIZER_FLAGS $UB_RELAX -fsanitize=fuzzer-no-link $DEBUG_FLAGS -O1"

# 1) sanitized project library
cmake -S "$SRC" -B "$FUZZ_BUILD" -DCMAKE_BUILD_TYPE=None \
  -DCMAKE_C_FLAGS="$FUZZ_CFLAGS" -DCMAKE_CXX_FLAGS="$FUZZ_CFLAGS" \
  -DCMAKE_SHARED_LINKER_FLAGS="$SANITIZER_FLAGS $UB_RELAX" \
  -DCMAKE_EXE_LINKER_FLAGS="$SANITIZER_FLAGS $UB_RELAX"
cmake --build "$FUZZ_BUILD" -j"$MAYHEM_JOBS" --target qucsschematic QucsFunctions

# 2) harnesses
QT_PKGS="Qt5Widgets Qt5Gui Qt5Core Qt5Svg Qt5PrintSupport Qt5Script Qt5Xml"
QT_CFLAGS="$(pkg-config --cflags $QT_PKGS) -fPIC"
QT_LIBS="$(pkg-config --libs $QT_PKGS)"
INC="-DHAVE_CONFIG_H -I$FUZZ_BUILD -I$FUZZ_BUILD/qucs -I$SRC/qucs -I$SRC/qucs/components -I$SRC/qucs/diagrams -I$SRC/qucs/dialogs -I$SRC/qucs/paintings"
LIBS="$FUZZ_BUILD/qucs/libQucsFunctions.a $FUZZ_BUILD/qucs/libqucsschematic.so -Wl,-rpath,$FUZZ_BUILD/qucs $QT_LIBS -ldl"
HFLAGS="$SANITIZER_FLAGS $UB_RELAX $DEBUG_FLAGS -O1 -std=c++17"

$CXX $HFLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$OUT/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OUT/standalone_main.o"
for mode in netlist:FUZZ_NETLIST print:FUZZ_PRINT dump:FUZZ_DUMP lib:FUZZ_LIB dat:FUZZ_DAT sub:FUZZ_SUB hdl:FUZZ_HDL va:FUZZ_VA; do
  name="fuzz_qucs_${mode%%:*}"; def="${mode##*:}"
  $CXX $HFLAGS -fsanitize=fuzzer-no-link $QT_CFLAGS $INC -DFUZZ_MODE="$def" \
    -c "$SRC/mayhem/fuzz_qucs.cpp" -o "$OUT/$name.o"
  $CXX $HFLAGS $LIB_FUZZING_ENGINE "$OUT/$name.o" "$OUT/lsan_off.o" $LIBS -o "$OUT/$name"
  $CXX $HFLAGS "$OUT/$name.o" "$OUT/standalone_main.o" "$OUT/lsan_off.o" $LIBS -o "$OUT/$name-standalone"
done

# 3) normal build of the CLI for the functional suite (project's own flags)
cmake -S "$SRC" -B "$TEST_BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS"
cmake --build "$TEST_BUILD" -j"$MAYHEM_JOBS" --target qucs.real
