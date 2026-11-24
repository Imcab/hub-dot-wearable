// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef MQTT_PUB_HPP
#define MQTT_PUB_HPP

#include <stddef.h>
#include <stdint.h>

// Publica el payload JSON de cada placa en el broker MQTT del reto.
//
//   Datos:  reto2026/<equipo>/data            QoS 1, sin retain
//   Estado: reto2026/<equipo>/status/<placa>  {"device_id":..,"online":true|false}, retain.
//           "online":false lo publica el broker solo (Last Will) si la placa se apaga o
//           pierde el WiFi sin despedirse
//
// <equipo> y el broker se configuran en `idf.py menuconfig` -> hub-dot WiFi. Como
// db_upload, nunca bloquea: la conexion y los reintentos corren en la tarea de esp-mqtt.

// Llamar una vez, despues de wifi_start(). board = "hub" o "dot". Tambien arranca SNTP
// para que "ts" sea la hora real (Unix, UTC)
void mqtt_pub_start(const char *board);

// "<equipo>-<placa>", por ejemplo "eq01-dot"
const char *mqtt_device_id();

// Segundos Unix (UTC) si SNTP ya sincronizo; si no, segundos desde el arranque (como
// millis()/1000 del sketch base). mqtt_time_synced() dice cual de los dos es
uint32_t mqtt_timestamp();
bool mqtt_time_synced();

// Encola el payload en el topico de datos con QoS 1. Si no hay conexion con el broker se
// descarta (ya viene otra lectura) y regresa false
bool mqtt_pub_publish(const char *payload, size_t len);

#endif
