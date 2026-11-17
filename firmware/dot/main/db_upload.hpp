#pragma once

// Sends readings to the PHP scripts of the MySQL server (XAMPP) with HTTP GET.
//
// All the network work happens in its own task. db_upload_send() only copies the
// text into a queue and returns at once, so a slow, missing or misconfigured server
// can never delay fall detection or the sensor loops.
//
// Every request also carries "&serie=<MAC>", the board's Dispositivo.numSerie (printed at
// start), so no board needs its own idDispositivo in the code.
//
// And "&retraso=<s>": how many seconds the reading waited in the
// queue. The PHP scripts subtract it from NOW(), so a fall that could only be sent
// after the WiFi came back is still stored with the time it happened.

// Call once, after wifi_start()
void db_upload_start();

// Queues "<SERVER_URL>/<script>?<query>&serie=<MAC>&retraso=<s>". The query is printf style.
// Avoid %f (format floats with fmt_centi), the same as the rest of the project.
//
// important = true: retried until it is saved (for up to 10 minutes). Use it for falls.
// important = false: dropped if it cannot be sent at the first try (periodic readings,
// a newer one is coming anyway).
//
// Returns false if it was not queued (queue full or text too long).
bool db_upload_send(const char *script, bool important, const char *query_fmt, ...)
    __attribute__((format(printf, 3, 4)));
