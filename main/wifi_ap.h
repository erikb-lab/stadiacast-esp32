#ifndef WIFI_AP_H
#define WIFI_AP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_AP_DEFAULT_SSID    "STADIA_ESP32"
#define WIFI_AP_DEFAULT_PASS    "stadiacast"
#define WIFI_AP_DEFAULT_TX_QDBM 52 /* +13 dBm */

void wifi_ap_init(void);
void wifi_ap_get_config(char *ssid, size_t ssid_len, int8_t *tx_qdbm);
void wifi_ap_get_pass(char *pass, size_t pass_len);
bool wifi_ap_set_tx_power(int8_t tx_qdbm);
bool wifi_ap_save_credentials(const char *ssid, const char *pass);

#endif /* WIFI_AP_H */
