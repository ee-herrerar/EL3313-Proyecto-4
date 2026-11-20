
#ifndef PWM_H
#define PWM_H

#include "xil_types.h"

#define PWM_REG_CTRL   0x00
#define PWM_REG_REF    0x04
#define PWM_REG_DEAD   0x08
#define PWM_REG_STAT   0x0C

#define PWM_CTRL_ENABLE    (1u << 0)
#define PWM_CTRL_RUNNING   (1u << 3)
#define PWM_STAT_LATCHED   (1u << 0)
#define PWM_STAT_FAULT_IN  (1u << 1)

/* Guarda la dirección base y deja el PWM deshabilitado. */
void pwm_init(u32 base_addr);

/* Configura frecuencia (0, 1 o 2), tiempo muerto en ns y referencia 0.*/
int pwm_configurar(u8 freq_sel, u32 deadtime_ns);

void pwm_habilitar(void);
void pwm_deshabilitar(void);
int  pwm_esta_corriendo(void);

/* Escritura directa del registro REF (Q15 con signo). */
void pwm_set_ref(s16 ref);

/* m en -1.0 .. +1.0 (Vab / Vdc). Se limita a +-PWM_M_MAX por seguridad. */
void pwm_escribir_duty(float m);

/* Estado de falla. */
u32  pwm_leer_estado(void);
void pwm_reiniciar_falla(void);

/* Límite de |m|. */
#define PWM_M_MAX  0.95f

#endif /* PWM_H */