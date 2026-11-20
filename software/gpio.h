#ifndef GPIO_H
#define GPIO_H
#include "xil_types.h"

#define BTN_EXPORTAR   0x1U
#define BTN_PARADA     0x2U

void gpio_init(UINTPTR base);
u32  gpio_leer_botones(void);

#endif