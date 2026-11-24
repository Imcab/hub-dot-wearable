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
| `components/net/` | `wifi` (conexión), `db_upload` (manda las lecturas a los PHP en segundo plano), `mqtt_pub` (publica el payload JSON) y `vital_ranges.hpp` (rangos normales) |
| `database/esquema.sql` | Esquema de la base de datos `ProyectoIoT` (MySQL de XAMPP), con las placas ya registradas |
| `datasheets/` | Hojas de datos de los sensores |

### Configuración

El WiFi, la URL del servidor y el broker MQTT se configuran con `idf.py menuconfig` → *hub-dot WiFi*. Al arrancar, cada placa imprime su `numSerie` (su MAC), que es el que hay que registrar en la tabla `Dispositivo`; cada chip va en `Sensor` con el `numSensor` que usa el firmware (`NUM_SENSOR_*` en cada `main.cpp`).

## Esquema de datos (payload JSON)

Cada placa publica un JSON plano, cada 5 s, en el tópico `reto2026/<equipo>/data` con **QoS 1**. El dot además lo publica en el momento en que detecta una caída. Los dos comparten el tópico y se distinguen por `device_id`. El JSON se arma con ArduinoJson 7 en `build_payload()` de cada `main.cpp`, y se imprime en el monitor serial antes de publicarse.

Un valor que todavía no es válido (sin dedo, o la temperatura sin estabilizar) va como `null`, nunca como `0`, para que no se grafique como lectura real.

### Campos comunes

| Campo | Unidad | Tipo | Valores | Descripción |
| --- | --- | --- | --- | --- |
| `device_id` | — | texto | `eq01-hub`, `eq01-dot` | Quién publica (`<equipo>-<placa>`) |
| `serie` | — | texto | 12 hex | MAC de la placa = `Dispositivo.numSerie` en la BD |
| `ts` | s | entero | Unix UTC | Hora de la lectura (SNTP) |
| `ts_sync` | — | booleano | | `false`: el reloj no se ha sincronizado y `ts` son segundos desde el arranque |
| `alarm` | — | booleano | | `true` si algún valor está fuera de rango o hubo caída |
| `alarm_reasons` | — | arreglo de texto | `spo2_low`, `hr_low`, `hr_high`, `temp_low`, `temp_high`, `fall` | Por qué hay alarma |

### Variables (rango normal = límite de las alarmas, `components/net/include/vital_ranges.hpp`)

| Variable | Placa | Campo | Unidad | Tipo | Rango normal |
| --- | --- | --- | --- | --- | --- |
| Oximetría | hub | `spo2` | % | decimal (1) | 95 – 100 |
| Frecuencia cardiaca | hub | `hr` | lpm | entero | 60 – 100 |
| Dedo en el sensor | hub | `finger` | — | booleano | `true` |
| Temperatura axilar | dot | `temp_c` | °C | decimal (1) | 35.5 – 37.5 * |
| Temperatura estable | dot | `temp_stable` | — | booleano | `true` |
| Pico de aceleración (últimos 5 s) | dot | `peak_g` | g | decimal (2) | 0 – 2.5 |
| Caída | dot | `fall` | — | booleano | `false` |

\* La axila marca unos 0.5 °C menos que la temperatura oral (36.1 – 37.2), por eso el rango baja.

### Ejemplos

```json
{"device_id":"eq01-hub","serie":"B81F3F2C9240","ts":1795537800,"ts_sync":true,"finger":true,"spo2":97.4,"hr":72,"alarm":false,"alarm_reasons":[]}
{"device_id":"eq01-dot","serie":"B81F3F2CA220","ts":1795537802,"ts_sync":true,"temp_c":36.6,"temp_stable":true,"peak_g":1.04,"fall":false,"alarm":false,"alarm_reasons":[]}
{"device_id":"eq01-dot","serie":"B81F3F2CA220","ts":1795537840,"ts_sync":true,"temp_c":36.6,"temp_stable":true,"peak_g":3.45,"fall":true,"alarm":true,"alarm_reasons":["fall"]}
```

### MQTT

| Tópico | Contenido | QoS | Retain |
| --- | --- | --- | --- |
| `reto2026/<equipo>/data` | El payload de arriba | 1 | no |
| `reto2026/<equipo>/status/<placa>` | `{"device_id":"eq01-dot","online":true}` al conectar; `"online":false` lo publica el broker (Last Will) si la placa se apaga o pierde el WiFi | 1 | sí |

**¿Por qué QoS 1?** Garantiza que el broker recibe cada mensaje al menos una vez: si el WiFi parpadea después de mandarlo, el ESP32 lo reenvía hasta recibir el PUBACK. Para un adulto mayor no queremos perder una caída ni una lectura fuera de rango. QoS 0 sí podría perderlas, y QoS 2 agrega un saludo de 4 pasos que no hace falta, porque un duplicado se detecta por `device_id` + `ts`. Si la placa no está conectada al broker cuando toca publicar, ese payload se descarta (ya viene otro en 5 s); las caídas, además, se guardan en MySQL por HTTP con reintentos de hasta 10 min.

El broker, el usuario y el equipo se configuran en `idf.py menuconfig` → *hub-dot WiFi*.

## Autores

- Imad Jared Cabrera Trejo
- Melissa Moreno Franca
