/* xnet_netsim test: see CMakeLists.txt. */
#include <stdio.h>
#include <string.h>

#include "xnet_netsim.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void parse(void)
{
    xn_cfg c;
    CHECK(!xn_parse(NULL, &c));
    CHECK(!xn_parse("", &c));
    CHECK(!xn_parse("delay", &c));
    CHECK(!xn_parse("delay=x", &c));
    CHECK(!xn_parse("speed=3", &c));
    CHECK(xn_parse("delay=150,", &c));          /* a trailing comma is fine */
    CHECK(!xn_parse("delay=150;jitter=1", &c));
    CHECK(!xn_parse("loss=101", &c));
    CHECK(!xn_parse("delay=0", &c));            /* asks for nothing: stays off */
    CHECK(!xn_parse("seed=4", &c));             /* the same */
    CHECK(xn_parse("delay=150,jitter=50,loss=2.5,seed=7", &c));
    CHECK(c.delay_ms == 150 && c.jitter_ms == 50 && c.loss_centi == 250 && c.seed == 7);
    CHECK(xn_parse("loss=2", &c));
    CHECK(c.delay_ms == 0 && c.loss_centi == 200 && c.seed == 1);
    CHECK(xn_parse("loss=0.25", &c) && c.loss_centi == 25);
}

/* off: nothing is held, nothing comes out */
static void off(void)
{
    xn_pkt p;
    CHECK(!xn_on());
    CHECK(xn_push(XN_OUT, 1, "a", 1, 2, 3, 0) == XN_DROPPED);
    CHECK(!xn_pop(1000, &p));
    CHECK(!xn_held());
}

/* half the round trip on each direction, and every field comes back */
static void delay(void)
{
    xn_cfg c;
    xn_pkt p;
    CHECK(xn_parse("delay=150", &c) && xn_start(&c));
    CHECK(xn_on());
    CHECK(xn_push(XN_OUT, 9, "abc", 3, 0x01020304u, 777, 1000) == XN_HELD);
    CHECK(xn_held() == 1);
    CHECK(!xn_pop(1074, &p));
    CHECK(xn_pop(1075, &p));
    CHECK(p.dir == XN_OUT && p.sock == 9 && p.ip == 0x01020304u && p.port == 777 && p.n == 3 && !memcmp(p.data, "abc", 3));
    CHECK(!xn_held());
    CHECK(!xn_pop(5000, &p));
    /* what is due goes first, whatever order it came in */
    CHECK(xn_push(XN_IN, 1, "x", 1, 0, 0, 200) == XN_HELD);
    CHECK(xn_push(XN_IN, 1, "y", 1, 0, 0, 100) == XN_HELD);
    CHECK(xn_pop(1000, &p) && p.data[0] == 'y');
    CHECK(xn_pop(1000, &p) && p.data[0] == 'x');
    xn_stop();
    CHECK(!xn_on());
}

/* the spread stays in +/-jitter/2 of delay/2 and uses the range */
static void jitter(void)
{
    xn_cfg c;
    xn_pkt p;
    int i, lo = 1 << 30, hi = -1;
    CHECK(xn_parse("delay=300,jitter=50,seed=3", &c) && xn_start(&c));
    for (i = 0; i < 500; i++) {
        int d;
        CHECK(xn_push(XN_OUT, 1, "a", 1, 0, 0, 0) == XN_HELD);
        CHECK(xn_pop(1000, &p));
        d = (int)p.due;
        if (d < lo) lo = d;
        if (d > hi) hi = d;
    }
    CHECK(lo >= 125 && hi <= 175);
    CHECK(lo <= 130 && hi >= 170);
    xn_stop();
    /* jitter over the delay never gives a negative wait */
    CHECK(xn_parse("delay=10,jitter=100", &c) && xn_start(&c));
    for (i = 0; i < 200; i++) {
        CHECK(xn_push(XN_OUT, 1, "a", 1, 0, 0, 1000) == XN_HELD);
        CHECK(xn_pop(2000, &p) && p.due >= 1000);
    }
    xn_stop();
}

/* the share lost is the share asked for, and repeats with the seed */
static int lost_of(const char *spec, int n)
{
    xn_cfg c;
    xn_pkt p;
    int i, got = 0;
    CHECK(xn_parse(spec, &c) && xn_start(&c));
    for (i = 0; i < n; i++) {
        if (xn_push(XN_OUT, 1, "a", 1, 0, 0, 0) == XN_HELD && xn_pop(0, &p))
            got++;
    }
    xn_stop();
    return n - got;
}

static void loss(void)
{
    int l = lost_of("loss=2,seed=5", 20000);
    CHECK(l > 300 && l < 500);
    CHECK(l == lost_of("loss=2,seed=5", 20000));
    CHECK(lost_of("loss=100", 500) == 500);
    CHECK(lost_of("delay=1,jitter=0", 500) == 0);
}

/* a full queue and a datagram over the limit are lost, not fatal */
static void limits(void)
{
    xn_cfg c;
    xn_pkt p;
    static uint8_t big[XN_MAX_DGRAM + 1];
    uint64_t pass, lost, over;
    int i, held = 0;
    CHECK(xn_parse("delay=1000", &c) && xn_start(&c));
    CHECK(xn_push(XN_OUT, 1, big, sizeof big, 0, 0, 0) == XN_DROPPED);
    CHECK(xn_push(XN_OUT, 1, big, 0, 0, 0, 0) == XN_DROPPED);
    CHECK(xn_push(XN_OUT, 1, big, XN_MAX_DGRAM, 0, 0, 0) == XN_HELD);
    while (xn_pop(5000, &p))
        ;
    for (i = 0; i < 1100; i++)
        held += xn_push(XN_OUT, 1, "a", 1, 0, 0, 0) == XN_HELD;
    CHECK(held == 1024 && xn_held() == 1024);
    xn_counts(&pass, &lost, &over);
    CHECK(pass == 1 && lost == 0 && over == 76);
    xn_stop();
    CHECK(!xn_held());
}

int main(void)
{
    parse();
    off();
    delay();
    jitter();
    loss();
    limits();
    printf("xnet_netsim: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
