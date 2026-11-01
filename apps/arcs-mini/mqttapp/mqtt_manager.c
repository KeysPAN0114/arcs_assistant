#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cJSON.h"
#include "lisa_log.h"
#include "lisa_ui.h"
#include "lisa_ui_fonts.h"
#include "lisa_ui_invoke.h"
#include "lisa_ui_llm_base.h"
#include "lisa_ui_nav_scr.h"
#include "lisa_ui_nav_scr_ids.h"
#include "lvgl.h"

#include "mqtt_manager.h"

#define TAG "mqtt.manager"

#define MQTT_MANAGER_STATE_LIGHT   (1U << 0)
#define MQTT_MANAGER_STATE_TEMP    (1U << 1)
#define MQTT_MANAGER_STATE_HUMID   (1U << 2)

typedef struct {
    uint32_t valid;
    bool light_on;
    float temperature;
    float humidity;
} mqtt_manager_state_t;

typedef struct {
    lisa_ui_llm_base_t base;
    lv_obj_t *light_label;
    lv_obj_t *temperature_label;
    lv_obj_t *humidity_label;
} mqtt_manager_screen_t;

static const struct lisa_ui_nav_scr s_mqtt_manager_nav;
static bool s_nav_registered;
static mqtt_manager_screen_t *s_screen;
static mqtt_manager_state_t s_state;

static void mqtt_manager_label_update(mqtt_manager_screen_t *screen,
                                      const mqtt_manager_state_t *state)
{
    char value_text[32];

    if (screen == NULL || state == NULL) {
        return;
    }

    if (screen->light_label != NULL) {
        if ((state->valid & MQTT_MANAGER_STATE_LIGHT) != 0U) {
            lv_label_set_text(screen->light_label,
                              state->light_on ? "开" : "关");
        } else {
            lv_label_set_text(screen->light_label, "--");
        }
    }

    if (screen->temperature_label != NULL) {
        if ((state->valid & MQTT_MANAGER_STATE_TEMP) != 0U) {
            snprintf(value_text, sizeof(value_text), "%.1f °C",
                     state->temperature);
            lv_label_set_text(screen->temperature_label, value_text);
        } else {
            lv_label_set_text(screen->temperature_label, "--");
        }
    }

    if (screen->humidity_label != NULL) {
        if ((state->valid & MQTT_MANAGER_STATE_HUMID) != 0U) {
            snprintf(value_text, sizeof(value_text), "%.1f %%",
                     state->humidity);
            lv_label_set_text(screen->humidity_label, value_text);
        } else {
            lv_label_set_text(screen->humidity_label, "--");
        }
    }
}

