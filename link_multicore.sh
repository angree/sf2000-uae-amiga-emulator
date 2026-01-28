#!/bin/bash
# Universal link script for SF2000/GB300V2 multicore FrogUI
# Usage: ./link_multicore.sh [SF2000|GB300V2]

set -e
export PATH=/opt/mips32-mti-elf/2019.09-03-2/bin:$PATH

FROGGY_TYPE=${1:-SF2000}
MULTICORE=/mnt/c/Temp_FrogUI/sf2000_multicore
MIPS=/opt/mips32-mti-elf/2019.09-03-2/bin/mips-mti-elf-

echo "Building for FROGGY_TYPE: $FROGGY_TYPE"

# Set platform-specific flags and linker script
if [ "$FROGGY_TYPE" = "GB300V2" ]; then
    PLATFORM_FLAGS="-DGB300V2 -DFROGGY_MXMV=0x28"
    CORE_LD="bisrv_GB300_V2-core.ld"
    echo "Platform: GB300 V2 FrogUI"
else
    PLATFORM_FLAGS="-DSF2000 -DFROGGY_MXMV=0x60"
    CORE_LD="bisrv_08_03-core.ld"
    echo "Platform: SF2000 FrogUI"
fi

CFLAGS="-EL -march=mips32 -mtune=mips32 -msoft-float -Os -G0 -mno-abicalls -fno-pic -ffunction-sections -fdata-sections -I${MULTICORE}/libs/libretro-common/include -DDEBUG_XLOG=1 ${PLATFORM_FLAGS}"

echo "Compiling multicore wrapper objects..."
${MIPS}gcc $CFLAGS -o core_api.o -c ${MULTICORE}/core_api.c
${MIPS}gcc $CFLAGS -o lib.o -c ${MULTICORE}/lib.c
${MIPS}gcc $CFLAGS -o debug.o -c ${MULTICORE}/debug.c
${MIPS}gcc $CFLAGS -o video_sf2000.o -c ${MULTICORE}/video_sf2000.c

echo "Preparing libraries..."
cp uae4all_libretro_sf2000.a libretro_core.a

echo "Linking core.elf with ${CORE_LD}..."
${MIPS}g++ -Wl,-Map=core.elf.map -EL -march=mips32 -mtune=mips32 -msoft-float \
    -Wl,--gc-sections --static -z max-page-size=32 \
    -e __core_entry__ -Tcore.ld ${CORE_LD} -o core.elf \
    -Wl,--start-group core_api.o lib.o debug.o video_sf2000.o libretro_core.a libretro-common.a -lc -Wl,--end-group

echo "Creating binary..."
${MIPS}objcopy -O binary -R .MIPS.abiflags -R .note.gnu.build-id -R ".rel*" core.elf core_87000000

echo "Cleaning up..."
rm -f core_api.o lib.o debug.o video_sf2000.o libretro_core.a core.elf core.elf.map
rm -f core.ld ${CORE_LD} libretro-common.a

SIZE=$(stat -c %s core_87000000)
echo "Done! Output: core_87000000 ($SIZE bytes)"
