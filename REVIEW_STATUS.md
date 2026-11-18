# Lisa integration review status

Base source: Claude reference archive, commit `89b3dd21ec6074d85170b826292042790482169f`.

## Already present in the base archive

The `lisa_audit_fixes.diff` audit changes are already applied in this tree:

- 16 MB partition layout: model offset `0x210000`, size `0xDF0000`.
- health logging and 48 KB recognizer task stack.
- PSRAM/internal-memory guard (2 MB total PSRAM, 768 KB largest PSRAM block,
  80 KB largest internal block).
- ESP-IDF sdkconfig cache/flash settings and task watchdog disabled.

Do not apply `lisa_audit_fixes.diff` again.

## Changes added in this review copy

- `main/main.c`: protects both `g_job_busy` accesses with a `portMUX_TYPE`;
  checks and logs failed `xQueueSend()` calls.
- `components/command_dtw/lisa_hardware.c`: preserves partial microphone samples
  returned together with an I2S timeout/error.
- `components/command_dtw/nd_command_dtw.c`: in QUESTION, rejects a clip when
  the closest class across all nine dictionary classes is neither `da` nor `ne`.
  The dedicated `zaspi` exception remains ahead of this guard.
- `tools/host/wav2f32.c`: host tool using the production VAD, DC blocker,
  Whistle speech gate and encoder.
- `tools/build_wdt_dictionary.py`: command-line input/output paths, direct
  `<class>_kids/*.f32` folder layout and firmware-aligned validation.
- `tools/host/test_yesno_guard.c`: regression test for unrelated-class rejection
  and valid `da`, `ne`, `zaspi` acceptance.
- ESP-IDF minimum documented as 5.5 or newer; USB Serial/JTAG selected as the
  application console for the board's native USB-Serial/JTAG interface.

## Checks completed

- `make -C tools/host clean all syntax-esp`: passed with host shim headers.
- Controller/FSM: 44 checks, 0 failed.
- VAD: 7 checks, 0 failed.
- QUESTION guard: unrelated `spri`-like input rejected; valid `da`, `ne`,
  and `zaspi` accepted.
- WDT builder: byte-for-byte identical to the previous packer on synthetic
  templates; CLI path tested.
- WDT loader smoke test: 9-class synthetic dictionary loaded and 10 synthetic
  templates were processed. This is a format/plumbing test, not an accuracy test.

## Not yet verified

- No real `whistle_d4.cact` model is included in the archives, so the full
  `test_pipeline` and `wav2f32` inference have not yet been run with the real model.
- No actual child recordings have been converted to `.f32` in this review run.
- No real ESP-IDF build or physical ESP32-S3 test has been run here.
- No firmware or model has been flashed.
- AEC is not implemented. A VAD threshold boost is not AEC and is not enabled.
- The 80 KB internal-memory guard is unchanged pending actual board health logs.
