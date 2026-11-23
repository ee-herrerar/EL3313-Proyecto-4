
#include "xparameters.h"
#include "xil_printf.h"
#include "xstatus.h"
#include "xtmrctr.h"
#include "xtmrctr_l.h"
#include "xuartlite_l.h"
#include "xil_types.h"
#include "xil_io.h"
#include "xinterrupt_wrap.h"

#include "pr.h"
#include "xadc.h"
#include "pwm.h"
#include "buffer.h"
#include "gpio.h"

#include <stdint.h>

/* ===========================================================
   PROYECTO EL3313
   VALIDACION INTEGRAL DEL CONTROL PR A 40 kHz

   MicroBlaze V: 100 MHz
   AXI Timer: 40 kHz
   PWM: 20 kHz

   CONTROL FISICO DESHABILITADO
   =========================================================== */

#define PR_CONFIGURADO        0

#define PWM_BASE             0x00010000U
#define GPIO_BASE            0x40000000U

#define PWM_FREQ_SEL         2U
#define DEADTIME_NS          200U

#define PERIODO              2500U
#define N_SENO               800U

#define MUESTRAS_ETAPA       400000U

#define I_MAX_MA             1500
#define SIGNO_SENSOR         1

#define PWM_POR_VOLT         (32767.0f / 12.0f)

/*
 * Tolerancia interna para comparar PR fijo y float.
 * No es el criterio oficial de seguimiento de corriente.
 */
#define TOL_PWM_CUENTAS      700U

/* ===========================================================
   COEFICIENTES PR EN PUNTO FIJO Q24

   Correspondientes a los coeficientes originales
   para Fs = 40 kHz.
   =========================================================== */

#define B0_Q24       278304923L
#define B1_Q24      (-549653375L)
#define B2_Q24       271365406L

#define A1_Q24      (-33549203L)
#define A2_Q24       16773022L

#define UMAX_INTERNO  (31128L * 32768L)

/* ===========================================================
   ESTRUCTURA DEL PR FIJO
   =========================================================== */

typedef struct
{
    s32 e1;
    s32 e2;

    s32 u1;
    s32 u2;

} pr_fijo_t;

/* ===========================================================
   VARIABLES GLOBALES
   =========================================================== */

static XTmrCtr Timer;

static pr_fijo_t prf;

static s16 seno_q15[N_SENO];

static volatile u32 irq_total = 0;

static volatile u8 estres_activo = 0;
static volatile u8 estres_terminado = 0;

static volatile u8 fase = 0;
static volatile u8 aborto = 0;

static volatile u32 n_fase[3] = {0};

static volatile u32 max_callback[5] = {0};
static volatile u32 max_lat_callback[5] = {0};

static volatile u32 cuenta_total = 0;

static volatile u32 interval_min = 0xFFFFFFFFU;
static volatile u32 interval_max = 0;

static volatile u32 avisos_2700 = 0;
static volatile u32 avisos_3750 = 0;

static volatile u32 fallo_positivo = 0;
static volatile u32 fallo_negativo = 0;

static u32 entrada_anterior = 0;

static u32 k_ref = 0;
static s32 amplitud_mA = 200;

static volatile u8 control_activo = 0;

/* ===========================================================
   CONTADOR MCYCLE
   =========================================================== */

static inline u32 mcycle_leer(void)
{
    u32 v;

    __asm__ volatile (
        "csrr %0, mcycle"
        : "=r"(v)
    );

    return v;
}

/* ===========================================================
   REINICIAR PR FIJO
   =========================================================== */

static void prf_reiniciar(pr_fijo_t *c)
{
    c->e1 = 0;
    c->e2 = 0;

    c->u1 = 0;
    c->u2 = 0;
}

/* ===========================================================
   PR EN PUNTO FIJO

   No se utilizan float.

   Acumulacion en 64 bits.
   Salida directamente en formato PWM Q15.

   Se evita duplicar la funcion para ahorrar BRAM.
   =========================================================== */

