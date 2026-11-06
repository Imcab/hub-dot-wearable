// Fall detector against synthetic signals, runs on the PC: see test/run.sh
// These signals are invented. Passing here proves the logic, not the accuracy on real people
#include "fall_detect.hpp"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static constexpr float FS = 100.0f;
static constexpr float PI = 3.14159265f;

struct V { float x, y, z; };

// Rotation applied to every signal, as if the board sat differently in the armpit
struct Mount {
    float m[3][3];
    V apply(V v) const
    {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
};

static const Mount UPRIGHT = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
static const Mount SIDEWAYS = {{{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}}};
static const Mount SKEWED = {{{0.612f, -0.354f, 0.707f}, {0.707f, 0.612f, -0.354f}, {-0.354f, 0.707f, 0.612f}}};

// Trunk tilted by deg from vertical, gravity only
static V tilted(float deg)
{
    const float r = deg * PI / 180.0f;
    return {sinf(r), 0, cosf(r)};
}

static float noise(float amp) { return amp * (2.0f * rand() / RAND_MAX - 1.0f); }

struct Run {
    FallDetector d{FS};
    Mount mount;
    int impacts = 0, falls = 0, cancelled = 0;
    float t_fall = -1;
    int n = 0;

    explicit Run(const Mount &m) : mount(m) {}

    void feed(V v)
    {
        const V r = mount.apply(v);
        switch (d.addSample(r.x, r.y, r.z)) {
        case FallDetector::Event::Impact: impacts++; break;
        case FallDetector::Event::Fall: falls++; t_fall = n / FS; break;
        case FallDetector::Event::Cancelled: cancelled++; break;
        default: break;
        }
        n++;
    }

    void still(float seconds, float deg = 0)
    {
        for (int i = 0; i < seconds * FS; i++) {
            V g = tilted(deg);
            feed({g.x + noise(0.01f), g.y + noise(0.01f), g.z + noise(0.01f)});
        }
    }

    // Two steps per second, peaks near 1.7 g on the vertical axis
    void walk(float seconds, float deg = 0)
    {
        for (int i = 0; i < seconds * FS; i++) {
            const float s = sinf(2 * PI * 2.0f * i / FS);
            V g = tilted(deg);
            feed({g.x + 0.15f * s + noise(0.03f), g.y + noise(0.03f), g.z + 0.6f * s * s + noise(0.03f)});
        }
    }

    void freefall(float seconds)
    {
        for (int i = 0; i < seconds * FS; i++) feed({noise(0.1f), noise(0.1f), 0.3f});
    }

    // Half sine on the direction of gravity at the end of the movement
    void impact(float peak_g, float deg)
    {
        const int len = (int)(0.1f * FS);
        V g = tilted(deg);
        for (int i = 0; i < len; i++) {
            const float k = 1.0f + (peak_g - 1.0f) * sinf(PI * (i + 0.5f) / len);
            feed({g.x * k, g.y * k, g.z * k});
        }
    }

    // Moving on the floor: never still, posture stays changed
    void struggle(float seconds, float deg)
    {
        for (int i = 0; i < seconds * FS; i++) {
            V g = tilted(deg);
            const float k = 1.0f + 0.3f * sinf(2 * PI * 1.3f * i / FS);
            feed({g.x * k + noise(0.05f), g.y + noise(0.05f), g.z * k + noise(0.05f)});
        }
    }
};

static void standing_still(const Mount &m)
{
    Run r(m);
    r.still(120);
    assert(r.impacts == 0 && r.falls == 0);
}

static void walking(const Mount &m)
{
    Run r(m);
    r.still(5);
    r.walk(120);
    assert(r.impacts == 0 && r.falls == 0);
}

static void hard_sit(const Mount &m)
{
    Run r(m);
    r.still(5);
    r.impact(3.0f, 20);
    r.still(20, 20);
    assert(r.impacts == 1 && r.falls == 0 && r.cancelled == 1);
}

static void stumble_and_keep_walking(const Mount &m)
{
    Run r(m);
    r.walk(5);
    r.impact(3.2f, 0);
    r.walk(20);
    assert(r.impacts >= 1 && r.falls == 0 && r.cancelled >= 1);
}

// Catches themselves on the furniture, stays bent and moving for a while, then stands up.
// Deciding before the body settles would raise a false alarm here
static void stumble_and_recover(const Mount &m)
{
    Run r(m);
    r.walk(5);
    r.impact(3.2f, 60);
    r.struggle(4, 60);
    r.walk(20);
    assert(r.impacts >= 1 && r.falls == 0 && r.cancelled >= 1);
}

static void fall_then_still(const Mount &m)
{
    Run r(m);
    r.still(5);
    r.freefall(0.4f);
    r.impact(4.0f, 90);
    r.still(20, 90);
    assert(r.falls == 1);
    assert(r.t_fall < 5.5f + 6.0f); // alert within seconds of the impact
    assert(r.d.report().peak_g >= 3.9f);
    assert(r.d.report().angle_deg > 80.0f && r.d.report().angle_deg < 100.0f);
}

static void fall_with_no_free_fall(const Mount &m)
{
    Run r(m);
    r.walk(5);
    r.impact(3.0f, 80);
    r.still(20, 80);
    assert(r.falls == 1);
}

static void fall_then_struggling(const Mount &m)
{
    Run r(m);
    r.still(5);
    r.impact(4.0f, 90);
    r.struggle(20, 90);
    assert(r.falls == 1); // never still, the timeout has to catch it
}

static void one_alert_per_fall(const Mount &m)
{
    Run r(m);
    r.still(5);
    r.impact(4.0f, 90);
    r.still(10, 90);
    assert(r.falls == 1);
    r.still(5, 0);      // manages to sit up...
    r.impact(3.0f, 90); // ...and drops again, still inside the same event
    r.still(20, 90);
    assert(r.falls == 1);
}

static void second_fall_after_cooldown(const Mount &m)
{
    Run r(m);
    r.still(5);
    r.impact(4.0f, 90);
    r.still(60, 90);
    r.still(10, 0); // got up
    r.impact(4.0f, 90);
    r.still(20, 90);
    assert(r.falls == 2);
}

static void impact_during_warmup_is_ignored(const Mount &m)
{
    Run r(m);
    r.still(1);
    r.impact(4.0f, 90);
    r.still(20, 90);
    assert(r.impacts == 0 && r.falls == 0); // no gravity from before: blind spot of the first 2 s
}

// Documented miss: pinned here so a change in behaviour shows up
static void slow_slide_is_missed(const Mount &m)
{
    Run r(m);
    r.still(5);
    for (int deg = 0; deg <= 90; deg++) r.still(0.05f, (float)deg);
    r.still(20, 90);
    assert(r.impacts == 0 && r.falls == 0);
}

int main()
{
    const Mount mounts[] = {UPRIGHT, SIDEWAYS, SKEWED};
    for (const Mount &m : mounts) {
        standing_still(m);
        walking(m);
        hard_sit(m);
        stumble_and_keep_walking(m);
        stumble_and_recover(m);
        fall_then_still(m);
        fall_with_no_free_fall(m);
        fall_then_struggling(m);
        one_alert_per_fall(m);
        second_fall_after_cooldown(m);
        impact_during_warmup_is_ignored(m);
        slow_slide_is_missed(m);
    }
    puts("test_fall_detect OK");
    return 0;
}
