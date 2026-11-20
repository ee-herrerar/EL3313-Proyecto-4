#include "buffer.h"

static muestra_t datos[N_BUF];
static volatile u32 idx = 0;
static volatile u8  lleno = 0;
static volatile u8  congelado = 0;

void buffer_reiniciar(void) { idx = 0; lleno = 0; }

void buffer_congelar(u8 c) { congelado = c; }

void buffer_guardar(s16 i_mA, s16 ref_mA)
{
    (void)ref_mA;
    if (congelado) return;
    datos[idx].i_mA = i_mA;
    if (++idx >= N_BUF) { idx = 0; lleno = 1; }
}

u32 buffer_cantidad(void) { return lleno ? N_BUF : idx; }
int buffer_lleno(void)    { return lleno; }

const muestra_t *buffer_leer(u32 i)
{
    u32 inicio = lleno ? idx : 0;
    return &datos[(inicio + i) % N_BUF];
}