/*
 * xadc.h
 * Lectura de la corriente de carga (ACS712 + acondicionador) con el XADC.
 */

#ifndef XADC_H
#define XADC_H

#include "xil_types.h"

/* Inicializa el driver del XADC. Devuelve 0 si todo salió bien. */
int xadc_init(void);

/* Lee el canal VAUX6. Devuelve el código de 12 bits (0 a 4095 = 0 a 1 V). */
u16 xadc_leer_codigo(void);

/* Mide el código a corriente cero (PWM apagado) y lo guarda como offset.
 * Promedia n muestras. Devuelve el offset obtenido. */
u16 xadc_calibrar_offset(u32 n);

/* Convierte un código a miliamperios usando el offset calibrado. */
s32 xadc_a_miliamperios(u16 codigo);

#endif /* XADC_H */
