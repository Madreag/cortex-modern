#!/bin/bash
# Builds oneTBB with ThreadSanitizer at the system libtbb's version, for TSan runs of the engine on Linux.
# libstdc++ runs std::execution::par_unseq on TBB; with the stock uninstrumented libtbb TSan cannot see the task
# scheduler's synchronization and reports every write before or after a parallel loop as a race with its workers.
# Usage: tools/linux/tsan_tbb.sh <prefix dir>; then run the TSan binary with LD_LIBRARY_PATH=<prefix>/lib.
set -eu
PREFIX=$(realpath -m "${1:?usage: tsan_tbb.sh <prefix dir>}")
VERSION=$(dpkg-query -W -f '${Version}' libtbb12 | cut -d- -f1)
WORK=$PREFIX.work
rm -rf "${WORK:?}"
git -c advice.detachedHead=false clone -q --depth 1 --branch "v$VERSION" https://github.com/oneapi-src/oneTBB.git "$WORK/src"
cmake -S "$WORK/src" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ -DTBB_SANITIZE=thread -DTBB_TEST=OFF -DTBB_EXAMPLES=OFF -DTBB_STRICT=OFF \
  -DTBBMALLOC_BUILD=OFF -DCMAKE_INSTALL_PREFIX="$PREFIX" > "$WORK/cmake.log" 2>&1
ninja -C "$WORK/build" tbb > "$WORK/build.log"
cmake --install "$WORK/build" --component runtime > /dev/null
nm -D "$PREFIX/lib/libtbb.so.12" | grep -q __tsan_func_entry
rm -rf "${WORK:?}"
echo "instrumented libtbb $VERSION: $PREFIX/lib"
