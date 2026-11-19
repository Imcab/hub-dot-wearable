#include "pulse_ox.hpp"

#include <math.h>

// Limites fisiologicos
static constexpr float MIN_BPM = 30.0f;
static constexpr float MAX_BPM = 220.0f;
static constexpr float LOST_BEAT_S = 2.5f;     // sin latidos este tiempo -> se pierde el ritmo

static constexpr float DC_TAU_S = 1.6f;        // constante de tiempo del filtro DC (mas lento que el pulso)
static constexpr float WARMUP_S = 1.5f;        // tiempo para que el DC converja al poner el dedo
static constexpr float THRESHOLD_FRAC = 0.4f;  // histeresis = 40% de la amplitud pico-valle promedio
static constexpr float PEAK_DECAY_PER_S = 0.78f; // el umbral baja si la senal se hace mas debil
static constexpr float MIN_AC_FRAC = 0.0005f;  // pulso minimo = 0.05% del DC (abajo de eso es ruido)
static constexpr int MIN_BEATS_VALID = 5;      // intervalos buenos antes de mostrar un valor (evita lecturas falsas al re-sincronizar)
static constexpr float EARLY_FRAC = 0.6f;      // un "latido" antes del 60% del intervalo promedio se ignora
static constexpr float OUTLIER_FRAC = 0.35f;   // intervalo que se aleja >35% del promedio = falso latido

PulseOximeter::PulseOximeter(float sample_rate_hz)
    : fs_(sample_rate_hz), warmup_samples_((uint32_t)(WARMUP_S * sample_rate_hz))
{
    reset();
}

void PulseOximeter::reset()
{
    finger_ = false;
    n_ = 0;
    ma_idx_ = ma_count_ = 0;
    resetBeats();
}

void PulseOximeter::resetBeats()
{
    looking_peak_ = true;
    ext_ = -INFINITY;
    have_trough_ = false;
    amp_avg_ = 0;
    amp_n_ = 0;
    have_last_beat_ = false;
    int_count_ = int_idx_ = 0;
    rejects_ = 0;
    ratio_count_ = ratio_idx_ = 0;
    resetWindow();
}

void PulseOximeter::resetWindow()
{
    win_red_min_ = win_ir_min_ = INFINITY;
    win_red_max_ = win_ir_max_ = -INFINITY;
}

float PulseOximeter::meanInterval() const
{
    float sum = 0;
    for (int i = 0; i < int_count_; i++) sum += intervals_[i];
    return int_count_ > 0 ? sum / int_count_ : 0;
}

bool PulseOximeter::addSample(uint32_t red_raw, uint32_t ir_raw)
{
    const float red = (float)red_raw;
    const float ir = (float)ir_raw;

    // 1. Dedo
    if (ir_raw < FINGER_IR_MIN) {
        if (finger_) reset();
        return false;
    }
    if (!finger_) {
        // Recien puesto: el DC arranca en el valor actual para converger rapido
        finger_ = true;
        n_ = 0;
        dc_red_ = red;
        dc_ir_ = ir;
    }
    n_++;

    // 2. DC (EMA lento) y AC
    const float a = 1.0f / (DC_TAU_S * fs_);
    dc_red_ += a * (red - dc_red_);
    dc_ir_ += a * (ir - dc_ir_);

    // 3. Promedio movil del AC
    ma_red_[ma_idx_] = red - dc_red_;
    ma_ir_[ma_idx_] = ir - dc_ir_;
    ma_idx_ = (ma_idx_ + 1) % MA_LEN;
    if (ma_count_ < MA_LEN) ma_count_++;
    float ac_red = 0, ac_ir = 0;
    for (int i = 0; i < ma_count_; i++) {
        ac_red += ma_red_[i];
        ac_ir += ma_ir_[i];
    }
    ac_red /= ma_count_;
    ac_ir /= ma_count_;

    if (n_ < warmup_samples_) return false;

    // Min/max del latido en curso (para SpO2)
    if (ac_red < win_red_min_) win_red_min_ = ac_red;
    if (ac_red > win_red_max_) win_red_max_ = ac_red;
    if (ac_ir < win_ir_min_) win_ir_min_ = ac_ir;
    if (ac_ir > win_ir_max_) win_ir_max_ = ac_ir;

    // Se perdio el ritmo (dedo movido, mala senal): empezar de nuevo
    if (have_last_beat_ && (n_ - last_beat_idx_) > (uint32_t)(LOST_BEAT_S * fs_)) {
        resetBeats();
    }

    // 4. Deteccion de latidos.
    // Con cada latido entra mas sangre y absorbe mas luz: el IR BAJA. Por eso se invierte,
    // asi cada latido es un pico positivo.
    const float x = -ac_ir;

    amp_avg_ *= powf(PEAK_DECAY_PER_S, 1.0f / fs_);
    float h = THRESHOLD_FRAC * amp_avg_;
    const float min_amp = MIN_AC_FRAC * dc_ir_;
    if (h < min_amp) h = min_amp;

    const uint32_t refractory = (uint32_t)(fs_ * 60.0f / MAX_BPM);
    bool beat = false;

    if (looking_peak_) {
        if (x > ext_) {
            ext_ = x;
            ext_idx_ = n_;
        }
        if (x < ext_ - h) {
            // Confirmado un maximo. Si llega demasiado pronto respecto al ritmo actual es ruido
            // o la muesca dicrotica: se ignora sin mover la referencia del ultimo latido, para
            // que el siguiente intervalo real no salga partido
            const uint32_t since = ext_idx_ - last_beat_idx_;
            const bool too_early = have_last_beat_ &&
                (since < refractory || (int_count_ >= 3 && since < EARLY_FRAC * meanInterval()));
            if (have_trough_ && !too_early) {
                const float amp = ext_ - last_trough_;
                amp_avg_ = (amp_n_ == 0) ? amp : 0.75f * amp_avg_ + 0.25f * amp;
                amp_n_++;
            }
            if (!too_early) {
                // Hasta tener 2 amplitudes el umbral aun no se adapta: el latido marca
                // tiempo pero su intervalo no se usa
                onBeat(ext_idx_, amp_n_ >= 2);
                beat = true;
            }
            looking_peak_ = false;
            ext_ = x;
            ext_idx_ = n_;
        }
    } else {
        if (x < ext_) {
            ext_ = x;
            ext_idx_ = n_;
        }
        if (x > ext_ + h) {
            // Confirmado el valle, ahora a buscar el siguiente pico
            last_trough_ = ext_;
            have_trough_ = true;
            looking_peak_ = true;
            ext_ = x;
            ext_idx_ = n_;
        }
    }
    return beat;
}

