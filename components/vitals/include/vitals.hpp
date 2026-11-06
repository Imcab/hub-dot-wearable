// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef VITALS_HPP
#define VITALS_HPP

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// One second of sensor readings
struct Readings {
    bool finger;
    bool hr_valid;
    bool spo2_valid;
    bool temp_valid;
    uint32_t ir;    // mean raw value of the last second
    uint32_t red;
    float temp_c;   // MAX30102 die temperature
    float bpm;
    float spo2;
};

// "31.44" without printf %f (two decimals, handles negatives)
inline void fmt_centi(char *buf, size_t len, float v)
{
    long c = lroundf(v * 100.0f);
    const char *sign = (c < 0) ? "-" : "";
    if (c < 0) c = -c;
    snprintf(buf, len, "%s%ld.%02ld", sign, c / 100, c % 100);
}

// Readings older than this are not reported: the sensor is stuck, restarting or gone.
// One report per second, so this is three missed reports
static constexpr long READINGS_STALE_MS = 3000;

// Writes the readings as JSON. age_ms = time since they were taken, negative if there are none yet.
// Without fresh readings every measured field is null: an old value must never look like a live one.
// Returns what snprintf returns
inline int readings_to_json(char *buf, size_t len, const Readings &r, long age_ms)
{
    const bool fresh = age_ms >= 0 && age_ms <= READINGS_STALE_MS;

    char age[24] = "null", ir[16] = "null", red[16] = "null", finger[8] = "null";
    char temp[16] = "null", bpm[16] = "null", spo2[16] = "null";
    if (age_ms >= 0) snprintf(age, sizeof(age), "%ld", age_ms);
    if (fresh) {
        snprintf(ir, sizeof(ir), "%lu", (unsigned long)r.ir);
        snprintf(red, sizeof(red), "%lu", (unsigned long)r.red);
        snprintf(finger, sizeof(finger), "%s", r.finger ? "true" : "false");
        if (r.temp_valid) fmt_centi(temp, sizeof(temp), r.temp_c);
        if (r.hr_valid) snprintf(bpm, sizeof(bpm), "%d", (int)lroundf(r.bpm));
        if (r.spo2_valid) snprintf(spo2, sizeof(spo2), "%d", (int)lroundf(r.spo2));
    }
    return snprintf(buf, len,
                    "{\"sensor_ok\":%s,\"age_ms\":%s,\"ir\":%s,\"red\":%s,\"temp_c\":%s,"
                    "\"finger\":%s,\"bpm\":%s,\"spo2\":%s}\n",
                    fresh ? "true" : "false", age, ir, red, temp, finger, bpm, spo2);
}

// State of the dot board, served by its HTTP server
struct DotReadings {
    bool temp_stable;   // armpit temperature reached a plateau, see AxillaryTemp
    float temp_c;
    uint32_t falls;     // falls detected since boot
};

// The accelerometer delivers every 100 ms, the thermometer every 30 s
static constexpr long MOTION_STALE_MS = 2000;
static constexpr long TEMP_STALE_MS = 90000;

// age = time since the last good data of each sensor, negative if there has been none yet.
// Same rule as readings_to_json: a value that is not fresh is null
inline int dot_readings_to_json(char *buf, size_t len, const DotReadings &r, long motion_age_ms, long temp_age_ms)
{
    const bool motion_ok = motion_age_ms >= 0 && motion_age_ms <= MOTION_STALE_MS;
    const bool temp_ok = temp_age_ms >= 0 && temp_age_ms <= TEMP_STALE_MS;

    char temp[16] = "null", age[24] = "null";
    if (temp_age_ms >= 0) snprintf(age, sizeof(age), "%ld", temp_age_ms);
    if (temp_ok) fmt_centi(temp, sizeof(temp), r.temp_c);

    return snprintf(buf, len,
                    "{\"motion_ok\":%s,\"temp_ok\":%s,\"temp_c\":%s,\"temp_stable\":%s,"
                    "\"temp_age_ms\":%s,\"falls\":%lu}\n",
                    motion_ok ? "true" : "false", temp_ok ? "true" : "false", temp,
                    (temp_ok && r.temp_stable) ? "true" : "false", age, (unsigned long)r.falls);
}

// Body of the fall alert. event_id is the same on every retry, so the receiver can drop duplicates.
// age_ms = time since the fall: there is no clock, the receiver gets the time by subtracting it
inline int fall_alert_to_json(char *buf, size_t len, const char *device, const char *event_id,
                              long age_ms, float peak_g, float angle_deg)
{
    char peak[16];
    fmt_centi(peak, sizeof(peak), peak_g);
    return snprintf(buf, len,
                    "{\"type\":\"fall\",\"device\":\"%s\",\"event_id\":\"%s\",\"age_ms\":%ld,"
                    "\"peak_g\":%s,\"angle_deg\":%d}\n",
                    device, event_id, age_ms, peak, (int)lroundf(angle_deg));
}

// Tells when the armpit temperature can be trusted. A sensor that just touched the skin, or an
// open arm, reads low and keeps moving: the value only counts after a plateau at body level
class AxillaryTemp {
public:
    static constexpr int WINDOW = 10;                // readings, 5 minutes at one every 30 s
    static constexpr float MAX_SPREAD_C = 0.2f;
    static constexpr float MIN_BODY_C = 34.0f;
    static constexpr float MAX_BODY_C = 42.0f;

    void add(float c)
    {
        readings_[idx_] = c;
        idx_ = (idx_ + 1) % WINDOW;
        if (count_ < WINDOW) count_++;
    }

    bool stable() const
    {
        if (count_ < WINDOW) return false;
        float lo = readings_[0], hi = readings_[0];
        for (int i = 1; i < WINDOW; i++) {
            if (readings_[i] < lo) lo = readings_[i];
            if (readings_[i] > hi) hi = readings_[i];
        }
        return hi - lo <= MAX_SPREAD_C && lo >= MIN_BODY_C && hi <= MAX_BODY_C;
    }

    void reset() { idx_ = count_ = 0; }

private:
    float readings_[WINDOW] = {};
    int idx_ = 0;
    int count_ = 0;
};

#endif
