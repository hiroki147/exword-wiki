#pragma once
#include <stdint.h>
#include <sh4a/input/exword_keys.h>
void keypad_read(void);
uint16_t get_key_pressed(uint8_t key_code);
