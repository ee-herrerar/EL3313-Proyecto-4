
#include "xparameters.h"
#include "xil_printf.h"
#include "xstatus.h"
#include "xtmrctr.h"
#include "xtmrctr_l.h"
#include "xuartlite_l.h"
#include "xil_types.h"
#include "xinterrupt_wrap.h"
#include <stdint.h>

#include "xadc.h"
#include "pwm.h"
#include "buffer.h"
#include "gpio.h"

/* ===========================================================
   PROYECTO EL3313: PR A 40 kHz EN PUNTO FIJO
   PWM de potencia bloqueado por defecto.
   No se modifica ningun driver existente.
   =========================================================== */

#define PR_CONFIGURADO       0
#define PWM_FREQ_SEL         2U
#define TIEMPO_MUERTO_NS     200U
#define PERIODO_US           25U
#define CICLOS_PERIODO       2500U
#define TIMER_RESET_VALUE    2499U
#define N_SENO               800U
#define I_MAX_MA             1500
#define SIGNO_SENSOR         1
#define MUESTRAS_ETAPA       400000U

/*
 * PR discreto, Fs = 40 kHz, 50 Hz.
 *
 * Coeficientes originales:
 *
 * b = {
 *    6.0749898553,
 *  -11.9981301409,
 *    5.9235103476
 * }
 *
 * a = {
 *    1,
 *   -1.9996883568,
 *    0.9997500338
 * }
 *
 * b se escala a unidades Q15_PWM/mA en Q24:
 *
 * BQ = round(b * (32767/12000) * 2^24)
 * AQ = round(a * 2^24)
 *
 * Estado de salida: cuentas PWM con 15 bits fraccionarios.
 *
 * Ninguna operacion float se ejecuta dentro de la ISR.
 */

#define B0_Q24   278304923L
#define B1_Q24  -549653375L
#define B2_Q24   271365406L

#define A1_Q24   -33549203L
#define A2_Q24    16773022L

/* Limite de modulacion: |m| <= 0.95 */

#define UMAX_Q15  (31128L * 32768L)

/* ===========================================================
   ESTADO DEL CONTROLADOR
   =========================================================== */

typedef struct
{
    s32 e1;
    s32 e2;

    s32 u1;
    s32 u2;

} pr_fixed_t;

static pr_fixed_t controlador;

static XTmrCtr Timer;

/* Tabla de referencia: solo 1600 bytes */

static s16 seno_q15[N_SENO];

/* ===========================================================
   ESTADO GENERAL
   =========================================================== */

static volatile u32 interrupciones = 0;

static volatile u8 control_activo = 0;
static volatile u8 falla_corriente = 0;

static volatile u8 estres_activo = 0;
static volatile u8 estres_terminado = 0;
static volatile u8 estres_abortado = 0;
static volatile u8 fase_estres = 0;

/* ===========================================================
   ESTADISTICAS DE ESTRES
   =========================================================== */

static volatile u32 muestras_fase[3] = {
    0, 0, 0
};

static volatile u32 max_ciclos[5] = {
    0, 0, 0, 0, 0
};

static volatile u32 max_lat_y_ciclos[5] = {
    0, 0, 0, 0, 0
};

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

/* ===========================================================
   CONTADOR DE CICLOS
   =========================================================== */

static inline u32 leer_mcycle(void)
{
    u32 v;

    __asm__ volatile (
        "csrr %0, mcycle"
        : "=r"(v)
    );

    return v;
}

/* ===========================================================
   REINICIAR PR
   =========================================================== */

static inline void pr_fijo_reiniciar(void)
{
    controlador.e1 = 0;
    controlador.e2 = 0;

    controlador.u1 = 0;
    controlador.u2 = 0;
}

/* ===========================================================
   PR EN PUNTO FIJO

   No hay operaciones flotantes.

   Los productos grandes se acumulan en 64 bits
   para evitar desbordamiento.

   La salida es directamente Q15 para el PWM.
   =========================================================== */

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

    /* Saturacion */

    if (u > UMAX_Q15)
    {
        u = UMAX_Q15;
    }

    if (u < -UMAX_Q15)
    {
        u = -UMAX_Q15;
    }

    /* Actualizar estados */

    controlador.e2 = controlador.e1;
    controlador.e1 = error_mA;

    controlador.u2 = controlador.u1;
    controlador.u1 = (s32)u;

    /* Salida Q15 con signo */

    return (s16)(((s32)u) >> 15);
}

/* ===========================================================
   TABLA SINUSOIDAL

   Las operaciones float se ejecutan solamente
   durante la inicializacion.
   =========================================================== */

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

/* ===========================================================
   INTERRUPCION DEL AXI TIMER
   =========================================================== */

