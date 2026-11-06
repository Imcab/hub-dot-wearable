// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef FALL_DETECT_HPP
#define FALL_DETECT_HPP

#include <stdint.h>

// Detects a fall from the accelerometer alone, sample by sample, with addSample().
//
// A fall is an impact followed by a lasting change of posture:
//   1. Impact: the acceleration magnitude passes IMPACT_G
//   2. Settling: the body comes to rest, this part is ignored
//   3. Watching: wait until the body is still, or until MAX_WATCH_S runs out
//   4. Decision: angle between gravity before the impact and gravity now
//
// Only the change of gravity counts, never its direction, so the way the board sits in the
// armpit does not matter and it needs no calibration.
//
// The thresholds are starting points, not measured values: tune them with recordings of the
// board being worn. Known misses: a slow slide to the floor (no impact) and a fall that ends
// close to the original posture, such as slumping against a wall.
class FallDetector {
public:
    enum class Event {
        None,
        Impact,     // an impact started the analysis, it may still be nothing
        Fall,       // the posture changed after the impact: raise the alert
        Cancelled,  // the posture did not change, it was not a fall
    };

    // What the last Fall or Cancelled was based on
    struct Report {
        float peak_g;
        float angle_deg;
    };

    static constexpr float IMPACT_G = 2.5f;         // walking stays under 2 g on the trunk
    static constexpr float MIN_ANGLE_DEG = 45.0f;   // sitting down leans the trunk less than this
    static constexpr float STILL_STD_G = 0.08f;     // spread of the magnitude of a body at rest
    static constexpr float SETTLE_S = 1.5f;
    static constexpr float STILL_WINDOW_S = 2.0f;
    static constexpr float MAX_WATCH_S = 10.0f;
    static constexpr float COOLDOWN_S = 30.0f;      // one fall must not raise several alerts

    explicit FallDetector(float sample_rate_hz);

    // Feeds one sample in g
    Event addSample(float ax, float ay, float az);

    Report report() const { return report_; }

    // Forgets everything (e.g. after restarting the sensor)
    void reset();

private:
    enum class State { Idle, Settling, Watching, Cooldown };

    struct Vec {
        float x, y, z;
    };

    static constexpr int HISTORY_LEN = 20;  // 2 s of gravity, one entry every 100 ms

    float fs_;
    uint32_t history_period_;   // samples between history entries
    uint32_t settle_samples_;
    uint32_t window_samples_;
    uint32_t watch_samples_;
    uint32_t cooldown_samples_;

    State state_ = State::Idle;
    uint32_t state_n_ = 0;      // samples spent in the current state

    // Gravity follows the posture slowly. The history keeps it from before the impact, when the
    // current value is already disturbed by the fall itself
    bool have_gravity_ = false;
    Vec gravity_ = {};
    Vec history_[HISTORY_LEN] = {};
    int history_idx_ = 0;
    int history_count_ = 0;
    uint32_t history_n_ = 0;

    Vec reference_ = {};        // gravity before the impact
    float peak_g_ = 0;

    // Current window of the Watching state
    Vec sum_ = {};
    double sum_mag_ = 0, sum_mag2_ = 0;
    uint32_t window_n_ = 0;

    Report report_ = {};

    void trackGravity(const Vec &a);
    void enter(State s);
    void resetWindow();
    Event decide(const Vec &mean);
};

#endif
