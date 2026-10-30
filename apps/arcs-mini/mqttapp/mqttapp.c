#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "cJSON.h"
#include "core_mqtt_agent.h"
#include "lisa_log.h"
#include "sysutils.h"
#include "sys_network_manager.h"
#include "voice_msg.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "mqttapp.h"
#include "mqttapp_config.h"

#define TAG "mqttapp"

#define MQTTAPP_EVENT_NETWORK_READY (1U << 0)
#define MQTTAPP_EVENT_MQTT_CONNECTED (1U << 1)
#define MQTTAPP_ONLINE_LIGHT_SUPPRESS_MS 5000

typedef struct {
    char command[64];
} mqttapp_voice_command_t;

typedef struct {
    mbedtls_net_context net_fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config ssl_config;
    mbedtls_x509_crt ca_cert;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
} mqttapp_tls_context_t;

struct MQTTAgentMessageContext {
    QueueHandle_t queue;
};

struct MQTTAgentCommandContext {
    SemaphoreHandle_t semaphore;
    MQTTStatus_t return_code;
};

struct NetworkContext {
    mqttapp_tls_context_t *tls_context;
};

static MQTTAgentContext_t s_agent_context;
static mqttapp_tls_context_t s_tls_context;
static struct MQTTAgentMessageContext s_agent_message_context;
static MQTTAgentCommand_t s_command_pool[MQTTAPP_AGENT_COMMAND_COUNT];
static bool s_command_in_use[MQTTAPP_AGENT_COMMAND_COUNT];
static uint8_t s_network_buffer[MQTTAPP_NETWORK_BUFFER_SIZE] __psram_bss__;
static QueueHandle_t s_agent_queue;
static QueueHandle_t s_voice_command_queue;
static SemaphoreHandle_t s_command_available;
static EventGroupHandle_t s_events;
static volatile bool s_agent_connected;
static bool s_online_light_command_seen;
static bool s_online_light_state;
static TickType_t s_online_light_command_tick;
static struct NetworkContext s_network_context = { .tls_context = &s_tls_context };

static int32_t mqttapp_transport_recv(NetworkContext_t *network_context,
                                      void *buffer,
                                      size_t bytes_to_receive)
{
    int result = mbedtls_ssl_read(&network_context->tls_context->ssl, buffer, bytes_to_receive);

    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (result < 0) {
        LOGE("TLS receive failed: -0x%04x", -result);
        return -1;
    }

    return result;
}

static int32_t mqttapp_transport_send(NetworkContext_t *network_context,
                                      const void *buffer,
                                      size_t bytes_to_send)
{
    int result = mbedtls_ssl_write(&network_context->tls_context->ssl, buffer, bytes_to_send);

    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (result < 0) {
        LOGE("TLS send failed: -0x%04x", -result);
        return -1;
    }

    return result;
}

static uint32_t mqttapp_time_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static bool mqttapp_agent_send_command(MQTTAgentMessageContext_t *message_context,
                                       MQTTAgentCommand_t *const *command_to_send,
                                       uint32_t block_time_ms)
{
    (void)message_context;
    return xQueueSend(s_agent_queue, command_to_send, pdMS_TO_TICKS(block_time_ms)) == pdPASS;
}

static bool mqttapp_agent_receive_command(MQTTAgentMessageContext_t *message_context,
                                          MQTTAgentCommand_t **received_command,
                                          uint32_t block_time_ms)
{
    (void)message_context;
    return xQueueReceive(s_agent_queue, received_command, pdMS_TO_TICKS(block_time_ms)) == pdPASS;
}

static MQTTAgentCommand_t *mqttapp_agent_get_command(uint32_t block_time_ms)
{
    if (xSemaphoreTake(s_command_available, pdMS_TO_TICKS(block_time_ms)) != pdTRUE) {
        return NULL;
    }

    taskENTER_CRITICAL();
    for (size_t index = 0; index < MQTTAPP_AGENT_COMMAND_COUNT; index++) {
        if (!s_command_in_use[index]) {
            s_command_in_use[index] = true;
            taskEXIT_CRITICAL();
            return &s_command_pool[index];
        }
    }
    taskEXIT_CRITICAL();

    xSemaphoreGive(s_command_available);
    return NULL;
}

