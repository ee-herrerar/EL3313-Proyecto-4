
#include "xparameters.h"
#include "xil_printf.h"
#include "xstatus.h"
#include "xtmrctr.h"
#include "xtmrctr_l.h"
#include "xuartlite_l.h"
#include "xil_types.h"
#include "xinterrupt_wrap.h"
#include "xil_io.h"
#include "xadc.h"
#include "pwm.h"
#include "buffer.h"
#include "gpio.h"
#include "sleep.h"
#include <stdint.h>

#define PR_CONFIGURADO 0
#define PWM_FREQ_SEL 2U
#define TIEMPO_MUERTO_NS 500U
#define PERIODO_US 25U
#define CICLOS_PERIODO 2500U
#define TIMER_RESET_VALUE 2499U
#define N_SENO 800U
#define I_MAX_MA 1500
#define SIGNO_SENSOR 1
#define MUESTRAS_ETAPA 400000U

#define B0_Q24 278304923L
#define B1_Q24 -549653375L
#define B2_Q24 271365406L
#define A1_Q24 -33549203L
#define A2_Q24 16773022L
#define UMAX_Q15 (31128L * 32768L)

typedef struct
{
    s32 e1;
    s32 e2;
    s32 u1;
    s32 u2;
} pr_fixed_t;

static pr_fixed_t controlador;
static XTmrCtr Timer;
static s16 seno_q15[N_SENO];

static volatile u32 interrupciones = 0;
static volatile u8 control_activo = 0;
static volatile u8 falla_corriente = 0;
static volatile u8 estres_activo = 0;
static volatile u8 estres_terminado = 0;
static volatile u8 estres_abortado = 0;
static volatile u8 fase_estres = 0;

static volatile u32 muestras_fase[3] = {0, 0, 0};
static volatile u32 max_ciclos[5] = {0, 0, 0, 0, 0};
static volatile u32 max_lat_y_ciclos[5] = {0, 0, 0, 0, 0};

static volatile u32 brechas_2700 = 0;
static volatile u32 brechas_3750 = 0;
static volatile u32 intervalo_min = 0xFFFFFFFFU;
static volatile u32 intervalo_max = 0;
static volatile u32 fallas_pos = 0;
static volatile u32 fallas_neg = 0;
static volatile u32 muestras_totales = 0;

static u32 entrada_anterior = 0;
static u32 k_ref = 0;
static s32 amplitud_mA = 200;

static inline u32 leer_mcycle(void)
{
    u32 v;

    __asm__ volatile (
        "csrr %0, mcycle"
        : "=r"(v)
    );

    return v;
}

static inline void pr_fijo_reiniciar(void)
{
    controlador.e1 = 0;
    controlador.e2 = 0;
    controlador.u1 = 0;
    controlador.u2 = 0;
}

static inline __attribute__((always_inline)) s16
pr_fijo_actualizar(s32 error_mA)
{
    int64_t num =
          (int64_t)B0_Q24 * error_mA
        + (int64_t)B1_Q24 * controlador.e1
        + (int64_t)B2_Q24 * controlador.e2;

    int64_t acc =
          num * 32768LL
        - (int64_t)A1_Q24 * controlador.u1
        - (int64_t)A2_Q24 * controlador.u2;

    int64_t u = acc >> 24;

    if (u > UMAX_Q15)
    {
        u = UMAX_Q15;
    }

    if (u < -UMAX_Q15)
    {
        u = -UMAX_Q15;
    }

    controlador.e2 = controlador.e1;
    controlador.e1 = error_mA;
    controlador.u2 = controlador.u1;
    controlador.u1 = (s32)u;

    return (s16)(((s32)u) >> 15);
}

static void preparar_seno(void)
{
    const float cw = 0.99996916f;
    const float sw = 0.00785390f;

    float s = 0.0f;
    float c = 1.0f;

    for (u32 k = 0; k < N_SENO; ++k)
    {
        seno_q15[k] = (s16)(s * 32767.0f);

        float ns = s * cw + c * sw;

        c = c * cw - s * sw;
        s = ns;
    }

    pr_fijo_reiniciar();
}

