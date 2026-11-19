// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef WIFI_HPP
#define WIFI_HPP

// Conecta el WiFi como estacion con los datos de `idf.py menuconfig` -> "hub-dot WiFi".
// No bloquea: la conexion y los reintentos corren en segundo plano.
// Con el SSID vacio no hace nada.
void wifi_start(const char *hostname);

// True si hay SSID configurado, o sea que wifi_start() si levanta la red
bool wifi_configured();

// Espera a que la placa tenga IP. Devuelve false si se acaba el tiempo o el WiFi esta apagado
bool wifi_wait_connected(int timeout_ms);

#endif
