idf_component_register(
    SRCS "main.c" "wt_ui.c" "wt_model.c" "wt_config.c"
        "wt_network.c" "wt_provider.c" "wt_runtime.c" "board_display.c"
        "app_orientation.c" "app_orientation_logic.c"
        "wt_speech.c" "wt_conversation.c" "wt_voice.c" "wt_cache.c"
        "wt_content.c" "wt_content_blob.c" "wt_manifest.c"
        "wt_progress.c" "wt_quiz.c" "wt_store.c"
        "wt_controls.c" "wt_controls_device.c"
    INCLUDE_DIRS "."
    REQUIRES esp_iris esp-mosaico-bsp esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris freertos nvs_flash
    PRIV_REQUIRES esp_timer esp-gsp esp_display_present esp_lcd esp_http_client
        esp_http_server esp_wifi esp_netif esp_event esp-tls mbedtls cjson
        esp_codec_dev spi_nand_flash esp_littlefs)
target_add_binary_data(${COMPONENT_LIB} "setup.html" TEXT)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "../ui/main.json")
gsp_enable_freetype()
target_add_binary_data(${COMPONENT_LIB} "../ui/fonts/LearningSans.ttf" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../assets/content/wt_content.bin" BINARY)
