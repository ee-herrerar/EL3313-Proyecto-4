
#include "xparameters.h"
#include "xil_printf.h"
#include "xstatus.h"
#include "xtmrctr.h"
#include "xtmrctr_l.h"
#include "xuartlite_l.h"
#include "xil_types.h"
#include "xinterrupt_wrap.h"

#include "xadc.h"
#include "pwm.h"
#include "pr.h"
#include "buffer.h"
#include "gpio.h"

/* ============================================================
   CONFIGURACION GENERAL
   ============================================================ */

#define PWM_FREQ_SEL          2
#define TIEMPO_MUERTO_NS      200U

#define PERIODO_CONTROL_US    25U

#define TIMER_RESET_VALUE \
    (PERIODO_CONTROL_US * 100U - 1U)

#define MUESTRAS_50HZ \
    (20000U / PERIODO_CONTROL_US)

#define VDC                   12.0f

#define PWM_CUENTAS_POR_VOLT \
    (32767.0f / VDC)

/* BLOQUEO DEL CONTROL FISICO */
#define PR_CONFIGURADO        0

#define SIGNO_SENSOR          1
#define I_MAX_MA              1500
#define DECIMACION_BUF        2U

/* ============================================================
   CONFIGURACION DEL ESTRES
   ============================================================ */

/* 10 segundos por amplitud a 40 kHz */

#define MUESTRAS_POR_ETAPA    400000U

#define ETAPAS_NORMALES      3U
#define ETAPAS_TOTALES       5U

/*
 * Diagnostico de intervalos entre interrupciones.
 * Estos umbrales suponen mcycle a 100 MHz.
 */

#define BRECHA_ADVERTENCIA       2700U
#define BRECHA_POSIBLE_PERDIDA   3750U

/* ============================================================
   COEFICIENTES PR NORMALIZADOS
   Fs = 40000 Hz
   Fr = 50 Hz
   ============================================================ */

#define PR_B0     6.0749898553f
#define PR_B1   -11.9981301409f
#define PR_B2     5.9235103476f

#define PR_A0     1.0f
#define PR_A1    -1.9996883568f
#define PR_A2     0.9997500338f

/* ============================================================
   VARIABLES PRINCIPALES
   ============================================================ */

static XTmrCtr Timer;
static pr_t pr;

static float seno[MUESTRAS_50HZ];

static volatile u32 contador_interrupciones = 0;

static volatile float amplitud_ref_A = 0.2f;

static u32 k_ref = 0;
static u8 dec_buf = 0;

static volatile u8 control_activo = 0;
static volatile u8 falla_sobrecorriente = 0;

/* ============================================================
   ESTADO DE LA PRUEBA
   ============================================================ */

static volatile u8 estres_activo = 0;
static volatile u8 estres_terminado = 0;

/*
 * Fases:
 * 0 = 0.2 A
 * 1 = 0.5 A
 * 2 = 1.0 A
 * 3 = falla simulada +1601 mA
 * 4 = falla simulada -1601 mA
 */

static volatile u8 estres_fase = 0;

/*
 * Motivos de aborto:
 * 0 = ninguno
 * 1 = sobrecorriente no simulada
 * 2 = problema de estado PWM
 * 3 = parada manual
 */

static volatile u8 estres_aborto = 0;

/* ============================================================
   ESTADISTICAS DE ESTRES
   ============================================================ */

static volatile u32
    estres_muestras[ETAPAS_NORMALES] = {0};

static volatile u32
    estres_ciclos_max[ETAPAS_TOTALES] = {0};

static volatile u32
    estres_latum_max[ETAPAS_TOTALES] = {0};

static volatile u32 estres_total = 0;

static volatile u32 estres_falla_pos = 0;
static volatile u32 estres_falla_neg = 0;

/* Estadisticas de periodicidad */

static volatile u32 estres_brechas = 0;

static volatile u32 estres_posibles_perdidas = 0;

