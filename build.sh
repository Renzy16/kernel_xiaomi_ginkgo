#!/usr/bin/env bash
#
# Copyright (C) 2023 Edwiin Kusuma Jaya (ryuzenn)
#
# Simple Local Kernel Build Script
#
# Configured for Redmi Note 8 / ginkgo custom kernel source
#
# Setup build env with akhilnarang/scripts repo
#
# Use this script on root of kernel directory

SECONDS=0 # builtin bash timer
DATE=$(TZ=Asia/Jakarta date +"%Y%m%d-%H%M")
ZIPNAME="Kazuya-Overdrive-Ginkgo-KSUNext-${DATE}.zip"
TC_DIR="$HOME/toolchain"
CLANG_DIR="${TC_DIR}/clang-r547379"
ARCH_DIR="${TC_DIR}/aarch64-linux-android-4.9"
ARM_DIR="${TC_DIR}/arm-linux-androideabi-4.9"
AK3_DIR="$HOME/Android/AnyKernel3"
DEFCONFIG="vendor/ginkgo_defconfig"

export PATH="$CLANG_DIR/bin:$ARCH_DIR/bin:$ARM_DIR/bin:$PATH"
export LD_LIBRARY_PATH="$CLANG_DIR/lib:$LD_LIBRARY_PATH"
export KBUILD_BUILD_VERSION="1"

if ! [ -d "${CLANG_DIR}" ]; then
echo "Clang not found! Cloning to ${TC_DIR}..."
if ! git clone --depth=1 -b master https://gitlab.com/kei-space/clang/r547379 ${CLANG_DIR}; then
echo "Cloning failed! Aborting..."
exit 1
fi
fi

if ! [ -d "${ARCH_DIR}" ]; then
echo "gcc not found! Cloning to ${ARCH_DIR}..."
if ! git clone --depth=1 -b lineage-19.1 https://github.com/LineageOS/android_prebuilts_gcc_linux-x86_aarch64_aarch64-linux-android-4.9.git ${ARCH_DIR}; then
echo "Cloning failed! Aborting..."
exit 1
fi
fi

if ! [ -d "${ARM_DIR}" ]; then
echo "gcc_32 not found! Cloning to ${ARM_DIR}..."
if ! git clone --depth=1 -b lineage-19.1 https://github.com/LineageOS/android_prebuilts_gcc_linux-x86_arm_arm-linux-androideabi-4.9.git ${ARM_DIR}; then
echo "Cloning failed! Aborting..."
exit 1
fi
fi

if [[ $1 = "-k" || $1 = "--ksu" ]]; then
echo -e "\nCleanup KernelSU first on local build\n"
rm -rf KernelSU drivers/kernelsu

echo -e "\nKSU Support, let's Make it On\n"
curl -kLSs "https://raw.githubusercontent.com/Renzy16/KernelSU-Next/next-susfs/kernel/setup.sh" | bash -s next-susfs

sed -i 's/CONFIG_KSU=n/CONFIG_KSU=y/g' arch/arm64/configs/vendor/ginkgo_defconfig
else
echo -e "\nKSU not Support, let's Skip\n"
fi

mkdir -p out
make O=out ARCH=arm64 $DEFCONFIG

echo -e "\nStarting compilation...\n"
make -j$(nproc --all) O=out \
 ARCH=arm64 \
 CC=clang \
 LD=ld.lld \
 AR=llvm-ar \
 AS=llvm-as \
 NM=llvm-nm \
 OBJCOPY=llvm-objcopy \
 OBJDUMP=llvm-objdump \
 STRIP=llvm-strip \
 CROSS_COMPILE=${ARCH_DIR}/bin/aarch64-linux-gnu- \
 CROSS_COMPILE_ARM32=${ARM_DIR}/bin/arm-linux-gnueabi- \
 CLANG_TRIPLE=aarch64-linux-gnu- \
 Image.gz-dtb \
 dtbo.img

if [ -f "out/arch/arm64/boot/Image.gz-dtb" ] && [ -f "out/arch/arm64/boot/dtbo.img" ]; then
echo -e "\nKernel compiled succesfully! Zipping up...\n"
if [ -d "$AK3_DIR" ]; then
cp -r $AK3_DIR AnyKernel3
elif ! git clone -q https://github.com/Renzy16/AnyKernel3; then
echo -e "\nAnyKernel3 repo not found locally and cloning failed! Aborting..."
exit 1
fi
cp out/arch/arm64/boot/Image.gz-dtb AnyKernel3
cp out/arch/arm64/boot/dtbo.img AnyKernel3
rm -f *zip
cd AnyKernel3
git checkout main &> /dev/null
zip -r9 "../$ZIPNAME" * -x '*.git*' README.md *placeholder
cd ..
rm -rf AnyKernel3
rm -rf out/arch/arm64/boot
echo -e "Completed in $((SECONDS / 60)) minute(s) and $((SECONDS % 60)) second(s) !"
echo "Zip: $ZIPNAME"
else
echo -e "\nCompilation failed!"
exit 1
fi
