// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef FALL_DETECT_HPP
#define FALL_DETECT_HPP

#include <stdint.h>

// Detecta caidas solo con el acelerometro (MPU6050, placa dot), muestra por muestra con addSample().
//
// Una caida es un golpe seguido de un cambio de postura que se mantiene:
//   1. Impacto: la magnitud de la aceleracion pasa de IMPACT_G
//   2. Asentamiento: el cuerpo se detiene, esta parte se ignora
//   3. Observacion: espera a que el cuerpo quede quieto, o a que se acabe MAX_WATCH_S
//   4. Decision: angulo entre la gravedad de antes del golpe y la de ahora
//
// Solo cuenta cuanto cambio la gravedad, nunca hacia donde, asi que no importa como quede la
// placa en la axila y no necesita calibracion.
//
// Los umbrales son puntos de partida, no valores medidos: hay que ajustarlos con grabaciones
// de la placa puesta. Fallas conocidas: deslizarse lento al piso (no hay golpe) y una caida que
// termina casi en la postura original, como quedar recargado en una pared.
class FallDetector {
public:
    enum class Event {
        None,
        Impact,     // un golpe inicio el analisis, todavia puede no ser nada
        Fall,       // la postura cambio despues del golpe: hay que avisar
        Cancelled,  // la postura no cambio, no fue caida
    };

    // En que se baso el ultimo Fall o Cancelled
    struct Report {
        float peak_g;
        float angle_deg;
    };

    static constexpr float IMPACT_G = 2.5f;         // caminar no pasa de 2 g en el torso
    static constexpr float MIN_ANGLE_DEG = 45.0f;   // sentarse inclina el torso menos que esto
    static constexpr float STILL_STD_G = 0.08f;     // variacion de la magnitud con el cuerpo en reposo
    static constexpr float SETTLE_S = 1.5f;
    static constexpr float STILL_WINDOW_S = 2.0f;
    static constexpr float MAX_WATCH_S = 10.0f;
    static constexpr float COOLDOWN_S = 30.0f;      // una caida no debe generar varias alertas

    explicit FallDetector(float sample_rate_hz);

    // Recibe una muestra en g
    Event addSample(float ax, float ay, float az);

    Report report() const { return report_; }

    // Olvida todo (por ejemplo, despues de reiniciar el sensor)
    void reset();

private:
    enum class State { Idle, Settling, Watching, Cooldown };

    struct Vec {
        float x, y, z;
    };

    static constexpr int HISTORY_LEN = 20;  // 2 s de gravedad, una entrada cada 100 ms

    float fs_;
    uint32_t history_period_;   // muestras entre entradas del historial
    uint32_t settle_samples_;
    uint32_t window_samples_;
    uint32_t watch_samples_;
    uint32_t cooldown_samples_;

    State state_ = State::Idle;
    uint32_t state_n_ = 0;      // muestras en el estado actual

    // La gravedad sigue a la postura lentamente. El historial guarda la de antes del golpe,
    // porque el valor actual ya esta alterado por la caida misma
    bool have_gravity_ = false;
    Vec gravity_ = {};
    Vec history_[HISTORY_LEN] = {};
    int history_idx_ = 0;
    int history_count_ = 0;
    uint32_t history_n_ = 0;

    Vec reference_ = {};        // gravedad antes del golpe
    float peak_g_ = 0;

    // Ventana actual del estado Watching
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
