/*
 * android_vibration.c - the title's rumble on the device's own vibrator.
 *
 * A handheld's built-in pad is an input device with no motors (the Retroid
 * Pocket 5's shows up as "Xbox Wireless Controller" and cannot rumble), so
 * the pad's rumble goes nowhere. The device has a vibrator of its own:
 * android.os.Vibrator, driven from here through JNI (no Java of the title's
 * is involved). The approach follows the design of Xerenge's Android build
 * (device vibration as a fallback for a pad without motors); the code is this
 * runtime's own.
 *
 * SDL2 cannot rumble a pad on Android at all (2.32.10's joystick driver:
 * ANDROID_JoystickRumble is SDL_Unsupported), so an external pad does not
 * rumble here, whatever it is; only a pad SDL's HIDAPI drivers have opened
 * (some USB pads) reports motors.
 *
 * RECOMP_ANDROID_VIBRATION = auto | controller | device | off (default auto):
 *   auto        the pad when SDL says it can rumble; else the device, when
 *               the pad is the device's own (pad_is_own: not external, or a
 *               motorless first or only pad, as a handheld's built-in one).
 *               An external pad without rumble gets nothing: the handheld
 *               does not shake on the table for a pad held elsewhere.
 *   controller  the pad only (what every other host does)
 *   device      the device only, whatever the pad; the pad is told to stop
 *   off         neither
 *
 * xbox_AndroidVibration() is called where the motors' levels reach the host
 * pad (xinput_device.c), after the rumble curve (input_core.c: peak, gate,
 * strength, release). It never blocks: it leaves an amplitude for a worker
 * thread, which makes the binder calls.
 *
 *   - Amplitude 0..255: the big motor in full, the small one at half weight.
 *     16 levels with one step of hysteresis (xbox_RumbleQuantise): the motor
 *     is given a new amplitude only when what is wanted is a whole level
 *     from what plays, so a level that wanders does not restart it.
 *   - Each amplitude is one long one-shot (5 s) that replaces what plays; it
 *     is renewed after 4 s only while the title keeps asking, so the motor
 *     ends by itself if the title stops calling (a pause, a crash).
 *   - No replacement sooner than 30 ms after the last; a pulse lasts at
 *     least 40 ms (the title's shortest rumbles are a frame or two).
 *   - Without amplitude control the motor is on above a threshold, else off.
 *   - The title's idle hum is silence by the rumble gate; before the gate
 *     RECOMP_ANDROID_VIBRATION_FLOOR was what did it, and it still sets the
 *     gate's speed when given (xinput_device.c).
 */
#if defined(__ANDROID__)

#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <SDL_system.h>

#include "rumble_curve.h"

enum { VIB_AUTO, VIB_CONTROLLER, VIB_DEVICE, VIB_OFF };

#define SHOT_MS       5000
#define RENEW_MS      4000
#define MIN_GAP_MS    30
#define MIN_PULSE_MS  40
#define ASK_STALE_MS  1000      /* the title re-sends a held rumble every 150 ms */
#define ONOFF_FLOOR   48        /* no amplitude control: on from here */

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv;
static int      s_started, s_mode = -1;
static int      s_want;             /* 0..255, as wanted (the worker quantises) */
static int      s_own_only;         /* auto: only for the device's own pad */
static int      s_vendor, s_product;    /* the first pad */
static uint64_t s_asked_ms;         /* the title's last call */
static unsigned s_sent, s_cancelled;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int mode(void)
{
    if (s_mode < 0) {
        const char *m = getenv("RECOMP_ANDROID_VIBRATION");
        s_mode = !m || !*m || !strcmp(m, "auto") ? VIB_AUTO
               : !strcmp(m, "controller") ? VIB_CONTROLLER
               : !strcmp(m, "device") ? VIB_DEVICE
               : !strcmp(m, "off") || !strcmp(m, "0") ? VIB_OFF : VIB_AUTO;
    }
    return s_mode;
}

