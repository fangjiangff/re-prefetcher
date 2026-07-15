#!/usr/bin/env bash

set -u
set -o pipefail

# Run this script from anywhere. It uses RSbypassSP/gadget_detect/codes as root.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLVM_BIN="$ROOT_DIR/../llvm-project/build/bin"
CLANG="$LLVM_BIN/clang"
CLANGXX="$LLVM_BIN/clang++"
LOCAL_PREFIX="$ROOT_DIR/local"

TARGET="aarch64-unknown-linux-gnu"
NPROC="$(nproc)"
RES_DIR="$ROOT_DIR/res"
SUMMARY_FILE="$RES_DIR/gadget_summary.txt"
mkdir -p "$RES_DIR"

# Prefer locally installed dependencies under codes/local.
export GPGRT_CONFIG="$LOCAL_PREFIX/bin/gpgrt-config"
export PKG_CONFIG_PATH="$LOCAL_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$LOCAL_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# AArch64 gadget detector + aggressive LLVM automatic loop software prefetching.
PREFETCH_FLAGS="-mllvm -aarch64-enable-loop-data-prefetch=true"
PREFETCH_FLAGS+=" -mllvm -prefetch-distance=2048"
PREFETCH_FLAGS+=" -mllvm -min-prefetch-stride=1"
PREFETCH_FLAGS+=" -mllvm -max-prefetch-iters-ahead=100000"
PREFETCH_FLAGS+=" -mllvm -loop-prefetch-writes=true"

CFLAGS="--target=$TARGET -mcpu=kryo -O2 $PREFETCH_FLAGS -mllvm -aarch64-mir-analyze -g"
CXXFLAGS="$CFLAGS"

LIB_NAMES=(libgcrypt libsodium wolfssl openssl mbedTLS BearSSL Nettle PQClean)
LOG_FILES=(
  "$RES_DIR/libgcrypt_O2.log"
  "$RES_DIR/libsodium_O2.log"
  "$RES_DIR/wolfssl_O2.log"
  "$RES_DIR/openssl_O2.log"
  "$RES_DIR/mbedtls_O2.log"
  "$RES_DIR/bearssl_O2.log"
  "$RES_DIR/nettle_O2.log"
  "$RES_DIR/pqclean_O2.log"
)

count_marker() {
  local pattern=$1
  local log_file=$2

  if [ ! -f "$log_file" ]; then
    printf "0"
    return
  fi

  awk -v pat="$pattern" 'index($0, pat) { count++ } END { printf "%d", count }' "$log_file"
}

print_summary_table() {
  local types=(
    "Control-flow load"
    "Data-flow load"
    "Control-flow store"
    "Data-flow store"
    "Control-flow swp"
    "Data-flow swp"
  )
  local markers=(
    "Control-flow load Gadgets Found in"
    "Data-flow load Gadgets Found in"
    "Control-flow store Gadgets Found in"
    "Data-flow store Gadgets Found in"
    "Control-flow swp Gadgets Found in"
    "Data-flow swp Gadgets Found in"
  )

  : > "$SUMMARY_FILE"
  {
    printf "[+] Final gadget summary\n"
    printf "%-20s" "Gadget Type"
    local lib
    for lib in "${LIB_NAMES[@]}"; do
      printf "\t%s" "$lib"
    done
    printf "\n"

    local i log_file
    for i in "${!types[@]}"; do
      printf "%-20s" "${types[$i]}"
      for log_file in "${LOG_FILES[@]}"; do
        printf "\t%s" "$(count_marker "${markers[$i]}" "$log_file")"
      done
      printf "\n"
    done
  } | tee "$SUMMARY_FILE"

  printf "[+] Summary written to %s\n" "$SUMMARY_FILE"
}

run_build() {
  local name=$1
  local log_file=$2
  local build_func=$3

  printf "[+] Building %-8s -> %s\n" "$name" "$log_file"
  : > "$log_file"
  if "$build_func" >> "$log_file" 2>&1; then
    printf "[+] %-8s done\n" "$name"
  else
    printf "[!] %-8s failed; see %s\n" "$name" "$log_file"
  fi
}

