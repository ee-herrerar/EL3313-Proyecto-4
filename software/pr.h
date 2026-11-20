
#ifndef PR_H
#define PR_H

typedef struct {
    /* coeficientes ya normalizados por a0 */
    float b0, b1, b2;
    float a1, a2;
    /* historia: errores y salidas anteriores */
    float e1, e2;
    float u1, u2;
    /* saturación de la salida: -u_max .. +u_max (voltios) */
    float u_max;
} pr_t;

/* Guarda los coeficientes (a0 distinto de 0) y pone la historia en cero. */
void pr_init(pr_t *c,
             float b0, float b1, float b2,
             float a0, float a1, float a2,
             float u_max);

/* Pone la historia en cero (usar al habilitar/deshabilitar el control). */
void pr_reiniciar(pr_t *c);

/* Un paso del controlador. ref y medida en amperios; devuelve voltios. */
float pr_actualizar(pr_t *c, float ref, float medida);

#endif /* PR_H */