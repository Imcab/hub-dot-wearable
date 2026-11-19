// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef DB_UPLOAD_HPP
#define DB_UPLOAD_HPP

#include <math.h>
#include <stddef.h>
#include <stdio.h>

// Manda las lecturas a los scripts PHP del servidor MySQL (XAMPP) con HTTP GET.
//
// Todo el trabajo de red pasa en su propia tarea. db_upload_send() solo copia el texto a
// una cola y regresa de inmediato, asi que un servidor lento, apagado o mal configurado
// nunca puede retrasar la deteccion de caidas ni los loops de los sensores.
//
// Cada peticion lleva tambien "&serie=<MAC>", el Dispositivo.numSerie de la placa (se
// imprime al arrancar), para que ninguna placa necesite su idDispositivo en el codigo.
//
// Y "&retraso=<s>": cuantos segundos espero la lectura en la cola. Los PHP lo restan de
// NOW(), asi que una caida que solo se pudo mandar cuando volvio el WiFi se guarda con la
// hora en que paso.

// Llamar una vez, despues de wifi_start(). Imprime el numSerie aunque el WiFi este apagado
void db_upload_start();

// Encola "<CONFIG_HUBDOT_DB_URL>/<script>?<query>&serie=<MAC>&retraso=<s>". El query es
// estilo printf. No uses %f: formatea los float con fmt_centi.
//
// important = true: se reintenta hasta que se guarde (hasta 10 minutos). Para las caidas.
// important = false: se descarta si no se pudo mandar al primer intento (lecturas
// periodicas, ya viene otra).
//
// Devuelve false si no se encolo (cola llena o texto demasiado largo).
bool db_upload_send(const char *script, bool important, const char *query_fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Escribe v con dos decimales ("31.44", acepta negativos) sin usar printf %f
inline void fmt_centi(char *buf, size_t len, float v)
{
    long c = lroundf(v * 100.0f);
    const char *sign = (c < 0) ? "-" : "";
    if (c < 0) c = -c;
    snprintf(buf, len, "%s%ld.%02ld", sign, c / 100, c % 100);
}

#endif
