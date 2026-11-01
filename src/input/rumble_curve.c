/**
 * rumble_curve -- see rumble_curve.h.
 */
#include "rumble_curve.h"

#include <math.h>

double xbox_RumbleCurve(unsigned speed, double peak, double strength)
{
    double u;

    if (!speed || !(strength > 0.0))
        return 0.0;
    if (!(peak >= 1.0))
        peak = XBOX_RUMBLE_PEAK;
    u = (double)speed / peak;
    if (u >= 1.0)
        return 1.0;
    return pow(u, 1.0 / strength);
}

int xbox_RumbleGate(int *open, unsigned speed, unsigned gate_off, unsigned gate_on)
{
    if (!gate_on)
        *open = speed != 0;
    else if (speed < gate_off)
        *open = 0;
    else if (speed >= gate_on)
        *open = 1;
    return *open;
}

double xbox_RumbleStep(xbox_rumble_motor *m, unsigned speed,
                       const xbox_rumble_params *p, double dt_ms)
{
    double want = xbox_RumbleGate(&m->open, speed, p->gate_off, p->gate_on)
                ? xbox_RumbleCurve(speed, p->peak, p->strength) : 0.0;

    if (want >= m->level) {
        m->level = want;
    } else if (dt_ms > 0.0) {
        m->level = want + (m->level - want) * exp(-dt_ms / XBOX_RUMBLE_RELEASE_MS);
        if (m->level - want < XBOX_RUMBLE_SETTLE)
            m->level = want;
    }
    return m->level;
}

int xbox_RumbleQuantise(int playing, int amp)
{
    int q;

    if (amp <= 0)
        return 0;
    if (amp > 255)
        amp = 255;
    if (playing > 0 && amp - playing < 16 && playing - amp < 16)
        return playing;
    q = (amp + 8) / 16 * 16;
    if (q < 16)
        q = 16;
    return q > 255 ? 255 : q;
}
