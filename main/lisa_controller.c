
#include "lisa_controller.h"
#include "lisa_player.h"
#include "lisa_hardware.h"

#include <stdio.h>

#ifdef ESP_PLATFORM
#include <esp_log.h>
static const char* TAG = "lisa_ctrl";
#else
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "lisa_ctrl";
#endif

/* After the prompt ends, keep the microphone muted a little longer so the
 * tail of the prompt in the room is not heard as a command. */
#define LISA_MIC_HOLD_AFTER_PROMPT_MS 400

/* The I2S transmitter repeats its last DMA buffer when it runs dry, which is
 * an audible buzz after a pause or at the end of a story. Writing a short
 * burst of zeros through the normal speaker path flushes it. */
static void speaker_silence(void)
{
    static const int16_t zeros[256] = {0};

    for (int i = 0; i < 10; i++) {
        size_t written = 0;

        if (!lisa_hw_write_speaker(zeros, 256, &written, 50)) {
            break;
        }
    }
}

static void publish_state(lisa_controller_t* c)
{
    c->shared_state = c->fsm.state;
}

void lisa_ctrl_init(lisa_controller_t* c, int total_stories, uint64_t now_ms)
{
    if (!c) {
        return;
    }

    lisa_fsm_init(&c->fsm, total_stories, now_ms);
    c->kind = LISA_PLAY_NONE;
    c->mic_hold_until_ms = 0;
    c->question_deadline_ms = 0;

    publish_state(c);
}

static bool start_question_audio(lisa_controller_t* c)
{
    /* Reset the previous answer timeout. */
    c->question_deadline_ms = 0;

    /* Per-story question first; the generic prompt as a fallback. */
    if (lisa_player_start_question(c->fsm.current_story_idx, 0) ||
        lisa_player_start_prompt(0)) {
        c->kind = LISA_PLAY_QUESTION;
        return true;
    }

    ESP_LOGW(TAG, "no question audio for story %d", c->fsm.current_story_idx);
    c->kind = LISA_PLAY_NONE;
    return false;
}

static void story_failed(lisa_controller_t* c, uint64_t now_ms)
{
    ESP_LOGW(TAG, "story %d cannot be played, back to IDLE", c->fsm.current_story_idx);
    c->kind = LISA_PLAY_NONE;
    lisa_player_stop();
    lisa_fsm_story_finished(&c->fsm, now_ms);
}

static void execute_action(lisa_controller_t* c, lisa_action_t action, uint64_t now_ms)
{
    switch (action) {

    case LISA_ACTION_ASK_STORY:
    case LISA_ACTION_REPEAT_QUESTION:
        lisa_player_stop();
        start_question_audio(c);
        break;

    case LISA_ACTION_START_STORY:
    case LISA_ACTION_PREV_STORY:
    case LISA_ACTION_NEXT_STORY:
        lisa_player_stop();

        if (lisa_player_start_story(c->fsm.current_story_idx, 0)) {
            c->kind = LISA_PLAY_STORY;
        } else {
            story_failed(c, now_ms);
        }
        break;

    case LISA_ACTION_PAUSE_STORY: {
        uint32_t pos = 0;

        if (lisa_player_pause(&pos)) {
            c->fsm.saved_position_bytes = pos;
        } else {
            /* Nothing was playing any more (e.g. it had just ended). */
            c->fsm.saved_position_bytes = 0;
        }

        speaker_silence();
        break;
    }

    case LISA_ACTION_RESUME_STORY:
        if (lisa_player_resume(c->fsm.saved_position_bytes)) {
            c->kind = LISA_PLAY_STORY;
        } else {
            story_failed(c, now_ms);
        }
        break;

    case LISA_ACTION_STOP_STORY:
    case LISA_ACTION_GO_SLEEP:
        lisa_player_stop();
        c->kind = LISA_PLAY_NONE;
        speaker_silence();
        break;

    case LISA_ACTION_NONE:
    default:
        break;
    }
}

lisa_action_t lisa_ctrl_on_command(lisa_controller_t* c, const char* command, uint64_t now_ms)
{
    if (!c) {
        return LISA_ACTION_NONE;
    }

    lisa_action_t action = lisa_fsm_process_command(&c->fsm, command, now_ms);

    /* If the FSM has left QUESTION, there is no longer an active
     * question-answer timeout. */
    if (c->fsm.state != LISA_STATE_QUESTION) {
        c->question_deadline_ms = 0;
    }

    if (action != LISA_ACTION_NONE) {
        ESP_LOGI(TAG, "%s -> %s (state %s)", command,
                 lisa_action_to_str(action),
                 lisa_state_to_str(c->fsm.state));
    }

    execute_action(c, action, now_ms);
    publish_state(c);

    return action;
}

bool lisa_ctrl_poll(lisa_controller_t* c, uint64_t now_ms)
{
    if (!c) {
        return false;
    }

    bool wrote = false;

    if (lisa_player_is_playing()) {
        wrote = true;
        lisa_player_process_audio_chunk();
    }

    if (!lisa_player_is_playing()) {

        if (wrote) {
            speaker_silence();   /* playback has just ended */
        }

        if (c->kind == LISA_PLAY_QUESTION) {
            /* The prompt ended. The FSM stays in QUESTION and waits for
             * da / ne / zaspi. */
            c->kind = LISA_PLAY_NONE;

            c->mic_hold_until_ms =
                (uint32_t)now_ms + LISA_MIC_HOLD_AFTER_PROMPT_MS;

            /* Wait 400 ms for the speaker tail, then allow 7 seconds
             * for the child to answer. */
            c->question_deadline_ms =
                now_ms + LISA_MIC_HOLD_AFTER_PROMPT_MS + 7000;

        } else if (c->kind == LISA_PLAY_STORY &&
                   c->fsm.state == LISA_STATE_PLAYING) {

            /* The story ended by itself (or the SD card failed mid-way):
             * go back to IDLE. A paused story is not "ended". */
            c->kind = LISA_PLAY_NONE;
            lisa_fsm_story_finished(&c->fsm, now_ms);

            ESP_LOGI(TAG, "story finished, IDLE");
        }
    }

    /* Repeat the question if no answer was received within 7 seconds. */
    if (c->fsm.state == LISA_STATE_QUESTION &&
        c->kind == LISA_PLAY_NONE &&
        c->question_deadline_ms != 0 &&
        now_ms >= c->question_deadline_ms) {

        c->question_deadline_ms = 0;

        execute_action(c, LISA_ACTION_REPEAT_QUESTION, now_ms);
    }

    /* A long story must not trip the 15-minute inactivity timer: that timer
     * is meant for a paused or unanswered session. */
    if (lisa_player_is_playing() && c->kind == LISA_PLAY_STORY) {
        c->fsm.last_activity_ms = now_ms;
    }

    lisa_action_t tick = lisa_fsm_tick(&c->fsm, now_ms);

    if (tick != LISA_ACTION_NONE) {
        execute_action(c, tick, now_ms);
    }

    publish_state(c);

    return wrote;
}

lisa_state_t lisa_ctrl_get_state(const lisa_controller_t* c)
{
    return c ? c->shared_state : LISA_STATE_IDLE;
}

bool lisa_ctrl_listening(const lisa_controller_t* c, uint64_t now_ms)
{
    if (!c) {
        return false;
    }

    if (c->kind == LISA_PLAY_QUESTION) {
        return false;
    }

    /* Wrap-safe comparison of 32-bit millisecond counters. */
    return (int32_t)((uint32_t)now_ms - c->mic_hold_until_ms) >= 0;
}
