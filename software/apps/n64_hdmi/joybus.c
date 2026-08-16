/**
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2023 Konrad Beckmann
 */

#include "joybus.h"

static struct {
    PIO pio_instance;
    uint32_t sm_instance;
    uint32_t last_value;
    bool saw_data;
} state;

void joybus_rx_init(PIO pio_instance, uint sm_instance)
{
    state.pio_instance = pio_instance;
    state.sm_instance = sm_instance;
    state.saw_data = false;
}

uint32_t joybus_rx_get_latest(void)
{
    static uint32_t last_value = 0;

    while (!pio_sm_is_rx_fifo_empty(state.pio_instance, state.sm_instance)) {
        last_value = pio_sm_get_blocking(state.pio_instance, state.sm_instance);
        state.saw_data = true;
    }

    return last_value;
}

bool joybus_rx_saw_data(void)
{
    return state.saw_data;
}