static lv_obj_t *mqtt_manager_create_card(lv_obj_t *container,
                                          const char *title,
                                          lv_obj_t **value_label)
{
    lv_obj_t *card = lv_obj_create(container);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, 42);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x302D2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(card, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_width(title_label, 66);
    lv_obj_set_style_text_color(title_label,
                                lv_color_hex(0xF5F0E8), LV_PART_MAIN);

    *value_label = lv_label_create(card);
    lv_label_set_text(*value_label, "--");
    lv_obj_set_flex_grow(*value_label, 1);
    lv_obj_set_style_text_color(*value_label,
                                lv_color_hex(0xFABE00), LV_PART_MAIN);
    lv_obj_set_style_text_align(*value_label, LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    return card;
}

static void mqtt_manager_screen_constructor(const lv_obj_class_t *class_p,
                                            lv_obj_t *obj)
{
    mqtt_manager_screen_t *screen = (mqtt_manager_screen_t *)obj;
    lv_obj_t *container;

    (void)class_p;
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xFFF6CC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title_label = lv_label_create(lisa_ui_llm_base_bar_get(obj));
    lv_label_set_text(title_label, "MQTT 管理");
    lv_obj_set_style_text_font(title_label, &lv_font_chinese_16,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label,
                                lv_color_hex(0xF5F0E8), LV_PART_MAIN);
    lv_obj_center(title_label);

    lisa_ui_llm_base_set_title(obj, "MQTT 管理");
    container = lisa_ui_llm_base_container_get(obj);
    if (container == NULL) {
        return;
    }

    lv_obj_set_style_pad_all(container, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(container, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    mqtt_manager_create_card(container, "灯", &screen->light_label);
    mqtt_manager_create_card(container, "温度", &screen->temperature_label);
    mqtt_manager_create_card(container, "湿度", &screen->humidity_label);

    lv_obj_t *topic_label = lv_label_create(container);
    lv_label_set_text(topic_label, "来源: /topic/espcli/up");
    lv_obj_set_width(topic_label, LV_PCT(100));
    lv_label_set_long_mode(topic_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(topic_label,
                                lv_color_hex(0x6A625A), LV_PART_MAIN);
    lv_obj_set_style_text_align(topic_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(topic_label, &lv_font_chinese_16,
                               LV_PART_MAIN);

    mqtt_manager_label_update(screen, &s_state);
}

static void mqtt_manager_screen_destructor(const lv_obj_class_t *class_p,
                                           lv_obj_t *obj)
{
    (void)class_p;
    (void)obj;
    s_screen = NULL;
}

static const lv_obj_class_t s_mqtt_manager_class = {
    .base_class = &lisa_ui_llm_base_class,
    .instance_size = sizeof(mqtt_manager_screen_t),
    .constructor_cb = mqtt_manager_screen_constructor,
    .destructor_cb = mqtt_manager_screen_destructor,
};

static int mqtt_manager_page_open(const struct lisa_ui_nav_scr *scr,
                                  void **data)
{
    lv_obj_t *obj;

    (void)scr;
    obj = lv_obj_class_create_obj(&s_mqtt_manager_class, lv_scr_act());
    if (obj == NULL) {
        return -1;
    }

    lv_obj_class_init_obj(obj);
    s_screen = (mqtt_manager_screen_t *)obj;
    *data = obj;
    return 0;
}

static int mqtt_manager_page_show(const struct lisa_ui_nav_scr *scr,
                                  void *data)
{
    (void)scr;
    lv_obj_clear_flag((lv_obj_t *)data, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load((lv_obj_t *)data);
    return 0;
}

static int mqtt_manager_page_pause(const struct lisa_ui_nav_scr *scr,
                                   void *data)
{
    (void)scr;
    lv_obj_add_flag((lv_obj_t *)data, LV_OBJ_FLAG_HIDDEN);
    return 0;
}

static int mqtt_manager_page_resume(const struct lisa_ui_nav_scr *scr,
                                    void *data)
{
    (void)scr;
    lv_obj_clear_flag((lv_obj_t *)data, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load((lv_obj_t *)data);
    return 0;
}

static int mqtt_manager_page_close(const struct lisa_ui_nav_scr *scr,
                                   void *data)
{
    lv_obj_t *obj = (lv_obj_t *)data;
    lv_obj_t *parent;

    (void)scr;
    parent = lv_obj_get_parent(obj);
    if (lv_scr_act() == obj && parent != NULL) {
        lv_scr_load(parent);
    }
    lv_obj_del(obj);
    s_screen = NULL;
    return 0;
}

static const struct lisa_ui_nav_scr s_mqtt_manager_nav = {
    .unique_id = LISA_UI_NAV_SCR_ID_MQTT_MANAGER,
    .open = mqtt_manager_page_open,
    .show = mqtt_manager_page_show,
    .pause = mqtt_manager_page_pause,
    .resume = mqtt_manager_page_resume,
    .close = mqtt_manager_page_close,
};

static void mqtt_manager_update_ui_worker(void *arg, uint32_t len)
{
    mqtt_manager_state_t state;

    if (arg == NULL || len != sizeof(state)) {
        return;
    }

    memcpy(&state, arg, sizeof(state));
    taskENTER_CRITICAL();
    if ((state.valid & MQTT_MANAGER_STATE_LIGHT) == 0U) {
        state.light_on = s_state.light_on;
    }
    if ((state.valid & MQTT_MANAGER_STATE_TEMP) == 0U) {
        state.temperature = s_state.temperature;
    }
    if ((state.valid & MQTT_MANAGER_STATE_HUMID) == 0U) {
        state.humidity = s_state.humidity;
    }
    state.valid |= s_state.valid;
    s_state = state;
    taskEXIT_CRITICAL();

    if (s_screen != NULL) {
        mqtt_manager_label_update(s_screen, &state);
    }
}

static cJSON *mqtt_manager_find_key(cJSON *object, const char *key,
                                    uint32_t depth)
{
    cJSON *item;

    if (object == NULL || depth > 3U) {
        return NULL;
    }

    item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (item != NULL) {
        return item;
    }

    if (!cJSON_IsObject(object)) {
        return NULL;
    }

    cJSON_ArrayForEach(item, object) {
        cJSON *found = mqtt_manager_find_key(item, key, depth + 1U);
        if (found != NULL) {
            return found;
        }
    }

    return NULL;
}

static bool mqtt_manager_read_float(cJSON *item, float *value)
{
    if (cJSON_IsNumber(item)) {
        *value = (float)item->valuedouble;
        return true;
    }

    if (cJSON_IsString(item) && item->valuestring != NULL) {
        char *end = NULL;
        float parsed = strtof(item->valuestring, &end);
        if (end != item->valuestring && end != NULL && *end == '\0') {
            *value = parsed;
            return true;
        }
    }

    return false;
}

void mqtt_manager_update_from_json(const char *payload, size_t length)
{
    cJSON *root;
    mqtt_manager_state_t state = { 0 };
    cJSON *item;

    if (payload == NULL || length == 0U) {
        return;
    }

    root = cJSON_ParseWithLength(payload, length);
    if (root == NULL) {
        LOGW("Ignoring invalid MQTT JSON payload");
        return;
    }

    item = mqtt_manager_find_key(root, "temperature", 0U);
    if (item == NULL) {
        item = mqtt_manager_find_key(root, "temp", 0U);
    }
    if (mqtt_manager_read_float(item, &state.temperature)) {
        state.valid |= MQTT_MANAGER_STATE_TEMP;
    }

    item = mqtt_manager_find_key(root, "humidity", 0U);
    if (item == NULL) {
        item = mqtt_manager_find_key(root, "hum", 0U);
    }
    if (item == NULL) {
        item = mqtt_manager_find_key(root, "rh", 0U);
    }
    if (item == NULL) {
        item = mqtt_manager_find_key(root, "humi", 0U);
    }
    if (mqtt_manager_read_float(item, &state.humidity)) {
        state.valid |= MQTT_MANAGER_STATE_HUMID;
    }

    cJSON_Delete(root);

    if (state.valid == 0U) {
        return;
    }

    if (lisa_ui_invoke_ui_delayed(mqtt_manager_update_ui_worker, &state,
                                  sizeof(state), 0U) != 0) {
        LOGW("Scheduling MQTT manager UI update failed");
    }
}

void mqtt_manager_set_light_state(bool light_on)
{
    mqtt_manager_state_t state = { 0 };

    state.valid = MQTT_MANAGER_STATE_LIGHT;
    state.light_on = light_on;
    if (lisa_ui_invoke_ui_delayed(mqtt_manager_update_ui_worker, &state,
                                  sizeof(state), 0U) != 0) {
        LOGW("Scheduling MQTT manager light update failed");
    }
}

int mqtt_manager_ui_init(void)
{
    int result;

    if (s_nav_registered) {
        return 0;
    }

    result = lisa_ui_nav_scr_add(&s_mqtt_manager_nav);
    if (result != 0) {
        LOGE("Registering MQTT manager screen failed: %d", result);
        return result;
    }

    taskENTER_CRITICAL();
    s_state.valid |= MQTT_MANAGER_STATE_LIGHT;
    s_state.light_on = false;
    taskEXIT_CRITICAL();

    s_nav_registered = true;
    return 0;
}

static void mqtt_manager_open_worker(void *arg, uint32_t len)
{
    (void)arg;
    (void)len;
    (void)lisa_ui_nav_scr_nav_to(LISA_UI_NAV_SCR_ID_MQTT_MANAGER);
}

int mqtt_manager_open(void)
{
    if (!s_nav_registered) {
        return -1;
    }

    return lisa_ui_invoke_ui_delayed(mqtt_manager_open_worker, NULL, 0U, 0U);
}

static void mqtt_manager_close_worker(void *arg, uint32_t len)
{
    (void)arg;
    (void)len;
    (void)lisa_ui_nav_scr_nav_default();
}

bool mqtt_manager_handle_long_hold(void)
{
    if (!s_nav_registered ||
        lisa_ui_nav_scr_get_top_id() != LISA_UI_NAV_SCR_ID_MQTT_MANAGER) {
        return false;
    }

    if (lisa_ui_invoke_ui_delayed(mqtt_manager_close_worker, NULL, 0U, 0U) != 0) {
        LOGW("Scheduling MQTT manager close failed");
        return false;
    }

    return true;
}
