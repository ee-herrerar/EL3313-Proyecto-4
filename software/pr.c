#include "pr.h"

void pr_init(pr_t *c, float b0, float b1, float b2,
             float a0, float a1, float a2, float umax)
{
    float inv = 1.0f / a0;
    c->b0 = b0 * inv;
    c->b1 = b1 * inv;
    c->b2 = b2 * inv;
    c->a1 = a1 * inv;
    c->a2 = a2 * inv;
    c->u_max = umax;
    pr_reiniciar(c);
}

void pr_reiniciar(pr_t *c)
{
    c->e1 = c->e2 = 0.0f;
    c->u1 = c->u2 = 0.0f;
}

/* u[k] = b0 e[k] + b1 e[k-1] + b2 e[k-2] - a1 u[k-1] - a2 u[k-2] */
float pr_actualizar(pr_t *c, float ref, float medida)
{
    float e = ref - medida;

    float u = c->b0 * e + c->b1 * c->e1 + c->b2 * c->e2
            - c->a1 * c->u1 - c->a2 * c->u2;

    /* Saturación; se guarda el valor saturado como anti-windup simple */

    if (u >  c->u_max) u =  c->u_max;
    if (u < -c->u_max) u = -c->u_max;

    return u;
}