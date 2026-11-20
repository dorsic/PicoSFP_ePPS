cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(ePPS_Rx C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(ePPS_Rx
    main.c
    sfp.c
    clock_startup.c
    picoDEC.pio
    picoSQ.pio
    picoPOFF.pio
)

pico_generate_pio_header(ePPS_Rx ${CMAKE_CURRENT_LIST_DIR}/picoDEC.pio)
pico_generate_pio_header(ePPS_Rx ${CMAKE_CURRENT_LIST_DIR}/picoSQ.pio)
pico_generate_pio_header(ePPS_Rx ${CMAKE_CURRENT_LIST_DIR}/picoPOFF.pio)


target_link_libraries(ePPS_Rx
    pico_stdlib
    pico_divider
    pico_double
    pico_bootrom
    hardware_clocks
    hardware_i2c
    hardware_pio
    hardware_pll
    hardware_ticks
    hardware_timer
    hardware_watchdog
    hardware_xosc
)

target_compile_options(ePPS_Rx PRIVATE -Wall -Wextra)
target_compile_definitions(ePPS_Rx PRIVATE
    PICO_USE_GPIO_COPROCESSOR=0
    PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
    PICO_RUNTIME_NO_INIT_CLOCKS=1
    XOSC_HZ=10000000
    SYS_CLK_HZ=80000000
    PLL_SYS_REFDIV=1
    PLL_SYS_VCO_FREQ_HZ=1600000000
    PLL_SYS_POSTDIV1=5
    PLL_SYS_POSTDIV2=4
    PLL_USB_REFDIV=1
    PLL_USB_VCO_FREQ_HZ=1200000000
    PLL_USB_POSTDIV1=5
    PLL_USB_POSTDIV2=5    
)

pico_enable_stdio_usb(ePPS_Rx 1)
pico_enable_stdio_uart(ePPS_Rx 0)
pico_add_extra_outputs(ePPS_Rx)
