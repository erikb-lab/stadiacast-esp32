#ifndef BLE_PROVISION_H
#define BLE_PROVISION_H

#include <stdbool.h>
#include <stddef.h>

void ble_provision_init(void);
bool ble_provision_get_auto(void);
void ble_provision_set_auto(bool enabled);
void ble_provision_trigger_now(void);
void ble_provision_get_status(char *buf, size_t len);

#endif /* BLE_PROVISION_H */
