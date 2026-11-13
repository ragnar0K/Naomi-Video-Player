# NAOMI Video Player Makefile
# ---------------------------
# Build player ROM (ready for Live Network Streaming via stream_client.py):
#   make
#
# Or optionally bake an offline video clip into the ROM (rom://movie.nvp):
#   make VIDEO=/path/to/movie.mp4 DURATION=180 FPS=15

all: naomi_player.bin tools/encode_nvp

SRCS += main.c

VIDEO ?=
DURATION ?= 30
FPS ?= 15

SERIAL := BHBV

include ${NAOMI_BASE}/tools/Makefile.base

tools/encode_nvp: tools/encode_nvp.c
	gcc -O3 -Wall $< -o $@

romfs/movie.nvp: tools/encode_nvp
	@mkdir -p romfs
ifneq ($(strip $(VIDEO)),)
	./tools/encode_nvp "$(VIDEO)" romfs/movie.nvp $(DURATION) $(FPS)
else
	@if [ ! -f romfs/movie.nvp ]; then \
		echo "No VIDEO= specified; creating empty romfs placeholder (ready for stream_client.py)..."; \
		touch romfs/.keep; \
	fi
endif

build/romfs.bin: ${ROMFSGEN_FILE} romfs/movie.nvp
	@mkdir -p $(dir $@)
	${ROMFSGEN} $@ romfs/

naomi_player.bin: ${MAKEROM_FILE} ${NAOMI_BIN_FILE} build/romfs.bin
	${MAKEROM} $@ \
		--title "NAOMI VIDEO PLAYER" \
		--publisher "Homebrew" \
		--serial "${SERIAL}" \
		--section ${NAOMI_BIN_FILE},${START_ADDR} \
		--entrypoint ${MAIN_ADDR} \
		--main-binary-includes-test-binary \
		--test-entrypoint ${TEST_ADDR} \
		--align-before-data 4 \
		--filedata build/romfs.bin

.PHONY: clean distclean
clean:
	rm -rf build
	rm -rf naomi_player.bin
	rm -f tools/encode_nvp

distclean: clean
	rm -rf romfs/movie.nvp
