#include "gpio.h"
#include "xgpio_l.h"

static UINTPTR gpio_base;

void gpio_init(UINTPTR base)
{
    gpio_base = base;
    XGpio_WriteReg(gpio_base, XGPIO_TRI_OFFSET, 0xFFFFFFFFU);  /* todos entradas */
}

u32 gpio_leer_botones(void)
{
    return XGpio_ReadReg(gpio_base, XGPIO_DATA_OFFSET);
}