# AArch64 Gadget Searching with Patched LLVM

This note describes how to download, patch, build, and run the customized LLVM/Clang used by `RSbypassSP/gadget_detect` for AArch64 gadget detection.

The current AArch64 pass detects six gadget classes:

```text
Control-flow load Gadgets Found in
Data-flow load Gadgets Found in
Control-flow store Gadgets Found in
Data-flow store Gadgets Found in
Control-flow swp Gadgets Found in
Data-flow swp Gadgets Found in
```

Here `swp` means software prefetch. On AArch64, the pass recognizes `PRFM` and `PRFUM` machine instructions.

## Analyzer Source and Patch

Analyzer implementation:

```text
gadget_detect/LLVM_FIX/AArch64MIRAnalyze.cpp
```

LLVM integration patch:

```text
gadget_detect/LLVM_FIX/AArch64MIRAnalyze_pass.patch
```

The patch integrates `AArch64MIRAnalyze.cpp` into the LLVM AArch64 backend. After rebuilding Clang, enable the pass while compiling target libraries with:

```bash
-mllvm -aarch64-mir-analyze
```

To also ask LLVM to insert AArch64 loop software prefetches, add:

```bash
-mllvm -aarch64-enable-loop-data-prefetch=true
```

The current `codes/search_gadget.sh` uses both options.

## Experiment Directories

Assume the repository is located at:

```bash
export ARTIFACT_ROOT=/home/jiangfang/workspace/RSbypassSP
export GD_ROOT="$ARTIFACT_ROOT/gadget_detect"
```

The expected layout is:

```text
$GD_ROOT/LLVM_FIX
$GD_ROOT/llvm-project
$GD_ROOT/codes
```

## Download and Patch LLVM

Run these commands from `gadget_detect`:

```bash
cd "$GD_ROOT"

git clone https://github.com/llvm/llvm-project.git
cd llvm-project
git checkout llvmorg-21.1.5
```

Apply the AArch64 pass patch:

```bash
cd "$GD_ROOT/llvm-project"
git apply ../LLVM_FIX/AArch64MIRAnalyze_pass.patch
```

If the patch fails because the LLVM files already contain the pass hook, check whether the patch was already applied:

```bash
grep -R "AArch64MIRAnalyze" llvm/lib/Target/AArch64 llvm/lib/Target/AArch64/CMakeLists.txt llvm/lib/Target/AArch64/AArch64.h
```

Make sure the analyzer source is present in LLVM:

```bash
cp "$GD_ROOT/LLVM_FIX/AArch64MIRAnalyze.cpp" \
   "$GD_ROOT/llvm-project/llvm/lib/Target/AArch64/AArch64MIRAnalyze.cpp"
```

## Build Patched Clang

```bash
cd "$GD_ROOT/llvm-project"
mkdir -p build
cd build

cmake -DLLVM_ENABLE_PROJECTS=clang \
      -G "Unix Makefiles" \
      -DLLVM_TARGETS_TO_BUILD=AArch64 \
      -DCMAKE_BUILD_TYPE=Release \
      ../llvm

make -j$(nproc)
```

After the build finishes, the customized compiler is:

```bash
export CLANG="$GD_ROOT/llvm-project/build/bin/clang"
export CLANGXX="$GD_ROOT/llvm-project/build/bin/clang++"
```

The helper script derives these paths automatically, so normally you do not need to export them by hand when using `codes/search_gadget.sh`.

## Target Libraries

The one-click script currently builds and analyzes these eight libraries:

| Library | Source directory | Output log |
|---|---|---|
| libgcrypt | `codes/libgcrypt` | `codes/res/libgcrypt_O2.log` |
| libsodium | `codes/libsodium` | `codes/res/libsodium_O2.log` |
| wolfSSL | `codes/wolfssl` | `codes/res/wolfssl_O2.log` |
| OpenSSL | `codes/OpenSSL` | `codes/res/openssl_O2.log` |
| mbedTLS | `codes/mbedTLS` | `codes/res/mbedtls_O2.log` |
| BearSSL | `codes/BearSSL` | `codes/res/bearssl_O2.log` |
| Nettle | `codes/Nettle` | `codes/res/nettle_O2.log` |
| PQClean | `codes/PQClean` | `codes/res/pqclean_O2.log` |

The final summary table is written to:

```text
codes/res/gadget_summary.txt
```

## Download Target Libraries

Run from `gadget_detect/codes`:

