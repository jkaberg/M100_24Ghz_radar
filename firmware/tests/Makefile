# Host tests. `make` runs everything except the long libFuzzer run; deps come from tools/setup.sh.
CC      ?= gcc
SAN     := -fsanitize=address,undefined
CFLAGS  := -std=gnu99 -Wall -Wextra -Werror -O1 -g $(SAN) -I../src
ROOT    := ../..
DEPS    := $(ROOT)/.deps
SDK     := $(DEPS)/tl_zigbee_sdk
STOCK   := $(DEPS)/dumps/stock_zt3l_aoclfnxz.bin
GD32    := $(DEPS)/dumps/ZY-M100-24GV2_TZE204_7gclukjs.bin
IMAGE   := ../out/zy_m100_zrd.bin
B       := build

CORE    := ../src/tuya_mcu.c ../src/radar.c
GLUE    := ../src/app_radar.c $(CORE)

all: unit handoff nv frames fuzz

$(B):
	mkdir -p $@

# pure logic, then the real app_radar.c against SDK stubs and a simulated radar MCU
unit: | $(B)
	$(CC) $(CFLAGS) -o $(B)/test_core test_core.c $(CORE)
	$(CC) $(CFLAGS) -Wno-unused-parameter -Istubs -I../src/include -o $(B)/test_glue test_glue.c $(GLUE)
	$(B)/test_core
	cd $(B) && ./test_glue

# power cuts / watchdog resets during the stock -> custom handoff, on a real stock dump
handoff: $(IMAGE) | $(B)
	$(CC) -O2 -DHOST_TEST -Ihandoff -Wall -o $(B)/test_handoff handoff/test_handoff.c ../src/ota_scheme/ensure_ota_scheme.c
	$(CC) -O2 -DHOST_TEST -Ihandoff -w -o $(B)/test_handoff_rom handoff/test_handoff_rom.c ../src/ota_scheme/ensure_ota_scheme.c
	$(B)/test_handoff $(STOCK) $(IMAGE)
	$(B)/test_handoff_rom $(STOCK) $(IMAGE)

# the SDK's own NV code on the converted flash (stub tl_common.h sits where drv_nv.c expects it)
nv: $(IMAGE) | $(B)
	mkdir -p $(B)/nv/sdk
	cp $(SDK)/proj/drivers/drv_nv.c $(SDK)/proj/drivers/drv_nv.h $(B)/nv/sdk/
	cp nv/tl_common.h $(B)/nv/
	$(CC) -O1 -g $(SAN) -w -I$(B)/nv -o $(B)/test_nv_leftovers nv/test_nv_leftovers.c $(B)/nv/sdk/drv_nv.c
	$(B)/test_nv_leftovers $(STOCK) $(IMAGE) | tail -3

# every frame the glue test sent, through the real GD32 receive parser (unicorn)
frames: unit
	$(ROOT)/.venv/bin/python -I $(ROOT)/tools/check_frames_gd32.py $(B)/tx_frames.bin $(GD32) | tail -3

fuzz: | $(B)
	$(CC) -DRT_STANDALONE -g -O1 $(SAN) -Wno-unused-parameter -Istubs -I../src/include -I../src \
	      -o $(B)/fuzz_uart_loop fuzz/fuzz_uart.c $(GLUE)
	$(B)/fuzz_uart_loop

libfuzzer: | $(B)
	clang -g -O1 -fsanitize=fuzzer,address,undefined -Wno-unused-parameter -Istubs -I../src/include -I../src \
	      -o $(B)/fuzz_uart fuzz/fuzz_uart.c $(GLUE)
	cd $(B) && ./fuzz_uart -max_total_time=180

$(IMAGE):
	@echo "build the firmware first: firmware/build.sh" && false

clean:
	rm -rf $(B)

.PHONY: all unit handoff nv frames fuzz libfuzzer clean