static __attribute__((noinline)) s16
prf_paso(pr_fijo_t *c, s32 error_mA)
{
    int64_t n =
          (int64_t)B0_Q24 * error_mA
        + (int64_t)B1_Q24 * c->e1
        + (int64_t)B2_Q24 * c->e2;

    int64_t a =
          n * 32768LL
        - (int64_t)A1_Q24 * c->u1
        - (int64_t)A2_Q24 * c->u2;

    int64_t u = a >> 24;

    /* Saturacion */

    if (u > UMAX_INTERNO)
    {
        u = UMAX_INTERNO;
    }

    if (u < -UMAX_INTERNO)
    {
        u = -UMAX_INTERNO;
    }

    /* Actualizar estados */

    c->e2 = c->e1;
    c->e1 = error_mA;

    c->u2 = c->u1;
    c->u1 = (s32)u;

    return (s16)(((s32)u) >> 15);
}

/* ===========================================================
   TABLA SINUSOIDAL

   Float solamente durante inicializacion.
   =========================================================== */

static void seno_preparar(void)
{
    const float cw = 0.99996916f;
    const float sw = 0.00785390f;

    float s = 0.0f;
    float c = 1.0f;

    for (u32 k = 0; k < N_SENO; ++k)
    {
        seno_q15[k] =
            (s16)(s * 32767.0f);

        float ns =
            s * cw + c * sw;

        c = c * cw - s * sw;
        s = ns;
    }

    prf_reiniciar(&prf);
}

/* ===========================================================
   PRUEBA 1
   EQUIVALENCIA PR FLOAT CONTRA PR FIJO

   Se realiza fuera de la interrupcion.

   Escenarios:
   - Medida igual a la mitad de referencia.
   - Medida igual a cero.
   - Medida alternante, con saturacion.

   Se ensayan las tres amplitudes.
   =========================================================== */

static int test_pr(void)
{
    int ok = 1;

    u32 max_global = 0;
    u32 saturaciones = 0;

    const s32 amplitudes[3] = {
        200, 500, 1000
    };

    xil_printf(
        "[PR] Comparacion de 9 escenarios "
        "de 800 muestras...\r\n"
    );

    for (u32 amp = 0; amp < 3U; ++amp)
    {
        u32 max_amp = 0;

        for (u32 caso = 0; caso < 3U; ++caso)
        {
            pr_t flotante;

            pr_fijo_t fijo = {
                0, 0, 0, 0
            };

            /* PR original */

            pr_init(
                &flotante,
                6.0749898553f,
                -11.9981301409f,
                5.9235103476f,
                1.0f,
                -1.9996883568f,
                0.9997500338f,
                PWM_M_MAX * 12.0f
            );

            for (u32 k = 0; k < N_SENO; ++k)
            {
                s32 ref =
                    (amplitudes[amp] *
                     (s32)seno_q15[k]) >> 15;

                s32 medida;

                if (caso == 0U)
                {
                    medida = ref / 2;
                }
                else if (caso == 1U)
                {
                    medida = 0;
                }
                else
                {
                    medida =
                        ((k / 200U) & 1U)
                        ? 1400
                        : -1400;
                }

                /* PR fijo */

                s16 pwm_fijo = prf_paso(
                    &fijo,
                    ref - medida
                );

                /* PR original float */

                float u_f = pr_actualizar(
                    &flotante,
                    (float)ref * 0.001f,
                    (float)medida * 0.001f
                );

                s32 pwm_float =
                    (s32)(u_f * PWM_POR_VOLT);

                /* Error entre ambas salidas */

                s32 diferencia =
                    (s32)pwm_fijo - pwm_float;

                if (diferencia < 0)
                {
                    diferencia = -diferencia;
                }

                if ((u32)diferencia > max_amp)
                {
                    max_amp = (u32)diferencia;
                }

                if (pwm_fijo >= 31128 ||
                    pwm_fijo <= -31128)
                {
                    saturaciones++;
                }
            }
        }

        if (max_amp > max_global)
        {
            max_global = max_amp;
        }

        xil_printf(
            "  Ref %u mA: error pico=%u "
            "cuentas PWM\r\n",
            (unsigned)amplitudes[amp],
            (unsigned)max_amp
        );
    }

    if (max_global > TOL_PWM_CUENTAS)
    {
        ok = 0;
    }

    xil_printf(
        "[PR] %s | max=%u | limite interno=%u "
        "| saturaciones=%u\r\n",
        ok ? "PASS preliminar" : "REVISAR",
        (unsigned)max_global,
        (unsigned)TOL_PWM_CUENTAS,
        (unsigned)saturaciones
    );

    prf_reiniciar(&prf);

    return ok;
}

