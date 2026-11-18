# Lisa — ESP32-S3 offline story player

Pipeline: INMP441 I2S microphone → DC blocker → VAD (`main/lisa_audio_in.c`) →
Whistle speech gate + encoder (`components/engine`) → DTW classifier
(`components/command_dtw`) → state machine + player → MAX98357A speaker.

## Hardware and build requirements

- ESP32-S3 N16R8: 16 MB flash and 8 MB octal PSRAM.
- INMP441 microphone and MAX98357A amplifier.
- ESP-IDF **5.5 or newer**.
- A microSD card with the story WAV files and command dictionary.

The firmware uses the 16 MB partition table in `partitions.csv`. The model
partition begins at `0x210000` and occupies `0xDF0000` bytes. Do not change
these values independently of the stripped model size.

## Model

The upstream Whistle model is 16.9 MB and does not fit in this board's flash
partition. It must be stripped to decoder depth 4. The depth-4 file is expected
to be exactly 14,515,887 bytes and is written at offset `0x210000`.

The existing `whistle-esp32` checkout contains `tools/whistle_strip.py`; use
that script and its matching `tools/whistle_cact.py` to create
`model/whistle/whistle_d4.cact`. Do not download an arbitrary pre-stripped
model or change the flash offset.

## Build the host WAV-to-embedding tool

From the project root:

```sh
make -C tools/host wav2f32
```

Usage:

```sh
./tools/host/build/wav2f32 model/whistle/whistle_d4.cact input.wav output.f32
```

Input must be PCM16, mono, 16 kHz WAV. Convert source recordings first, for
example:

```sh
ffmpeg -i input.m4a -ac 1 -ar 16000 -sample_fmt s16 output.wav
```

Each WAV must contain one utterance, approximately 0.3–0.5 seconds of real room
noise before speech, and at least 0.6 seconds of trailing silence so VAD can
close the utterance. `wav2f32` uses the production VAD, DC blocker, Whistle
speech gate, and encoder. The VAD constants in this host tool match the current
`Kconfig.projbuild` defaults; if those firmware defaults are changed, update
the matching constants in `tools/host/wav2f32.c` before regenerating templates.

## Build the command dictionary

The `.f32` files go directly inside each class folder, with no extra nesting:

```text
recordings/templates/hey_lisa_kids/*.f32
recordings/templates/napred_kids/*.f32
recordings/templates/pauziraj_kids/*.f32
recordings/templates/predishna_kids/*.f32
recordings/templates/sledvashta_kids/*.f32
recordings/templates/spri_kids/*.f32
recordings/templates/zaspi_kids/*.f32
recordings/templates/da_kids/*.f32
recordings/templates/ne_kids/*.f32
```

Build the dictionary (all nine class folders are required):

```sh
python tools/build_wdt_dictionary.py recordings/templates recordings/commands.wdt
```

The builder validates frame counts, finite float values, and the firmware's
256-frame per-template limit. The seven command thresholds remain the values
from the existing project; `da` and `ne` are uncalibrated and use the firmware's
yes/no threshold.

## SD card

The firmware expects these files at the SD root:

```text
/commands.wdt
/izberi.wav
/001_question.wav       (optional)
/stories/001.wav
/stories/002.wav
```

All player WAV files must be PCM mono, 16-bit, 22050 Hz.

## Host checks

```sh
make -C tools/host all syntax-esp
./tools/host/build/test_controller
./tools/host/build/test_vad
./tools/host/build/test_yesno_guard
```

`syntax-esp` uses local shim headers and is not a substitute for an actual
ESP-IDF build. `test_yesno_guard` is a synthetic regression test for rejecting
unrelated classes during QUESTION; it is not an accuracy measurement. The full
encoder test requires the real `whistle_d4.cact` model.

## Important limitations before first hardware test

- Internal/PSRAM availability must be checked from the device health logs.
  The current 80 KB internal contiguous-block guard is intentionally retained
  until real measurements justify changing it.
- The microphone is muted during Lisa's prompt, but it remains active while a
  story is playing. Echo cancellation is not implemented. A +dB VAD boost is
  not AEC and is not enabled by default.
- Do not flash until the model size, partition table, generated WDT dictionary,
  and ESP-IDF build have all been verified.
