/**
 * rumble_curve -- what a title's motor speed becomes on a host motor.
 *
 * Pure arithmetic, no platform and no clock: input_core.c owns the per-port
 * state and the time base, android_vibration.c the device's amplitude steps,
 * tests/input_rumble checks the numbers.
 *
 *   u    = min(1, speed / peak)       peak: the title's own maximum (a host
 *                                     option; 65535 unless the title says)
 *   gate : 0 while speed < 800; once off, on again only from 1200
 *   out  = u ^ (1 / strength)         1.0 = linear, the console's own;
 *                                     0 = off; above 1 lifts the low speeds
 *   time : a rise is immediate, a fall follows down exponentially with a
 *          release of about 90 ms
 *
 * The strength is the exponent, never a multiplier before a clip: full stays
 * the only speed that reaches full, so an impact and its decay stay apart at
 * any strength. The gate stands for the start-up threshold of the Duke's
 * eccentric-mass motors, which did not turn at an engine hum of 1 % of full;
 * the release for their spin-down.
 */
#ifndef XBOX_RUMBLE_CURVE_H
#define XBOX_RUMBLE_CURVE_H

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_RUMBLE_PEAK        65535.0
#define XBOX_RUMBLE_GATE_OFF    800u
#define XBOX_RUMBLE_GATE_ON     1200u
#define XBOX_RUMBLE_RELEASE_MS  90.0
/* A fall this close to where it is going is there. */
#define XBOX_RUMBLE_SETTLE      0.02

typedef struct xbox_rumble_params {
    double   peak;              /* the speed that is full */
    double   strength;          /* the exponent's inverse; 0 = off */
    unsigned gate_off, gate_on; /* speeds; gate_on 0 = no gate */
} xbox_rumble_params;

#define XBOX_RUMBLE_PARAMS_DEFAULT \
    { XBOX_RUMBLE_PEAK, 1.0, XBOX_RUMBLE_GATE_OFF, XBOX_RUMBLE_GATE_ON }

/* One motor: the gate's state and the envelope's level. Zero is idle. */
typedef struct xbox_rumble_motor {
    int    open;
    double level;               /* 0..1, what the host motor is given */
} xbox_rumble_motor;

/* The curve alone: the motor fraction 0..1 of a speed, no gate, no time. */
double xbox_RumbleCurve(unsigned speed, double peak, double strength);

/* The gate: 1 when the speed passes. *open is its state. */
int xbox_RumbleGate(int *open, unsigned speed, unsigned gate_off, unsigned gate_on);

/* The motor's level dt_ms after the last call, the title asking for speed
 * since then. dt_ms 0 takes a new speed: a rise at once, a fall not yet. */
double xbox_RumbleStep(xbox_rumble_motor *m, unsigned speed,
                       const xbox_rumble_params *p, double dt_ms);

/* A device vibrator's amplitude (android_vibration.c): 16 levels of 16 up to
 * 255, with one step of hysteresis. amp is what is wanted (0..255), playing
 * what the motor has; the answer is playing unless amp is a whole step away
 * from it. 0 is always 0, and a wanted amplitude is never rounded to 0. */
int xbox_RumbleQuantise(int playing, int amp);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_RUMBLE_CURVE_H */
