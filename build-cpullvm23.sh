#!/usr/bin/env bash
set -e

SECONDS=0

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

export DEVICE="${DEVICE:-r8q}"

PREBUILT_MODE=0

for arg in "$@"; do
    case "$arg" in
        -p|--prebuilt)
            PREBUILT_MODE=1
            ;;
        -h|--help)
            echo "Uso: ./build-cpullvm23.sh [-p|--prebuilt]"
            echo "  -p, --prebuilt   Compila o kernel e exporta os arquivos prebuilt (Image, dtb, dtbo.img, headers) em out/prebuilt-kernel"
            echo "  (sem argumentos) Compila o kernel e gera o zip flashavel AnyKernel3"
            exit 0
            ;;
        *)
            ;;
    esac
done

# ===== AnyKernel3 =====
AK3_REPO="https://github.com/Lazy-Encrypted/AnyKernel3"
AK3_BRANCH="${AK3_BRANCH:-oplus}"

CPULLVM_VERSION="${CPULLVM_VERSION:-23.1.0}"
CPULLVM_URL="${CPULLVM_URL:-https://github.com/qualcomm/cpullvm-toolchain/releases/download/cpullvm-${CPULLVM_VERSION}/cpullvm-${CPULLVM_VERSION}-Linux-x86_64-Ubuntu24.tar.xz}"

ZIPNAME="tachyon-kernel-$(date '+%Y%m%d')-cpullvm23.zip"
TC_DIR="${TC_DIR:-$(pwd)/tc/sdclang23}"
if [ "$PREBUILT_MODE" -eq 1 ]; then
    DEFCONFIG="vendor/kona-perf_defconfig vendor/samsung/kona-sec-common.config vendor/samsung/$DEVICE.config vendor/not/localversion.config vendor/extra/sdclang.config"
else
    DEFCONFIG="vendor/kona-perf_defconfig vendor/samsung/kona-sec-common.config vendor/samsung/$DEVICE.config vendor/not/ksu.config vendor/not/localversion.config vendor/extra/sdclang.config"
fi

OUT_DIR="$(pwd)/out"
BOOT_DIR="$OUT_DIR/arch/arm64/boot"
DTS_DIR="$BOOT_DIR/dts/vendor/qcom"
PREBUILT_DIR="$OUT_DIR/prebuilt-kernel"

if test -z "$(git rev-parse --show-cdup 2>/dev/null)" &&
   head=$(git rev-parse --verify HEAD 2>/dev/null); then
    ZIPNAME="tachyon-kernel-$(date '+%Y%m%d')-$(echo "$head" | cut -c1-12)-cpullvm23-$DEVICE.zip"
fi

if [ ! -e KernelSU/kernel/Makefile ]; then
    git submodule update --init --recursive
fi

if ! [ -d "$TC_DIR/bin" ]; then
    echo -e "${YELLOW}Qualcomm CPULLVM ${CPULLVM_VERSION} not found at $TC_DIR! Downloading...${NC}"
    mkdir -p "$TC_DIR"
    if ! curl -fL "$CPULLVM_URL" | tar -xJ -C "$TC_DIR" --strip-components=1; then
        echo -e "${RED}Failed to download or extract CPULLVM ${CPULLVM_VERSION}!${NC}"
        rm -rf "$TC_DIR"
        exit 1
    fi
    echo -e "${GREEN}Qualcomm CPULLVM ${CPULLVM_VERSION} ready!${NC}"
fi

if ! command -v python >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    mkdir -p "$(pwd)/bin"
    ln -sf "$(command -v python3)" "$(pwd)/bin/python"
fi

if [ -x /usr/bin/clang ] && [ -x /usr/bin/clang++ ]; then
    HOST_CC=/usr/bin/clang
    HOST_CXX=/usr/bin/clang++
else
    HOST_CC=gcc
    HOST_CXX=g++
fi

export PATH="$(pwd)/bin:$TC_DIR/bin:$PATH"

mkdir -p "$OUT_DIR"
echo -e "${YELLOW}building with: $DEFCONFIG${NC}"

MAKE_ARGS=(
    -j"$(nproc --all)"
    HOSTCC="$HOST_CC"
    HOSTCXX="$HOST_CXX"
    O=out
    CC=clang
    ARCH=arm64
    LD=ld.lld
    LD_COMPAT=ld.lld
    AS=llvm-as
    AR=llvm-ar
    NM=llvm-nm
    OBJCOPY=llvm-objcopy
    OBJDUMP=llvm-objdump
    STRIP=llvm-strip
    CROSS_COMPILE=aarch64-linux-gnu-
    CROSS_COMPILE_ARM32=arm-linux-gnueabi-
    LLVM=1
    LLVM_IAS=1
)

