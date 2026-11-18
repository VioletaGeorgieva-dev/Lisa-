#include "nd_command_dtw.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *names[9] = {
    "hey_lisa_kids", "napred_kids", "pauziraj_kids",
    "predishna_kids", "sledvashta_kids", "spri_kids",
    "zaspi_kids", "da_kids", "ne_kids"
};
static float frames[9][EMBEDDING_DIM];
static dtw_template_t templates[9];
static dtw_class_t classes[9];

static void init_dict(dtw_dictionary_t *d) {
    memset(frames, 0, sizeof(frames));
    memset(classes, 0, sizeof(classes));
    for (int c=0;c<9;c++) {
        frames[c][c] = 1.0f;
        templates[c].frames = frames[c];
        templates[c].num_frames = 1;
        snprintf(classes[c].name, sizeof(classes[c].name), "%s", names[c]);
        classes[c].num_templates = 1;
        classes[c].templates = &templates[c];
        classes[c].has_threshold = c < 7;
        classes[c].threshold_distance = 0.3f;
        classes[c].threshold_margin = 0.0f;
    }
    d->num_classes=9; d->classes=classes;
}

int main(void) {
    dtw_dictionary_t d; init_dict(&d);
    float input[EMBEDDING_DIM]={0};
    classification_result_t r;
    /* A clip identical to spri, but still within 0.30 of da.
       It must be rejected as unrelated speech, not accepted as da. */
    input[5] = sqrtf(0.8f); input[10] = sqrtf(0.2f);
    if (!dtw_classify_state_aware(input,1,&d,LISA_STATE_QUESTION,&r)) return 2;
    printf("unrelated: accepted=%d prediction=%s reason=%s diag=%s dist_da_ne=%.3f\n",
           r.accepted_7,r.prediction_7,r.reason_7,r.diag_best_9,r.best_distance_7);
    if (r.accepted_7 || strcmp(r.reason_7,"QUESTION_OTHER_CLASS_REJECT")) return 1;
    /* A true da template must still pass. */
    memset(input,0,sizeof(input)); input[7]=1.0f;
    if (!dtw_classify_state_aware(input,1,&d,LISA_STATE_QUESTION,&r)) return 2;
    printf("true da: accepted=%d prediction=%s reason=%s diag=%s\n",
           r.accepted_7,r.prediction_7,r.reason_7,r.diag_best_9);
    if (!r.accepted_7 || strcmp(r.prediction_7,"da_kids")) return 1;
    /* A true ne template must still pass. */
    memset(input,0,sizeof(input)); input[8]=1.0f;
    if (!dtw_classify_state_aware(input,1,&d,LISA_STATE_QUESTION,&r) ||
        !r.accepted_7 || strcmp(r.prediction_7,"ne_kids")) return 1;
    printf("true ne: accepted=%d prediction=%s reason=%s\n",
           r.accepted_7,r.prediction_7,r.reason_7);
    /* The dedicated zaspi exception must remain available in QUESTION. */
    memset(input,0,sizeof(input)); input[6]=1.0f;
    if (!dtw_classify_state_aware(input,1,&d,LISA_STATE_QUESTION,&r) ||
        !r.accepted_7 || strcmp(r.prediction_7,"zaspi_kids")) return 1;
    printf("true zaspi: accepted=%d prediction=%s reason=%s\n",
           r.accepted_7,r.prediction_7,r.reason_7);
    return 0;
}