/* ===========================================================
   PRUEBA 2
   BUFFER CIRCULAR Y DECIMACION DE 4
   =========================================================== */

static int test_buffer(void)
{
    buffer_congelar(0);
    buffer_reiniciar();

    /* Probar sobrescritura */

    for (u32 i = 0; i < N_BUF + 5U; ++i)
    {
        buffer_guardar((s16)i, 0);
    }

    int circular =
        buffer_cantidad() == N_BUF
        && buffer_lleno()
        && buffer_leer(0)->i_mA == 5
        && buffer_leer(N_BUF - 1U)->i_mA ==
           (s16)(N_BUF + 4U);

    /* Probar decimacion */

    buffer_reiniciar();

    for (u32 k = 0; k < 32U; ++k)
    {
        if ((k & 3U) == 0U)
        {
            buffer_guardar((s16)k, 0);
        }
    }

    int decima =
        buffer_cantidad() == 8U
        && buffer_leer(0)->i_mA == 0
        && buffer_leer(7)->i_mA == 28;

    xil_printf(
        "[BUFFER] circular=%s "
        "| decimacion 4=%s "
        "| ventana=153.6 ms\r\n",
        circular ? "PASS" : "FAIL",
        decima ? "PASS" : "FAIL"
    );

    buffer_reiniciar();

    return circular && decima;
}

/* ===========================================================
   PRUEBA 3
   LECTURA DEL REGISTRO PWM

   El PWM NO se habilita.
   =========================================================== */

static int test_pwm(void)
{
    if (pwm_esta_corriendo())
    {
        return 0;
    }

    const s16 valores[3] = {
        0, 16384, -16384
    };

    int ok = 1;

    for (u32 k = 0; k < 3U; ++k)
    {
        pwm_set_ref(valores[k]);

        u16 leido = (u16)Xil_In32(
            PWM_BASE + PWM_REG_REF
        );

        if (leido != (u16)valores[k])
        {
            ok = 0;
        }
    }

    pwm_set_ref(0);

    if (pwm_esta_corriendo())
    {
        ok = 0;
    }

    xil_printf(
        "[PWM] readback=%s | run=%d\r\n",
        ok ? "PASS" : "REVISAR RTL",
        pwm_esta_corriendo()
    );

    return ok;
}

/* ===========================================================
   INTERRUPCION DEL AXI TIMER

   CONTROL PR FIJO + PROTECCION + BUFFER + PWM
   =========================================================== */