void PulseOximeter::onBeat(uint32_t idx, bool count_it)
{
    if (have_last_beat_ && count_it) {
        const float interval = (float)(idx - last_beat_idx_);
        const float min_int = fs_ * 60.0f / MAX_BPM;
        const float max_int = fs_ * 60.0f / MIN_BPM;

        bool ok = interval >= min_int && interval <= max_int;
        if (ok && int_count_ >= 3) {
            const float mean = meanInterval();
            if (fabsf(interval - mean) > OUTLIER_FRAC * mean) {
                ok = false;
                // Varios rechazos seguidos = el ritmo cambio de verdad, re-sincronizar
                if (++rejects_ >= 3) {
                    int_count_ = int_idx_ = 0;
                    ratio_count_ = ratio_idx_ = 0;
                    rejects_ = 0;
                    ok = true;
                }
            }
        }

        if (ok) {
            rejects_ = 0;
            intervals_[int_idx_] = interval;
            int_idx_ = (int_idx_ + 1) % MAX_INTERVALS;
            if (int_count_ < MAX_INTERVALS) int_count_++;

            // 5. Ratio de SpO2 con la amplitud de este latido
            const float ac_red = win_red_max_ - win_red_min_;
            const float ac_ir = win_ir_max_ - win_ir_min_;
            if (ac_red > 0 && ac_ir > 0 && dc_red_ > 0 && dc_ir_ > 0) {
                const float r = (ac_red / dc_red_) / (ac_ir / dc_ir_);
                if (r > 0.2f && r < 2.0f) {
                    ratios_[ratio_idx_] = r;
                    ratio_idx_ = (ratio_idx_ + 1) % MAX_RATIOS;
                    if (ratio_count_ < MAX_RATIOS) ratio_count_++;
                }
            }
        }
    }
    have_last_beat_ = true;
    last_beat_idx_ = idx;
    resetWindow();
}

PulseOximeter::Result PulseOximeter::result() const
{
    Result res{};
    res.finger = finger_;

    if (int_count_ >= MIN_BEATS_VALID) {
        res.bpm = 60.0f * fs_ / meanInterval();
        res.hr_valid = res.bpm >= MIN_BPM && res.bpm <= MAX_BPM;
    }

    // El buffer de ratios guarda 4; ademas se exige el mismo numero de latidos que para BPM
    if (ratio_count_ >= MAX_RATIOS && int_count_ >= MIN_BEATS_VALID) {
        float r = 0;
        for (int i = 0; i < ratio_count_; i++) r += ratios_[i];
        r /= ratio_count_;
        // Curva de calibracion de la nota de aplicacion de Maxim (RD117) para el MAX30102
        float spo2 = -45.060f * r * r + 30.354f * r + 94.845f;
        if (spo2 > 100.0f) spo2 = 100.0f;
        res.spo2 = spo2;
        res.spo2_valid = spo2 >= 70.0f;
    }
    return res;
}
