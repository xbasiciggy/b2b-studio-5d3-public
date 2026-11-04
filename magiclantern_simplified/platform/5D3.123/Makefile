MODEL = 5D3
FW_VERSION = 123

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120
ML_BOOT_OBJ = boot-d45.o

#CANON_FIR = 5D300123.FIR

MAIN_FIRMWARE_ADDR = 0xFF0C0000
PLATFORM_ARCH = armv5te

# DryOSmemory map
# RESTARTSTART is selected to be just above the end of the bss
#
RESTARTSTART = 0x69500

ML_BITRATE_OBJ = bitrate-5d3.o
ML_AF_PATTERNS_OBJ = n

ML_SRC_EXTRA_OBJS = $(BUILD_DIR)/video_hacks.o $(BUILD_DIR)/afma.o $(BUILD_DIR)/asm.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/filmroll.o $(BUILD_DIR)/fos_gfx.o $(BUILD_DIR)/fos_film.o $(BUILD_DIR)/fos_ui.o $(BUILD_DIR)/fos_shelf.o $(BUILD_DIR)/fos_pages.o $(BUILD_DIR)/fos_hud.o $(BUILD_DIR)/fos_bake.o $(BUILD_DIR)/fos_studio.o $(BUILD_DIR)/fos_preview.o $(BUILD_DIR)/fos_quick.o $(BUILD_DIR)/tjpgd.o $(BUILD_DIR)/fos_nodark.o $(BUILD_DIR)/fos_jpeg.o $(BUILD_DIR)/fos_bench.o

include ../Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete
