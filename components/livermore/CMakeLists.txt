idf_component_register(
    SRCS "livermore_esp32.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${PROJECT_DIR}/main/asm"
    REQUIRES esp_common esp_timer perfmon
)