build_libgcrypt() {
  cd "$ROOT_DIR/libgcrypt" || return 1
  ./configure --disable-doc \
    CC="$CLANG" \
    CFLAGS="$CFLAGS" \
    CXX="$CLANGXX" \
    CXXFLAGS="$CXXFLAGS" || return 1
  make clean || true
  make -j"$NPROC"
}

build_libsodium() {
  cd "$ROOT_DIR/libsodium" || return 1
  ./configure \
    CC="$CLANG" \
    CFLAGS="$CFLAGS" \
    CXX="$CLANGXX" \
    CXXFLAGS="$CXXFLAGS" || return 1
  make clean || true
  make -j"$NPROC"
}

build_wolfssl() {
  cd "$ROOT_DIR/wolfssl" || return 1
  if [ ! -x ./configure ]; then
    ./autogen.sh || return 1
  fi
  ./configure \
    CC="$CLANG" \
    CFLAGS="$CFLAGS" \
    CXX="$CLANGXX" \
    CXXFLAGS="$CXXFLAGS" || return 1
  make clean || true
  make -j"$NPROC"
}

build_openssl() {
  cd "$ROOT_DIR/OpenSSL" || return 1
  ./Configure linux-aarch64 no-shared no-tests no-asm \
    CC="$CLANG" \
    CFLAGS="$CFLAGS" || return 1
  make clean || true
  make -j"$NPROC"
}

build_mbedtls() {
  cd "$ROOT_DIR/mbedTLS" || return 1
  git submodule update --init --recursive || return 1
  rm -rf build-gadget
  mkdir -p build-gadget
  cd build-gadget || return 1
  cmake .. \
    -DENABLE_TESTING=Off \
    -DENABLE_PROGRAMS=Off \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CLANG" \
    -DCMAKE_C_FLAGS="$CFLAGS -Wno-error=unterminated-string-initialization" \
    -DCMAKE_CXX_COMPILER="$CLANGXX" \
    -DCMAKE_CXX_FLAGS="$CXXFLAGS" || return 1
  make -j"$NPROC"
}

build_bearssl() {
  cd "$ROOT_DIR/BearSSL" || return 1
  make clean || true
  make -j"$NPROC" \
    CC="$CLANG" \
    LD="$CLANG" \
    LDDLL="$CLANG" \
    CFLAGS="$CFLAGS -W -Wall -fPIC"
}

build_nettle() {
  cd "$ROOT_DIR/Nettle" || return 1
  if [ ! -x ./configure ]; then
    autoreconf -fi || return 1
  fi
  ./configure \
    --host="$TARGET" \
    --disable-assembler \
    --enable-mini-gmp \
    CC="$CLANG" \
    CFLAGS="$CFLAGS" || return 1
  make clean || true
  make -j"$NPROC"
}

build_pqclean() {
  cd "$ROOT_DIR/PQClean" || return 1
  local pq_targets=(
    crypto_kem/ml-kem-512/clean
    crypto_kem/ml-kem-768/clean
    crypto_sign/ml-dsa-44/clean
    crypto_sign/ml-dsa-65/clean
    crypto_sign/falcon-512/clean
  )
  local pq_target
  for pq_target in "${pq_targets[@]}"; do
    echo "[+] PQClean $pq_target"
    make -C "$pq_target" clean || true
    make -C "$pq_target" \
      CC="$CLANG" \
      EXTRAFLAGS="$CFLAGS" || return 1
  done
}

echo "[+] CLANG        = $CLANG"
echo "[+] TARGET       = $TARGET"
echo "[+] CFLAGS       = $CFLAGS"
echo "[+] GPGRT_CONFIG = $GPGRT_CONFIG"
echo

run_build libgcrypt "${LOG_FILES[0]}" build_libgcrypt
run_build libsodium "${LOG_FILES[1]}" build_libsodium
run_build wolfssl   "${LOG_FILES[2]}" build_wolfssl
run_build openssl   "${LOG_FILES[3]}" build_openssl
run_build mbedTLS   "${LOG_FILES[4]}" build_mbedtls
run_build BearSSL   "${LOG_FILES[5]}" build_bearssl
run_build Nettle    "${LOG_FILES[6]}" build_nettle
run_build PQClean   "${LOG_FILES[7]}" build_pqclean

print_summary_table