void atender_timer(void *referencia, u8 numero)
{
    (void)referencia;
    (void)numero;

    u32 comienzo = leer_mcycle();

    u8 probando = estres_activo;
    u8 fase = fase_estres;

    u32 latencia = 0;

    interrupciones++;

    if (probando)
    {
        u32 t0 = XTmrCtr_GetValue(&Timer, 0);

        latencia = TIMER_RESET_VALUE - t0;

        if (entrada_anterior != 0U)
        {
            u32 intervalo = comienzo - entrada_anterior;

            if (intervalo < intervalo_min)
            {
                intervalo_min = intervalo;
            }

            if (intervalo > intervalo_max)
            {
                intervalo_max = intervalo;
            }

            if (intervalo > 2700U)
            {
                brechas_2700++;
            }

            if (intervalo >= 3750U)
            {
                brechas_3750++;
            }
        }

        entrada_anterior = comienzo;
    }

    if (control_activo || probando)
    {
        s32 i_mA =
            SIGNO_SENSOR *
            xadc_a_miliamperios(
                xadc_leer_codigo()
            );

        if (probando && fase < 3U)
        {
            i_mA = 0;
        }

        if (probando && fase == 3U)
        {
            i_mA = 1601;
        }

        if (probando && fase == 4U)
        {
            i_mA = -1601;
        }

        if (i_mA > I_MAX_MA || i_mA < -I_MAX_MA)
        {
            control_activo = 0;

            pwm_deshabilitar();
            pwm_set_ref(0);

            if (probando)
            {
                if (fase == 3U)
                {
                    fallas_pos++;
                }
                else if (fase == 4U)
                {
                    fallas_neg++;
                }
                else
                {
                    estres_abortado = 1U;
                    estres_activo = 0U;
                    estres_terminado = 1U;
                }
            }
            else
            {
                falla_corriente = 1U;
            }
        }
        else
        {
            s32 ref_mA =
                (amplitud_mA *
                (s32)seno_q15[k_ref]) >> 15;

            if (++k_ref >= N_SENO)
            {
                k_ref = 0;
            }

            buffer_guardar((s16)i_mA, 0);

            s16 ref_pwm =
                pr_fijo_actualizar(ref_mA - i_mA);

            pwm_set_ref(ref_pwm);
        }
    }

    if (probando)
    {
        muestras_totales++;

        if (!estres_abortado)
        {
            if (fase < 3U)
            {
                u32 n = ++muestras_fase[fase];

                if (n >= MUESTRAS_ETAPA)
                {
                    fase_estres = fase + 1U;

                    if (fase == 0U)
                    {
                        amplitud_mA = 500;
                    }

                    if (fase == 1U)
                    {
                        amplitud_mA = 1000;
                    }
                }
            }
            else if (fase == 3U)
            {
                fase_estres = 4U;
            }
            else
            {
                estres_activo = 0U;
                estres_terminado = 1U;
            }
        }

        u32 duracion = leer_mcycle() - comienzo;
        u32 total = latencia + duracion;

        if (duracion > max_ciclos[fase])
        {
            max_ciclos[fase] = duracion;
        }

        if (total > max_lat_y_ciclos[fase])
        {
            max_lat_y_ciclos[fase] = total;
        }
    }
}

static void detener(void)
{
    control_activo = 0;

    if (estres_activo)
    {
        estres_abortado = 3U;
        estres_activo = 0U;
        estres_terminado = 1U;
    }

    pwm_deshabilitar();
    pwm_set_ref(0);
}

