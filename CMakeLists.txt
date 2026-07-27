cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_PROJECT_NAME BOOTLOADER_PLCJS)
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

enable_language(C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

add_executable(${CMAKE_PROJECT_NAME})

# ---------- Module identity (overridable per module variant) ---------------
# Pass -DPRODUCT_ID=0x... and/or -DHW_REVISION=0xMMmm on the cmake command line
# to build a bootloader for a specific hardware variant.
#
# HW_REVISION encoding: (major << 8) | minor  →  e.g. hw:01.02 = 0x0102
# OTA validation: product_id exact match + hw_revision major byte match.
set(PRODUCT_ID   "0x12D1D4A0" CACHE STRING "Product ID for this module variant")
set(HW_REVISION  "0x0101"     CACHE STRING "HW revision (major<<8|minor), e.g. hw:01.01 = 0x0101")

# ---------- Compile definitions ------------------------------------------
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    USE_HAL_DRIVER
    STM32F407xx
    NMBS_CLIENT_DISABLED=1
    $<$<CONFIG:Debug>:DEBUG>
    PRODUCT_ID_DEFAULT=${PRODUCT_ID}u
    HW_REVISION_DEFAULT=${HW_REVISION}u
)

# ---------- Include paths ------------------------------------------------
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Inc
    LWIP/App
    LWIP/Target
    Middlewares/Third_Party/LwIP/src/include
    Middlewares/Third_Party/LwIP/system
    Middlewares/Third_Party/LwIP/system/arch
    Middlewares/Third_Party/LwIP/src/include/lwip
    Middlewares/Third_Party/LwIP/src/include/lwip/priv
    Middlewares/Third_Party/LwIP/src/include/lwip/prot
    Middlewares/Third_Party/LwIP/src/include/netif
    Middlewares/Third_Party/LwIP/src/include/compat/posix
    Middlewares/Third_Party/LwIP/src/include/compat/posix/arpa
    Middlewares/Third_Party/LwIP/src/include/compat/posix/net
    Middlewares/Third_Party/LwIP/src/include/compat/posix/sys
    Middlewares/Third_Party/LwIP/src/include/compat/stdc
    Drivers/STM32F4xx_HAL_Driver/Inc
    Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
    Drivers/CMSIS/Device/ST/STM32F4xx/Include
    Drivers/CMSIS/Include
    # Application
    Application/boot
    Application/flash
    Application/crc
    Application/validate
    Application/led
    Application/modbus
    Application/installer
    Application/third_party/nanomodbus
)

# ---------- Application sources ------------------------------------------
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Core
    Core/Src/main.c
    Core/Src/gpio.c
    Core/Src/stm32f4xx_it.c
    Core/Src/stm32f4xx_hal_msp.c
    Core/Src/system_stm32f4xx.c
    Core/Src/sysmem.c
    Core/Src/syscalls.c
    Core/Startup/startup_stm32f407xx.s
    # LWIP app
    LWIP/App/lwip.c
    LWIP/App/sys_arch.c
    LWIP/Target/ethernetif.c
    LWIP/Target/eth_custom_phy_interface.c
    # Application
    Application/boot/boot_main.c
    Application/boot/boot_entry.c
    Application/boot/boot_jump.c
    Application/flash/flash_if.c
    Application/flash/metadata.c
    Application/crc/crc32.c
    Application/validate/app_validate.c
    Application/led/led_indication.c
    Application/modbus/modbus_boot_server.c
    Application/modbus/fw_update_proto.c
    Application/installer/fw_installer.c
    Application/third_party/nanomodbus/nanomodbus.c
)

# ---------- STM32 HAL drivers --------------------------------------------
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ramfunc.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_eth.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_exti.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim_ex.c
)

# ---------- LwIP core (NO_SYS = 1, no PPP, no sequential API) -----------
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Middlewares/Third_Party/LwIP/src/core/init.c
    Middlewares/Third_Party/LwIP/src/core/def.c
    Middlewares/Third_Party/LwIP/src/core/dns.c
    Middlewares/Third_Party/LwIP/src/core/inet_chksum.c
    Middlewares/Third_Party/LwIP/src/core/ip.c
    Middlewares/Third_Party/LwIP/src/core/mem.c
    Middlewares/Third_Party/LwIP/src/core/memp.c
    Middlewares/Third_Party/LwIP/src/core/netif.c
    Middlewares/Third_Party/LwIP/src/core/pbuf.c
    Middlewares/Third_Party/LwIP/src/core/raw.c
    Middlewares/Third_Party/LwIP/src/core/stats.c
    Middlewares/Third_Party/LwIP/src/core/sys.c
    Middlewares/Third_Party/LwIP/src/core/altcp.c
    Middlewares/Third_Party/LwIP/src/core/altcp_alloc.c
    Middlewares/Third_Party/LwIP/src/core/altcp_tcp.c
    Middlewares/Third_Party/LwIP/src/core/tcp.c
    Middlewares/Third_Party/LwIP/src/core/tcp_in.c
    Middlewares/Third_Party/LwIP/src/core/tcp_out.c
    Middlewares/Third_Party/LwIP/src/core/timeouts.c
    Middlewares/Third_Party/LwIP/src/core/udp.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/autoip.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/dhcp.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/etharp.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/icmp.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/igmp.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/ip4.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/ip4_addr.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/ip4_frag.c
    Middlewares/Third_Party/LwIP/src/netif/ethernet.c
    Middlewares/Third_Party/LwIP/src/api/err.c
)

# Remove wrong libob.a
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# ---------- Output files (.elf / .hex / .bin) ----------------------------
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES SUFFIX ".elf")

# Print firmware size after linking and generate .hex / .bin images.
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${CMAKE_PROJECT_NAME}> $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.hex and ${CMAKE_PROJECT_NAME}.bin"
)