static volatile u32 estres_intervalo_min =
    0xFFFFFFFFU;

static volatile u32 estres_intervalo_max = 0;

static u32 estres_entrada_anterior = 0;

/* ============================================================
   LECTURA DEL CONTADOR MCYCLE
   ============================================================ */

static inline u32 leer_mcycle(void)
{
    u32 c;

    __asm__ volatile (
        "csrr %0, mcycle"
        : "=r"(c)
    );

    return c;
}

/* ============================================================
   INTERRUPCION DEL AXI TIMER
   ============================================================ */

void atender_timer(void *referencia, u8 numero)
{
    /* Inicio de la medicion independiente */
    u32 inicio = leer_mcycle();

    (void)referencia;
    (void)numero;

    /* Timer periodico */
    u32 t0 = XTmrCtr_GetValue(&Timer, 0);

    u32 latencia = TIMER_RESET_VALUE - t0;

    contador_interrupciones++;

    u8 probando = estres_activo;
    u8 fase = estres_fase;

    /* ========================================================
       MEDIR INTERVALOS ENTRE ENTRADAS DE INTERRUPCION
       ======================================================== */

    if (probando)
    {
        if (estres_entrada_anterior != 0U)
        {
            u32 separacion =
                inicio - estres_entrada_anterior;

            if (separacion < estres_intervalo_min)
            {
                estres_intervalo_min = separacion;
            }

            if (separacion > estres_intervalo_max)
            {
                estres_intervalo_max = separacion;
            }

            if (separacion > BRECHA_ADVERTENCIA)
            {
                estres_brechas++;
            }

            if (separacion >= BRECHA_POSIBLE_PERDIDA)
            {
                estres_posibles_perdidas++;
            }
        }

        estres_entrada_anterior = inicio;
    }

    /* ========================================================
       RUTA COMUN: CONTROL NORMAL Y PRUEBA

       XADC + PROTECCION + REFERENCIA + BUFFER + PR + PWM
       ======================================================== */

    if (control_activo || probando)
    {
        /* 1. Leer XADC */

        s32 i_mA = SIGNO_SENSOR *
                   xadc_a_miliamperios(
                       xadc_leer_codigo()
                   );

        /* ====================================================
           INYECCION DE FALLAS SIMULADAS

           Solamente en las dos fases finales.
           No modifica el sensor ni aplica corriente real.
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

            /* Misma ruta de apagado del control */
            pwm_deshabilitar();
            pwm_set_ref(0);

            if (probando)
            {
                if (fase == 3U)
                {
                    estres_falla_pos++;
                }
                else if (fase == 4U)
                {
                    estres_falla_neg++;
                }
                else
                {
                    /* Sobrecorriente inesperada */
                    estres_aborto = 1;
                    estres_activo = 0;
                    estres_terminado = 1;
                }
            }
            else
            {
                falla_sobrecorriente = 1;
            }
        }
        else
        {
            /* =================================================
               3. CONVERSION DE CORRIENTE
               ================================================= */

            float i_A =
                (float)i_mA * 0.001f;

            /* =================================================
               4. REFERENCIA SINUSOIDAL
               ================================================= */

            float r_A =
                amplitud_ref_A * seno[k_ref];

            k_ref++;

            if (k_ref >= MUESTRAS_50HZ)
            {
                k_ref = 0;
            }

            /* =================================================
               5. BUFFER CIRCULAR
               ================================================= */

            dec_buf++;

            if (dec_buf >= DECIMACION_BUF)
            {
                dec_buf = 0;

                buffer_guardar(
                    (s16)i_mA,
                    0
                );
            }

            /* =================================================
               6. CONTROLADOR PR
               ================================================= */

            float u_V = pr_actualizar(
                &pr,
                r_A,
                i_A
            );

            /* =================================================
               7. ESCRITURA PWM OPTIMIZADA

               Durante la prueba las salidas siguen apagadas.
               ================================================= */

            pwm_set_ref(
                (s16)(
                    u_V * PWM_CUENTAS_POR_VOLT
                )
            );
        }
    }

    /* ========================================================
       CONTROL DE LAS ETAPAS DE ESTRES
       ======================================================== */

    if (probando)
    {
        estres_total++;

        if (!estres_aborto)
        {
            if (fase < ETAPAS_NORMALES)
            {
                estres_muestras[fase]++;

                if (estres_muestras[fase] >=
                    MUESTRAS_POR_ETAPA)
                {
                    estres_fase = fase + 1U;

                    /* Cambiar amplitud sin reiniciar PR */
                    if (fase == 0U)
                    {
                        amplitud_ref_A = 0.5f;
                    }

                    if (fase == 1U)
                    {
                        amplitud_ref_A = 1.0f;
                    }
                }
            }
            else if (fase == 3U)
            {
                /* Segunda prueba: falla negativa */
                estres_fase = 4U;
            }
            else
            {
                /* Finalizar despues de las dos fallas */
                estres_activo = 0;
                estres_terminado = 1;
            }
        }

        /* ====================================================
           MEDICION FINAL CON MCYCLE

           Incluye el procesamiento y gran parte de
           la instrumentacion de esta funcion.

           No incluye el prologo y epilogo completos
           del manejador externo de interrupciones.
           ==================================================== */

        u32 duracion =
            leer_mcycle() - inicio;

        u32 latencia_y_callback =
            latencia + duracion;

        if (duracion > estres_ciclos_max[fase])
        {
            estres_ciclos_max[fase] = duracion;
        }

        if (latencia_y_callback >
            estres_latum_max[fase])
        {
            estres_latum_max[fase] =
                latencia_y_callback;
        }
    }
}