void atender_timer(void *referencia, u8 numero)
{
    (void)referencia;
    (void)numero;

    u32 inicio = mcycle_leer();

    u8 activo = estres_activo;
    u8 f = fase;

    u32 lat = 0;

    irq_total++;

    /* ========================================================
       ESTADISTICAS DE PERIODICIDAD
       ======================================================== */

    if (activo)
    {
        lat =
            (PERIODO - 1U) -
            XTmrCtr_GetValue(&Timer, 0);

        if (entrada_anterior != 0U)
        {
            u32 dt =
                inicio - entrada_anterior;

            if (dt < interval_min)
            {
                interval_min = dt;
            }

            if (dt > interval_max)
            {
                interval_max = dt;
            }

            if (dt > 2700U)
            {
                avisos_2700++;
            }

            if (dt >= 3750U)
            {
                avisos_3750++;
            }
        }

        entrada_anterior = inicio;
    }

    /* ========================================================
       CONTROL NORMAL Y ESTRES
       ======================================================== */

    if (activo || control_activo)
    {
        /* 1. Adquirir corriente */

        s32 i_mA =
            SIGNO_SENSOR *
            xadc_a_miliamperios(
                xadc_leer_codigo()
            );

        /* 2. Fallas simuladas */

        if (activo && f == 3U)
        {
            i_mA = 1601;
        }

        if (activo && f == 4U)
        {
            i_mA = -1601;
        }

        /* 3. Proteccion */

        if (i_mA > I_MAX_MA ||
            i_mA < -I_MAX_MA)
        {
            control_activo = 0;

            pwm_deshabilitar();
            pwm_set_ref(0);

            if (activo)
            {
                if (f == 3U)
                {
                    fallo_positivo++;
                }
                else if (f == 4U)
                {
                    fallo_negativo++;
                }
                else
                {
                    aborto = 1;
                    estres_activo = 0;
                    estres_terminado = 1;
                }
            }
        }
        else
        {
            /* 4. Referencia sinusoidal */

            s32 referencia_mA =
                (amplitud_mA *
                 (s32)seno_q15[k_ref]) >> 15;

            /* 5. Buffer con decimacion de 4 */

            if ((k_ref & 3U) == 0U)
            {
                buffer_guardar(
                    (s16)i_mA,
                    0
                );
            }

            /* 6. Avanzar referencia */

            if (++k_ref >= N_SENO)
            {
                k_ref = 0;
            }

            /* 7. PR fijo */

            s16 ref_pwm = prf_paso(
                &prf,
                referencia_mA - i_mA
            );

            /* 8. Escribir PWM */

            pwm_set_ref(ref_pwm);
        }
    }

    /* ========================================================
       CONTROL DE ETAPAS
       ======================================================== */

    if (activo)
    {
        cuenta_total++;

        if (aborto == 0U)
        {
            if (f < 3U)
            {
                if (++n_fase[f] >= MUESTRAS_ETAPA)
                {
                    fase = f + 1U;

                    if (f == 0U)
                    {
                        amplitud_mA = 500;
                    }
                    else if (f == 1U)
                    {
                        amplitud_mA = 1000;
                    }
                }
            }
            else if (f == 3U)
            {
                fase = 4U;
            }
            else
            {
                estres_activo = 0;
                estres_terminado = 1;
            }
        }

        /* ====================================================
           MEDICION MCYCLE
           ==================================================== */

        u32 duracion =
            mcycle_leer() - inicio;

        u32 total = lat + duracion;

        if (duracion > max_callback[f])
        {
            max_callback[f] = duracion;
        }

        if (total > max_lat_callback[f])
        {
            max_lat_callback[f] = total;
        }
    }
}

/* ===========================================================
   DETENER SISTEMA
   =========================================================== */

static void detener(void)
{
    control_activo = 0;

    if (estres_activo)
    {
        aborto = 3;
        estres_activo = 0;
        estres_terminado = 1;
    }

    pwm_deshabilitar();
    pwm_set_ref(0);
}

/* ===========================================================
   INICIAR VALIDACION INTEGRAL
   =========================================================== */