static void cambiar_frecuencia(u8 seleccion)
{
    u32 base = XPAR_PWM_AXI_LITE_0_BASEADDR;
    u32 ctrl = Xil_In32(base + PWM_REG_CTRL);

    if (estres_activo ||
        estres_terminado ||
        control_activo ||
        (ctrl & PWM_CTRL_ENABLE) ||
        pwm_esta_corriendo())
    {
        xil_printf(
            "Cambio rechazado: detener PWM primero con x\r\n"
        );
        return;
    }

    if (pwm_configurar(seleccion, TIEMPO_MUERTO_NS) != 0)
    {
        xil_printf("ERROR al configurar frecuencia\r\n");
        return;
    }

    pwm_set_ref(0);

    ctrl = Xil_In32(base + PWM_REG_CTRL);

    xil_printf("\r\n=== CONFIGURACION PWM ===\r\n");

    if (seleccion == 0U)
    {
        xil_printf("Frecuencia seleccionada: 10 kHz\r\n");
    }
    else if (seleccion == 1U)
    {
        xil_printf("Frecuencia seleccionada: 12.5 kHz\r\n");
    }
    else
    {
        xil_printf("Frecuencia seleccionada: 20 kHz\r\n");
    }

    xil_printf(
        "CTRL=0x%08X\r\n",
        (unsigned)ctrl
    );

    xil_printf(
        "Tiempo muerto: %u ns\r\n",
        (unsigned)TIEMPO_MUERTO_NS
    );

    xil_printf(
        "PWM run=%d\r\n",
        pwm_esta_corriendo()
    );

    xil_printf("=========================\r\n");
}

static void iniciar_estres(void)
{
    if (estres_activo ||
        estres_terminado ||
        control_activo ||
        pwm_esta_corriendo())
    {
        xil_printf(
            "Prueba ocupada o PWM habilitado\r\n"
        );
        return;
    }

    pwm_deshabilitar();
    pwm_set_ref(0);

    pr_fijo_reiniciar();

    k_ref = 0;
    amplitud_mA = 200;

    buffer_congelar(0);
    buffer_reiniciar();

    for (u32 i = 0; i < 3U; ++i)
    {
        muestras_fase[i] = 0;
    }

    for (u32 i = 0; i < 5U; ++i)
    {
        max_ciclos[i] = 0;
        max_lat_y_ciclos[i] = 0;
    }

    brechas_2700 = 0;
    brechas_3750 = 0;

    intervalo_min = 0xFFFFFFFFU;
    intervalo_max = 0;

    entrada_anterior = 0;

    fallas_pos = 0;
    fallas_neg = 0;
    muestras_totales = 0;

    estres_abortado = 0;
    estres_terminado = 0;
    fase_estres = 0;

    falla_corriente = 0;

    xil_printf(
        "ESTRES PR FIJO: 10s @0.2 A, "
        "10s @0.5 A, 10s @1.0 A\r\n"
    );

    xil_printf(
        "Luego fallas simuladas +/-1601 mA; "
        "PWM apagado\r\n"
    );

    xil_printf(
        "MODO PR: corriente simulada, sin sensor fisico\r\n"
    );

    estres_activo = 1U;
}

static void mostrar_estres(void)
{
    estres_terminado = 0;

    pwm_deshabilitar();
    pwm_set_ref(0);

    xil_printf(
        "=== ESTRES PR OPTIMIZADO 40 kHz ===\r\n"
    );

    xil_printf(
        "Muestras=%u | abortado=%u\r\n",
        (unsigned)muestras_totales,
        (unsigned)estres_abortado
    );

    for (u32 i = 0; i < 3U; ++i)
    {
        xil_printf(
            "Etapa %u: n=%u | mcycle_max=%u "
            "| lat+callback=%u\r\n",
            (unsigned)(i + 1U),
            (unsigned)muestras_fase[i],
            (unsigned)max_ciclos[i],
            (unsigned)max_lat_y_ciclos[i]
        );
    }

    xil_printf(
        "Falla +: %u | ciclos=%u | total=%u\r\n",
        (unsigned)fallas_pos,
        (unsigned)max_ciclos[3],
        (unsigned)max_lat_y_ciclos[3]
    );

    xil_printf(
        "Falla -: %u | ciclos=%u | total=%u\r\n",
        (unsigned)fallas_neg,
        (unsigned)max_ciclos[4],
        (unsigned)max_lat_y_ciclos[4]
    );

    xil_printf(
        "Intervalos min/max=%u/%u "
        "| >2700=%u | >=3750=%u\r\n",
        (unsigned)intervalo_min,
        (unsigned)intervalo_max,
        (unsigned)brechas_2700,
        (unsigned)brechas_3750
    );

    xil_printf(
        "Buffer=%u (Ts=25us) | PWM run=%d\r\n",
        (unsigned)buffer_cantidad(),
        pwm_esta_corriendo()
    );

    xil_printf("=== FIN ESTRES ===\r\n");

    pr_fijo_reiniciar();
}