/* Whether the pad with this USB vendor and product is the device's own.
 * android.view.InputDevice.isExternal() (API 29) is not enough: a handheld's
 * built-in pad is often a USB or virtual device inside the case and reports
 * itself external (the Retroid Pocket 5's "Xbox Wireless Controller"
 * 2022:3002: bus USB, IsExternal true), and auto mode then went silent on
 * the very device it was written for. So a pad counts as the device's own
 * when Android says it is not external, or when it has no vibrator of its
 * own (InputDevice.getVibrator().hasVibrator(); the pads Android can rumble,
 * an Xbox or DualShock pad over Bluetooth or USB, report one) and it is the
 * only pad or the first one (getControllerNumber() 1: a built-in pad is
 * there from boot, a pad paired later gets the next number). What this still
 * gets wrong: a single motorless pad on a phone without controls of its own
 * shakes the phone; Input > Vibration > Controller or Off is the answer
 * there. Its own when nothing says otherwise (no such device, an older
 * Android). Worker thread. */
static int pad_is_own(JNIEnv *env, int vendor, int product)
{
    jclass idc = (*env)->FindClass(env, "android/view/InputDevice");
    jclass vbc = (*env)->FindClass(env, "android/os/Vibrator");
    jmethodID ids_m, get_m, vid_m, pid_m, src_m, ext_m, num_m, gv_m, hv_m = NULL;
    jintArray ids;
    jint *id;
    jsize n, i;
    int own = 1, found = 0, pads = 0, ext = 0, padvib = 0, num = 0;

    (*env)->ExceptionClear(env);
    if (!idc)
        return 1;
    ids_m = (*env)->GetStaticMethodID(env, idc, "getDeviceIds", "()[I");
    get_m = (*env)->GetStaticMethodID(env, idc, "getDevice", "(I)Landroid/view/InputDevice;");
    vid_m = (*env)->GetMethodID(env, idc, "getVendorId", "()I");
    pid_m = (*env)->GetMethodID(env, idc, "getProductId", "()I");
    src_m = (*env)->GetMethodID(env, idc, "getSources", "()I");
    ext_m = (*env)->GetMethodID(env, idc, "isExternal", "()Z");
    num_m = (*env)->GetMethodID(env, idc, "getControllerNumber", "()I");
    gv_m = (*env)->GetMethodID(env, idc, "getVibrator", "()Landroid/os/Vibrator;");
    if (vbc)
        hv_m = (*env)->GetMethodID(env, vbc, "hasVibrator", "()Z");
    (*env)->ExceptionClear(env);
    ids = ids_m && get_m && vid_m && pid_m && src_m && ext_m
        ? (jintArray)(*env)->CallStaticObjectMethod(env, idc, ids_m) : NULL;
    if (ids && !(*env)->ExceptionCheck(env)) {
        n = (*env)->GetArrayLength(env, ids);
        id = (*env)->GetIntArrayElements(env, ids, NULL);
        for (i = 0; id && i < n; i++) {
            jobject d = (*env)->CallStaticObjectMethod(env, idc, get_m, id[i]);
            jint src;
            if (!d || (*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
                continue;
            }
            src = (*env)->CallIntMethod(env, d, src_m);
            /* SOURCE_GAMEPAD 0x401, SOURCE_JOYSTICK 0x1000010 */
            if ((src & 0x401) == 0x401 || (src & 0x1000010) == 0x1000010) {
                pads++;
                if (!found && (*env)->CallIntMethod(env, d, vid_m) == vendor
                        && (*env)->CallIntMethod(env, d, pid_m) == product) {
                    jobject v;
                    found = 1;
                    ext = (*env)->CallBooleanMethod(env, d, ext_m) ? 1 : 0;
                    num = num_m ? (*env)->CallIntMethod(env, d, num_m) : 0;
                    v = gv_m ? (*env)->CallObjectMethod(env, d, gv_m) : NULL;
                    if ((*env)->ExceptionCheck(env)) {
                        (*env)->ExceptionClear(env);
                        v = NULL;
                    }
                    padvib = v && hv_m && (*env)->CallBooleanMethod(env, v, hv_m) ? 1 : 0;
                    if (v)
                        (*env)->DeleteLocalRef(env, v);
                }
            }
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, d);
        }
        if (id)
            (*env)->ReleaseIntArrayElements(env, ids, id, JNI_ABORT);
    }
    (*env)->ExceptionClear(env);
    if (ids)
        (*env)->DeleteLocalRef(env, ids);
    if (vbc)
        (*env)->DeleteLocalRef(env, vbc);
    (*env)->DeleteLocalRef(env, idc);
    if (found)
        own = !ext || (!padvib && (pads == 1 || num == 1));
    fprintf(stderr, "  [RUMBLE] pad %04x:%04x is %s%s (Android: %s, %s, controller %d of %d)\n",
            vendor, product,
            own ? "this device's own: its rumble goes to the device's vibrator"
                : "an external pad: no rumble (SDL2 cannot rumble a pad on Android;"
                  " Input > Vibration > This device shakes the device anyway)",
            found ? "" : " (no answer from the input devices for it, taken as its own)",
            ext ? "external" : "not external", padvib ? "has a vibrator" : "no vibrator of its own",
            num, pads);
    return own;
}