static void iniciar_integral(void)
{
    if (estres_activo ||
        estres_terminado ||
        control_activo ||
        pwm_esta_corriendo())
    {
        xil_printf(
            "No se inicia: prueba o PWM ocupado\r\n"
        );

        return;
    }

    pwm_deshabilitar();
    pwm_set_ref(0);

    xil_printf(
        "=== PREPRUEBAS (PWM APAGADO) ===\r\n"
    );

    /* ========================================================
       XADC
       ======================================================== */

    u16 offset =
        xadc_calibrar_offset(200);

    s32 corriente =
        xadc_a_miliamperios(
            xadc_leer_codigo()
        );

    xil_printf(
        "[XADC] offset=%u | corriente=%d mA\r\n",
        (unsigned)offset,
        (int)corriente
    );

    /* ========================================================
       PR, BUFFER Y PWM
       ======================================================== */

    int pr_ok = test_pr();
    int buffer_ok = test_buffer();
    int pwm_ok = test_pwm();

    xil_printf(
        "[UART] Texto enviado a terminal\r\n"
    );

    xil_printf(
        "[GPIO] botones=0x%X\r\n",
        (unsigned)(
            gpio_leer_botones() & 0x0FU
        )
    );

    xil_printf(
        "Prepruebas: PR=%s "
        "BUFFER=%s PWM_REG=%s\r\n",
        pr_ok ? "PASS" : "REVISAR",
        buffer_ok ? "PASS" : "FAIL",
        pwm_ok ? "PASS" : "REVISAR"
    );

    if (!pwm_ok)
    {
        xil_printf(
            "ATENCION: revisar lectura PWM_REF "
            "en RTL; se continua solo con PWM apagado\r\n"
        );
    }

    if (!buffer_ok || pwm_esta_corriendo())
    {
        xil_printf(
            "Estres no iniciado: "
            "buffer incorrecto o PWM activo\r\n"
        );

        return;
    }

    /* ========================================================
       PREPARAR ESTRES
       ======================================================== */

    prf_reiniciar(&prf);

    buffer_congelar(0);
    buffer_reiniciar();

    k_ref = 0;
    amplitud_mA = 200;

    for (u32 i = 0; i < 3U; ++i)
    {
        n_fase[i] = 0;
    }

    for (u32 i = 0; i < 5U; ++i)
    {
        max_callback[i] = 0;
        max_lat_callback[i] = 0;
    }

    entrada_anterior = 0;

    interval_min = 0xFFFFFFFFU;
    interval_max = 0;

    avisos_2700 = 0;
    avisos_3750 = 0;

    fallo_positivo = 0;
    fallo_negativo = 0;

    cuenta_total = 0;

    fase = 0;
    aborto = 0;

    xil_printf(
        "ESTRES: 3 x 400000 muestras "
        "+ fallas +/-1601 mA\r\n"
    );

    /* El PWM continua deshabilitado */

    estres_activo = 1;
}

/* ===========================================================
   RESULTADOS DE VALIDACION
   =========================================================== */

static void resultados(void)
{
    estres_terminado = 0;

    pwm_deshabilitar();
    pwm_set_ref(0);

    xil_printf(
        "=== VALIDACION INTEGRAL 40 kHz ===\r\n"
    );

    xil_printf(
        "Muestras=%u | aborto=%u "
        "| periodo=2500 ciclos\r\n",
        (unsigned)cuenta_total,
        (unsigned)aborto
    );

    for (u32 i = 0; i < 3U; ++i)
    {
        xil_printf(
            "Etapa %u: n=%u | mcycle_max=%u "
            "| lat+callback_max=%u\r\n",
            (unsigned)(i + 1U),
            (unsigned)n_fase[i],
            (unsigned)max_callback[i],
            (unsigned)max_lat_callback[i]
        );
    }

    xil_printf(
        "Falla +: %u (ciclos=%u) "
        "| Falla -: %u (ciclos=%u)\r\n",
        (unsigned)fallo_positivo,
        (unsigned)max_callback[3],
        (unsigned)fallo_negativo,
        (unsigned)max_callback[4]
    );

    xil_printf(
        "Intervalo min/max=%u/%u "
        "| >2700=%u | >=3750=%u\r\n",
        (unsigned)interval_min,
        (unsigned)interval_max,
        (unsigned)avisos_2700,
        (unsigned)avisos_3750
    );

    xil_printf(
        "BUFFER=%u | Ts=100 us "
        "| ventana=153.6 ms\r\n",
        (unsigned)buffer_cantidad()
    );

    xil_printf(
        "PWM run=%d | ref=0x%X\r\n",
        pwm_esta_corriendo(),
        (unsigned)(
            (u16)Xil_In32(
                PWM_BASE + PWM_REG_REF
            )
        )
    );

    xil_printf(
        "=== FIN VALIDACION ===\r\n"
    );

    prf_reiniciar(&prf);
}

/* ===========================================================
   EXPORTAR BUFFER POR UART
   =========================================================== */