static void exportar_buffer(void)
{
    if (estres_activo || control_activo)
    {
        xil_printf(
            "No exportar durante control/estres\r\n"
        );
        return;
    }

    buffer_congelar(1);

    u32 n = buffer_cantidad();

    xil_printf(
        "# INICIO muestras=%u Ts_us=25 unidad=mA\r\n",
        (unsigned)n
    );

    xil_printf("# i_mA\r\n");

    for (u32 i = 0; i < n; ++i)
    {
        const muestra_t *m = buffer_leer(i);

        xil_printf(
            "%d\r\n",
            (int)m->i_mA
        );
    }

    xil_printf(
        "# FIN muestras_enviadas=%u Ts_us=25\r\n",
        (unsigned)n
    );

    buffer_congelar(0);
}

static void mostrar_diagnostico(void)
{
    if (estres_activo)
    {
        xil_printf(
            "Diagnostico no disponible durante estres\r\n"
        );
        return;
    }

    u32 base = XPAR_PWM_AXI_LITE_0_BASEADDR;

    u32 ctrl = Xil_In32(base + PWM_REG_CTRL);
    u32 ref = Xil_In32(base + PWM_REG_REF);
    u32 dead = Xil_In32(base + PWM_REG_DEAD);
    u32 stat = Xil_In32(base + PWM_REG_STAT);

    u32 freq = (ctrl >> 1U) & 0x3U;

    u32 max_total = 0;

    for (u32 i = 0; i < 3U; ++i)
    {
        if (max_lat_y_ciclos[i] > max_total)
        {
            max_total = max_lat_y_ciclos[i];
        }
    }

    xil_printf("\r\n");
    xil_printf("========================================\r\n");
    xil_printf("       DIAGNOSTICO DIGITAL EL3313\r\n");
    xil_printf("========================================\r\n");

    xil_printf("\r\n[ MICRO BLAZE V ]\r\n");
    xil_printf("Estado: EJECUTANDO\r\n");
    xil_printf("Timer configurado: 40 kHz\r\n");
    xil_printf("Periodo nominal: 25 us\r\n");

    xil_printf(
        "Interrupciones: %u\r\n",
        (unsigned)interrupciones
    );

    xil_printf("\r\n[ PWM AXI4-LITE ]\r\n");

    xil_printf(
        "CTRL: 0x%08X\r\n",
        (unsigned)ctrl
    );

    xil_printf(
        "Habilitado: %u\r\n",
        (unsigned)((ctrl & PWM_CTRL_ENABLE) ? 1U : 0U)
    );

    xil_printf(
        "Running: %u\r\n",
        (unsigned)((ctrl & PWM_CTRL_RUNNING) ? 1U : 0U)
    );

    if (freq == 0U)
    {
        xil_printf(
            "Frecuencia seleccionada: 10 kHz\r\n"
        );
    }
    else if (freq == 1U)
    {
        xil_printf(
            "Frecuencia seleccionada: 12.5 kHz\r\n"
        );
    }
    else if (freq == 2U)
    {
        xil_printf(
            "Frecuencia seleccionada: 20 kHz\r\n"
        );
    }
    else
    {
        xil_printf(
            "Frecuencia seleccionada: INVALIDA\r\n"
        );
    }

    xil_printf(
        "Tiempo muerto: %u ciclos (%u ns)\r\n",
        (unsigned)dead,
        (unsigned)(dead * 10U)
    );

    xil_printf(
        "Referencia Q15: %d\r\n",
        (int)(s16)(ref & 0xFFFFU)
    );

    xil_printf(
        "Falla enclavada: %u\r\n",
        (unsigned)((stat & PWM_STAT_LATCHED) ? 1U : 0U)
    );

    xil_printf(
        "Entrada de falla: %u\r\n",
        (unsigned)((stat & PWM_STAT_FAULT_IN) ? 1U : 0U)
    );

    xil_printf("\r\n[ CONTROLADOR PR ]\r\n");
    xil_printf("Implementacion: Q24, 50 Hz\r\n");

#if PR_CONFIGURADO
    xil_printf("Lazo cerrado: HABILITABLE\r\n");
#else
    xil_printf("Lazo cerrado: BLOQUEADO\r\n");
#endif

    xil_printf(
        "Control activo: %u\r\n",
        (unsigned)control_activo
    );

    if (muestras_totales == 0U)
    {
        xil_printf("Prueba PR: NO EJECUTADA\r\n");
    }
    else if (estres_abortado != 0U)
    {
        xil_printf("Prueba PR: ABORTADA\r\n");
    }
    else if (muestras_fase[0] == MUESTRAS_ETAPA &&
             muestras_fase[1] == MUESTRAS_ETAPA &&
             muestras_fase[2] == MUESTRAS_ETAPA &&
             fallas_pos > 0U &&
             fallas_neg > 0U)
    {
        xil_printf("Prueba PR: COMPLETADA\r\n");
    }
    else
    {
        xil_printf("Prueba PR: INCOMPLETA\r\n");
    }

    xil_printf(
        "Muestras procesadas: %u\r\n",
        (unsigned)muestras_totales
    );

    xil_printf(
        "Etapas: %u / %u / %u\r\n",
        (unsigned)muestras_fase[0],
        (unsigned)muestras_fase[1],
        (unsigned)muestras_fase[2]
    );

    if (max_total != 0U)
    {
        xil_printf(
            "Max latencia + callback: %u ciclos\r\n",
            (unsigned)max_total
        );

        xil_printf(
            "Presupuesto temporal: %u ciclos\r\n",
            (unsigned)CICLOS_PERIODO
        );

        if (intervalo_min != 0xFFFFFFFFU)
        {
            xil_printf(
                "Intervalos min/max: %u / %u ciclos\r\n",
                (unsigned)intervalo_min,
                (unsigned)intervalo_max
            );
        }
    }
    else
    {
        xil_printf("Rendimiento PR: NO MEDIDO\r\n");
    }

    xil_printf(
        "\r\n[ PROTECCIONES SIMULADAS ]\r\n"
    );

    xil_printf(
        "Sobrecorriente positiva: %u\r\n",
        (unsigned)fallas_pos
    );

    xil_printf(
        "Sobrecorriente negativa: %u\r\n",
        (unsigned)fallas_neg
    );

    xil_printf("\r\n[ ADQUISICION Y DATOS ]\r\n");
    xil_printf("XADC: INICIALIZADO\r\n");
    xil_printf("Medicion fisica: NO VALIDADA\r\n");

    xil_printf(
        "Buffer: %u / %u muestras\r\n",
        (unsigned)buffer_cantidad(),
        (unsigned)N_BUF
    );

    xil_printf("UART: OPERATIVA\r\n");

    xil_printf("\r\n========================================\r\n");
    xil_printf("          FIN DIAGNOSTICO\r\n");
    xil_printf("========================================\r\n");
}

