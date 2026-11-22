/* Phase 3-5: Display Link (UDP 50000) + Game Session Rumble (UDP 50002). */
#ifndef STADIA_TRANSPORT_H
#define STADIA_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

void stadia_transport_init(void);
void stadia_transport_set_remote(const char *ice_ufrag, const char *ice_pwd,
                                 const char *cand_ip, uint16_t cand_port);
void stadia_transport_set_rumble(uint16_t left_strong, uint16_t right_weak);

bool stadia_transport_is_ready(void);
int  stadia_transport_get_battery_pct(void);
bool stadia_transport_is_headset_plugged(void);

#endif /* STADIA_TRANSPORT_H */
