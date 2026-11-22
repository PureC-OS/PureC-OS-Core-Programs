#pragma once
#include <stdint.h>
#include <stdbool.h>

struct mouse_state
{
    int32_t x, y;
    int32_t dx, dy;
    uint8_t buttons;
    bool has_data;
    int32_t wheel;
};

struct mouse_debug_state
{
    uint32_t irq_count;
    uint32_t poll_count;
    uint32_t packet_count;
    uint8_t controller_status;
    uint8_t last_byte;
    uint8_t reset_ack;
    uint8_t enable_ack;
    bool initialized;
    bool enabled;
    bool interrupts_enabled;
};
