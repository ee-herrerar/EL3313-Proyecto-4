#ifndef BUFFER_H
#define BUFFER_H
#include "xil_types.h"

#define N_BUF 1536

typedef struct {
    s16 i_mA;
} muestra_t;

void buffer_reiniciar(void);
void buffer_guardar(s16 i_mA, s16 ref_mA);   /* llamar desde la ISR */
u32  buffer_cantidad(void);
int  buffer_lleno(void);
const muestra_t *buffer_leer(u32 i);         /* 0 = la más antigua */
void buffer_congelar(u8 congelar);

#endif