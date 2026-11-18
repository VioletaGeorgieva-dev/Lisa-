/* wav2f32: reproduce Lisa's microphone -> VAD -> speech gate -> Whistle
 * encoder path on a host PC. Input: PCM16, mono, 16 kHz WAV.
 * Output: raw little-endian float32 embeddings, [frames][512].
 *
 * This intentionally uses the production lisa_audio_in.c and nd_whistle
 * implementation. It does not use a separate Python feature extractor.
 */
#include "lisa_audio_in.h"
#include "nd_whistle.h"
#include "nd_command_dtw.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 16000u
#define MAX_UTTERANCE_MS 3000u
#define MAX_SAMPLES ((size_t)MAX_UTTERANCE_MS * (SAMPLE_RATE / 1000u))
#define MIN_SPEECH_DB 10.0f
#define SPEECH_MARGIN_MS 150u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_wav_pcm16_mono_16k(const char *path, int16_t **samples_out,
                                  size_t *count_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t header[12];
    uint16_t format = 0, channels = 0, bits = 0, block_align = 0;
    uint32_t sample_rate = 0, data_size = 0;
    uint8_t *data = NULL;
    int have_fmt = 0, have_data = 0;

    if (!f) {
        fprintf(stderr, "cannot open WAV '%s': %s\n", path, strerror(errno));
        return 0;
    }
    if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) {
        fprintf(stderr, "invalid RIFF/WAVE file: %s\n", path);
        fclose(f);
        return 0;
    }

    while (!have_data) {
        uint8_t chdr[8];
        if (fread(chdr, 1, sizeof(chdr), f) != sizeof(chdr))
            break;
        uint32_t n = rd32(chdr + 4);
        if (!memcmp(chdr, "fmt ", 4)) {
            uint8_t fmt[40];
            if (n < 16 || n > sizeof(fmt) || fread(fmt, 1, n, f) != n) {
                fprintf(stderr, "unsupported/truncated WAV fmt chunk\n");
                fclose(f);
                return 0;
            }
            format = rd16(fmt + 0);
            channels = rd16(fmt + 2);
            sample_rate = rd32(fmt + 4);
            block_align = rd16(fmt + 12);
            bits = rd16(fmt + 14);
            have_fmt = 1;
        } else if (!memcmp(chdr, "data", 4)) {
            data = (uint8_t *)malloc(n ? n : 1);
            if (!data || fread(data, 1, n, f) != n) {
                fprintf(stderr, "cannot read WAV data chunk\n");
                free(data);
                fclose(f);
                return 0;
            }
            data_size = n;
            have_data = 1;
        } else {
            if (fseek(f, (long)n, SEEK_CUR) != 0) {
                fprintf(stderr, "invalid WAV chunk length\n");
                fclose(f);
                return 0;
            }
        }
        if (n & 1u) (void)fseek(f, 1, SEEK_CUR); /* RIFF chunks are word padded */
    }
    fclose(f);

    if (!have_fmt || !have_data || format != 1 || channels != 1 ||
        sample_rate != SAMPLE_RATE || bits != 16 || block_align != 2 ||
        (data_size & 1u)) {
        fprintf(stderr,
                "WAV must be PCM16 mono 16000 Hz (got format=%u channels=%u rate=%u bits=%u align=%u)\n",
                format, channels, sample_rate, bits, block_align);
        free(data);
        return 0;
    }

    size_t count = data_size / 2;
    int16_t *samples = (int16_t *)malloc(count * sizeof(int16_t));
    if (!samples) {
        fprintf(stderr, "out of memory for %zu WAV samples\n", count);
        free(data);
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        uint16_t u = rd16(data + i * 2);
        samples[i] = (int16_t)(u <= 32767u ? (int32_t)u : (int32_t)u - 65536);
    }

    free(data);
    *samples_out = samples;
    *count_out = count;
    return 1;
}

static int write_f32_le(const char *path, const float *x, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot create '%s': %s\n", path, strerror(errno));
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t u;
        uint8_t b[4];
        memcpy(&u, &x[i], sizeof(u));
        b[0] = (uint8_t)u;
        b[1] = (uint8_t)(u >> 8);
        b[2] = (uint8_t)(u >> 16);
        b[3] = (uint8_t)(u >> 24);
        if (fwrite(b, 1, 4, f) != 4) {
            fprintf(stderr, "write failed for '%s'\n", path);
            fclose(f);
            return 0;
        }
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "close failed for '%s'\n", path);
        return 0;
    }
    return 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "Usage: %s <whistle.cact> <input_16k_mono_pcm16.wav> <output.f32>\n", argv0);
    fprintf(stderr, "WAV must contain one utterance, with about 0.3-0.5 s room noise before it and >=0.6 s tail silence.\n");
}

