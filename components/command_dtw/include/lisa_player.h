
#ifndef LISA_PLAYER_H
#define LISA_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void lisa_player_set_base_dir(const char* base_dir);

bool lisa_player_start_story(int story_idx, uint32_t start_position_bytes);

bool lisa_player_start_question(int story_idx, uint32_t start_position_bytes);

bool lisa_player_start_prompt(uint32_t start_position_bytes);

bool lisa_player_pause(uint32_t* out_position_bytes);

bool lisa_player_resume(uint32_t saved_position_bytes);

bool lisa_player_stop(void);

bool lisa_player_is_playing(void);

bool lisa_player_has_finished(void);

uint32_t lisa_player_get_position_bytes(void);

bool lisa_player_process_audio_chunk(void);

#ifdef __cplusplus
}
#endif

#endif
