// Hub and dot 2026
// Imad Jared Cabrera Trejo
// Melissa Moreno Franca

#ifndef ALERT_HPP
#define ALERT_HPP

// Starts the task that delivers the alerts. Call it after wifi_start().
// Without an SSID or an alert URL the alerts only go to the log
void alert_start();

// Queues a fall alert and returns at once, so the detection never waits for the network.
// It is repeated until the server answers 2xx. Pending alerts are lost if the board restarts
void alert_fall(float peak_g, float angle_deg);

#endif