make O=out ARCH=arm64 HOSTCC="$HOST_CC" HOSTCXX="$HOST_CXX" CC=clang LD=ld.lld LLVM=1 LLVM_IAS=1 $DEFCONFIG
make O=out ARCH=arm64 HOSTCC="$HOST_CC" HOSTCXX="$HOST_CXX" CC=clang LD=ld.lld LLVM=1 LLVM_IAS=1 olddefconfig

echo -e "\n${YELLOW}Starting compilation...${NC}\n"

make "${MAKE_ARGS[@]}" dtbo.img
make "${MAKE_ARGS[@]}" Image

if [ -f "$BOOT_DIR/Image" ]; then
    echo -e "${GREEN}Kernel Image found!${NC}"

    if [ -d "$DTS_DIR" ]; then
        echo -e "${BLUE}Generating dtb from $DTS_DIR...${NC}"
        cat $(find "$DTS_DIR" -type f -name "*.dtb" | sort) > "$BOOT_DIR/dtb"

        if [ -f "$BOOT_DIR/dtb" ]; then
            echo -e "${GREEN}dtb generated successfully!${NC}"
        else
            echo -e "${RED}Failed to generate dtb! Check if dtbs were compiled.${NC}"
            exit 1
        fi
    else
        echo -e "${RED}DTS directory not found. Compilation might be incomplete.${NC}"
        exit 1
    fi
else
    echo -e "\n${RED}Compilation failed! Image not found.${NC}"
    exit 1
fi

if [ "$PREBUILT_MODE" -eq 1 ]; then
    echo -e "\n${YELLOW}Modo prebuilt ativado! Preparando arquivos em $PREBUILT_DIR...${NC}\n"
    rm -rf "$PREBUILT_DIR"
    mkdir -p "$PREBUILT_DIR/kernel-headers"

    echo -e "${BLUE}Instalando kernel headers...${NC}"
    make "${MAKE_ARGS[@]}" INSTALL_HDR_PATH="$PREBUILT_DIR/kernel-headers" headers_install

    find "$PREBUILT_DIR/kernel-headers" \( -name "..install.cmd" -o -name ".install" \) -delete 2>/dev/null || true

    if [ -d "$PREBUILT_DIR/kernel-headers/include" ]; then
        cp -rn "$PREBUILT_DIR/kernel-headers/include/"* "$PREBUILT_DIR/kernel-headers/" 2>/dev/null || true
    fi

    echo -e "${BLUE}Copiando Image, dtb e dtbo.img...${NC}"
    cp "$BOOT_DIR/Image" "$PREBUILT_DIR/Image"
    cp "$BOOT_DIR/dtb" "$PREBUILT_DIR/dtb"
    cp "$BOOT_DIR/dtbo.img" "$PREBUILT_DIR/dtbo.img"

    echo -e "\n${GREEN}Arquivos prebuilt gerados com sucesso em $PREBUILT_DIR:${NC}"
    ls -la "$PREBUILT_DIR"
else
    if ! [ -d AnyKernel3 ]; then
        echo "[*] Cloning AnyKernel3 for $DEVICE"
        git clone -q -b "$AK3_BRANCH" "$AK3_REPO" --depth=1 AnyKernel3 || exit 1
    fi

    echo -e "Preparing zip...\n"

    cp "$BOOT_DIR/dtbo.img" AnyKernel3/dtbo.img
    cp "$BOOT_DIR/Image" AnyKernel3/Image
    cp "$BOOT_DIR/dtb" AnyKernel3/dtb

    if command -v zip >/dev/null 2>&1; then
        (cd AnyKernel3 && zip -r9 "../$ZIPNAME" * -x .git README.md '*placeholder')
    else
        python3 - "$ZIPNAME" <<'EOF'
import os, sys, zipfile
zpath = sys.argv[1]
skip = {'.git', 'README.md', 'placeholder'}
with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk('AnyKernel3'):
        dirs[:] = [d for d in dirs if d not in skip]
        for fn in files:
            if fn in skip:
                continue
            full = os.path.join(root, fn)
            zf.write(full, os.path.relpath(full, 'AnyKernel3'))
EOF
    fi
fi

echo -e "\n${GREEN}Completed in $((SECONDS / 60)) minute(s) and $((SECONDS % 60)) second(s)!${NC}"
if [ "$PREBUILT_MODE" -eq 1 ]; then
    echo -e "${GREEN}Prebuilt kernel salvo em: $PREBUILT_DIR${NC}"
else
    echo -e "${GREEN}Zip: $ZIPNAME${NC}"
fi
