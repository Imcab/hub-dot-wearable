# hub-dot

Wearable para monitorear a adultos mayores sin que tengan que hacer nada: mide signos vitales por tandas y detecta caídas. Son dos placas Seeed XIAO ESP32-C3 colocadas en partes distintas del cuerpo.

| Placa | Ubicación | Sensores | Función |
| --- | --- | --- | --- |
| **hub** | Muñeca | MAX30102 | Frecuencia cardiaca y SpO2, salida por WiFi |
| **dot** | Axila | MPU6050, TMP117 | Detección de caídas y temperatura axilar |

## Cómo está organizado

Cada placa tiene su propio programa en `firmware/`. Los dos usan piezas de `components/`, que son de tres tipos:

```
SENSOR (driver)        →  ALGORITMO                          →  RED
lee el chip por I2C       interpreta las lecturas                sube a la base de datos

HUB (muñeca)
  max30102  ─────────→  pulse_ox       pulso y SpO2  ────────┐
                                                              │
DOT (axila)                                                   ├─→ net → scripts PHP → MySQL
  mpu6050   ─────────→  fall_detect    ¿se cayó?  ───────────┤
  tmp117    ─────────→  axillary_temp  ¿ya es estable?  ─────┘
```

| Carpeta | Qué hay |
| --- | --- |
| `firmware/hub/main/main.cpp` | Programa de la muñeca: lee el MAX30102 y sube el SpO2 cada 15 s |
| `firmware/dot/main/main.cpp` | Programa de la axila: detecta caídas y sube la temperatura cuando es estable |
| `components/sensors/` | Drivers: `max30102`, `mpu6050`, `tmp117`. Solo hablan con el chip y entregan números crudos |
| `components/algorithms/` | `pulse_ox` (pulso y SpO2), `fall_detect` (caídas), `axillary_temp` (temperatura confiable) |
| `components/net/` | `wifi` (conexión) y `db_upload` (manda las lecturas a los PHP en segundo plano) |
| `datasheets/` | Hojas de datos de los sensores |

### Configuración

El WiFi y la URL del servidor se configuran con `idf.py menuconfig` → *hub-dot WiFi*. Al arrancar, cada placa imprime su `numSerie` (su MAC), que es el que hay que registrar en la tabla `Dispositivo`.

## Autores

- Imad Jared Cabrera Trejo
- Melissa Moreno Franca
