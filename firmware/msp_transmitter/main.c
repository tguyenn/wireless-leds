#include "ti_msp_dl_config.h"
#include <stdio.h>

extern void main_radio(void);

int main(void)
{
    SYSCFG_DL_init();
    //main_radio();
    for (;;) {
                for (const char *p = "Hello World!\r\n"; *p; ++p) {
            DL_UART_Main_transmitDataBlocking(UART_0_INST, (uint8_t)*p);
        }
        delay_cycles(CPUCLK_FREQ);
    }

}