void atender_timer(void *referencia, u8 numero)
{
    (void)referencia;
    (void)numero;

    u32 comienzo = leer_mcycle();

    u8 probando = estres_activo;
    u8 fase = fase_estres;

    u32 latencia = 0;

    interrupciones++;

    /* ========================================================
       MEDICION DE LA PERIODICIDAD
       Solo durante la prueba de estres.
       ======================================================== */

    if (probando)
    {
        u32 t0 = XTmrCtr_GetValue(&Timer, 0);

        latencia = TIMER_RESET_VALUE - t0;

        if (entrada_anterior != 0U)
        {
            u32 intervalo =
                comienzo - entrada_anterior;

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

    /* ========================================================
       RUTA COMUN:
       CONTROL NORMAL + PRUEBA DE ESTRES
       ======================================================== */

    if (control_activo || probando)
    {
        /* 1. Lectura real del XADC */

        s32 i_mA =
            SIGNO_SENSOR *
            xadc_a_miliamperios(
                xadc_leer_codigo()
            );

        /* ====================================================
           FALLAS SIMULADAS

           Solamente durante el diagnostico.
           ==================================================== */

        if (probando && fase == 3U)
        {
            i_mA = 1601;
        }

        if (probando && fase == 4U)
        {
            i_mA = -1601;
        }

        /* ====================================================
           2. PROTECCION POR SOBRECORRIENTE
           ==================================================== */

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
            /* =================================================
               3. REFERENCIA SINUSOIDAL EN mA

               Una multiplicacion entera y un desplazamiento.
               ================================================= */

            s32 ref_mA =
                (amplitud_mA *
                 (s32)seno_q15[k_ref]) >> 15;

            if (++k_ref >= N_SENO)
            {
                k_ref = 0;
            }

            /* =================================================
               4. BUFFER

               Guardar una muestra por interrupcion.
               Ts = 25 us.
               ================================================= */

            buffer_guardar(
                (s16)i_mA,
                0
            );

            /* =================================================
               5. CONTROLADOR PR EN PUNTO FIJO
               ================================================= */

            s16 ref_pwm = pr_fijo_actualizar(
                ref_mA - i_mA
            );

            /* =================================================
               6. ESCRITURA DIRECTA PWM Q15
               ================================================= */

            pwm_set_ref(ref_pwm);
        }
    }

    /* ========================================================
       CONTROL DE ETAPAS DEL ESTRES
       ======================================================== */

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

        /* ====================================================
           MEDICION FINAL CON MCYCLE

           No depende del reinicio del AXI Timer.
           ==================================================== */

        u32 duracion =
            leer_mcycle() - comienzo;

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

/* ===========================================================
   DETENER
   =========================================================== */

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

/* ===========================================================
   INICIAR ESTRES
   =========================================================== */

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

    estres_activo = 1U;
}

/* ===========================================================
   MOSTRAR RESULTADOS
   =========================================================== */

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

    xil_printf(
        "=== FIN ESTRES ===\r\n"
    );

    pr_fijo_reiniciar();
}

/* ===========================================================
   EXPORTAR BUFFER
   =========================================================== */

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

    xil_printf("# FIN\r\n");

    buffer_congelar(0);
}

/* ===========================================================
   COMANDOS UART
   =========================================================== */

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
            if (!estres_activo)
            {
                s32 val =
                    xadc_a_miliamperios(
                        xadc_leer_codigo()
                    );

                xil_printf(
                    "Corriente=%d mA\r\n",
                    (int)val
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

        default:
        {
            break;
        }
    }
}

/* ===========================================================
   INICIALIZACION DEL SISTEMA
   =========================================================== */

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

    /* GPIO */

    gpio_init(0x40000000U);

    /* XADC */

    if (xadc_init() != 0)
    {
        return XST_FAILURE;
    }

    /* PWM */

    pwm_init(0x00010000U);

    if (pwm_configurar(
        PWM_FREQ_SEL,
        TIEMPO_MUERTO_NS
    ) != 0)
    {
        return XST_FAILURE;
    }

    pwm_set_ref(0);

    /* Preparar PR y tabla sinusoidal */

    preparar_seno();

    /* Iniciar temporizador */

    XTmrCtr_Start(&Timer, 0);

    return XST_SUCCESS;
}

/* ===========================================================
   MAIN
   =========================================================== */

int main(void)
{
    xil_printf(
        "PR fijo Q24 / 40 kHz / PWM apagado\r\n"
    );

    if (inicializar_sistema() != XST_SUCCESS)
    {
        xil_printf(
            "ERROR inicializacion\r\n"
        );

        return -1;
    }

    xil_printf(
        "c=calibrar i=corriente n=estres "
        "x=detener e=exportar s=bloqueado\r\n"
    );

    u32 ultimo = 0;
    u32 botones_previos = 0;

    while (1)
    {
        /* Estado PWM */

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

            xil_printf(
                "FALLA PWM\r\n"
            );
        }

        if (falla_corriente)
        {
            falla_corriente = 0;

            xil_printf(
                "SOBRECORRIENTE\r\n"
            );
        }

        /* Comandos */

        procesar_uart();

        /* Botones */

        u32 botones =
            gpio_leer_botones() & 0x0FU;

        u32 nuevos =
            botones & ~botones_previos;

        botones_previos = botones;

        if (nuevos & BTN_PARADA)
        {
            detener();
        }
        else if (nuevos & BTN_EXPORTAR)
        {
            exportar_buffer();
        }

        /* Resultados */

        if (estres_terminado)
        {
            mostrar_estres();
        }

        /* Estadisticas solo fuera del estres */

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
