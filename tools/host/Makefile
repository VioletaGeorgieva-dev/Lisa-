# Host (x86) build of everything that does not need the ESP32 hardware.
#   make -C tools/host test  MODEL=../../model/whistle/whistle_d4.cact
# The synthetic dictionary/clips come from tools/make_synthetic_wdt.py and
# are only for plumbing and timing - never for accuracy claims.
ROOT   := ../..
CD     := $(ROOT)/components/command_dtw
EG     := $(ROOT)/components/engine
INC    := -I$(CD)/include -I$(EG)/include -I$(ROOT)/main
CFLAGS := -O2 -D_GNU_SOURCE -Wall -Wextra $(INC)
OUT    := build
MODEL  ?= $(ROOT)/model/whistle/whistle_d4.cact
SYN    := $(OUT)/synth

DTW_SRC := $(CD)/nd_command_dtw.c $(CD)/command_dictionary.c $(CD)/command_state.c

all: $(OUT)/test_controller $(OUT)/test_vad $(OUT)/test_pipeline $(OUT)/pc_dtw_runner $(OUT)/bench_dtw $(OUT)/wav2f32 $(OUT)/test_yesno_guard

$(OUT):
	mkdir -p $(OUT)

$(OUT)/test_controller: | $(OUT)
	$(CC) $(CFLAGS) test_controller.c host_hw.c $(ROOT)/main/lisa_controller.c $(CD)/lisa_player.c $(CD)/command_state.c -o $@

$(OUT)/test_vad: | $(OUT)
	$(CC) $(CFLAGS) test_vad.c $(ROOT)/main/lisa_audio_in.c -lm -o $@

$(OUT)/test_pipeline: | $(OUT)
	$(CC) $(CFLAGS) test_pipeline.c $(ROOT)/main/lisa_recognizer.c $(ROOT)/main/lisa_audio_in.c $(EG)/src/*.c $(DTW_SRC) -lm -o $@

$(OUT)/pc_dtw_runner: | $(OUT)
	$(CC) $(CFLAGS) $(ROOT)/tools/pc_dtw_runner.c $(DTW_SRC) -lm -o $@

$(OUT)/bench_dtw: | $(OUT)
	$(CC) $(CFLAGS) $(ROOT)/tools/bench_dtw.c $(DTW_SRC) -lm -o $@

$(OUT)/wav2f32: wav2f32.c $(ROOT)/main/lisa_audio_in.c $(EG)/src/*.c | $(OUT)
	$(CC) $(CFLAGS) wav2f32.c $(ROOT)/main/lisa_audio_in.c $(EG)/src/*.c -lm -o $@

$(OUT)/test_yesno_guard: test_yesno_guard.c $(DTW_SRC) | $(OUT)
	$(CC) $(CFLAGS) test_yesno_guard.c $(DTW_SRC) -lm -o $@

$(SYN)/commands.wdt: | $(OUT)
	python3 -I $(ROOT)/tools/make_synthetic_wdt.py $(SYN)

test: all $(SYN)/commands.wdt
	$(OUT)/test_controller | tail -1
	$(OUT)/test_vad | tail -1
	$(OUT)/test_yesno_guard
	$(OUT)/test_pipeline $(MODEL) $(SYN)/commands.wdt | tail -1
	$(OUT)/pc_dtw_runner $(SYN)/commands.wdt $(SYN)/test | sed -n '/SUMMARY/,$$p'

# ESP-side sources type-checked against the local shim headers (NOT real IDF)
syntax-esp:
	for f in $(ROOT)/main/*.c; do $(CC) -std=gnu11 -Wall -Wextra -fsyntax-only -DESP_PLATFORM -Ishim $(INC) $$f || exit 1; done

clean:
	rm -rf $(OUT)
.PHONY: all test syntax-esp clean