/* The worker: owns every JNI object. */
static void *worker(void *arg)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();     /* attaches this thread */
    jobject act, vib = NULL, name;
    jclass cls, eff = NULL;
    jmethodID get, vibrate = NULL, cancel = NULL, one_shot = NULL, has, amp;
    int have = 0, has_amp = 0, playing = 0;
    int own = 1, own_vendor = -1, own_product = -1;
    uint64_t last_call = 0, pulse_at = 0, shot_at = 0;
    struct timespec until;

    (void)arg;
    pthread_setname_np(pthread_self(), "vibration");
    act = env ? (jobject)SDL_AndroidGetActivity() : NULL;
    if (act) {
        cls = (*env)->GetObjectClass(env, act);
        get = (*env)->GetMethodID(env, cls, "getSystemService",
                                  "(Ljava/lang/String;)Ljava/lang/Object;");
        name = (*env)->NewStringUTF(env, "vibrator");
        vib = get ? (*env)->CallObjectMethod(env, act, get, name) : NULL;
        if (vib && !(*env)->ExceptionCheck(env)) {
            jclass vc = (*env)->GetObjectClass(env, vib);
            vib = (*env)->NewGlobalRef(env, vib);
            has = (*env)->GetMethodID(env, vc, "hasVibrator", "()Z");
            amp = (*env)->GetMethodID(env, vc, "hasAmplitudeControl", "()Z");
            vibrate = (*env)->GetMethodID(env, vc, "vibrate", "(Landroid/os/VibrationEffect;)V");
            cancel = (*env)->GetMethodID(env, vc, "cancel", "()V");
            eff = (*env)->FindClass(env, "android/os/VibrationEffect");
            if (eff) {
                eff = (jclass)(*env)->NewGlobalRef(env, eff);
                one_shot = (*env)->GetStaticMethodID(env, eff, "createOneShot",
                                                     "(JI)Landroid/os/VibrationEffect;");
            }
            have = has && vibrate && cancel && one_shot
                && (*env)->CallBooleanMethod(env, vib, has);
            has_amp = have && amp && (*env)->CallBooleanMethod(env, vib, amp);
        }
        (*env)->ExceptionClear(env);
    }
    fprintf(stderr, "  [RUMBLE] device vibrator: %s%s\n", have ? "present" : "none",
            have ? (has_amp ? ", amplitude control" : ", on/off only (no amplitude control)") : "");
    if (!have)
        return NULL;

    pthread_mutex_lock(&s_mu);
    for (;;) {
        uint64_t t = now_ms();
        int want = s_want, level;

        if (s_own_only && (s_vendor != own_vendor || s_product != own_product)) {
            own_vendor = s_vendor;
            own_product = s_product;
            pthread_mutex_unlock(&s_mu);
            own = pad_is_own(env, own_vendor, own_product);
            pthread_mutex_lock(&s_mu);
            continue;
        }
        if (s_own_only && !own)
            want = 0;                           /* an external pad's rumble */
        if (want && t - s_asked_ms > ASK_STALE_MS)
            want = 0;                           /* the title went quiet */
        if (!has_amp)
            level = want >= ONOFF_FLOOR ? 255 : 0;
        else
            level = xbox_RumbleQuantise(playing, want);
        if (level != playing || (playing && t - shot_at >= RENEW_MS)) {
            uint64_t wait_until = last_call + MIN_GAP_MS;
            if (!level && playing && pulse_at + MIN_PULSE_MS > wait_until)
                wait_until = pulse_at + MIN_PULSE_MS;
            if (t < wait_until) {
                clock_gettime(CLOCK_MONOTONIC, &until);
                until.tv_nsec += (long)(wait_until - t) * 1000000L;
                until.tv_sec += until.tv_nsec / 1000000000L;
                until.tv_nsec %= 1000000000L;
                pthread_cond_timedwait(&s_cv, &s_mu, &until);
                continue;
            }
            pthread_mutex_unlock(&s_mu);
            if (level) {
                jobject e = (*env)->CallStaticObjectMethod(env, eff, one_shot,
                                (jlong)SHOT_MS, (jint)(has_amp ? (level < 1 ? 1 : level) : -1));
                if (e) {
                    (*env)->CallVoidMethod(env, vib, vibrate, e);
                    (*env)->DeleteLocalRef(env, e);
                }
                s_sent++;
            } else {
                (*env)->CallVoidMethod(env, vib, cancel);
                s_cancelled++;
            }
            (*env)->ExceptionClear(env);
            pthread_mutex_lock(&s_mu);
            last_call = now_ms();
            if (level) {
                shot_at = last_call;            /* this one-shot runs from here */
                if (!playing)
                    pulse_at = last_call;       /* the motor started */
            }
            playing = level;
            continue;
        }
        /* Nothing to do: until a change, the renewal, or the title going quiet. */
        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_nsec += (playing ? 100 : 1000) * 1000000L;
        until.tv_sec += until.tv_nsec / 1000000000L;
        until.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&s_cv, &s_mu, &until);
    }
    return NULL;
}