/* ============================================================
   PREPARAR CONTROLADOR PR
   ============================================================ */

static void preparar_control(void)
{
    const float cw = 0.99996916f;
    const float sw = 0.00785390f;

    float s = 0.0f;
    float c = 1.0f;

    for (u32 k = 0; k < MUESTRAS_50HZ; k++)
    {
        seno[k] = s;

        float sig_s =
            s * cw + c * sw;

        c = c * cw - s * sw;
        s = sig_s;
    }

    pr_init(
        &pr,
        PR_B0,
        PR_B1,
        PR_B2,
        PR_A0,
        PR_A1,
        PR_A2,
        PWM_M_MAX * VDC
    );
}

/* ============================================================
   BLOQUEAR ARRANQUE FISICO
   ============================================================ */

static void iniciar_control(void)
{
#if !PR_CONFIGURADO

    xil_printf(
        "No se inicia: PR_CONFIGURADO = 0\r\n"
    );

#else

    /*
     * El arranque fisico NO esta implementado
     * en este programa temporal de diagnostico.
     */

    xil_printf(
        "Modo estres: control fisico no disponible\r\n"
    );

#endif
}

/* ============================================================
   DETENER SISTEMA
   ============================================================ */

static void detener(void)
{
    control_activo = 0;

    if (estres_activo)
    {
        estres_aborto = 3;
        estres_activo = 0;
        estres_terminado = 1;
    }

    pwm_deshabilitar();
    pwm_set_ref(0);
}

/* ============================================================
   INICIAR PRUEBA MAXIMA
   ============================================================ */

