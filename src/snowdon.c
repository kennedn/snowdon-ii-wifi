#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "nec.pio.h"
#include "snowdon.h"
#include "tcp.h"

uint32_t rgb_mask = 1 << 17 | 1 << 18 | 1 << 19;
uint32_t rgb_base_pin = 17;

int main() {
    stdio_init_all();

    gpio_init_mask(rgb_mask);

    uint nec_offset = pio_add_program(PIO_INSTANCE, &nec_program);
    nec_program_init(PIO_INSTANCE, 0 , nec_offset, IR_PIN);
    run_tcp_server();
    return 0;
}