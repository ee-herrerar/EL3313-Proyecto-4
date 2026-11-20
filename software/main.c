#include "xparameters.h"
#include "xil_printf.h"
#include "xstatus.h"
#include "xtmrctr.h"
#include "xtmrctr_l.h"
#include "xuartlite_l.h"
#include "xil_types.h"
#include "xil_assert.h"
#include "xil_io.h"
#include "xinterrupt_wrap.h"
#include <math.h>
#include "xadc.h"
#include "pwm.h"
#include "pr.h"
#include "buffer.h"

/* ============================ CONFIGURACIÓN ============================ */

/* PWM: 0 = 10 kHz, 1 = 12,5 kHz, 2 = 20 kHz de portadora. */
#define PWM_FREQ_SEL          2
/* Periodo de control en microsegundos. */
#define PERIODO_CONTROL_US    25
#define TIEMPO_MUERTO_NS      200
#define VDC                   12.0f      /* bus DC (V) */

/* Con 0, el control no arranca. */
#define PR_CONFIGURADO        0

/* Signo del sensor de corriente: 1 normal, -1 si la corriente medida sale invertida. */
#define SIGNO_SENSOR   1

/* Si la corriente medida pasa de este valor (A), se apaga el PWM.*/
#define I_MAX_A   1.5f

#define DECIMACION_BUF   2

/* Coeficientes del PR . Normalizados. */
#define PR_B0   6.0750f
#define PR_B1   -11.9981f
#define PR_B2   5.9235f
#define PR_A0   1.0000f
#define PR_A1   -1.9997f
#define PR_A2   0.9998f

/* ====================== VALORES DERIVADOS (no tocar) ====================== */
#define MUESTRAS_50HZ       (20000U / PERIODO_CONTROL_US)      /* muestras por periodo de 50 Hz */
#define TIMER_RESET_VALUE   (PERIODO_CONTROL_US * 100U - 1U)   /* reloj de 100 MHz */

/* ================================ ESTADO ================================ */

static XTmrCtr Timer;
static volatile u32 contador_interrupciones = 0;

/* Controlador y referencia */
static pr_t pr;
static float seno[MUESTRAS_50HZ];
static volatile float amplitud_ref_A = 0.2f;
static u32 k_ref = 0;
static volatile u8 control_activo = 0;
static volatile u8 falla_sobrecorriente = 0;

/* Duración máxima de la rutina de interrupción, en ciclos de 10 ns */
static volatile u32 isr_ciclos_max = 0;
static volatile u32 lat_min = 0xFFFFFFFFU;
static volatile u32 lat_max = 0;
static u8 dec_buf = 0;

/* ======================= RUTINA DE INTERRUPCIÓN ======================= */
void atender_timer(void *referencia, u8 numero)
{
    (void)referencia;
    (void)numero;

    u32 t0 = XTmrCtr_GetValue(&Timer, 0);     
    
    u32 lat = TIMER_RESET_VALUE - t0;   
    if (lat < lat_min) lat_min = lat;
    if (lat > lat_max) lat_max = lat;

    contador_interrupciones++;

    if (control_activo)
    {
        /* 1) Medir. En modo evento, aquí habrá que esperar a que el dato esté listo. */
        s32 i_mA = SIGNO_SENSOR * xadc_a_miliamperios(xadc_leer_codigo());
        float i_A = (float)i_mA * 0.001f;
        
        if (i_A > I_MAX_A || i_A < -I_MAX_A)
        {
            control_activo = 0;
            pwm_deshabilitar();
            pwm_set_ref(0);
            falla_sobrecorriente = 1;
        }
        else
        {
            /* 2) Referencia senoidal de 50 Hz desde la tabla */
            float r_A = amplitud_ref_A * seno[k_ref];
            if (++k_ref >= MUESTRAS_50HZ) k_ref = 0;
            if (++dec_buf >= DECIMACION_BUF)
            {
                dec_buf = 0;
                buffer_guardar((s16)i_mA, (s16)(r_A * 1000.0f));
            }
            /* 3) Controlador PR -> tensión pedida (V) */
            float u_V = pr_actualizar(&pr, r_A, i_A);
            /* 4) Al PWM: m = Vab / Vdc entre -1 y +1 */
            pwm_escribir_duty(u_V / VDC);
        }

    }

    /* Medición del tiempo de ejecución de esta rutina */
    u32 t1 = XTmrCtr_GetValue(&Timer, 0);
    u32 dur = (t0 >= t1) ? (t0 - t1) : (t0 + TIMER_RESET_VALUE + 1U - t1);
    if (dur > isr_ciclos_max) isr_ciclos_max = dur;
}

