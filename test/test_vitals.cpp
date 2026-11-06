// JSON and temperature logic, runs on the PC: see test/run.sh
#include "vitals.hpp"

#include <assert.h>
#include <string.h>

static char b[256];
static bool has(const char *s) { return strstr(b, s) != nullptr; }

static void test_hub_json()
{
    Readings r{};
    r.finger = r.hr_valid = r.spo2_valid = r.temp_valid = true;
    r.ir = r.red = 262143;
    r.temp_c = -12.5f;
    r.bpm = 71.6f;
    r.spo2 = 97.4f;

    readings_to_json(b, sizeof b, r, 900);
    assert(has("\"sensor_ok\":true") && has("\"bpm\":72") && has("\"spo2\":97") && has("\"temp_c\":-12.50"));

    readings_to_json(b, sizeof b, r, READINGS_STALE_MS);
    assert(has("\"sensor_ok\":true"));

    readings_to_json(b, sizeof b, r, READINGS_STALE_MS + 1);
    assert(has("\"sensor_ok\":false") && has("\"age_ms\":3001") && has("\"bpm\":null") && has("\"spo2\":null")
           && has("\"finger\":null") && has("\"ir\":null") && has("\"red\":null") && has("\"temp_c\":null"));

    readings_to_json(b, sizeof b, Readings{}, -1);
    assert(has("\"sensor_ok\":false") && has("\"age_ms\":null") && has("\"bpm\":null"));

    Readings none{};
    none.ir = 100;
    readings_to_json(b, sizeof b, none, 100);
    assert(has("\"sensor_ok\":true") && has("\"finger\":false") && has("\"bpm\":null"));

    assert(readings_to_json(b, 192, r, 2147483647L) < 192); // the size the hub uses
}

static void test_dot_json()
{
    DotReadings r{};
    r.temp_c = 36.5f;
    r.temp_stable = true;
    r.falls = 2;

    dot_readings_to_json(b, sizeof b, r, 100, 1000);
    assert(has("\"motion_ok\":true") && has("\"temp_ok\":true") && has("\"temp_c\":36.50")
           && has("\"temp_stable\":true") && has("\"falls\":2"));

    // A dead thermometer must not take the accelerometer status with it, and the other way around
    dot_readings_to_json(b, sizeof b, r, 100, TEMP_STALE_MS + 1);
    assert(has("\"motion_ok\":true") && has("\"temp_ok\":false") && has("\"temp_c\":null") && has("\"temp_stable\":false"));
    dot_readings_to_json(b, sizeof b, r, MOTION_STALE_MS + 1, 1000);
    assert(has("\"motion_ok\":false") && has("\"temp_ok\":true"));

    dot_readings_to_json(b, sizeof b, DotReadings{}, -1, -1);
    assert(has("\"motion_ok\":false") && has("\"temp_ok\":false") && has("\"temp_age_ms\":null"));

    assert(dot_readings_to_json(b, 192, r, 2147483647L, 2147483647L) < 192); // the size the dot uses
}

static void test_alert_json()
{
    int n = fall_alert_to_json(b, 192, "aa:bb:cc:dd:ee:ff", "1a2b3c4d-4294967295", 2147483647L, 15.99f, 180.0f);
    assert(n < 192); // the size the alert task uses
    assert(has("\"type\":\"fall\"") && has("\"device\":\"aa:bb:cc:dd:ee:ff\"") && has("\"peak_g\":15.99") && has("\"angle_deg\":180"));
}

static void test_axillary_temp()
{
    AxillaryTemp t;
    for (int i = 0; i < AxillaryTemp::WINDOW - 1; i++) t.add(36.5f);
    assert(!t.stable()); // not enough readings yet
    t.add(36.5f);
    assert(t.stable());

    t.add(35.8f); // arm opened: the plateau is broken
    assert(!t.stable());

    AxillaryTemp warming;
    for (int i = 0; i < 2 * AxillaryTemp::WINDOW; i++) warming.add(30.0f + 0.2f * i);
    assert(!warming.stable()); // still climbing

    AxillaryTemp off_body;
    for (int i = 0; i < AxillaryTemp::WINDOW; i++) off_body.add(25.0f);
    assert(!off_body.stable()); // flat, but it is the room

    AxillaryTemp fever;
    for (int i = 0; i < AxillaryTemp::WINDOW; i++) fever.add(39.5f);
    assert(fever.stable());
}

int main()
{
    test_hub_json();
    test_dot_json();
    test_alert_json();
    test_axillary_temp();
    puts("test_vitals OK");
    return 0;
}
