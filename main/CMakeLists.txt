set(COMPONENT_INCLUDE_DIRS
    .
    ../config
)

idf_component_register(SRCS  "user_fram.c" 
                                "user_time.c"
                                "iot_shrimp.c"
                                "tcp_server_com"
                                "user_output.c"
                                "user_crc32.cpp"
                                "user_time.c"
                                "user_system.c"
                                "user_console.c"
                                "user_storage.c"
                                "user_ota.c"
                                "user_azure.c"
                                "user_http_server.c"
                                "wifi_config_manager.c"
                                "web_portal.c"
                                "dns_server.c"
                                "RS485.c"
                                "user_external_flash.c"
                                "user_log_stream.c"
                    INCLUDE_DIRS "." ${COMPONENT_INCLUDE_DIRS}
                    REQUIRES    coreMQTT
                                azure-sdk-for-c 
                                azure-iot-middleware-freertos 
                                sample-azure-iot 
                                driver 
                                nvs_flash
                                esp_netif
                                esp_wifi
                                esp_https_ota
                                app_update
                                esp_http_server
                                spi_flash
                                spiffs
                    )
