// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef MPU6050_HPP
#define MPU6050_HPP

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define MPU6050_TAG "[MPU6050]"

#define MPU6050_ADDRESS 0x68 // AD0 en bajo
#define MPU6050_I2C_FREQ_HZ 100000
#define MPU6050_I2C_TIMEOUT_MS 100

// Mapa de registros (RM-MPU-6000A-00)
#define MPU6050_REG_SMPLRT_DIV 0x19
#define MPU6050_REG_CONFIG 0x1A
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_REG_FIFO_EN 0x23
#define MPU6050_REG_USER_CTRL 0x6A
#define MPU6050_REG_PWR_MGMT_1 0x6B
#define MPU6050_REG_PWR_MGMT_2 0x6C
#define MPU6050_REG_FIFO_COUNTH 0x72
#define MPU6050_REG_FIFO_R_W 0x74
#define MPU6050_REG_WHO_AM_I 0x75

#define MPU6050_EXPECTED_WHO_AM_I 0x68

#define MPU6050_PWR1_DEVICE_RESET (1 << 7)
#define MPU6050_PWR1_TEMP_DIS (1 << 3)    // no se usa la temperatura del chip
#define MPU6050_PWR2_STBY_GYRO 0x07       // el giroscopio gasta mucho mas que el acelerometro
#define MPU6050_DLPF_44HZ 0x03            // 1 kHz interno, 44 Hz de ancho de banda del acelerometro
#define MPU6050_ACCEL_FS_16G (0x03 << 3)  // un golpe puede pasar de 8 g
#define MPU6050_FIFO_EN_ACCEL (1 << 3)
#define MPU6050_USER_FIFO_EN (1 << 6)
#define MPU6050_USER_FIFO_RESET (1 << 2)

#define MPU6050_BYTES_PER_SAMPLE 6        // X, Y, Z, 16 bits cada uno
#define MPU6050_FIFO_FULL_BYTES 1020      // 170 muestras completas del FIFO de 1024 bytes

// Driver del MPU6050 (acelerometro de la axila, placa dot). Solo entrega muestras crudas:
// decidir si hubo una caida lo hace FallDetector (algorithms/fall_detect).
//
// Lee el acelerometro por el FIFO: el sensor lleva el tiempo, asi que un loop lento o el
// WiFi ocupado no deforman la señal
class MPU6050 {
public:
    static constexpr float SAMPLE_RATE_HZ = 100.0f;
    static constexpr float LSB_PER_G = 2048.0f; // a +-16 g

    // Muestra cruda del acelerometro, LSB_PER_G cuentas por g
    struct Sample {
        int16_t x;
        int16_t y;
        int16_t z;
    };

    MPU6050() = default;
    ~MPU6050(); // quita el dispositivo del bus

    // El objeto es dueño del handle del bus, copiarlo no tiene sentido
    MPU6050(const MPU6050 &) = delete;
    MPU6050 &operator=(const MPU6050 &) = delete;

    // Agrega el sensor al bus I2C, lo resetea y empieza a muestrear a SAMPLE_RATE_HZ
    esp_err_t begin(i2c_master_bus_handle_t bus);

    // Quita el dispositivo del bus, para poder llamar begin() otra vez
    void end() { release(); }

    // Lee las muestras que haya en el FIFO (hasta maxSamples).
    // Devuelve cuantas leyo, o -1 si hubo error. Un FIFO lleno se vacia y cuenta como 0:
    // ya no tiene muestras completas, no se puede confiar en el
    int readSamples(Sample *out, int maxSamples);

private:
    i2c_master_dev_handle_t dev_ = nullptr;

    esp_err_t configure();
    esp_err_t resetFifo(); // vacia el FIFO y lo deja habilitado
    void release();

    esp_err_t writeReg(uint8_t reg, uint8_t value);
    esp_err_t readRegs(uint8_t reg, uint8_t *data, size_t len);
};

#endif
