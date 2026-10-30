#ifndef ARCS_MINI_MQTTAPP_H
#define ARCS_MINI_MQTTAPP_H

#include <stddef.h>

int mqttapp_init(void);
int mqttapp_publish_voice_command(const char *command, size_t command_length);

#endif