static void procesar_uart(void)
{
    if (XUartLite_IsReceiveEmpty(STDIN_BASEADDRESS))
    {
        return;
    }

    char c =
        (char)XUartLite_RecvByte(STDIN_BASEADDRESS);

    switch (c)
    {
        case 's':
        {
#if !PR_CONFIGURADO
            xil_printf(
                "No se inicia: PR_CONFIGURADO=0\r\n"
            );
#else
            if (!estres_activo &&
                !control_activo &&
                !pwm_esta_corriendo())
            {
                pwm_deshabilitar();
                pwm_set_ref(0);

                (void)xadc_calibrar_offset(200);

                pr_fijo_reiniciar();
                k_ref = 0;

                buffer_congelar(0);
                buffer_reiniciar();

                pwm_habilitar();
                control_activo = 1U;
            }
#endif
            break;
        }

        case 'x':
        {
            detener();
            xil_printf("Detenido\r\n");
            break;
        }

        case 'a':
        {
            cambiar_frecuencia(0U);
            break;
        }

        case 'b':
        {
            cambiar_frecuencia(1U);
            break;
        }

        case 'd':
        {
            cambiar_frecuencia(2U);
            break;
        }

        case 'v':
        {
            mostrar_diagnostico();
            break;
        }

        case 'c':
        {
            if (!estres_activo &&
                !control_activo &&
                !pwm_esta_corriendo())
            {
                u16 offset =
                    xadc_calibrar_offset(200);

                xil_printf(
                    "XADC calibrado offset=%u\r\n",
                    (unsigned)offset
                );
            }
            break;
        }

        case 'i':
        {
            if (!estres_activo && !control_activo)
            {
                u16 cod = xadc_leer_codigo();

                u32 voltaje_uV =
                    ((u32)cod * 1000000U) / 4096U;

                xil_printf(
                    "cod=%u | V=%u uV | I=%d mA\r\n",
                    (unsigned)cod,
                    (unsigned)voltaje_uV,
                    (int)xadc_a_miliamperios(cod)
                );
            }
            break;
        }

        case 'e':
        {
            exportar_buffer();
            break;
        }

        case 'n':
        {
            iniciar_estres();
            break;
        }

        case 'p':
        {
            if (!estres_activo && !control_activo)
            {
                pwm_deshabilitar();
                pwm_set_ref(8000);
                pwm_habilitar();

                xil_printf(
                    "CTRL=0x%08X\r\n",
                    (unsigned)Xil_In32(
                        XPAR_PWM_AXI_LITE_0_BASEADDR
                    )
                );

                xil_printf(
                    "p: run=%d est=0x%X\r\n",
                    pwm_esta_corriendo(),
                    (unsigned)pwm_leer_estado()
                );
            }
            else
            {
                xil_printf(
                    "p ignorado: estres=%u ctrl=%u\r\n",
                    (unsigned)estres_activo,
                    (unsigned)control_activo
                );
            }
            break;
        }

        case '1':
        {
            if (!estres_activo)
            {
                amplitud_mA = 200;
            }
            break;
        }

        case '2':
        {
            if (!estres_activo)
            {
                amplitud_mA = 500;
            }
            break;
        }

        case '3':
        {
            if (!estres_activo)
            {
                amplitud_mA = 1000;
            }
            break;
        }

        case 'm':
        {
            if (!estres_activo && !control_activo)
            {
                u32 suma = 0;
                u16 mn = 0xFFFF;
                u16 mx = 0;

                for (u32 k = 0; k < 1000; ++k)
                {
                    u16 cod = xadc_leer_codigo();

                    suma += cod;

                    if (cod < mn)
                    {
                        mn = cod;
                    }

                    if (cod > mx)
                    {
                        mx = cod;
                    }

                    usleep(200);
                }

                u32 media = suma / 1000U;

                xil_printf(
                    "media=%u min=%u max=%u (~%u uV)\r\n",
                    (unsigned)media,
                    (unsigned)mn,
                    (unsigned)mx,
                    (unsigned)(media * 244U)
                );
            }
            break;
        }

        default:
        {
            break;
        }
    }
}

