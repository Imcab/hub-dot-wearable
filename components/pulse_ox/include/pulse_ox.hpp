// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef PULSE_OX_HPP
#define PULSE_OX_HPP

#include <stdint.h>

// Calcula frecuencia cardiaca (BPM) y oxigenacion (SpO2) a partir de las muestras
// crudas del MAX30102 (rojo + IR). Se alimenta muestra por muestra con addSample().
//
// Pipeline por muestra:
//   1. Deteccion de dedo (nivel DC del IR)
//   2. Separar DC (nivel base) y AC (pulso) con un filtro EMA lento
//   3. Suavizar el AC con promedio movil (quita ruido)
//   4. Detectar latidos en el IR (histeresis adaptativa + periodo refractario) -> BPM
//   5. Por cada latido: R = (ACrojo/DCrojo) / (ACir/DCir) -> SpO2 (curva de Maxim)
class PulseOximeter {
public:
    struct Result {
        bool finger;      // hay un dedo sobre el sensor
        bool hr_valid;    // ya hay suficientes latidos para confiar en bpm
        bool spo2_valid;  // ya hay suficientes latidos para confiar en spo2
        float bpm;
        float spo2;       // %
    };

    // sample_rate_hz = muestras EFECTIVAS por segundo (SPO2_SR / SMP_AVE)
    explicit PulseOximeter(float sample_rate_hz);

    // Alimenta una muestra cruda (18 bits). Regresa true si en esta muestra se detecto un latido
    bool addSample(uint32_t red, uint32_t ir);

    Result result() const;

    // Olvida todo (p. ej. al quitar el dedo o reiniciar el sensor)
    void reset();

    // Por debajo de este valor IR crudo se considera que no hay dedo.
    // Ajustalo viendo los valores crudos (PRINT_RAW en main.cpp) con y sin dedo
    static constexpr uint32_t FINGER_IR_MIN = 15000;

private:
    static constexpr int MA_LEN = 4;         // promedio movil del AC
    static constexpr int MAX_INTERVALS = 8;  // latidos promediados para BPM
    static constexpr int MAX_RATIOS = 4;     // latidos promediados para SpO2

    float fs_;

    // Filtros
    float dc_red_ = 0, dc_ir_ = 0;
    float ma_red_[MA_LEN] = {}, ma_ir_[MA_LEN] = {};
    int ma_idx_ = 0, ma_count_ = 0;

    // Estado general
    bool finger_ = false;
    uint32_t n_ = 0;            // indice de muestra desde que se puso el dedo
    uint32_t warmup_samples_;   // muestras ignoradas mientras el filtro DC converge

    // Deteccion de latidos
    // Histeresis: se busca un maximo, se confirma cuando la senal baja H; luego se busca
    // un minimo, se confirma cuando sube H. No depende de la linea base (respiracion, movimiento)
    bool looking_peak_ = true;
    float ext_ = 0;             // maximo (o minimo) en curso
    uint32_t ext_idx_ = 0;
    float last_trough_ = 0;
    bool have_trough_ = false;
    float amp_avg_ = 0;         // amplitud pico-valle promedio (umbral adaptativo)
    int amp_n_ = 0;             // cuantas amplitudes se han medido (los primeros latidos no cuentan)
    bool have_last_beat_ = false;
    uint32_t last_beat_idx_ = 0;

    // Intervalos entre latidos (en muestras)
    float intervals_[MAX_INTERVALS] = {};
    int int_count_ = 0, int_idx_ = 0;
    int rejects_ = 0;

    // Min/max del AC dentro del latido actual (para SpO2)
    float win_red_min_, win_red_max_, win_ir_min_, win_ir_max_;
    float ratios_[MAX_RATIOS] = {};
    int ratio_count_ = 0, ratio_idx_ = 0;

    void resetBeats();
    void resetWindow();
    void onBeat(uint32_t idx, bool count_it);
    float meanInterval() const;
};

#endif