static bool mqttapp_agent_release_command(MQTTAgentCommand_t *command_to_release)
{
    ptrdiff_t index = command_to_release - s_command_pool;

    if (index < 0 || index >= MQTTAPP_AGENT_COMMAND_COUNT) {
        return false;
    }

    taskENTER_CRITICAL();
    if (!s_command_in_use[index]) {
        taskEXIT_CRITICAL();
        return false;
    }
    s_command_in_use[index] = false;
    taskEXIT_CRITICAL();

    return xSemaphoreGive(s_command_available) == pdTRUE;
}

static void mqttapp_incoming_publish_callback(MQTTAgentContext_t *agent_context,
                                              uint16_t packet_id,
                                              MQTTPublishInfo_t *publish_info)
{
    (void)agent_context;
    (void)packet_id;

    LOGI("Received MQTT message: topic=%.*s payload=%.*s",
         (int)publish_info->topicNameLength,
         publish_info->pTopicName,
         (int)publish_info->payloadLength,
         (const char *)publish_info->pPayload);
}

static void mqttapp_command_complete_callback(MQTTAgentCommandContext_t *command_context,
                                              MQTTAgentReturnInfo_t *return_info)
{
    if (command_context != NULL && command_context->semaphore != NULL) {
        command_context->return_code = return_info->returnCode;
        xSemaphoreGive(command_context->semaphore);
    }
}

static void mqttapp_tls_cleanup(mqttapp_tls_context_t *tls_context)
{
    (void)mbedtls_ssl_close_notify(&tls_context->ssl);
    mbedtls_net_free(&tls_context->net_fd);
    mbedtls_x509_crt_free(&tls_context->ca_cert);
    mbedtls_ssl_free(&tls_context->ssl);
    mbedtls_ssl_config_free(&tls_context->ssl_config);
    mbedtls_ctr_drbg_free(&tls_context->ctr_drbg);
    mbedtls_entropy_free(&tls_context->entropy);
    memset(tls_context, 0, sizeof(*tls_context));
}

