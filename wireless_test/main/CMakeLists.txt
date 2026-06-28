idf_component_register(
    SRCS "main.cc"
    INCLUDE_DIRS "."
    REQUIRES
        ai_model
        module_sim
        mpu6050
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        lwip
)