/* The first pad's motor levels (0..65535 each, after the rumble curve);
 * pad_can_rumble: SDL says the pad has motors; vendor, product: the pad's.
 * Returns 1 when the pad itself should rumble, 0 when it should be told to
 * stop. */
int xbox_AndroidVibration(unsigned left, unsigned right, int pad_can_rumble,
                          int vendor, int product)
{
    int m = mode(), device, q;

    if (m == VIB_OFF)
        return 0;
    if (m == VIB_CONTROLLER)
        return 1;
    device = m == VIB_DEVICE || !pad_can_rumble;
    q = device ? (int)((left >> 8) + (right >> 9)) : 0;    /* big in full, small at half */
    if (q > 255)
        q = 255;
    pthread_mutex_lock(&s_mu);
    if (!s_started) {
        pthread_t th;
        pthread_condattr_t ca;
        if (!device) {                  /* the pad rumbles, and no worker yet */
            pthread_mutex_unlock(&s_mu);
            return 1;
        }
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&s_cv, &ca);
        s_started = 1;
        if (pthread_create(&th, NULL, worker, NULL) == 0)
            pthread_detach(th);
    }
    s_asked_ms = now_ms();
    s_own_only = m == VIB_AUTO;
    s_vendor = vendor;
    s_product = product;
    if (q != s_want) {
        s_want = q;
        pthread_cond_signal(&s_cv);
    }
    pthread_mutex_unlock(&s_mu);
    return !device;
}

#else
typedef int xbox_android_vibration_unused;
#endif
