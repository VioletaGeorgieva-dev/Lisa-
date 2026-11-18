#define _DEFAULT_SOURCE

#include "nd_command_dtw.h"
#include "command_dictionary.h"
#include "command_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define PATH_SIZE 4096

static int tested, positive_total, positive_ok;
static int negative_total, negative_ok, errors;

static const char* expected_class(const char* folder, lisa_state_t* state)
{
    *state = LISA_STATE_IDLE;

    if (!strcmp(folder, "negative_speech_42")) {
        *state = LISA_STATE_QUESTION;
        return NULL;
    }

    if (!strcmp(folder, "hey_lisa_kids"))
        return "hey_lisa_kids";

    if (!strcmp(folder, "da_kids")) {
        *state = LISA_STATE_QUESTION;
        return "da_kids";
    }

    if (!strcmp(folder, "ne_kids")) {
        *state = LISA_STATE_QUESTION;
        return "ne_kids";
    }

    if (!strcmp(folder, "napred_kids")) {
        *state = LISA_STATE_PAUSED;
        return "napred_kids";
    }

    if (!strcmp(folder, "pauziraj_kids") ||
        !strcmp(folder, "predishna_kids") ||
        !strcmp(folder, "sledvashta_kids") ||
        !strcmp(folder, "spri_kids") ||
        !strcmp(folder, "zaspi_kids")) {
        *state = LISA_STATE_PLAYING;
        return folder;
    }

    return NULL;
}

static int has_f32_extension(const char* name)
{
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".f32") == 0;
}

static void test_file(
    const char* path,
    const char* expected,
    lisa_state_t state,
    const dtw_dictionary_t* dict)
{
    FILE* f = fopen(path, "rb");

    if (!f) {
        printf("FILE_ERROR: %s\n", path);
        errors++;
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        errors++;
        return;
    }

    long bytes = ftell(f);
    rewind(f);

    if (bytes <= 0 ||
        bytes % (EMBEDDING_DIM * (long)sizeof(float)) != 0) {
        printf("INVALID_SIZE: %s (%ld bytes)\n", path, bytes);
        fclose(f);
        errors++;
        return;
    }

    int frames = (int)(bytes / (EMBEDDING_DIM * sizeof(float)));

    if (frames < 1 || frames > MAX_FRAMES_LIMIT) {
        printf("INVALID_FRAMES: %s (%d)\n", path, frames);
        fclose(f);
        errors++;
        return;
    }

    size_t count = (size_t)frames * EMBEDDING_DIM;
    float* data = malloc(count * sizeof(float));

    if (!data) {
        fclose(f);
        errors++;
        return;
    }

    if (fread(data, sizeof(float), count, f) != count) {
        printf("READ_ERROR: %s\n", path);
        free(data);
        fclose(f);
        errors++;
        return;
    }

    fclose(f);

    classification_result_t result;
    int valid = dtw_classify_state_aware(data, frames, dict, state, &result);
    free(data);

    tested++;

    if (!valid) {
        printf("CLASSIFY_ERROR: %s\n", path);
        errors++;
        return;
    }

    int correct;

    if (expected) {
        positive_total++;
        correct = result.accepted_7 &&
                  strcmp(result.prediction_7, expected) == 0;
        if (correct) positive_ok++;
    } else {
        negative_total++;
        correct = !result.accepted_7;
        if (correct) negative_ok++;
    }

    printf(
        "%s | state=%s | expected=%s | predicted=%s"
        " | best=%s %.6f | second=%s %.6f"
        " | margin=%.6f | reason=%s | %s\n",
        path,
        lisa_state_to_str(state),
        expected ? expected : "REJECT",
        result.prediction_7,
        result.best_command_7,
        result.best_distance_7,
        result.second_command_7,
        result.second_distance_7,
        result.margin_7,
        result.reason_7,
        correct ? "PASS" : "FAIL"
    );
}

static void scan_directory(const char* root, const dtw_dictionary_t* dict)
{
    DIR* dir = opendir(root);

    if (!dir) {
        printf("DIRECTORY_ERROR: %s\n", root);
        errors++;
        return;
    }

    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") ||
            !strcmp(entry->d_name, ".."))
            continue;

        char folder_path[PATH_SIZE];

        int n = snprintf(folder_path, sizeof(folder_path),
                         "%s/%s", root, entry->d_name);

        if (n < 0 || n >= (int)sizeof(folder_path))
            continue;

        struct stat st;

        if (stat(folder_path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        lisa_state_t state;
        const char* expected = expected_class(entry->d_name, &state);

        DIR* sub = opendir(folder_path);
        if (!sub)
            continue;

        struct dirent* item;

        while ((item = readdir(sub)) != NULL) {
            if (!has_f32_extension(item->d_name))
                continue;

            char file_path[PATH_SIZE];

            n = snprintf(file_path, sizeof(file_path),
                         "%s/%s", folder_path, item->d_name);

            if (n < 0 || n >= (int)sizeof(file_path))
                continue;

            struct stat fst;

            if (stat(file_path, &fst) != 0 || !S_ISREG(fst.st_mode))
                continue;

            test_file(file_path, expected, state, dict);
        }

        closedir(sub);
    }

    closedir(dir);
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        printf("Usage: %s dictionary.wdt test_directory\n", argv[0]);
        return 1;
    }

    dtw_dictionary_t dict = {0};

    printf("Loading dictionary: %s\n", argv[1]);

    if (!wdt_dictionary_load(argv[1], &dict)) {
        printf("DICTIONARY LOAD FAILED\n");
        return 1;
    }

    printf("Dictionary loaded: %d classes\n\n", dict.num_classes);

    scan_directory(argv[2], &dict);

    printf("\n========== SUMMARY ==========\n");
    printf("Total tested:       %d\n", tested);
    printf("Positive samples:   %d\n", positive_total);
    printf("Correct positives:  %d\n", positive_ok);
    printf("Negative samples:   %d\n", negative_total);
    printf("Correct rejections: %d\n", negative_ok);
    printf("Errors:             %d\n", errors);
    printf("=============================\n");

    wdt_dictionary_free(&dict);

    return errors ? 2 :
           (positive_ok == positive_total &&
            negative_ok == negative_total) ? 0 : 3;
}