static void iniciar_estres(void)
{
    if (estres_activo ||
        estres_terminado ||
        control_activo ||
        pwm_esta_corriendo())
    {
        xil_printf(
            "No se inicia: PWM ocupado o prueba pendiente\r\n"
        );

        return;
    }

    /* Asegurar PWM apagado */
    pwm_deshabilitar();
    pwm_set_ref(0);

    /* Reiniciar controlador */
    pr_reiniciar(&pr);

    /* Reiniciar buffer */
    buffer_congelar(0);
    buffer_reiniciar();

    /* Reiniciar referencia */
    k_ref = 0;
    dec_buf = 0;

    amplitud_ref_A = 0.2f;

    falla_sobrecorriente = 0;

    /* Reiniciar contadores de etapas */
    for (u32 f = 0; f < ETAPAS_NORMALES; f++)
    {
        estres_muestras[f] = 0;
    }

    /* Reiniciar maximos temporales */
    for (u32 f = 0; f < ETAPAS_TOTALES; f++)
    {
        estres_ciclos_max[f] = 0;
        estres_latum_max[f] = 0;
    }

    estres_fase = 0;
    estres_aborto = 0;
    estres_total = 0;

    estres_falla_pos = 0;
    estres_falla_neg = 0;

    estres_brechas = 0;
    estres_posibles_perdidas = 0;

    estres_intervalo_min = 0xFFFFFFFFU;
    estres_intervalo_max = 0;

    estres_entrada_anterior = 0;

    xil_printf(
        "ESTRES: 10 s a 0.2 A + 10 s a 0.5 A + 10 s a 1.0 A\r\n"
    );

    xil_printf(
        "Luego fallas virtuales +1601 mA y -1601 mA\r\n"
    );

    xil_printf(
        "PWM deshabilitado durante toda la prueba\r\n"
    );

    /* Iniciar en la siguiente interrupcion */
    estres_activo = 1;
}

/* ============================================================
   MOSTRAR RESULTADOS
   ============================================================ */

static void imprimir_resultados(void)
{
    estres_terminado = 0;

    pwm_deshabilitar();
    pwm_set_ref(0);

    xil_printf(
        "=== ESTRES 40 kHz ===\r\n"
    );

    xil_printf(
        "Muestras totales: %u | Abortado: %u\r\n",
        (unsigned int)estres_total,
        (unsigned int)estres_aborto
    );

    /* Tres amplitudes */

    for (u32 f = 0; f < ETAPAS_NORMALES; f++)
    {
        xil_printf(
            "Etapa %u: muestras=%u mcycle_max=%u lat+callback_max=%u\r\n",
            (unsigned int)(f + 1U),
            (unsigned int)estres_muestras[f],
            (unsigned int)estres_ciclos_max[f],
            (unsigned int)estres_latum_max[f]
        );
    }

    /* Falla positiva */

    xil_printf(
        "Falla positiva: %u | mcycle=%u | lat+callback=%u\r\n",
        (unsigned int)estres_falla_pos,
        (unsigned int)estres_ciclos_max[3],
        (unsigned int)estres_latum_max[3]
    );

    /* Falla negativa */

    xil_printf(
        "Falla negativa: %u | mcycle=%u | lat+callback=%u\r\n",
        (unsigned int)estres_falla_neg,
        (unsigned int)estres_ciclos_max[4],
        (unsigned int)estres_latum_max[4]
    );

    /* Periodicidad de interrupciones */

    xil_printf(
        "Intervalo min/max mcycle: %u/%u\r\n",
        (unsigned int)estres_intervalo_min,
        (unsigned int)estres_intervalo_max
    );

    xil_printf(
        "Intervalos >2700: %u | >=3750 (posibles perdidas): %u\r\n",
        (unsigned int)estres_brechas,
        (unsigned int)estres_posibles_perdidas
    );

    /* Estado final */

    xil_printf(
        "Buffer: %u | PWM run=%d | periodo AXI=2500 ciclos\r\n",
        (unsigned int)buffer_cantidad(),
        pwm_esta_corriendo()
    );

    xil_printf(
        "=== FIN ESTRES ===\r\n"
    );

    pr_reiniciar(&pr);
}

/* ============================================================
   COMANDOS UART
   ============================================================ */

