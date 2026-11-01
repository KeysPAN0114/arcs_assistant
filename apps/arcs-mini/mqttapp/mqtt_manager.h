#ifndef ARCS_MINI_MQTT_MANAGER_H
#define ARCS_MINI_MQTT_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int mqtt_manager_ui_init(void);
int mqtt_manager_open(void);
bool mqtt_manager_handle_long_hold(void);
void mqtt_manager_update_from_json(const char *payload, size_t length);
void mqtt_manager_set_light_state(bool light_on);

#endif
