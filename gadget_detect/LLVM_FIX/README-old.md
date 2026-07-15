# Gadget Searching in Common Libraries

This artifact contains the LLVM-based gadget-searching pass used for Section 4.3, "Existence of RS Contention Gadgets". The experiment reproduces the gadget counts reported in Table 4 by compiling common libraries with a patched Clang pass.

## Analyzer Source and Patch

Analyzer implementation:

```text
gadget_detect/LLVM_FIX/X86MIRanalyze.cpp
```

LLVM integration patch:

```text
gadget_detect/LLVM_FIX/X86MIRanalyze_pass.patch
```

`X86MIRanalyze_pass.patch` integrates `X86MIRanalyze.cpp` into the LLVM X86 backend. After rebuilding Clang, enable the pass during library compilation with:

```text
-mllvm -x86-mir-analyze
```

During compilation, the pass prints one line for each detected gadget. The result script counts the following two markers:

```text
Variable-time memory access Gadgets Found
REP-MOVSB Gadgets Found
```

## Environment

Reference environment:

```text
CPU:    Intel(R) Xeon(R) Gold 6248R CPU @ 3.00GHz
Kernel: 6.8.0-85-generic
OS:     Ubuntu 22.04.1
LLVM:   llvmorg-21.1.5
```

## Experiment Directories

Run the commands in this README from `gadget_detect/LLVM_FIX/`. Set the artifact root, work directory, and log directory:

```bash
export ARTIFACT_ROOT=/path/to/Anonymous_Repo
export AE_WORK="$PWD/ae_work"
export AE_OUT="$PWD/ae_out"
mkdir -p "$AE_WORK" "$AE_OUT"
```

## Build Patched Clang

```bash
cd "$AE_WORK"
git clone https://github.com/llvm/llvm-project.git
cd llvm-project
git checkout llvmorg-21.1.5
git apply "$ARTIFACT_ROOT/gadget_detect/LLVM_FIX/X86MIRanalyze_pass.patch"

mkdir -p build
cd build
cmake -DLLVM_ENABLE_PROJECTS=clang \
      -G "Unix Makefiles" \
      -DLLVM_TARGETS_TO_BUILD=X86 \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_USE_LINKER=gold \
      ../llvm
make -j$(nproc)

export CLANG="/home/jiangfang/workspace/RSbypassSP/gadget_detect/llvm-project/build/bin/clang"
export CLANGXX="/home/jiangfang/workspace/RSbypassSP/gadget_detect/llvm-project/build/bin/clang++"
```

## Target Libraries

| Library | Version/tag | Output log |
|---|---|---|
| OpenSSL | `OpenSSL_1_1_1q` | `openssl_O2.log` |
| Libgcrypt | `libgcrypt-1.11.0` | `libgcrypt_O2.log` |
| libsodium | `1.0.20-RELEASE` | `libsodium_O2.log` |
| OpenBLAS | `v0.3.0` | `OpenBLAS_O2.log` |
| musl | `v1.2.5` | `musl_O2.log` |
| zlib | `v1.3.1` | `zlib_O2.log` |
| wolfSSL | `v5.8.4-stable` | `wolfssl_O2.log` |
| mbedTLS | `v3.6.2` | `mbedtls_O2.log` |
| BearSSL | commit `7bea48e` | `bearssl_O2.log` |
| Nettle | `nettle_4.0_release_20260205` | `nettle_O2.log` |
| PQClean | commit `202a8f9` | `pqclean_O2.log` |

## Run the Analysis

### OpenSSL 1.1.1q

```bash
cd "$AE_WORK"
git clone https://github.com/openssl/openssl.git
cd openssl
git checkout OpenSSL_1_1_1q

./config \
  CC="$CLANG" \
  CFLAGS="-O2 -mllvm -x86-mir-analyze -g" \
  CXX="$CLANGXX" \
  CXXFLAGS="-O2 -mllvm -x86-mir-analyze -g"

make clean
make -j$(nproc) > "$AE_OUT/openssl_O2.log" 2>&1
```

If OpenSSL reports `test/v3ext.c:201:24: error: call to undeclared library function 'memcmp'`, add `#include <string.h>` to `test/v3ext.c` and rebuild.

