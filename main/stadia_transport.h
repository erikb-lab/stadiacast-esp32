/* Phase 3-5: Display Link (UDP 50000+slot*4) + Game Session Rumble (UDP 50002+slot*4). */
#ifndef STADIA_TRANSPORT_H
#define STADIA_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#define STADIA_LOCAL_UDP_PORT      50000
#define STADIA_GAME_LOCAL_UDP_PORT 50002

#define STADIA_SLOT_DISP_PORT(slot) (uint16_t)(STADIA_LOCAL_UDP_PORT + (slot) * 4)
#define STADIA_SLOT_GAME_PORT(slot) (uint16_t)(STADIA_GAME_LOCAL_UDP_PORT + (slot) * 4)

void stadia_transport_init(void);
void stadia_transport_set_remote(const char *ice_ufrag, const char *ice_pwd,
                                 const char *cand_ip, uint16_t cand_port);
void stadia_transport_set_remote_slot(int slot, const char *ice_ufrag, const char *ice_pwd,
                                      const char *cand_ip, uint16_t cand_port);
void stadia_transport_set_rumble(uint16_t left_strong, uint16_t right_weak);
void stadia_transport_set_rumble_slot(int slot, uint16_t left_strong, uint16_t right_weak);

bool stadia_transport_is_ready(void);
bool stadia_transport_is_slot_ready(int slot);
int  stadia_transport_get_battery_pct(void);
int  stadia_transport_get_slot_battery_pct(int slot);
bool stadia_transport_is_headset_plugged(void);
int  stadia_transport_get_connected_count(void);

#endif /* STADIA_TRANSPORT_H */