static int inicializar_sistema(void)
{
    int rc = XTmrCtr_Initialize(
        &Timer,
        XPAR_XTMRCTR_0_BASEADDR
    );

    if (rc != XST_SUCCESS)
    {
        return XST_FAILURE;
    }

    rc = XSetupInterruptSystem(
        &Timer,
        (XInterruptHandler)XTmrCtr_InterruptHandler,
        Timer.Config.IntrId,
        Timer.Config.IntrParent,
        XINTERRUPT_DEFAULT_PRIORITY
    );

    if (rc != XST_SUCCESS)
    {
        return XST_FAILURE;
    }

    XTmrCtr_SetHandler(
        &Timer,
        atender_timer,
        &Timer
    );

    XTmrCtr_SetOptions(
        &Timer,
        0,
        XTC_AUTO_RELOAD_OPTION |
        XTC_DOWN_COUNT_OPTION |
        XTC_INT_MODE_OPTION
    );

    XTmrCtr_SetResetValue(
        &Timer,
        0,
        TIMER_RESET_VALUE
    );

    gpio_init(XPAR_AXI_GPIO_0_BASEADDR);

    if (xadc_init() != 0)
    {
        return XST_FAILURE;
    }

    pwm_init(XPAR_PWM_AXI_LITE_0_BASEADDR);
    pwm_deshabilitar();

    if (pwm_configurar(
        PWM_FREQ_SEL,
        TIEMPO_MUERTO_NS
    ) != 0)
    {
        return XST_FAILURE;
    }

    pwm_set_ref(0);
    preparar_seno();

    XTmrCtr_Start(&Timer, 0);

    return XST_SUCCESS;
}

