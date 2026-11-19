// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef AXILLARY_TEMP_HPP
#define AXILLARY_TEMP_HPP

// Decide cuando se puede confiar en la temperatura de la axila (TMP117, placa dot). Un sensor
// que acaba de tocar la piel, o el brazo abierto, marcan bajo y siguen cambiando: el valor solo
// cuenta despues de una meseta a temperatura corporal
class AxillaryTemp {
public:
    // PRUEBA TEMPORAL para confirmar la subida al aire. Valores reales: 10, 0.2f, 34.0f, 42.0f
    static constexpr int WINDOW = 2;                 // lecturas, 5 minutos a una cada 30 s
    static constexpr float MAX_SPREAD_C = 1.0f;      // variacion maxima dentro de la ventana
    static constexpr float MIN_BODY_C = 0.0f;
    static constexpr float MAX_BODY_C = 60.0f;

    void add(float c)
    {
        readings_[idx_] = c;
        idx_ = (idx_ + 1) % WINDOW;
        if (count_ < WINDOW) count_++;
    }

    // True cuando las ultimas WINDOW lecturas son parejas y estan en rango corporal
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
