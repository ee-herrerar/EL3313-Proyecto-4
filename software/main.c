#include "xparameters.h"
#include "xil_printf.h"
#include "xstatus.h"
#include "xtmrctr.h"
#include "xtmrctr_l.h"
#include "xil_types.h"
#include "xil_assert.h"

static XTmrCtr Timer;

// Prototipos de funciones
void inicializar_sistema(void);
int inicializar_timer(void);

int main(void)
{
    xil_printf("Iniciando sistema...\r\n"); //Print

    inicializar_sistema(); //Llamar inicializar sistema

    while (1)
    {
    }

    return 0;
}

void inicializar_sistema(void) //Iniciar todo el sistema
{
    if (inicializar_timer() != XST_SUCCESS) //Si iniciar el timer fue exitoso
    {
        xil_printf("ERROR: AXI Timer\r\n");
        return;
    }

    xil_printf("AXI Timer inicializado correctamente\r\n");
}

int inicializar_timer(void) //Iniciar timer
{
    int estado;
    estado = XTmrCtr_Initialize( //Se intenta realizar la inicializacion con el address de timer y el periferico indicado
        &Timer,
        XPAR_XTMRCTR_0_BASEADDR
    );

    if (estado != XST_SUCCESS)
    {
        return estado;
    }
    
    XTmrCtr_SetOptions(
        &Timer, 
        0, 
        XTC_AUTO_RELOAD_OPTION | XTC_DOWN_COUNT_OPTION);

    XTmrCtr_SetResetValue(&Timer, 0, 99999);

    XTmrCtr_Start(&Timer, 0);

    u32 lectura1;
    u32 lectura2;

    lectura1 = XTmrCtr_GetValue(&Timer, 0);

    for (volatile int i = 0; i < 1000; i++)
    {
        // Pequeña espera para que avance el contador
    }

    lectura2 = XTmrCtr_GetValue(&Timer, 0);

    xil_printf("Lectura 1: %u\r\n", (unsigned int)lectura1);
    xil_printf("Lectura 2: %u\r\n", (unsigned int)lectura2);

    return XST_SUCCESS;
}
