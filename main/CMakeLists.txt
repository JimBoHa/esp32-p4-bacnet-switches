set(private_dependencies
    app_update
    esp_app_format
    esp_driver_gpio
    esp_driver_tsens
    esp_eth
    esp_event
    esp_http_server
    esp_https_server
    esp_hw_support
    esp_netif
    esp_partition
    esp_system
    esp_timer
    lwip
    mbedtls
    nvs_flash
)

if(DEFINED IDF_VERSION_MAJOR AND IDF_VERSION_MAJOR GREATER_EQUAL 6)
    list(APPEND private_dependencies ip101)
endif()

get_filename_component(project_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(ota_embed_files)
if(CONFIG_OTA_HTTPS_ENABLED)
    set(ota_secrets_dir "${project_root}/secrets")
    if(DEFINED ENV{ESP32_P4_OTA_SECRETS_DIR})
        set(ota_secrets_dir "$ENV{ESP32_P4_OTA_SECRETS_DIR}")
    endif()
    set(ota_private_key "${ota_secrets_dir}/ota_server_key.pem")
    set(ota_token_file "${ota_secrets_dir}/ota_token.txt")
    foreach(secret_file IN ITEMS "${ota_private_key}" "${ota_token_file}")
        if(NOT EXISTS "${secret_file}")
            message(FATAL_ERROR
                "Missing OTA secret ${secret_file}. Run tools/generate_ota_credentials.py "
                "or set ESP32_P4_OTA_SECRETS_DIR.")
        endif()
    endforeach()
    add_custom_target(validate_ota_credentials ALL
        COMMAND "${PYTHON}"
            "${project_root}/tools/validate_ota_credentials.py"
            --certificate "${CMAKE_CURRENT_LIST_DIR}/ota_server_cert.pem"
            --private-key "${ota_private_key}"
            --token-file "${ota_token_file}"
        DEPENDS
            "${project_root}/tools/validate_ota_credentials.py"
            "${CMAKE_CURRENT_LIST_DIR}/ota_server_cert.pem"
            "${ota_private_key}"
            "${ota_token_file}"
        COMMENT "Validating HTTPS OTA credentials"
        VERBATIM
    )
    list(APPEND ota_embed_files
        "ota_server_cert.pem"
        "${ota_private_key}"
        "${ota_token_file}"
    )
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "bacnet_codec.c"
        "bacnet_server.c"
        "diagnostics.c"
        "ota_auth.c"
        "ota_health.c"
        "ota_server.c"
        "switch_inputs.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${private_dependencies}
    EMBED_TXTFILES ${ota_embed_files}
)

if(CONFIG_OTA_HTTPS_ENABLED)
    add_dependencies(${COMPONENT_LIB} validate_ota_credentials)
endif()

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${project_root}"
    OUTPUT_VARIABLE project_git_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE git_revision_result
)
if(NOT git_revision_result EQUAL 0 OR project_git_revision STREQUAL "")
    set(project_git_revision "unknown")
else()
    execute_process(
        COMMAND git diff --quiet --ignore-submodules HEAD
        WORKING_DIRECTORY "${project_root}"
        RESULT_VARIABLE git_dirty_result
    )
    if(NOT git_dirty_result EQUAL 0)
        string(APPEND project_git_revision "-dirty")
    endif()
endif()
target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE PROJECT_GIT_REVISION="${project_git_revision}"
)
