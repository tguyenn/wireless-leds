#include "ti_msp_dl_config.h"
#include <stdio.h>

extern void main_radio(void);
extern void main_packet32(void);

int main(void)
{
    SYSCFG_DL_init();
    // Uncomment ONE radio demo below (neither returns).
    //main_radio();     // one-character packets, "HELLO" loop
    //main_packet32();  // single fixed-length 32-byte packet TX -> RX test
    for (;;) {
                for (const char *p = "Hello World!\r\n"; *p; ++p) {
            DL_UART_Main_transmitDataBlocking(UART_0_INST, (uint8_t)*p);
        }
        delay_cycles(CPUCLK_FREQ);
    }

}
