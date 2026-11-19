// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca
//
// Driver del TMP117 (termometro de la axila, placa dot). Solo habla con el chip por I2C:
// decidir si la temperatura ya es confiable lo hace AxillaryTemp (algorithms/axillary_temp)

#ifndef TMP117_HPP
#define TMP117_HPP

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define TMP117_TAG "[TMP117]"

#define TMP117_ADDRESS 0x48 // ADD0 a GND
#define TMP117_I2C_FREQ_HZ 100000
#define TMP117_I2C_TIMEOUT_MS 100

// Registros (16 bits, primero el byte alto)
#define TMP117_REG_TEMP_RESULT 0x00
#define TMP117_REG_CONFIG 0x01
#define TMP117_REG_DEVICE_ID 0x0F

#define TMP117_DEVICE_ID 0x0117
#define TMP117_DEVICE_ID_MASK 0x0FFF // los bits 15:12 son la revision

// Registro CONFIG
#define TMP117_CFG_DATA_READY (1 << 13)
#define TMP117_CFG_MOD_SHUTDOWN (0x01 << 10)
#define TMP117_CFG_MOD_ONE_SHOT (0x03 << 10)
#define TMP117_CFG_AVG_8 (0x01 << 5)  // promedia 8 conversiones, unos 125 ms
#define TMP117_CFG_SOFT_RESET (1 << 1)

#define TMP117_RESOLUTION_C 0.0078125f // 1/128 C por cuenta
#define TMP117_RESULT_INVALID 0x8000   // valor al encender, todavia no termina ninguna conversion

// Se queda apagado y solo convierte cuando se le pide: casi no gasta entre lecturas
class TMP117 {
public:
    TMP117() = default;
    ~TMP117(); // quita el dispositivo del bus

    // El objeto es dueño del handle del bus, copiarlo no tiene sentido
    TMP117(const TMP117 &) = delete;
    TMP117 &operator=(const TMP117 &) = delete;

    // Agrega el sensor al bus I2C, revisa DEVICE_ID, lo resetea y lo deja apagado
    esp_err_t begin(i2c_master_bus_handle_t bus);

    // Quita el dispositivo del bus, para poder llamar begin() otra vez
    void end() { release(); }

    bool started() const { return dev_ != nullptr; }

    // Hace una conversion y devuelve la temperatura en Celsius. Bloquea unos 150 ms
    esp_err_t readOneShot(float *out_c);

private:
    i2c_master_dev_handle_t dev_ = nullptr;

    esp_err_t configure();
    void release();

    esp_err_t writeReg(uint8_t reg, uint16_t value);
    esp_err_t readReg(uint8_t reg, uint16_t *value);
};

#endif