static void procesar_uart(void)
{
    if (XUartLite_IsReceiveEmpty(STDIN_BASEADDRESS))
    {
        return;
    }

    char c =
        (char)XUartLite_RecvByte(STDIN_BASEADDRESS);

    if (c == 's')
    {
        iniciar_control();
    }
    else if (c == 'x')
    {
        detener();

        xil_printf(
            "Prueba detenida\r\n"
        );
    }
    else if (c == 'c')
    {
        if (estres_activo ||
            estres_terminado ||
            pwm_esta_corriendo())
        {
            xil_printf(
                "No calibrar durante la prueba\r\n"
            );
        }
        else
        {
            u16 offset =
                xadc_calibrar_offset(200);

            xil_printf(
                "XADC calibrado | offset=%u\r\n",
                (unsigned int)offset
            );
        }
    }
    else if (c == 'i')
    {
        if (!estres_activo)
        {
            s32 mA =
                xadc_a_miliamperios(
                    xadc_leer_codigo()
                );

            xil_printf(
                "Corriente=%d mA\r\n",
                (int)mA
            );
        }
    }
    else if (c == 'n')
    {
        iniciar_estres();
    }
}

/* ============================================================
   INICIALIZACION GENERAL
   ============================================================ */

static int inicializar_sistema(void)
{
    int estado;

    /* Inicializar AXI Timer */

    estado = XTmrCtr_Initialize(
        &Timer,
        XPAR_XTMRCTR_0_BASEADDR
    );

    if (estado != XST_SUCCESS)
    {
        return XST_FAILURE;
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

    XTmrCtr_Start(&Timer, 0);

    /* GPIO */

    gpio_init(0x40000000U);

    /* XADC */

    if (xadc_init() != 0)
    {
        return XST_FAILURE;
    }

    /* PWM deshabilitado */

    pwm_init(0x00010000U);

    if (pwm_configurar(
        PWM_FREQ_SEL,
        TIEMPO_MUERTO_NS
    ) != 0)
    {
        return XST_FAILURE;
    }

    pwm_set_ref(0);

    /* PR */

    preparar_control();

    return XST_SUCCESS;
}

/* ============================================================
   MAIN
   ============================================================ */

int main(void)
{
    xil_printf(
        "Inicio: estres de control 40 kHz, PWM deshabilitado\r\n"
    );

    if (inicializar_sistema() != XST_SUCCESS)
    {
        xil_printf(
            "ERROR inicializacion\r\n"
        );

        return -1;
    }

    xil_printf(
        "Comandos: c=calibrar i=corriente n=ESTRES x=detener s=bloqueado\r\n"
    );

    u32 ultimo = 0;
    u32 botones_previos = 0;

    while (1)
    {
        /* ====================================================
           SUPERVISION DEL PWM
           ==================================================== */

        u32 estado = pwm_leer_estado();

        if (estres_activo && pwm_esta_corriendo())
        {
            estres_aborto = 2;
            estres_activo = 0;
            estres_terminado = 1;

            pwm_deshabilitar();
            pwm_set_ref(0);
        }

        if (estres_activo &&
            (estado & PWM_STAT_LATCHED))
        {
            estres_aborto = 2;
            estres_activo = 0;
            estres_terminado = 1;

            pwm_deshabilitar();
            pwm_set_ref(0);
        }

        /* ====================================================
           UART
           ==================================================== */

        procesar_uart();

        /* ====================================================
           BOTON FISICO DE PARADA
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
           RESULTADOS AL TERMINAR
           ==================================================== */

        if (estres_terminado)
        {
            imprimir_resultados();
        }

        /* ====================================================
           ESTADO GENERAL

           Evitar imprimir durante la prueba.
           ==================================================== */

        u32 actual = contador_interrupciones;

        if (!estres_activo &&
            (actual - ultimo) >= 40000U)
        {
            ultimo = actual;

            xil_printf(
                "Int=%u | PWM run=%d | est=0x%X\r\n",
                (unsigned int)actual,
                pwm_esta_corriendo(),
                (unsigned int)estado
            );
        }
    }

    return 0;
}