/* ============================= CONTROL ============================= */

static void control_preparar(void)
{
    /* cos y sin de 2*pi/400; válido solo para MUESTRAS_50HZ = 400 */
    const float cw = 0.99996916f;
    const float sw = 0.00785390f;
    float s = 0.0f, c = 1.0f;

    for (u32 k = 0; k < MUESTRAS_50HZ; k++)
    {
        seno[k] = s;
        float s_nuevo = s * cw + c * sw;
        c = c * cw - s * sw;
        s = s_nuevo;
    }

    pr_init(&pr, PR_B0, PR_B1, PR_B2, PR_A0, PR_A1, PR_A2, PWM_M_MAX * VDC);
}

static int control_iniciar(void)
{
#if !PR_CONFIGURADO
    xil_printf("No se inicia: faltan los coeficientes reales del PR (PR_CONFIGURADO = 0)\r\n");
    return -1;
#else
    control_activo = 0;
    pwm_deshabilitar();
    pwm_set_ref(0);

    /* PWM apagado = corriente cero: se calibra el offset del sensor */
    u16 off = xadc_calibrar_offset(200);
    xil_printf("Offset calibrado: %u\r\n", (unsigned int)off);

    pr_reiniciar(&pr);
    k_ref = 0;
    buffer_reiniciar();
    isr_ciclos_max = 0;

    lat_min = 0xFFFFFFFFU;
    lat_max = 0;

    pwm_habilitar();
    control_activo = 1;
    xil_printf("Control iniciado\r\n");
    return 0;
#endif
}

static void control_detener(void)
{
    control_activo = 0;
    pwm_deshabilitar();
    pwm_set_ref(0);
}

/* ===================== EXPORTACIÓN POR UART ===================== */
/* Congela el buffer mientras se envía; el lazo de control sigue corriendo. */

static void exportar_muestras(void)
{
    buffer_congelar(1);
    u32 n = buffer_cantidad();
    xil_printf("# INICIO muestras=%u Ts_us=%u unidad=mA ref_pico_mA=%d\r\n",
               (unsigned)n, (unsigned)PERIODO_CONTROL_US,
               (int)(amplitud_ref_A * 1000.0f));
    xil_printf("# i_mA\r\n");
    for (u32 k = 0; k < n; k++) {
        const muestra_t *m = buffer_leer(k);
        xil_printf("%d\r\n", (int)m->i_mA);
    }
    xil_printf("# FIN\r\n");
    buffer_congelar(0);
}
/* ============================= TECLADO ============================= */
static void procesar_teclado(void)
{
    if (XUartLite_IsReceiveEmpty(STDIN_BASEADDRESS))
    {
        return;
    }

    char c = (char)XUartLite_RecvByte(STDIN_BASEADDRESS);

    switch (c)
    {
        case 's': control_iniciar(); break;
        case 'x': control_detener(); xil_printf("Control detenido\r\n"); break;
        case 'e': exportar_muestras(); break;
        case '1': amplitud_ref_A = 0.2f; xil_printf("Referencia: 0.2 A pico\r\n"); break;
        case '2': amplitud_ref_A = 0.5f; xil_printf("Referencia: 0.5 A pico\r\n"); break;
        case '3': amplitud_ref_A = 1.0f; xil_printf("Referencia: 1.0 A pico\r\n"); break;
        default: break;
    }
}

