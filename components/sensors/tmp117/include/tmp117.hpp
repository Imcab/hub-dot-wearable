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
#include "driver/gpio.h"
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
#define TMP117_CFG_POL_ACTIVE_LOW (0 << 3)   // ALERT es open-drain: baja cuando avisa
#define TMP117_CFG_DR_ALERT (1 << 2)          // el pin ALERT marca Data_Ready, no alertas de temperatura
#define TMP117_CFG_SOFT_RESET (1 << 1)

#define TMP117_RESOLUTION_C 0.0078125f // 1/128 C por cuenta
#define TMP117_RESULT_INVALID 0x8000   // valor al encender, todavia no termina ninguna conversion

// Se queda apagado y solo convierte cuando se le pide: casi no gasta entre lecturas.
//
// La lectura no bloquea: startOneShot() arranca la conversion (~125 ms), conversionDone() dice
// si ya termino y readResult() la lee. Con el pin ALERT conectado, el TMP117 lo baja al terminar
// (modo Data Ready) y conversionDone() solo lee el GPIO, sin usar el bus I2C. Si el pin no baja
// (cable flojo), a los ALERT_TIMEOUT_MS se lee por I2C de todos modos y se avisa en el log
class TMP117 {
public:
    TMP117() = default;
    ~TMP117(); // quita el dispositivo del bus

    // El objeto es dueño del handle del bus, copiarlo no tiene sentido
    TMP117(const TMP117 &) = delete;
    TMP117 &operator=(const TMP117 &) = delete;

    static constexpr int CONVERSION_MS = 150;     // 8 conversiones promediadas tardan unos 125 ms
    static constexpr int ALERT_TIMEOUT_MS = 300;  // si ALERT no bajo para entonces, algo falla en el pin

    // Agrega el sensor al bus I2C, revisa DEVICE_ID, lo resetea y lo deja apagado.
    // alert_pin = GPIO conectado al pin ALERT, o GPIO_NUM_NC para esperar CONVERSION_MS
    esp_err_t begin(i2c_master_bus_handle_t bus, gpio_num_t alert_pin = GPIO_NUM_NC);

    // Quita el dispositivo del bus, para poder llamar begin() otra vez
    void end() { release(); }

    bool started() const { return dev_ != nullptr; }

    // Arranca una conversion y regresa de inmediato
    esp_err_t startOneShot();

    // True cuando ya se puede llamar readResult(). No usa el bus I2C
    bool conversionDone() const;

    // Lee la temperatura en Celsius de la conversion que arranco startOneShot()
    esp_err_t readResult(float *out_c);

private:
    i2c_master_dev_handle_t dev_ = nullptr;
    gpio_num_t alert_pin_ = GPIO_NUM_NC;
    bool converting_ = false;
    int64_t start_us_ = 0;
    bool alert_warned_ = false;

    esp_err_t configure();
    void release();

    esp_err_t writeReg(uint8_t reg, uint16_t value);
    esp_err_t readReg(uint8_t reg, uint16_t *value);
};

#endif
