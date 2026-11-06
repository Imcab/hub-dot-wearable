# hub-dot

Wearable para monitorear a adultos mayores sin que tengan que hacer nada: mide signos vitales por tandas y detecta caídas. Son dos placas Seeed XIAO ESP32-C3 colocadas en partes distintas del cuerpo.

| Placa | Ubicación | Sensores | Función | Estado |
| --- | --- | --- | --- | --- |
| **hub** | Muñeca | MAX30102 | Frecuencia cardiaca y SpO2, salida por WiFi | Funcional, ajustado para dedo |
| **dot** | Axila | MPU6050, TMP117 | Detección de caídas y temperatura axilar | Compila, sin probar en placa |

Las placas no se comunican entre sí: cada una se conecta a la red por su cuenta. Las alertas de caída salen directo desde el dot, para que no esperen a una tanda ni dependan de la otra placa. El envío de las lecturas a una base de datos sigue pendiente.

> **No es un dispositivo médico.** La SpO2 usa la curva genérica de Maxim y aún no se ha comparado contra un oxímetro comercial. No uses estas lecturas para tomar decisiones clínicas.

## Estructura

```text
hub-dot/
├── components/        # Código compartido entre firmwares
│   ├── max30102/      # Driver del sensor de pulso
│   ├── pulse_ox/      # Cálculo de BPM y SpO2 (C++ puro)
│   ├── mpu6050/       # Driver del acelerómetro
│   ├── tmp117/        # Driver del termómetro
│   ├── fall_detect/   # Detección de caídas (C++ puro)
│   ├── vitals/        # Tipos de datos, JSON y estabilidad de la temperatura
│   └── net/           # WiFi, servidor HTTP y envío de alertas
├── firmware/
│   ├── hub/           # Proyecto ESP-IDF de la placa de la muñeca
│   └── dot/           # Proyecto ESP-IDF de la placa de la axila
├── test/              # Pruebas que corren en la PC
└── tools/             # Receptor de alertas para pruebas
```

## Compilar y flashear

Requiere [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v6.1. El target `esp32c3` ya viene en `sdkconfig.defaults`, así que no hace falta `idf.py set-target`.

```bash
. $IDF_PATH/export.sh
cd firmware/hub                # o firmware/dot
idf.py menuconfig              # opcional: "hub-dot WiFi" para configurar la red
idf.py build flash monitor
```

Cada placa se configura por separado. Sin SSID el WiFi queda apagado, pero los sensores funcionan y las lecturas salen por el monitor serie. El ESP32-C3 solo se conecta a redes de 2.4 GHz.

## Conexiones

Todos los sensores van por I2C, en los mismos pines de cada placa:

| Señal | XIAO ESP32-C3 |
| --- | --- |
| SDA | D4 (GPIO6) |
| SCL | D5 (GPIO7) |

En el dot, los dos sensores comparten el bus:

| Sensor | Dirección | Pin de dirección |
| --- | --- | --- |
| MPU6050 | 0x68 | AD0 a GND |
| TMP117 | 0x48 | ADD0 a GND |

El firmware activa los pull-ups internos, que son débiles. Si tus módulos no traen pull-ups en SDA y SCL, agrega resistencias externas de 4.7 kΩ. El TMP117 necesita buen contacto térmico con la piel.

## Lecturas por HTTP

Con WiFi activo, `GET /` en el puerto 80 devuelve el último estado de la placa.

### Hub

```json
{"sensor_ok":true,"age_ms":420,"ir":98210,"red":81544,"temp_c":31.44,"finger":true,"bpm":72,"spo2":97}
```

| Campo | Significado |
| --- | --- |
| `sensor_ok` | `true` si la lectura tiene 3 s o menos. Si es `false`, todos los campos medidos son `null`. |
| `age_ms` | Antigüedad de la lectura. Es `null` si aún no hay ninguna. |
| `ir`, `red` | Promedio crudo del último segundo. |
| `temp_c` | Temperatura del chip MAX30102. **No es la temperatura corporal.** |
| `finger` | Si hay contacto con el sensor. El umbral aún está ajustado para dedo, no para muñeca. |
| `bpm`, `spo2` | Son `null` hasta tener suficientes latidos válidos. |

### Dot

```json
{"motion_ok":true,"temp_ok":true,"temp_c":36.52,"temp_stable":true,"temp_age_ms":12040,"falls":0}
```

| Campo | Significado |
| --- | --- |
| `motion_ok` | `true` si llegaron muestras del acelerómetro en los últimos 2 s. **Si es `false`, no se están detectando caídas.** |
| `temp_ok` | `true` si hay una temperatura de hace 90 s o menos. Si es `false`, `temp_c` es `null`. |
| `temp_c` | Última lectura del TMP117, una cada 30 s. |
| `temp_stable` | `true` tras 5 minutos con una variación de 0.2 °C o menos, entre 34 y 42 °C. Si es `false`, no la tomes como temperatura corporal. |
| `temp_age_ms` | Antigüedad de la temperatura. Es `null` si aún no hay ninguna. |
| `falls` | Caídas detectadas desde el arranque. |

## Detección de caídas

Una caída es un impacto de más de 2.5 g seguido de un cambio de postura de 45° o más. Solo se usa el acelerómetro, y como se compara la gravedad antes y después, la orientación de la placa no importa.

**Estos umbrales son puntos de partida: aún no se han ajustado con datos reales.** Para ajustarlos, pon `PRINT_RAW = true` en `firmware/dot/main/main.cpp` y graba el monitor serie con la placa puesta. La salida es CSV (`ms,x,y,z`, 2048 cuentas por g).

No detecta:

- Deslizarse despacio al suelo, porque no hay impacto.
- Caídas que terminan casi en la postura inicial, como quedar sentado contra una pared.
- Impactos en los 2 s después de arrancar o de reiniciar el sensor.

### Alertas

Configura la URL en `menuconfig` -> "hub-dot WiFi". Por cada caída, el dot manda un POST:

```json
{"type":"fall","device":"aa:bb:cc:dd:ee:ff","event_id":"1a2b3c4d-1","age_ms":1500,"peak_g":4.27,"angle_deg":88}
```

- La alerta se repite hasta que el servidor responda 2xx, así que puede llegar más de una vez. Usa `event_id` para descartar duplicados.
- No hay reloj: la hora de la caída es la hora de recepción menos `age_ms`.
- Las alertas pendientes se pierden si la placa se reinicia.
- Si el dot se apaga o se queda sin batería, nadie se entera. Todavía no hay una señal periódica de que sigue vivo.

Para probarlas sin servidor, corre `python3 tools/alert_receiver.py` en tu PC, apunta la URL a `http://<IP de tu PC>:8000/` y simula una caída: agita la placa con fuerza, gírala 90° y déjala quieta. La alerta tarda unos 4 s.

## Pruebas

```bash
test/run.sh
```

Corren en la PC y solo necesitan g++. Cubren el JSON, la estabilidad de la temperatura y la detección de caídas. Las señales de caídas son sintéticas: comprueban la lógica, no la precisión con personas reales.

## Seguridad

El servidor HTTP no tiene cifrado ni autenticación, así que cualquiera en la misma red puede leer los signos vitales. Las alertas tampoco se autentican: el servidor no puede saber si vienen de un dot real, y con una URL `http://` viajan sin cifrar. En modo WPA2-Enterprise tampoco se valida el certificado del servidor. Úsalo solo en redes de prueba de confianza y sin datos de pacientes reales hasta corregirlo.

## Autores

- Imad Jared Cabrera Trejo
- Melissa Moreno Franca
