/* lisa_controller - glue between the FSM (command_state) and the player
 * (lisa_player). It owns no audio mechanism of its own: every sound goes
 * through lisa_player_* -> lisa_hw_write_speaker. No FreeRTOS inside, so it
 * can be driven from a host test with a fake clock.
 *
 * Threading: all functions must be called from ONE task (the controller
 * task). lisa_ctrl_get_state() and lisa_ctrl_listening() are word-sized
 * volatile reads and may be called from other tasks. */
#ifndef LISA_CONTROLLER_H
#define LISA_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>
#include "command_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LISA_PLAY_NONE = 0,
    LISA_PLAY_QUESTION,   /* the "Искаш ли приказка?" prompt */
    LISA_PLAY_STORY
} lisa_play_kind_t;

typedef struct {
    lisa_fsm_t fsm;
    volatile lisa_play_kind_t kind;
    volatile lisa_state_t shared_state;
    volatile uint32_t mic_hold_until_ms;
    uint64_t question_deadline_ms;
} lisa_controller_t;

/* total_stories <= 0 means "none found"; the FSM then still works with 1. */
void lisa_ctrl_init(lisa_controller_t* c, int total_stories, uint64_t now_ms);

/* A recognised command (already accepted by the classifier). Runs the FSM
 * and carries out the resulting action. Returns the action taken. */
lisa_action_t lisa_ctrl_on_command(lisa_controller_t* c, const char* command, uint64_t now_ms);

/* Call in a loop. Pumps one audio chunk if something is playing, detects the
 * end of a story (-> IDLE) or of the prompt, and runs the inactivity timer.
 * Returns true if audio was written (caller should not sleep). */
bool lisa_ctrl_poll(lisa_controller_t* c, uint64_t now_ms);

lisa_state_t lisa_ctrl_get_state(const lisa_controller_t* c);

/* False while Lisa's own prompt is playing (and shortly after it), so the
 * microphone does not classify the prompt itself. */
bool lisa_ctrl_listening(const lisa_controller_t* c, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
