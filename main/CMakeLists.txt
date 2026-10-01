# Include Kconfig for benchmark selection
set(BENCHMARK_SRCS "")
if(CONFIG_BENCHMARK_TYPE_LIVERMORE)
    # Entry point; the implementation is the separate components/livermore component.
    set(BENCHMARK_SRCS "livermore_main.c")
    set(BENCHMARK_REQUIRES "esp_common esp_timer perfmon livermore")
else()
    # Default: Cray SAXPY benchmark
    set(BENCHMARK_SRCS "cray_benchmark.c" "memory_bottleneck.c")
    set(BENCHMARK_REQUIRES "esp_common esp_timer perfmon")
endif()

# Pair48 library sources (always included)
set(PAIR48_SRCS
    "asm/pair48_asm.S"
    "asm/pair48_vector.c"
    "asm/pair48_selftest.c"
)

idf_component_register(
    SRCS "Kconfig" ${BENCHMARK_SRCS} ${PAIR48_SRCS}
    INCLUDE_DIRS "." "asm"
    REQUIRES ${BENCHMARK_REQUIRES}
)