static void exportar(void)
{
    if (estres_activo || control_activo)
    {
        xil_printf(
            "Exportacion bloqueada "
            "durante control\r\n"
        );

        return;
    }

    buffer_congelar(1);

    u32 n = buffer_cantidad();

    xil_printf(
        "# INICIO muestras=%u "
        "Ts_us=100 unidad=mA\r\n",
        (unsigned)n
    );

    xil_printf("# i_mA\r\n");

    for (u32 i = 0; i < n; ++i)
    {
        xil_printf(
            "%d\r\n",
            (int)buffer_leer(i)->i_mA
        );
    }

    xil_printf("# FIN\r\n");

    buffer_congelar(0);
}

/* ===========================================================
   COMANDOS UART
   =========================================================== */

static void comandos(void)
{
    if (XUartLite_IsReceiveEmpty(
        STDIN_BASEADDRESS))
    {
        return;
    }

    char c = (char)XUartLite_RecvByte(
        STDIN_BASEADDRESS
    );

    switch (c)
    {
        case 'n':
        {
            iniciar_integral();
            break;
        }

        case 'x':
        {
            detener();

            xil_printf("Detenido\r\n");
            break;
        }

        case 'e':
        {
            exportar();
            break;
        }

        case 's':
        {
            xil_printf(
                "Inicio fisico bloqueado: "
                "PR_CONFIGURADO=0\r\n"
            );

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

static int sistema_init(void)
{
    /* AXI Timer */

    if (XTmrCtr_Initialize(
        &Timer,
        XPAR_XTMRCTR_0_BASEADDR
    ) != XST_SUCCESS)
    {
        return XST_FAILURE;
    }

    if (XSetupInterruptSystem(
        &Timer,
        (XInterruptHandler)
            XTmrCtr_InterruptHandler,
        Timer.Config.IntrId,
        Timer.Config.IntrParent,
        XINTERRUPT_DEFAULT_PRIORITY
    ) != XST_SUCCESS)
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
        PERIODO - 1U
    );

    /* GPIO */

    gpio_init(GPIO_BASE);

    /* XADC */

    if (xadc_init() != 0)
    {
        return XST_FAILURE;
    }

    /* PWM deshabilitado */

    pwm_init(PWM_BASE);

    if (pwm_configurar(
        PWM_FREQ_SEL,
        DEADTIME_NS
    ) != 0)
    {
        return XST_FAILURE;
    }

    pwm_set_ref(0);

    /* Preparar PR */

    seno_preparar();

    /* Iniciar Timer */

    XTmrCtr_Start(&Timer, 0);

    return XST_SUCCESS;
}

/* ===========================================================
   MAIN
   =========================================================== */

int main(void)
{
    xil_printf(
        "EL3313 VALIDACION INTEGRAL 40 kHz\r\n"
    );

    xil_printf(
        "Control fisico deshabilitado\r\n"
    );

    if (sistema_init() != XST_SUCCESS)
    {
        xil_printf(
            "ERROR inicializando perifericos\r\n"
        );

        return -1;
    }

    xil_printf(
        "Comandos: n=validar todo, "
        "e=exportar, x=detener, "
        "s=bloqueado\r\n"
    );

    u32 botones_previos = 0;

    while (1)
    {
        /* ====================================================
           SUPERVISION PWM
           ==================================================== */

        u32 estado = pwm_leer_estado();

        if (estres_activo &&
            (pwm_esta_corriendo() ||
            (estado & PWM_STAT_LATCHED)))
        {
            aborto = 2;
            estres_activo = 0;
            estres_terminado = 1;

            pwm_deshabilitar();
            pwm_set_ref(0);
        }

        /* ====================================================
           COMANDOS
           ==================================================== */

        comandos();

        /* ====================================================
           BOTONES
           ==================================================== */

        u32 botones =
            gpio_leer_botones() & 0x0FU;

        if ((botones & BTN_PARADA) &&
            !(botones_previos & BTN_PARADA))
        {
            detener();

            xil_printf(
                "Parada por boton\r\n"
            );
        }

        botones_previos = botones;

        /* ====================================================
           RESULTADOS
           ==================================================== */

        if (estres_terminado)
        {
            resultados();
        }
    }
}