static int mqttapp_tls_connect(mqttapp_tls_context_t *tls_context)
{
    static const char personalization[] = "arcs-mini3-mqttapp";
    char port_text[8];
    int result;

    mbedtls_net_init(&tls_context->net_fd);
    mbedtls_ssl_init(&tls_context->ssl);
    mbedtls_ssl_config_init(&tls_context->ssl_config);
    mbedtls_x509_crt_init(&tls_context->ca_cert);
    mbedtls_ctr_drbg_init(&tls_context->ctr_drbg);
    mbedtls_entropy_init(&tls_context->entropy);

    result = mbedtls_ctr_drbg_seed(&tls_context->ctr_drbg,
                                   mbedtls_entropy_func,
                                   &tls_context->entropy,
                                   (const unsigned char *)personalization,
                                   sizeof(personalization) - 1);
    if (result != 0) {
        LOGE("TLS random seed failed: -0x%04x", -result);
        return -1;
    }

    snprintf(port_text, sizeof(port_text), "%d", MQTTAPP_BROKER_PORT);
    result = mbedtls_net_connect(&tls_context->net_fd,
                                 MQTTAPP_BROKER_HOST,
                                 port_text,
                                 MBEDTLS_NET_PROTO_TCP);
    if (result != 0) {
        LOGE("TCP connection failed: -0x%04x", -result);
        return -1;
    }

    result = mbedtls_net_set_nonblock(&tls_context->net_fd);
    if (result != 0) {
        LOGE("Setting nonblocking TLS socket failed: -0x%04x", -result);
        return -1;
    }

    result = mbedtls_ssl_config_defaults(&tls_context->ssl_config,
                                         MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) {
        LOGE("TLS configuration failed: -0x%04x", -result);
        return -1;
    }

#if MQTTAPP_TLS_VERIFY_CERTIFICATE
    if (MQTTAPP_CA_CERT_PEM[0] == '\0') {
        LOGE("MQTTAPP_CA_CERT_PEM is empty");
        return -1;
    }

    result = mbedtls_x509_crt_parse(&tls_context->ca_cert,
                                    (const unsigned char *)MQTTAPP_CA_CERT_PEM,
                                    strlen(MQTTAPP_CA_CERT_PEM) + 1);
    if (result < 0) {
        LOGE("Parsing MQTT broker CA failed: -0x%04x", -result);
        return -1;
    }

    mbedtls_ssl_conf_ca_chain(&tls_context->ssl_config, &tls_context->ca_cert, NULL);
    mbedtls_ssl_conf_authmode(&tls_context->ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);
#else
    LOGW("MQTT broker certificate verification is disabled");
    mbedtls_ssl_conf_authmode(&tls_context->ssl_config, MBEDTLS_SSL_VERIFY_NONE);
#endif

    mbedtls_ssl_conf_rng(&tls_context->ssl_config,
                         mbedtls_ctr_drbg_random,
                         &tls_context->ctr_drbg);

    result = mbedtls_ssl_setup(&tls_context->ssl, &tls_context->ssl_config);
    if (result != 0) {
        LOGE("TLS context setup failed: -0x%04x", -result);
        return -1;
    }

    result = mbedtls_ssl_set_hostname(&tls_context->ssl, MQTTAPP_BROKER_HOST);
    if (result != 0) {
        LOGE("Setting TLS hostname failed: -0x%04x", -result);
        return -1;
    }

    mbedtls_ssl_set_bio(&tls_context->ssl,
                        &tls_context->net_fd,
                        mbedtls_net_send,
                        mbedtls_net_recv,
                        NULL);

    do {
        result = mbedtls_ssl_handshake(&tls_context->ssl);
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (result != 0);

    if (result != 0) {
        LOGE("TLS handshake failed: -0x%04x", -result);
        return -1;
    }

    LOGI("TLS connected to %s:%d", MQTTAPP_BROKER_HOST, MQTTAPP_BROKER_PORT);
    return 0;
}

static MQTTStatus_t mqttapp_agent_init(NetworkContext_t *network_context,
                                       TransportInterface_t *transport)
{
    MQTTFixedBuffer_t network_buffer = {
        .pBuffer = s_network_buffer,
        .size = MQTTAPP_NETWORK_BUFFER_SIZE,
    };
    MQTTAgentMessageInterface_t message_interface = {
        .pMsgCtx = &s_agent_message_context,
        .send = mqttapp_agent_send_command,
        .recv = mqttapp_agent_receive_command,
        .getCommand = mqttapp_agent_get_command,
        .releaseCommand = mqttapp_agent_release_command,
    };

    transport->recv = mqttapp_transport_recv;
    transport->send = mqttapp_transport_send;
    transport->pNetworkContext = network_context;

    return MQTTAgent_Init(&s_agent_context,
                          &message_interface,
                          &network_buffer,
                          transport,
                          mqttapp_time_ms,
                          mqttapp_incoming_publish_callback,
                          NULL);
}

static void mqttapp_agent_connect_and_run(void)
{
    TransportInterface_t transport = { 0 };
    MQTTConnectInfo_t connect_info = { 0 };
    MQTTStatus_t mqtt_status;
    bool session_present = false;

    mqtt_status = mqttapp_agent_init(&s_network_context, &transport);
    if (mqtt_status != MQTTSuccess) {
        LOGE("MQTT Agent initialization failed: %d", mqtt_status);
        return;
    }

    connect_info.cleanSession = true;
    connect_info.pClientIdentifier = MQTTAPP_CLIENT_ID;
    connect_info.clientIdentifierLength = strlen(MQTTAPP_CLIENT_ID);
    connect_info.keepAliveSeconds = 60;
    if (MQTTAPP_USERNAME[0] != '\0') {
        connect_info.pUserName = MQTTAPP_USERNAME;
        connect_info.userNameLength = strlen(MQTTAPP_USERNAME);
    }
    if (MQTTAPP_PASSWORD[0] != '\0') {
        connect_info.pPassword = MQTTAPP_PASSWORD;
        connect_info.passwordLength = strlen(MQTTAPP_PASSWORD);
    }

    mqtt_status = MQTT_Connect(&s_agent_context.mqttContext,
                               &connect_info,
                               NULL,
                               10000,
                               &session_present);
    if (mqtt_status != MQTTSuccess) {
        LOGE("MQTT connection failed: %d", mqtt_status);
        return;
    }

    s_agent_connected = true;
    xEventGroupSetBits(s_events, MQTTAPP_EVENT_MQTT_CONNECTED);
    LOGI("MQTT connected as %s", MQTTAPP_CLIENT_ID);

    mqtt_status = MQTTAgent_CommandLoop(&s_agent_context);
    LOGW("MQTT Agent loop stopped: %d", mqtt_status);

    s_agent_connected = false;
    xEventGroupClearBits(s_events, MQTTAPP_EVENT_MQTT_CONNECTED);
    (void)MQTTAgent_CancelAll(&s_agent_context);
    (void)MQTT_Disconnect(&s_agent_context.mqttContext);
}

static void mqttapp_agent_task(void *argument)
{
    (void)argument;

    while (1) {
        xEventGroupWaitBits(s_events,
                            MQTTAPP_EVENT_NETWORK_READY,
                            pdFALSE,
                            pdTRUE,
                            portMAX_DELAY);

        if (mqttapp_tls_connect(&s_tls_context) != 0) {
            mqttapp_tls_cleanup(&s_tls_context);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        mqttapp_agent_connect_and_run();
        mqttapp_tls_cleanup(&s_tls_context);
        xEventGroupClearBits(s_events, MQTTAPP_EVENT_MQTT_CONNECTED);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static MQTTStatus_t mqttapp_subscribe(void)
{
    MQTTAgentSubscribeArgs_t subscribe_args = { 0 };
    MQTTSubscribeInfo_t subscribe_info = { 0 };
    MQTTAgentCommandInfo_t command_info = { 0 };
    struct MQTTAgentCommandContext command_context = {
        .semaphore = xSemaphoreCreateBinary(),
    };
    MQTTStatus_t mqtt_status;

    if (command_context.semaphore == NULL) {
        return MQTTBadParameter;
    }

    subscribe_info.qos = MQTTAPP_SUBSCRIBE_QOS;
    subscribe_info.pTopicFilter = MQTTAPP_SUBSCRIBE_TOPIC;
    subscribe_info.topicFilterLength = strlen(MQTTAPP_SUBSCRIBE_TOPIC);
    subscribe_args.pSubscribeInfo = &subscribe_info;
    subscribe_args.numSubscriptions = 1;

    command_info.cmdCompleteCallback = mqttapp_command_complete_callback;
    command_info.pCmdCompleteCallbackContext = &command_context;
    command_info.blockTimeMs = 5000;

    mqtt_status = MQTTAgent_Subscribe(&s_agent_context, &subscribe_args, &command_info);
    if (mqtt_status == MQTTSuccess) {
        xSemaphoreTake(command_context.semaphore, portMAX_DELAY);
    }

    vSemaphoreDelete(command_context.semaphore);
    return mqtt_status;
}

static MQTTStatus_t mqttapp_publish_to_topic(const char *topic, const char *payload)
{
    MQTTAgentCommandInfo_t command_info = { 0 };
    MQTTPublishInfo_t publish_info = { 0 };
    struct MQTTAgentCommandContext command_context = {
        .semaphore = xSemaphoreCreateBinary(),
        .return_code = MQTTRecvFailed,
    };
    MQTTStatus_t mqtt_status;

    if (command_context.semaphore == NULL) {
        return MQTTNoMemory;
    }

    publish_info.qos = MQTTAPP_PUBLISH_QOS;
    publish_info.pTopicName = topic;
    publish_info.topicNameLength = strlen(topic);
    publish_info.pPayload = payload;
    publish_info.payloadLength = strlen(payload);
    command_info.cmdCompleteCallback = mqttapp_command_complete_callback;
    command_info.pCmdCompleteCallbackContext = &command_context;
    command_info.blockTimeMs = MQTTAPP_PUBLISH_TIMEOUT_MS;

    mqtt_status = MQTTAgent_Publish(&s_agent_context, &publish_info, &command_info);
    if (mqtt_status == MQTTSuccess) {
        xSemaphoreTake(command_context.semaphore, portMAX_DELAY);
    }

    if (mqtt_status == MQTTSuccess) {
        mqtt_status = command_context.return_code;
    }

    vSemaphoreDelete(command_context.semaphore);
    return mqtt_status;
}

static bool mqttapp_format_light_payload(const char *command,
                                         char *payload,
                                         size_t payload_size)
{
    static const char light_on_payload[] = "{\"light\":1}";
    static const char light_off_payload[] = "{\"light\":0}";
    const char *formatted_payload = NULL;

    if (strcmp(command, "开灯") == 0 || strcmp(command, "打开灯") == 0) {
        formatted_payload = light_on_payload;
    } else if (strcmp(command, "关灯") == 0 || strcmp(command, "关闭灯") == 0) {
        formatted_payload = light_off_payload;
    }

    if (formatted_payload == NULL || strlen(formatted_payload) >= payload_size) {
        return false;
    }

    strcpy(payload, formatted_payload);
    return true;
}

int mqttapp_publish_voice_command(const char *command, size_t command_length)
{
    mqttapp_voice_command_t message = { 0 };

    if (command == NULL || command_length == 0 ||
        command_length >= sizeof(message.command)) {
        return -1;
    }

    if (s_voice_command_queue == NULL) {
        return -1;
    }

    memcpy(message.command, command, command_length);
    message.command[command_length] = '\0';

    if (xQueueSend(s_voice_command_queue, &message, 0) != pdPASS) {
        LOGW("Voice command queue is full");
        return -1;
    }

    return 0;
}

static void mqttapp_voice_command_message(void *unused,
                                          uint32_t message_id,
                                          void *data,
                                          uint32_t length,
                                          void *user_data)
{
    voice_msg_cloud_recognized_command_t *recognized = data;
    size_t command_length = 0;

    (void)unused;
    (void)message_id;
    (void)user_data;

    if (recognized == NULL || length < sizeof(*recognized)) {
        return;
    }

    while (command_length < sizeof(recognized->command) &&
           recognized->command[command_length] != '\0') {
        command_length++;
    }

    if (command_length == 0 || command_length >= sizeof(recognized->command)) {
        LOGW("Ignoring invalid recognized voice command");
        return;
    }

    if (s_online_light_command_seen &&
        (xTaskGetTickCount() - s_online_light_command_tick) <
            pdMS_TO_TICKS(MQTTAPP_ONLINE_LIGHT_SUPPRESS_MS) &&
        ((s_online_light_state &&
          (strcmp(recognized->command, "开灯") == 0 ||
           strcmp(recognized->command, "打开灯") == 0)) ||
         (!s_online_light_state &&
          (strcmp(recognized->command, "关灯") == 0 ||
           strcmp(recognized->command, "关闭灯") == 0)))) {
        LOGI("Light command already forwarded by online MCP");
        return;
    }

    LOGI("Forwarding voice command: %.*s",
         (int)command_length,
         recognized->command);
    (void)mqttapp_publish_voice_command(recognized->command, command_length);
}

static void mqttapp_cloud_mcp_message(void *unused,
                                      uint32_t message_id,
                                      void *data,
                                      uint32_t length,
                                      void *user_data)
{
    cJSON *root = NULL;
    cJSON *method = NULL;
    cJSON *params = NULL;
    cJSON *tool_name = NULL;
    cJSON *arguments = NULL;
    cJSON *value = NULL;
    const char *command = NULL;
    bool light_state;

    (void)unused;
    (void)message_id;
    (void)user_data;

    if (data == NULL || length == 0 ||
        ((const char *)data)[length - 1] != '\0') {
        return;
    }

    root = cJSON_Parse((const char *)data);
    if (root == NULL) {
        return;
    }

    method = cJSON_GetObjectItemCaseSensitive(root, "method");
    params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (!cJSON_IsString(method) ||
        strcmp(method->valuestring, "tools/call") != 0 ||
        !cJSON_IsObject(params)) {
        cJSON_Delete(root);
        return;
    }

    tool_name = cJSON_GetObjectItemCaseSensitive(params, "name");
    arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (!cJSON_IsString(tool_name) ||
        strcmp(tool_name->valuestring, "ls.led_switch") != 0 ||
        !cJSON_IsObject(arguments)) {
        cJSON_Delete(root);
        return;
    }

    value = cJSON_GetObjectItemCaseSensitive(arguments, "value");
    if (!cJSON_IsBool(value)) {
        cJSON_Delete(root);
        return;
    }

    light_state = cJSON_IsTrue(value);
    command = light_state ? "开灯" : "关灯";
    if (mqttapp_publish_voice_command(command, strlen(command)) == 0) {
        s_online_light_state = light_state;
        s_online_light_command_tick = xTaskGetTickCount();
        s_online_light_command_seen = true;
    }

    cJSON_Delete(root);
}

static void mqttapp_voice_command_task(void *argument)
{
    mqttapp_voice_command_t message;

    (void)argument;

    while (1) {
        MQTTStatus_t mqtt_status;
        char payload[32];

        if (xQueueReceive(s_voice_command_queue, &message, portMAX_DELAY) != pdPASS) {
            continue;
        }

        xEventGroupWaitBits(s_events,
                            MQTTAPP_EVENT_NETWORK_READY |
                            MQTTAPP_EVENT_MQTT_CONNECTED,
                            pdFALSE,
                            pdTRUE,
                            portMAX_DELAY);

        if (!s_agent_connected) {
            continue;
        }

        if (!mqttapp_format_light_payload(message.command,
                                          payload,
                                          sizeof(payload))) {
            LOGW("Ignoring unsupported voice command: %s", message.command);
            continue;
        }

        mqtt_status = mqttapp_publish_to_topic(MQTTAPP_VOICE_COMMAND_TOPIC,
                                                payload);
        if (mqtt_status == MQTTSuccess) {
            LOGI("Published light command to %s: %s",
                 MQTTAPP_VOICE_COMMAND_TOPIC,
                 payload);
        } else {
            LOGE("Publishing voice command failed: %d", mqtt_status);
        }
    }
}

static void mqttapp_application_task(void *argument)
{
    bool subscribed = false;

    (void)argument;

    while (1) {
        EventBits_t event_bits = xEventGroupWaitBits(s_events,
                                                     MQTTAPP_EVENT_NETWORK_READY |
                                                     MQTTAPP_EVENT_MQTT_CONNECTED,
                                                     pdFALSE,
                                                     pdTRUE,
                                                     portMAX_DELAY);
        if ((event_bits & (MQTTAPP_EVENT_NETWORK_READY | MQTTAPP_EVENT_MQTT_CONNECTED)) !=
            (MQTTAPP_EVENT_NETWORK_READY | MQTTAPP_EVENT_MQTT_CONNECTED)) {
            continue;
        }

        if (!subscribed) {
            MQTTStatus_t subscribe_status = mqttapp_subscribe();
            subscribed = subscribe_status == MQTTSuccess;
            if (subscribed) {
                LOGI("Subscribed to %s", MQTTAPP_SUBSCRIBE_TOPIC);
            }
        }

#if MQTTAPP_DIAGNOSTIC_MESSAGES
        MQTTStatus_t publish_status = mqttapp_publish_to_topic(
            MQTTAPP_PUBLISH_TOPIC,
            "Hello from ARCS-MINI mqttapp over MQTT/TLS");
        if (publish_status == MQTTSuccess) {
            LOGI("Published startup message to %s", MQTTAPP_PUBLISH_TOPIC);
        } else {
            LOGE("Publishing startup message failed: %d", publish_status);
        }
#endif

        while (s_agent_connected) {
#if MQTTAPP_DIAGNOSTIC_MESSAGES && MQTTAPP_PUBLISH_INTERVAL_MS > 0
            vTaskDelay(pdMS_TO_TICKS(MQTTAPP_PUBLISH_INTERVAL_MS));
#else
            vTaskDelay(pdMS_TO_TICKS(1000));
#endif
            if (!s_agent_connected) {
                break;
            }

#if MQTTAPP_DIAGNOSTIC_MESSAGES
            publish_status = mqttapp_publish_to_topic(
                MQTTAPP_PUBLISH_TOPIC,
                "Periodic message from ARCS-MINI mqttapp");
            if (publish_status == MQTTSuccess) {
                LOGI("Published periodic message");
            } else {
                LOGE("Publishing periodic message failed: %d", publish_status);
            }
#endif
        }

        subscribed = false;
    }
}

static void mqttapp_network_message(void *unused,
                                    uint32_t message_id,
                                    void *data,
                                    uint32_t length,
                                    void *user_data)
{
    (void)unused;
    (void)data;
    (void)length;
    (void)user_data;

    if (message_id == VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS) {
        LOGI("Network is ready");
        xEventGroupSetBits(s_events, MQTTAPP_EVENT_NETWORK_READY);
    } else if (message_id == VOICE_MSG_SYSTEM_NETWORK_PROBE_FAIL ||
               message_id == VOICE_MSG_WIFI_DISCONNECTED) {
        LOGW("Network is not ready");
        s_agent_connected = false;
        xEventGroupClearBits(s_events,
                             MQTTAPP_EVENT_NETWORK_READY | MQTTAPP_EVENT_MQTT_CONNECTED);
    }
}

int mqttapp_init(void)
{
    BaseType_t result;

    if (MQTTAPP_BROKER_HOST[0] == '\0' ||
        MQTTAPP_CLIENT_ID[0] == '\0' ||
        MQTTAPP_PUBLISH_TOPIC[0] == '\0' ||
        MQTTAPP_SUBSCRIBE_TOPIC[0] == '\0' ||
        MQTTAPP_VOICE_COMMAND_TOPIC[0] == '\0') {
        LOGE("MQTT app configuration is incomplete");
        return -1;
    }

#if MQTTAPP_TLS_VERIFY_CERTIFICATE
    if (MQTTAPP_CA_CERT_PEM[0] == '\0') {
        LOGE("MQTT app CA certificate is empty");
        return -1;
    }
#endif

    s_agent_queue = xQueueCreate(MQTTAPP_AGENT_COMMAND_COUNT, sizeof(MQTTAgentCommand_t *));
    s_voice_command_queue = xQueueCreate(MQTTAPP_VOICE_COMMAND_QUEUE_LENGTH,
                                         sizeof(mqttapp_voice_command_t));
    s_command_available = xSemaphoreCreateCounting(MQTTAPP_AGENT_COMMAND_COUNT,
                                                   MQTTAPP_AGENT_COMMAND_COUNT);
    s_events = xEventGroupCreate();
    if (s_agent_queue == NULL || s_voice_command_queue == NULL ||
        s_command_available == NULL || s_events == NULL) {
        LOGE("Creating MQTT app queues failed");
        return -1;
    }

    s_agent_message_context.queue = s_agent_queue;
    memset(s_command_in_use, 0, sizeof(s_command_in_use));

    result = voice_msg_sub(VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS,
                           mqttapp_network_message,
                           NULL);
    result |= voice_msg_sub(VOICE_MSG_SYSTEM_NETWORK_PROBE_FAIL,
                            mqttapp_network_message,
                            NULL);
    result |= voice_msg_sub(VOICE_MSG_WIFI_DISCONNECTED,
                            mqttapp_network_message,
                            NULL);
    result |= voice_msg_sub(VOICE_MSG_RECOGNIZED_ONLINE_COMMAND,
                            mqttapp_voice_command_message,
                            NULL);
    result |= voice_msg_sub(VOICE_MSG_RECOGNIZED_OFFLINE_COMMAND,
                            mqttapp_voice_command_message,
                            NULL);
    result |= voice_msg_sub(VOICE_MSG_CLOUD_MCP,
                            mqttapp_cloud_mcp_message,
                            NULL);
    if (result != 0) {
        LOGE("Subscribing to network messages failed");
        return -1;
    }

    sys_network_status_t network_status = { 0 };
    if (sys_network_get_status(&network_status) == 0 && network_status.connected) {
        xEventGroupSetBits(s_events, MQTTAPP_EVENT_NETWORK_READY);
    }

    result = xTaskCreate(mqttapp_agent_task,
                         "mqttapp_agent",
                         MQTTAPP_AGENT_TASK_STACK_SIZE,
                         NULL,
                         MQTTAPP_TASK_PRIORITY,
                         NULL);
    if (result != pdPASS) {
        LOGE("Creating MQTT Agent task failed");
        return -1;
    }

    result = xTaskCreate(mqttapp_application_task,
                         "mqttapp_app",
                         MQTTAPP_APP_TASK_STACK_SIZE,
                         NULL,
                         MQTTAPP_TASK_PRIORITY - 1,
                         NULL);
    if (result != pdPASS) {
        LOGE("Creating MQTT application task failed");
        return -1;
    }

    result = xTaskCreate(mqttapp_voice_command_task,
                         "mqttapp_voice",
                         MQTTAPP_VOICE_COMMAND_TASK_STACK_SIZE,
                         NULL,
                         MQTTAPP_TASK_PRIORITY,
                         NULL);
    if (result != pdPASS) {
        LOGE("Creating MQTT voice command task failed");
        return -1;
    }

    LOGI("mqttapp initialized for %s:%d", MQTTAPP_BROKER_HOST, MQTTAPP_BROKER_PORT);
    return 0;
}
