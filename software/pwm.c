/*
 * pwm.c
 * Ver pwm.h.
 */
#include "pwm.h"
#include "xil_io.h"

#define PWM_CLK_NS  10u          /* reloj de 100 MHz: 10 ns por ciclo */

static u32 pwm_base = 0;
static u32 pwm_freq_sel = 0;     /* copia local de bits [2:1] de CTRL */

static void escribir(u32 offset, u32 valor)
{
    Xil_Out32(pwm_base + offset, valor);
}

static u32 leer(u32 offset)
{
    return Xil_In32(pwm_base + offset);
}

void pwm_init(u32 base_addr)
{
    pwm_base = base_addr;
    pwm_freq_sel = 0;
    escribir(PWM_REG_CTRL, 0);   /* deshabilitado */
}

int pwm_configurar(u8 freq_sel, u32 deadtime_ns)
{
    u32 ciclos = deadtime_ns / PWM_CLK_NS;

    if (freq_sel > 2) {
        return -1;
    }
    if (ciclos < 1)   ciclos = 1;      /* el hardware también limita a 1..100 */
    if (ciclos > 100) ciclos = 100;

    pwm_freq_sel = freq_sel;
    escribir(PWM_REG_DEAD, ciclos);
    escribir(PWM_REG_REF, 0);
    escribir(PWM_REG_CTRL, (pwm_freq_sel << 1));   /* sin enable */
    return 0;
}

void pwm_habilitar(void)
{
    escribir(PWM_REG_CTRL, (pwm_freq_sel << 1) | PWM_CTRL_ENABLE);
}

void pwm_deshabilitar(void)
{
    escribir(PWM_REG_CTRL, (pwm_freq_sel << 1));
}

int pwm_esta_corriendo(void)
{
    return (leer(PWM_REG_CTRL) & PWM_CTRL_RUNNING) ? 1 : 0;
}

void pwm_set_ref(s16 ref)
{
    /* El hardware toma los 16 bits bajos como número con signo. */
    escribir(PWM_REG_REF, (u32)(u16)ref);
}

void pwm_escribir_duty(float m)
{
    if (m >  PWM_M_MAX) m =  PWM_M_MAX;
    if (m < -PWM_M_MAX) m = -PWM_M_MAX;
    pwm_set_ref((s16)(m * 32767.0f));
}

u32 pwm_leer_estado(void)
{
    return leer(PWM_REG_STAT);
}

void pwm_reiniciar_falla(void)
{
    /* El hardware borra la falla retenida al escribir enable = 0 mientras la
     * entrada de falla está en 0. */
    pwm_deshabilitar();
}