int main(int argc, char **argv)
{
    void *model = NULL;
    size_t model_size = 0;
    int16_t *pcm = NULL;
    size_t pcm_count = 0;
    float *vad_buf = NULL, *block = NULL, *embedding = NULL;
    size_t utterance_len = 0;
    float floor_rms = 0.0f, peak = 0.0f;
    lisa_vad_t vad;
    lisa_vad_params_t vp;
    nd_whistle whistle;
    FILE *mf = NULL;
    int rc = 1;

    if (argc != 4) { usage(argv[0]); return 2; }

    mf = fopen(argv[1], "rb");
    if (!mf || fseek(mf, 0, SEEK_END) != 0) {
        fprintf(stderr, "cannot open model '%s'\n", argv[1]);
        goto done;
    }
    long msz = ftell(mf);
    if (msz <= 0 || fseek(mf, 0, SEEK_SET) != 0) {
        fprintf(stderr, "invalid model size\n");
        goto done;
    }
    model_size = (size_t)msz;
    model = malloc(model_size);
    if (!model || fread(model, 1, model_size, mf) != model_size) {
        fprintf(stderr, "cannot read model (%zu bytes)\n", model_size);
        goto done;
    }
    fclose(mf); mf = NULL;

    if (nd_whistle_open(&whistle, model, model_size) != 0) {
        fprintf(stderr, "nd_whistle_open failed: invalid/incompatible .cact model\n");
        goto done;
    }

    if (whistle.d != EMBEDDING_DIM) {
        fprintf(stderr, "model embedding width %u != expected %d\n",
                (unsigned)whistle.d, EMBEDDING_DIM);
        nd_whistle_close(&whistle);
        goto done;
    }

    if (!read_wav_pcm16_mono_16k(argv[2], &pcm, &pcm_count)) {
        nd_whistle_close(&whistle);
        goto done;
    }
    if (pcm_count < SAMPLE_RATE / 5) {
        fprintf(stderr, "WAV is too short\n");
        nd_whistle_close(&whistle);
        goto done;
    }

    vad_buf = (float *)malloc(MAX_SAMPLES * sizeof(float));
    block = (float *)malloc(LISA_VAD_WINDOW * sizeof(float));
    if (!vad_buf || !block) {
        fprintf(stderr, "out of memory for VAD buffers\n");
        nd_whistle_close(&whistle);
        goto done;
    }

    lisa_vad_default_params(&vp);
    vp.start_db = 10.0f;       /* CONFIG_LISA_VAD_START_DB default */
    vp.abs_min_rms = 0.003f;   /* CONFIG_LISA_VAD_ABS_MIN_RMS_MILLI default */
    vp.hangover_ms = 600;      /* CONFIG_LISA_VAD_HANGOVER_MS default */
    vp.preroll_ms = 300;       /* CONFIG_LISA_VAD_PREROLL_MS default */
    lisa_vad_init(&vad, &vp, vad_buf, MAX_SAMPLES);

    /* Same DC blocker as mic_task. I2S 24-bit data is left-aligned in 32-bit
     * slots and divided by 2^31; PCM16 is divided by 2^15, both yielding the
     * same normalised full-scale range [-1, 1]. */
    float prev_in = 0.0f, prev_out = 0.0f;
    for (size_t pos = 0; pos < pcm_count; ) {
        size_t n = pcm_count - pos;
        if (n > LISA_VAD_WINDOW) n = LISA_VAD_WINDOW;
        for (size_t i = 0; i < n; i++) {
            float in = (float)pcm[pos + i] * (1.0f / 32768.0f);
            float out = in - prev_in + 0.995f * prev_out;
            prev_in = in;
            prev_out = out;
            block[i] = out;
            float a = fabsf(out);
            if (a > peak) peak = a;
        }
        if (lisa_vad_feed(&vad, block, n)) {
            utterance_len = vad.len;
            floor_rms = vad.floor_rms;
            break;
        }
        pos += n;
    }

    if (!utterance_len) {
        fprintf(stderr, "VAD did not complete an utterance. Ensure >=100 ms speech and >=600 ms trailing silence.\n");
        nd_whistle_close(&whistle);
        goto done;
    }

    uint32_t s0 = 0, s1 = 0;
    if (!nd_whistle_find_speech(vad.buf, (uint32_t)utterance_len, floor_rms,
                                MIN_SPEECH_DB, SPEECH_MARGIN_MS, &s0, &s1)) {
        fprintf(stderr, "Whistle speech gate rejected the VAD utterance\n");
        nd_whistle_close(&whistle);
        goto done;
    }

    uint32_t speech_samples = s1 - s0;
    uint32_t frames = nd_whistle_frames(&whistle, speech_samples);
    if (!frames || frames > MAX_FRAMES_LIMIT) {
        fprintf(stderr, "invalid encoder frame count %u (limit %u)\n",
                (unsigned)frames, (unsigned)MAX_FRAMES_LIMIT);
        nd_whistle_close(&whistle);
        goto done;
    }

    embedding = (float *)malloc((size_t)frames * EMBEDDING_DIM * sizeof(float));
    if (!embedding) {
        fprintf(stderr, "out of memory for %u x %d embedding\n", frames, EMBEDDING_DIM);
        nd_whistle_close(&whistle);
        goto done;
    }

    if (nd_whistle_encode(&whistle, vad.buf + s0, speech_samples, embedding) != 0) {
        fprintf(stderr, "nd_whistle_encode failed\n");
        nd_whistle_close(&whistle);
        goto done;
    }

    for (size_t i = 0; i < (size_t)frames * EMBEDDING_DIM; i++) {
        if (!isfinite(embedding[i])) {
            fprintf(stderr, "encoder produced NaN/Inf at element %zu\n", i);
            nd_whistle_close(&whistle);
            goto done;
        }
    }

    if (!write_f32_le(argv[3], embedding, (size_t)frames * EMBEDDING_DIM)) {
        nd_whistle_close(&whistle);
        goto done;
    }

    printf("OK: %u frames x %d = %zu float32 values, %zu bytes\n",
           (unsigned)frames, EMBEDDING_DIM,
           (size_t)frames * EMBEDDING_DIM,
           (size_t)frames * EMBEDDING_DIM * sizeof(float));
    printf("VAD utterance %.3f s; speech gate %.3f s; floor %.6f; peak %.6f\n",
           (double)utterance_len / SAMPLE_RATE,
           (double)speech_samples / SAMPLE_RATE,
           floor_rms, peak);
    rc = 0;
    nd_whistle_close(&whistle);

done:
    if (mf) fclose(mf);
    free(model);
    free(pcm);
    free(vad_buf);
    free(block);
    free(embedding);
    return rc;
}