### Libgcrypt 1.11.0

```bash
cd "$AE_WORK"
git clone https://github.com/gpg/libgcrypt.git
cd libgcrypt
git checkout libgcrypt-1.11.0
bash autogen.sh

./configure --disable-doc \
  CC="$CLANG" \
  CFLAGS="-O2 -mllvm -x86-mir-analyze -g" \
  CXX="$CLANGXX" \
  CXXFLAGS="-O2 -mllvm -x86-mir-analyze -g"

make clean
make -j$(nproc) > "$AE_OUT/libgcrypt_O2.log" 2>&1
```

### libsodium 1.0.20

```bash
cd "$AE_WORK"
git clone https://github.com/jedisct1/libsodium.git
cd libsodium
git checkout 1.0.20-RELEASE

./configure \
  CC="$CLANG" \
  CFLAGS="-O2 -mllvm -x86-mir-analyze -g" \
  CXX="$CLANGXX" \
  CXXFLAGS="-O2 -mllvm -x86-mir-analyze -g"

make clean
make > "$AE_OUT/libsodium_O2.log" 2>&1
```

### OpenBLAS 0.3.0

```bash
cd "$AE_WORK"
git clone https://github.com/OpenMathLib/OpenBLAS.git
cd OpenBLAS
git checkout v0.3.0

mkdir -p build
cd build
cmake .. \
  -DCMAKE_C_COMPILER="$CLANG" \
  -DCMAKE_C_FLAGS="-O2 -mllvm -x86-mir-analyze -g -Wno-incompatible-function-pointer-types" \
  -DCMAKE_CXX_COMPILER="$CLANGXX" \
  -DCMAKE_CXX_FLAGS="-O2 -mllvm -x86-mir-analyze -g -Wno-incompatible-function-pointer-types"

make clean
make -j$(nproc) > "$AE_OUT/OpenBLAS_O2.log" 2>&1
```

### musl 1.2.5

```bash
cd "$AE_WORK"
git clone https://github.com/kraj/musl.git
cd musl
git checkout v1.2.5

./configure \
  CC="$CLANG" \
  CFLAGS="-O2 -mllvm -x86-mir-analyze -g"

make clean
make > "$AE_OUT/musl_O2.log" 2>&1
```

### zlib 1.3.1

```bash
cd "$AE_WORK"
git clone https://github.com/madler/zlib.git
cd zlib
git checkout v1.3.1

./configure
make clean
make -j$(nproc) \
  CC="$CLANG" \
  CFLAGS="-O2 -mllvm -x86-mir-analyze -g" \
  > "$AE_OUT/zlib_O2.log" 2>&1
```

### wolfSSL 5.8.4

```bash
cd "$AE_WORK"
git clone https://github.com/wolfSSL/wolfssl.git
cd wolfssl
git checkout v5.8.4-stable

./autogen.sh
./configure \
  CC="$CLANG" \
  CFLAGS="-O2 -mllvm -x86-mir-analyze -g"

make clean
make > "$AE_OUT/wolfssl_O2.log" 2>&1
```


### Extra crypto libraries in `codes/`

The current `codes/search_gadget.sh` also contains AArch64 rules for the following additional crypto libraries. The script assumes the patched AArch64 pass is enabled with:

```bash
-mllvm -aarch64-enable-loop-data-prefetch=true -mllvm -aarch64-mir-analyze
```

If `libgcrypt` requires `libgpg-error >= 1.49`, install it under `codes/local` and let the script export:

```bash
export GPGRT_CONFIG="$ROOT_DIR/local/bin/gpgrt-config"
export PKG_CONFIG_PATH="$ROOT_DIR/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$ROOT_DIR/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

Download the additional libraries into `gadget_detect/codes`:

```bash
cd "$ARTIFACT_ROOT/gadget_detect/codes"