int main(void)
{
    xil_printf(
        "PR fijo Q24 / 40 kHz / PWM apagado\r\n"
    );

    if (inicializar_sistema() != XST_SUCCESS)
    {
        xil_printf("ERROR inicializacion\r\n");
        return -1;
    }

    xil_printf(
        "a=10k b=12.5k d=20k p=PWM x=detener\r\n"
    );

    xil_printf(
        "n=estres v=diagnostico e=exportar "
        "c=calibrar i=corriente s=bloqueado\r\n"
    );

    u32 ultimo = 0;
    u32 botones_previos = 0;

    while (1)
    {
        u32 estado = pwm_leer_estado();

        if (estres_activo &&
            (pwm_esta_corriendo() ||
             (estado & PWM_STAT_LATCHED)))
        {
            estres_abortado = 2U;
            estres_activo = 0U;
            estres_terminado = 1U;

            pwm_deshabilitar();
            pwm_set_ref(0);
        }

        if (control_activo &&
            (estado & PWM_STAT_LATCHED))
        {
            detener();
            xil_printf("FALLA PWM\r\n");
        }

        if (falla_corriente)
        {
            falla_corriente = 0;
            xil_printf("SOBRECORRIENTE\r\n");
        }

        procesar_uart();

        u32 botones = gpio_leer_botones() & 0x0FU;
        u32 nuevos = botones & ~botones_previos;

        botones_previos = botones;

        if (nuevos & BTN_PARADA)
        {
            detener();
        }
        else if (nuevos & BTN_EXPORTAR)
        {
            exportar_buffer();
        }

        if (estres_terminado)
        {
            mostrar_estres();
        }

        u32 actual = interrupciones;

        if (!estres_activo &&
            (actual - ultimo >= 40000U))
        {
            ultimo = actual;

            xil_printf(
                "Int=%u | ctrl=%u | PWM run=%d "
                "| est=0x%X\r\n",
                (unsigned)actual,
                (unsigned)control_activo,
                pwm_esta_corriendo(),
                (unsigned)estado
            );
        }
    }
}