```bash
cd "$GD_ROOT/codes"

git clone https://github.com/gpg/libgcrypt.git libgcrypt
cd libgcrypt
git checkout libgcrypt-1.11.0
bash autogen.sh
cd ..

git clone https://github.com/jedisct1/libsodium.git libsodium
cd libsodium
git checkout 1.0.20-RELEASE
cd ..

git clone https://github.com/wolfSSL/wolfssl.git wolfssl
cd wolfssl
git checkout v5.8.4-stable
./autogen.sh
cd ..

git clone --depth 1 --branch OpenSSL_1_1_1q https://github.com/openssl/openssl.git OpenSSL
git clone --depth 1 --branch v3.6.2 https://github.com/Mbed-TLS/mbedtls.git mbedTLS
git clone --depth 1 https://www.bearssl.org/git/BearSSL BearSSL
git clone --depth 1 --branch nettle_4.0_release_20260205 https://github.com/gnutls/nettle.git Nettle
git clone --depth 1 https://github.com/PQClean/PQClean.git PQClean
```

For mbedTLS, fetch submodules once:

```bash
cd "$GD_ROOT/codes/mbedTLS"
git submodule update --init --recursive
```

`search_gadget.sh` also runs this command before building mbedTLS, so repeated runs are safe.

## Local libgpg-error for libgcrypt

Recent libgcrypt may require `libgpg-error >= 1.49`. If the system package is older, install it locally under `codes/local`:

```bash
cd "$GD_ROOT/codes"
git clone https://github.com/gpg/libgpg-error.git libgpg-error
cd libgpg-error
git checkout libgpg-error-1.49
./autogen.sh
./configure --prefix="$GD_ROOT/codes/local"
make -j$(nproc)
make install
```

Verify:

```bash
$GD_ROOT/codes/local/bin/gpgrt-config --version
```

Expected output:

```text
1.49
```

The script automatically prefers this local installation via:

```bash
export GPGRT_CONFIG="$GD_ROOT/codes/local/bin/gpgrt-config"
export PKG_CONFIG_PATH="$GD_ROOT/codes/local/lib/pkgconfig:$PKG_CONFIG_PATH"
export LD_LIBRARY_PATH="$GD_ROOT/codes/local/lib:$LD_LIBRARY_PATH"
```

## Run the Analysis

Run the one-click script:

```bash
cd "$GD_ROOT/codes"
bash search_gadget.sh
```

The script compiles all eight libraries with the customized AArch64 Clang and these flags:

```bash
--target=aarch64-unknown-linux-gnu \
-O2 \
-mllvm -aarch64-enable-loop-data-prefetch=true \
-mllvm -aarch64-mir-analyze \
-g
```

Each library log is saved under `codes/res/`. At the end, the script prints a 9-column summary table:

```text
Gadget Type    libgcrypt  libsodium  wolfssl  openssl  mbedTLS  BearSSL  Nettle  PQClean
```

The same table is also written to:

```text
$GD_ROOT/codes/res/gadget_summary.txt
```

## Notes on Individual Libraries

- `mbedTLS`: the script disables tests/programs and adds `-Wno-error=unterminated-string-initialization` because newer Clang versions may otherwise turn a string-initialization warning into a build error.
- `Nettle`: the script runs `autoreconf -fi` if `configure` is not present, then uses `--enable-mini-gmp --disable-assembler` to avoid an external GMP dependency and focus on C code.
- `OpenSSL`: the script uses `./Configure linux-aarch64 no-shared no-tests no-asm`.
- `PQClean`: PQClean is not a single monolithic library. The script builds several representative clean implementations:

```text
crypto_kem/ml-kem-512/clean
crypto_kem/ml-kem-768/clean
crypto_sign/ml-dsa-44/clean
crypto_sign/ml-dsa-65/clean
crypto_sign/falcon-512/clean
```

## Rebuilding After Editing the Pass

If you modify `AArch64MIRAnalyze.cpp`, synchronize it into LLVM and rebuild:

```bash
cp "$GD_ROOT/LLVM_FIX/AArch64MIRAnalyze.cpp" \
   "$GD_ROOT/llvm-project/llvm/lib/Target/AArch64/AArch64MIRAnalyze.cpp"

cd "$GD_ROOT/llvm-project/build"
make -j$(nproc)
```

Then rerun:

```bash
cd "$GD_ROOT/codes"
bash search_gadget.sh
```
