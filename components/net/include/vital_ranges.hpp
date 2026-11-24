// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef VITAL_RANGES_HPP
#define VITAL_RANGES_HPP

// Rangos normales del esquema de datos (README -> Esquema de datos). Fuera de ellos el payload
// lleva "alarm": true y la razon en "alarm_reasons". Son los mismos de las alarmas (Semana 9)

namespace vital {

constexpr float SPO2_MIN = 95.0f;    // %, hasta 100
constexpr int HR_MIN = 60;           // lpm
constexpr int HR_MAX = 100;
// Axilar, no oral: la axila marca ~0.5 C menos, por eso el rango baja respecto a 36.1 - 37.2
constexpr float TEMP_MIN_C = 35.5f;
constexpr float TEMP_MAX_C = 37.5f;
constexpr float ACCEL_PEAK_MAX_G = 2.5f; // = FallDetector::IMPACT_G: caminar no pasa de 2 g en el torso

} // namespace vital

#endif
