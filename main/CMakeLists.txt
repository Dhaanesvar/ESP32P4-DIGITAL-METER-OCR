set(APP_DIR app)

set(MINIMAL_APP_SOURCES
    "main.c"
    "${APP_DIR}/app_pushlog.c"
    "${APP_DIR}/app_pushlog_camera.c"
    "${APP_DIR}/app_pushlog_web.c"
    "${APP_DIR}/AI/app_pushlog_ai_overlay.cpp"
    "${APP_DIR}/AI/app_simple_ocr.cpp"
    "${APP_DIR}/AI/app_humanface_detect.cpp"
    "${APP_DIR}/AI/app_pedestrian_detect.cpp"
    "${APP_DIR}/AI/app_drawing_utils.cpp"
    "${APP_DIR}/Video/app_video.c"
    "${APP_DIR}/Video/app_video_utils.c"
)

idf_component_register(
    SRCS ${MINIMAL_APP_SOURCES}
    INCLUDE_DIRS "." ${APP_DIR} ${APP_DIR}/Video ${APP_DIR}/AI
    REQUIRES
        esp_driver_gpio
        esp_driver_ppa
        esp_driver_uart
        esp_driver_jpeg
        nvs_flash
        esp_event
        esp_http_server
        esp_http_client
        esp_netif
        esp_wifi
        esp-tls
        esp_video
        human_face_detect
        pedestrian_detect
)