include("${CMAKE_CURRENT_LIST_DIR}/../../board.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../../../common/sources.cmake")
if(ASTER_BOARD_AUDIO_ADAPTER)
    include("${ASTER_BOARD_AUDIO_ADAPTER}")
endif()

set(media_sources)
set(media_deps)
if(ASTER_BOARD_MEDIA_PLAYER)
    list(APPEND media_sources ${ASTER_COMMON_MEDIA_SOURCES} player.c hifi_playlist.c bt_audio.c)
    list(APPEND media_deps nvs_flash)
    if(CONFIG_IDF_TARGET_ESP32S31)
        list(APPEND media_sources "${ASTER_COMMON_ROOT}/audio/aster_audio_prefetch.c")
    endif()
    if(ASTER_BOARD_BT_AUDIO_NAME)
        list(APPEND media_deps bt console nvs_flash)
        if(CONFIG_BT_ENABLED AND CONFIG_BT_CLASSIC_ENABLED)
            list(APPEND media_sources bt_avrcp.c bt_cover.c bt_source.c)
        endif()
    endif()
    if(ASTER_BOARD_AUDIO_SPECTRUM)
        list(APPEND media_sources "${ASTER_COMMON_ROOT}/audio/aster_audio_spectrum.c")
    endif()
elseif(ASTER_BOARD_BT_AUDIO_NAME OR ASTER_BOARD_AUDIO_SPECTRUM)
    message(FATAL_ERROR "Bluetooth audio and spectrum require ASTER_BOARD_MEDIA_PLAYER")
endif()
if(NOT ASTER_BOARD_AUDIO_SOURCES)
    message(FATAL_ERROR "Declare the board audio port in aster_board.cmake")
endif()

idf_component_register(
    SRCS ${ASTER_BOARD_AUDIO_SOURCES} ${media_sources} ${ASTER_COMMON_AUDIO_SOURCES}
         "${ASTER_PLATFORM_AUDIO}/audio.c" "${ASTER_PLATFORM_AUDIO}/audio_effect_engine.c"
         "${ASTER_PLATFORM_AUDIO}/audio_file_reader.c" "${ASTER_PLATFORM_AUDIO}/audio_lyrics.c"
         "${ASTER_PLATFORM_AUDIO}/minimp3_impl.c"
    INCLUDE_DIRS "." "../../../../contract/include" "../../../../common/audio"
                 "${ASTER_PLATFORM_AUDIO}" "../../../../modules/minimp3"
    PRIV_INCLUDE_DIRS "../../../../third_party/dr_libs" "../../../../common/port"
                      ${ASTER_BOARD_AUDIO_INCLUDE_DIRS}
    PRIV_REQUIRES ${media_deps} ${ASTER_BOARD_AUDIO_REQUIRES}
    REQUIRES aster_render esp_driver_i2c esp_driver_i2s esp_driver_gpio freertos)

target_compile_definitions(${COMPONENT_LIB}
    PUBLIC CONFIG_ASTER_AUDIO=1 CONFIG_ASTER_AUDIO_RATE=16000
           CONFIG_ASTER_AUDIO_CHANNELS=2 CONFIG_ASTER_AUDIO_BITS=16
    PRIVATE ${ASTER_BOARD_AUDIO_DEFINITIONS})
if(ASTER_BOARD_MEDIA_PLAYER)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ASTER_IDF_MEDIA_PLAYER=1)
    if(CONFIG_IDF_TARGET_ESP32S31)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ASTER_S31_AUDIO_PREFETCH=1)
    endif()
endif()
if(ASTER_BOARD_BT_AUDIO_NAME AND CONFIG_BT_ENABLED AND CONFIG_BT_CLASSIC_ENABLED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        ASTER_IDF_BT_AUDIO=1 "ASTER_IDF_BT_NAME=\"${ASTER_BOARD_BT_AUDIO_NAME}\"")
endif()
if(ASTER_BOARD_AUDIO_SPECTRUM)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ASTER_AUDIO_SPECTRUM_ENABLED=1)
endif()
