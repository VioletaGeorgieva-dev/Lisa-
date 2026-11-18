#include "command_state.h"
#include <string.h>

void lisa_fsm_init(lisa_fsm_t* fsm, int total_stories, uint64_t now_ms) {
    if (!fsm) return;
    fsm->state = LISA_STATE_IDLE;
    fsm->last_activity_ms = now_ms;
    fsm->current_story_idx = 0;
    fsm->total_stories = (total_stories > 0) ? total_stories : 1;
    fsm->saved_position_bytes = 0;
}

static bool is_cmd(const char* cmd, const char* base) {
    if (!cmd || !base) return false;
    if (strcmp(cmd, base) == 0) return true;
    size_t blen = strlen(base);
    if (strncmp(cmd, base, blen) == 0 && strcmp(cmd + blen, "_kids") == 0) {
        return true;
    }
    return false;
}

lisa_action_t lisa_fsm_process_command(lisa_fsm_t* fsm, const char* command, uint64_t now_ms) {
    if (!fsm || !command || strcmp(command, "REJECT") == 0) {
        return LISA_ACTION_NONE;
    }

    switch (fsm->state) {
        case LISA_STATE_IDLE:
        case LISA_STATE_SLEEP:
            if (is_cmd(command, "hey_lisa")) {
                fsm->state = LISA_STATE_QUESTION;
                fsm->last_activity_ms = now_ms;
                return LISA_ACTION_ASK_STORY;
            }
            return LISA_ACTION_NONE;

       case LISA_STATE_QUESTION:
    if (is_cmd(command, "da")) {
        fsm->state = LISA_STATE_PLAYING;
        fsm->saved_position_bytes = 0;
        fsm->last_activity_ms = now_ms;
        return LISA_ACTION_START_STORY;
    }

    if (is_cmd(command, "ne")) {
        fsm->current_story_idx++;

        if (fsm->current_story_idx >= fsm->total_stories) {
            fsm->current_story_idx = 0;
        }

        fsm->last_activity_ms = now_ms;
        return LISA_ACTION_REPEAT_QUESTION;
    }

    if (is_cmd(command, "zaspi")) {
        fsm->state = LISA_STATE_SLEEP;
        fsm->last_activity_ms = now_ms;
        return LISA_ACTION_GO_SLEEP;
    }

    return LISA_ACTION_NONE;
        case LISA_STATE_PLAYING:
            if (is_cmd(command, "pauziraj")) {
                fsm->state = LISA_STATE_PAUSED;
                fsm->last_activity_ms = now_ms;
                return LISA_ACTION_PAUSE_STORY;
            }
            if (is_cmd(command, "spri")) {
                fsm->state = LISA_STATE_IDLE;
                fsm->saved_position_bytes = 0;
                fsm->last_activity_ms = now_ms;
                return LISA_ACTION_STOP_STORY;
            }
            if (is_cmd(command, "predishna")) {
                fsm->current_story_idx = (fsm->current_story_idx - 1 + fsm->total_stories) % fsm->total_stories;
                fsm->saved_position_bytes = 0;
                fsm->last_activity_ms = now_ms;
                return LISA_ACTION_PREV_STORY;
            }
            if (is_cmd(command, "sledvashta")) {
                fsm->current_story_idx = (fsm->current_story_idx + 1) % fsm->total_stories;
                fsm->saved_position_bytes = 0;
                fsm->last_activity_ms = now_ms;
                return LISA_ACTION_NEXT_STORY;
            }
            if (is_cmd(command, "zaspi")) {
                fsm->state = LISA_STATE_SLEEP;
                fsm->saved_position_bytes = 0;
                fsm->last_activity_ms = now_ms;
                return LISA_ACTION_GO_SLEEP;
            }
            return LISA_ACTION_NONE;

        case LISA_STATE_PAUSED:
            if (is_cmd(command, "napred")) {
                fsm->state = LISA_STATE_PLAYING;
                fsm->last_activity_ms = now_ms;
                return LISA_ACTION_RESUME_STORY;
            }
            if (is_cmd(command, "spri")) {
                fsm->state = LISA_STATE_IDLE;
                fsm->saved_position_bytes = 0;
                fsm->last_activity_ms = now_ms;
                return LISA_ACTION_STOP_STORY;
            }
            if (is_cmd(command, "zaspi")) {
                fsm->state = LISA_STATE_SLEEP;
                fsm->saved_position_bytes = 0;
                fsm->last_activity_ms = now_ms;
                return LISA_ACTION_GO_SLEEP;
            }
            return LISA_ACTION_NONE;
    }

    return LISA_ACTION_NONE;
}

lisa_action_t lisa_fsm_tick(lisa_fsm_t* fsm, uint64_t now_ms) {
    if (!fsm) return LISA_ACTION_NONE;
    if (fsm->state != LISA_STATE_SLEEP && fsm->state != LISA_STATE_IDLE) {
        if (now_ms >= fsm->last_activity_ms && (now_ms - fsm->last_activity_ms) >= LISA_INACTIVITY_TIMEOUT_MS) {
            fsm->state = LISA_STATE_SLEEP;
            fsm->saved_position_bytes = 0;
            return LISA_ACTION_GO_SLEEP;
        }
    }
    return LISA_ACTION_NONE;
}

const char* lisa_state_to_str(lisa_state_t state) {
    switch (state) {
        case LISA_STATE_IDLE:     return "IDLE";
        case LISA_STATE_QUESTION: return "QUESTION";
        case LISA_STATE_PLAYING:  return "PLAYING";
        case LISA_STATE_PAUSED:   return "PAUSED";
        case LISA_STATE_SLEEP:    return "SLEEP";
        default:                  return "UNKNOWN";
    }
}

const char* lisa_action_to_str(lisa_action_t action) {
    switch (action) {
        case LISA_ACTION_NONE:            return "NONE";
        case LISA_ACTION_ASK_STORY:       return "ASK_STORY";
        case LISA_ACTION_START_STORY:     return "START_STORY";
        case LISA_ACTION_REPEAT_QUESTION: return "REPEAT_QUESTION";
        case LISA_ACTION_PAUSE_STORY:     return "PAUSE_STORY";
        case LISA_ACTION_RESUME_STORY:    return "RESUME_STORY";
        case LISA_ACTION_STOP_STORY:      return "STOP_STORY";
        case LISA_ACTION_PREV_STORY:      return "PREV_STORY";
        case LISA_ACTION_NEXT_STORY:      return "NEXT_STORY";
        case LISA_ACTION_GO_SLEEP:        return "GO_SLEEP";
        default:                          return "UNKNOWN";
    }
}


bool lisa_fsm_story_finished(lisa_fsm_t* fsm, uint64_t now_ms) {
    if (!fsm || fsm->state != LISA_STATE_PLAYING) return false;
    fsm->state = LISA_STATE_IDLE;
    fsm->saved_position_bytes = 0;
    fsm->last_activity_ms = now_ms;
    return true;
}
