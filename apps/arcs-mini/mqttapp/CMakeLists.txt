listenai_library_named(app-mqttapp)

listenai_library_sources(
    mqttapp.c
    mqtt_manager.c
)

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_TOP_DIR}/apps-ui/apps/llm/views
)
