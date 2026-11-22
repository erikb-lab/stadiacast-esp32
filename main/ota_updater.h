#ifndef OTA_UPDATER_H
#define OTA_UPDATER_H

#include <stdbool.h>
#include <stddef.h>

#define OTA_DEFAULT_GITHUB_URL "https://github.com/erikb-lab/stadiacast-esp32/releases/latest/download/stadiacast-esp32.bin"

/* Run one-shot Wi-Fi STA auto-OTA if scheduled in NVS; reboots when done (never returns if triggered). */
void ota_updater_check_and_run(void);

/* Consume one-shot USB Serial/Flash Mode flag from NVS (clears immediately so replug boots normally). */
bool ota_updater_consume_serial_boot(void);

/* Schedule one-shot STA OTA on next boot (returns false if no Home Wi-Fi SSID is saved). */
bool ota_updater_schedule_sta_ota(void);

void ota_updater_get_config(char *sta_ssid, size_t ssid_len, char *ota_url, size_t url_len);
bool ota_updater_save_config(const char *sta_ssid, const char *sta_pass, const char *ota_url);

void ota_updater_get_last_msg(char *msg, size_t msg_len);
void ota_updater_set_last_msg(const char *msg);

/* Cleanly deauth STA, stop Wi-Fi, disconnect USB, optionally enter ROM USB bootloader, and restart. */
void ota_updater_reboot_clean(bool to_bootloader);

#endif /* OTA_UPDATER_H */
