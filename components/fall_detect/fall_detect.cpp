#include "fall_detect.hpp"

#include <math.h>

static constexpr float GRAVITY_TAU_S = 0.5f;
static constexpr float HISTORY_PERIOD_S = 0.1f;
static constexpr float RAD_TO_DEG = 57.29578f;

FallDetector::FallDetector(float sample_rate_hz)
    : fs_(sample_rate_hz),
      history_period_((uint32_t)(HISTORY_PERIOD_S * sample_rate_hz)),
      settle_samples_((uint32_t)(SETTLE_S * sample_rate_hz)),
      window_samples_((uint32_t)(STILL_WINDOW_S * sample_rate_hz)),
      watch_samples_((uint32_t)(MAX_WATCH_S * sample_rate_hz)),
      cooldown_samples_((uint32_t)(COOLDOWN_S * sample_rate_hz))
{
    reset();
}

void FallDetector::reset()
{
    enter(State::Idle);
    have_gravity_ = false;
    history_idx_ = history_count_ = 0;
    history_n_ = 0;
    peak_g_ = 0;
    report_ = {};
}

void FallDetector::enter(State s)
{
    state_ = s;
    state_n_ = 0;
    resetWindow();
}

void FallDetector::resetWindow()
{
    sum_ = {};
    sum_mag_ = sum_mag2_ = 0;
    window_n_ = 0;
}

void FallDetector::trackGravity(const Vec &a)
{
    if (!have_gravity_) {
        gravity_ = a;
        have_gravity_ = true;
    }
    const float k = 1.0f / (GRAVITY_TAU_S * fs_);
    gravity_.x += k * (a.x - gravity_.x);
    gravity_.y += k * (a.y - gravity_.y);
    gravity_.z += k * (a.z - gravity_.z);

    if (++history_n_ >= history_period_) {
        history_n_ = 0;
        history_[history_idx_] = gravity_;
        history_idx_ = (history_idx_ + 1) % HISTORY_LEN;
        if (history_count_ < HISTORY_LEN) history_count_++;
    }
}

FallDetector::Event FallDetector::decide(const Vec &mean)
{
    const float dot = reference_.x * mean.x + reference_.y * mean.y + reference_.z * mean.z;
    const float norms = sqrtf(reference_.x * reference_.x + reference_.y * reference_.y + reference_.z * reference_.z)
                      * sqrtf(mean.x * mean.x + mean.y * mean.y + mean.z * mean.z);
    float c = (norms > 0) ? dot / norms : 1.0f;
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;

    report_.peak_g = peak_g_;
    report_.angle_deg = acosf(c) * RAD_TO_DEG;

    if (report_.angle_deg >= MIN_ANGLE_DEG) {
        enter(State::Cooldown);
        return Event::Fall;
    }
    enter(State::Idle);
    return Event::Cancelled;
}

FallDetector::Event FallDetector::addSample(float ax, float ay, float az)
{
    const Vec a = {ax, ay, az};
    const float mag = sqrtf(ax * ax + ay * ay + az * az);

    trackGravity(a);
    state_n_++;

    switch (state_) {
    case State::Idle:
        // Without the full history there is no gravity from before the impact to compare with
        if (mag > IMPACT_G && history_count_ == HISTORY_LEN) {
            reference_ = history_[history_idx_]; // the oldest entry
            peak_g_ = mag;
            enter(State::Settling);
            return Event::Impact;
        }
        break;

    case State::Settling:
        if (mag > peak_g_) peak_g_ = mag;
        if (state_n_ >= settle_samples_) enter(State::Watching);
        break;

    case State::Watching: {
        if (mag > peak_g_) peak_g_ = mag;
        sum_.x += ax;
        sum_.y += ay;
        sum_.z += az;
        sum_mag_ += mag;
        sum_mag2_ += (double)mag * mag;
        window_n_++;

        if (window_n_ >= window_samples_) {
            const float n = (float)window_n_;
            const Vec mean = {sum_.x / n, sum_.y / n, sum_.z / n};
            const double m = sum_mag_ / window_n_;
            const double var = sum_mag2_ / window_n_ - m * m;
            const bool still = sqrt(var > 0 ? var : 0) < STILL_STD_G;

            // Out of time counts too: someone struggling on the floor never gets still,
            // and someone who walked away is upright again, so the angle still tells them apart
            if (still || state_n_ >= watch_samples_) return decide(mean);
            resetWindow();
        }
        break;
    }

    case State::Cooldown:
        if (state_n_ >= cooldown_samples_) enter(State::Idle);
        break;
    }
    return Event::None;
}