git clone --depth 1 --branch OpenSSL_1_1_1q https://github.com/openssl/openssl.git OpenSSL
git clone --depth 1 --branch v3.6.2 https://github.com/Mbed-TLS/mbedtls.git mbedTLS
git clone --depth 1 https://www.bearssl.org/git/BearSSL BearSSL
git clone --depth 1 --branch nettle_4.0_release_20260205 https://github.com/gnutls/nettle.git Nettle
git clone --depth 1 https://github.com/PQClean/PQClean.git PQClean
```

Run all enabled rules from the `codes` directory:

```bash
cd "$ARTIFACT_ROOT/gadget_detect/codes"
bash search_gadget.sh
```

The added rules produce:

```text
res/openssl_O2.log
res/mbedtls_O2.log
res/bearssl_O2.log
res/nettle_O2.log
res/pqclean_O2.log
```

#### OpenSSL

```bash
cd "$ARTIFACT_ROOT/gadget_detect/codes/OpenSSL"
./Configure linux-aarch64 no-shared no-tests no-asm \
  CC="$CLANG" \
  CFLAGS="$CFLAGS"
make clean || true
make -j$(nproc) > "$AE_OUT/openssl_O2.log" 2>&1
```

#### mbedTLS

```bash
cd "$ARTIFACT_ROOT/gadget_detect/codes/mbedTLS"
git submodule update --init --recursive
rm -rf build-gadget
mkdir -p build-gadget
cd build-gadget
cmake .. \
  -DENABLE_TESTING=Off \
  -DENABLE_PROGRAMS=Off \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CLANG" \
  -DCMAKE_C_FLAGS="$CFLAGS -Wno-error=unterminated-string-initialization" \
  -DCMAKE_CXX_COMPILER="$CLANGXX" \
  -DCMAKE_CXX_FLAGS="$CXXFLAGS"
make -j$(nproc) > "$AE_OUT/mbedtls_O2.log" 2>&1
```

#### BearSSL

```bash
cd "$ARTIFACT_ROOT/gadget_detect/codes/BearSSL"
make clean || true
make -j$(nproc) \
  CC="$CLANG" \
  LD="$CLANG" \
  LDDLL="$CLANG" \
  CFLAGS="$CFLAGS -W -Wall -fPIC" \
  > "$AE_OUT/bearssl_O2.log" 2>&1
```

#### Nettle

The git checkout may not contain a generated `configure` script. Generate it once before configuring.

```bash
cd "$ARTIFACT_ROOT/gadget_detect/codes/Nettle"
if [ ! -x ./configure ]; then
  autoreconf -fi
fi
./configure \
  --host="aarch64-unknown-linux-gnu" \
  --disable-assembler \
  --enable-mini-gmp \
  CC="$CLANG" \
  CFLAGS="$CFLAGS"
make clean || true
make -j$(nproc) > "$AE_OUT/nettle_O2.log" 2>&1
```

#### PQClean

PQClean is not a single monolithic library. The script builds representative clean implementations to trigger the LLVM pass on concrete algorithm code:

```bash
cd "$ARTIFACT_ROOT/gadget_detect/codes/PQClean"
PQ_TARGETS=(
  crypto_kem/ml-kem-512/clean
  crypto_kem/ml-kem-768/clean
  crypto_sign/ml-dsa-44/clean
  crypto_sign/ml-dsa-65/clean
  crypto_sign/falcon-512/clean
)

: > "$AE_OUT/pqclean_O2.log"
for pq_target in "${PQ_TARGETS[@]}"; do
  make -C "$pq_target" clean >> "$AE_OUT/pqclean_O2.log" 2>&1 || true
  make -C "$pq_target" \
    CC="$CLANG" \
    EXTRAFLAGS="$CFLAGS" \
    >> "$AE_OUT/pqclean_O2.log" 2>&1
done
```

## Collect Results

After all builds finish, generate the Table 4 data:

```bash
bash "$ARTIFACT_ROOT/gadget_detect/LLVM_FIX/scripts/collect_gadget_table.sh" "$AE_OUT"
```

Expected output:

| Gadget type | OpenSSL 1.1.1q | Libgcrypt 1.11.0 | libsodium 1.0.20 | OpenBLAS 0.3.0 | musl 1.2.5 | zlib 1.3.1 | wolfssl 5.8.4 |
|---|---:|---:|---:|---:|---:|---:|---:|
| variable-time memory access gadgets | 52 | 77 | 46 | 24 | 26 | 5 | 15 |
| REP-MOVSB gadgets | 28 | 13 | 0 | 0 | 10 | 9 | 10 |
