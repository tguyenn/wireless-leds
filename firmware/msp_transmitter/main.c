#include "FreeRTOS.h"
#include "task.h"
#include "ti_msp_dl_config.h"

extern void main_radio(void);

int main(void)
{
    SYSCFG_DL_init();
    main_radio();
    for (;;) {}
}

#if configCHECK_FOR_STACK_OVERFLOW
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void) task;
    (void) name;
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}
#endif