/* ========================= INICIALIZACIÓN ========================= */

static int inicializar_timer(void)
{
    int estado;

    estado = XTmrCtr_Initialize(&Timer, XPAR_XTMRCTR_0_BASEADDR);
    if (estado != XST_SUCCESS)
    {
        return estado;
    }

    estado = XSetupInterruptSystem(
        &Timer,
        (XInterruptHandler)XTmrCtr_InterruptHandler,
        Timer.Config.IntrId,
        Timer.Config.IntrParent,
        XINTERRUPT_DEFAULT_PRIORITY
    );
    if (estado != XST_SUCCESS)
    {
        return estado;
    }

    XTmrCtr_SetHandler(&Timer, atender_timer, &Timer);

    XTmrCtr_SetOptions(
        &Timer,
        0,
        XTC_AUTO_RELOAD_OPTION | XTC_DOWN_COUNT_OPTION | XTC_INT_MODE_OPTION);

    XTmrCtr_SetResetValue(&Timer, 0, TIMER_RESET_VALUE);

    XTmrCtr_Start(&Timer, 0);

    return XST_SUCCESS;
}

static int inicializar_sistema(void)
{
    if (inicializar_timer() != XST_SUCCESS)
    {
        xil_printf("ERROR: AXI Timer\r\n");
        return XST_FAILURE;
    }
    xil_printf("AXI Timer inicializado correctamente\r\n");

    if (xadc_init() != 0)
    {
        xil_printf("ERROR: XADC\r\n");
        return XST_FAILURE;
    }
    xil_printf("XADC inicializado correctamente\r\n");

    pwm_init(XPAR_PWM_AXI_LITE_0_BASEADDR);
    if (pwm_configurar(PWM_FREQ_SEL, TIEMPO_MUERTO_NS) != 0)
    {
        xil_printf("ERROR: Configuracion PWM\r\n");
        return XST_FAILURE;
    }
    pwm_set_ref(0);
    xil_printf("PWM configurado (apagado)\r\n");

    control_preparar();
    return XST_SUCCESS;
}

/* ================================ MAIN ================================ */

int main(void)
{
    xil_printf("Iniciando sistema...\r\n");

    if (inicializar_sistema() != XST_SUCCESS)
    {
        return -1;
    }

    xil_printf("Teclas: s=iniciar x=detener e=exportar 1/2/3=amplitud\r\n");

    u32 ultimo = 0;

    while (1)
    {
        /* Seguridad: si el PWM reporta falla retenida, detener el control */
        u32 estado = pwm_leer_estado();
        if (control_activo && (estado & PWM_STAT_LATCHED))
        {
            control_detener();
            xil_printf("FALLA detectada: control detenido\r\n");
        }

        if (falla_sobrecorriente)
        {
            falla_sobrecorriente = 0;
            xil_printf("SOBRECORRIENTE: control detenido\r\n");
        }

        procesar_teclado();

        /* TODO: cuando exista el AXI GPIO, llamar a exportar_muestras()
         * al detectar el botón, en lugar de (o además de) la tecla 'e'. */

        /* Estado una vez por segundo (con periodo de 50 us son 20 000 interrupciones) */
        u32 actual = contador_interrupciones;
        if ((actual - ultimo) >= (1000000U / PERIODO_CONTROL_US))
        {
            ultimo = actual;
            xil_printf("Int=%u | ctrl=%d | PWM run=%d est=0x%X | ISR max=%u | lat min/max=%u/%u\r\n",
                (unsigned int)actual,
                (int)control_activo,
                pwm_esta_corriendo(),
                (unsigned int)estado,
                (unsigned int)isr_ciclos_max,
                (unsigned int)lat_min,
                (unsigned int)lat_max);
        }
    }

    return 0;
}