
#include "xadc.h"
#include "xparameters.h"
#include "xsysmon.h"
#include "xstatus.h"
#include "sleep.h"

/* ---- Parámetros del hardware ---------------- */

#ifdef XPAR_XADC_WIZ_0_DEVICE_ID
#define XADC_ID  XPAR_XADC_WIZ_0_DEVICE_ID
#else
#define XADC_ID  XPAR_XADC_WIZ_0_BASEADDR
#endif

#define XADC_CANAL_CORRIENTE   (XSM_CH_AUX_MIN + 6)   /* VAUX6 */

#define XADC_SENS_MV_POR_A     185   /* ACS712 de 5 A: 185 mV por amperio       */
#define XADC_GANANCIA          1     /* ganancia del acondicionador (1 = x1)    */

/* Offset provisional: 0,5 V a corriente cero = 2048 de 4096. */
#define XADC_OFFSET_INICIAL    2048

/* mA = diff * 1 000 000 / (4096 * sensibilidad * ganancia) */
#define XADC_FACTOR_Q16 \
    ((s32)((1000000LL * 65536LL) / (4096LL * XADC_SENS_MV_POR_A * XADC_GANANCIA)))

/* ---- Estado interno -------------------------------------------------------- */

static XSysMon Sysmon;
static u16 offset_codigo = XADC_OFFSET_INICIAL;

/* ---- Funciones públicas ---------------------------------------------------- */

int xadc_init(void)
{
    XSysMon_Config *cfg = XSysMon_LookupConfig(XADC_ID);
    if (cfg == NULL) {
        return -1;
    }
    if (XSysMon_CfgInitialize(&Sysmon, cfg, cfg->BaseAddress) != XST_SUCCESS) {
        return -1;
    }
    return 0;
}

u16 xadc_leer_codigo(void)
{
    /* El dato viene en 16 bits; los 12 altos son la muestra. */
    return (u16)(XSysMon_GetAdcData(&Sysmon, XADC_CANAL_CORRIENTE) >> 4);
}

u16 xadc_calibrar_offset(u32 n)
{
    u32 suma = 0;

    if (n == 0) {
        return offset_codigo;
    }
    for (u32 i = 0; i < n; i++) {
        suma += xadc_leer_codigo();
        usleep(1000);   /* espera para promediar muestras independientes */
    }
    offset_codigo = (u16)(suma / n);
    return offset_codigo;
}

s32 xadc_a_miliamperios(u16 codigo)
{
    s32 diff = (s32)codigo - (s32)offset_codigo;
    /* |diff| <= 4095 y el factor es ~86 000, así que cabe en 32 bits. */
    return (diff * XADC_FACTOR_Q16) >> 16;
}
