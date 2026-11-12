SECONDS=0 # builtin bash timer

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

export DEVICE=r8q

PREBUILT_MODE=0

for arg in "$@"; do
    case "$arg" in
        -p|--prebuilt)
            PREBUILT_MODE=1
            ;;
        -h|--help)
            echo "Uso: ./build-sdclang20.sh [-p|--prebuilt]"
            echo "  -p, --prebuilt   Compila o kernel e exporta os arquivos prebuilt (Image, dtb, dtbo.img, headers) em out/prebuilt-kernel"
            echo "  (sem argumentos) Compila o kernel e gera o zip flashavel AnyKernel3"
            exit 0
            ;;
        *)
            ;;
    esac
done

# ===== AnyKernel3 =====
AK3_REPO="https://github.com/doubledashdot/AnyKernel3"
AK3_BRANCH="$DEVICE-ksu"

ZIPNAME="vanadium-$(date '+%Y%m%d').zip"
TC_DIR="$(pwd)/tc/sdclang20"
if [ "$PREBUILT_MODE" -eq 1 ]; then
DEFCONFIG="vendor/kona-vanadium_defconfig vendor/samsung/$DEVICE.config vendor/extra/no-ksu.config vendor/extra/sdclang.config"
else
DEFCONFIG="vendor/kona-vanadium_defconfig vendor/samsung/$DEVICE.config vendor/extra/ksu.config vendor/extra/sdclang.config"
fi

OUT_DIR="$(pwd)/out"
BOOT_DIR="$OUT_DIR/arch/arm64/boot"
DTS_DIR="$BOOT_DIR/dts/vendor/qcom"
PREBUILT_DIR="$OUT_DIR/prebuilt-kernel"

if test -z "$(git rev-parse --show-cdup 2>/dev/null)" &&
   head=$(git rev-parse --verify HEAD 2>/dev/null); then
    ZIPNAME="${ZIPNAME::-4}-$(echo $head | cut -c1-8)-$DEVICE.zip"
fi

rm -rf KernelSU

curl -LSs "https://raw.githubusercontent.com/doubledashdot/sKernelSU/master/kernel/setup.sh" | bash -s master

export PATH="$TC_DIR/bin:$PATH"

if ! [ -d "$TC_DIR" ]; then
    echo -e "${YELLOW}Snapdragon LLVM ARM Compiler 19.0.0 not found!${NC}"
    exit 1
fi

mkdir -p out
echo -e "${YELLOW}building with: $DEFCONFIG${NC}"

make O=out ARCH=arm64 $DEFCONFIG
make O=out ARCH=arm64 olddefconfig

echo -e "\n${YELLOW}Starting compilation...${NC}\n"

MAKE_ARGS=(
    -j$(nproc --all)
    HOSTCC=/usr/bin/clang
    HOSTCXX=/usr/bin/clang++
    O=out
    CC=clang
    ARCH=arm64
    LD=ld.lld
    LD_COMPAT=ld.lld
    KCFLAGS="-Wno-pointer-sign -Wno-default-const-init-field-unsafe -Wno-default-const-init-var-unsafe -Wno-undefined-optimized"
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

    # Limpar arquivos de controle gerados pelo kbuild
    find "$PREBUILT_DIR/kernel-headers" \( -name "..install.cmd" -o -name ".install" \) -delete 2>/dev/null

    # Copiar headers da pasta include para a raiz de kernel-headers para compatibilidade ampla
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
    rm -rf AnyKernel3
    echo "[*] Cloning AnyKernel3 for $DEVICE"
    git clone -q -b "$AK3_BRANCH" "$AK3_REPO" --depth=1 AnyKernel3 || exit 1

    echo -e "Preparing zip...\n"

    rm -rf *.zip
    cp "$BOOT_DIR/dtbo.img" AnyKernel3/dtbo.img
    cp "$BOOT_DIR/Image" AnyKernel3/Image
    cp "$BOOT_DIR/dtb" AnyKernel3/dtb

    cd AnyKernel3

    zip -r9 "../$ZIPNAME" * -x .git README.md *placeholder
    cd ..
fi

git restore drivers/Kconfig
git restore drivers/Makefile

echo -e "\n${GREEN}Completed in $((SECONDS / 60)) minute(s) and $((SECONDS % 60)) second(s)!${NC}"
if [ "$PREBUILT_MODE" -eq 1 ]; then
    echo -e "${GREEN}Prebuilt kernel salvo em: $PREBUILT_DIR${NC}"
else
    echo -e "${GREEN}Zip: $ZIPNAME${NC}"
fi
