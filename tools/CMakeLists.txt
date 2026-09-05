add_library(tinylsm_cli_app STATIC cli/cli_app.cpp)
target_include_directories(tinylsm_cli_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tinylsm_cli_app PUBLIC tinylsm::tinylsm)
target_compile_features(tinylsm_cli_app PUBLIC cxx_std_20)
tinylsm_set_warnings(tinylsm_cli_app)
tinylsm_enable_sanitizers(tinylsm_cli_app)

add_executable(tinylsm_cli tinylsm_cli.cpp)
target_link_libraries(tinylsm_cli PRIVATE tinylsm_cli_app)
target_compile_features(tinylsm_cli PRIVATE cxx_std_20)
tinylsm_set_warnings(tinylsm_cli)
tinylsm_enable_sanitizers(tinylsm_cli)

add_library(tinylsm_lab_core STATIC lab/lab_session.cpp)
target_include_directories(tinylsm_lab_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                              PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tinylsm_lab_core PUBLIC tinylsm::tinylsm)
target_compile_features(tinylsm_lab_core PUBLIC cxx_std_20)
tinylsm_set_warnings(tinylsm_lab_core)
tinylsm_enable_sanitizers(tinylsm_lab_core)

add_library(tinylsm_lab_http STATIC lab/lab_http_server.cpp)
target_include_directories(tinylsm_lab_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                              PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tinylsm_lab_http PUBLIC tinylsm_lab_core httplib::httplib
                                        nlohmann_json::nlohmann_json)
target_compile_features(tinylsm_lab_http PUBLIC cxx_std_20)
tinylsm_set_warnings(tinylsm_lab_http)
tinylsm_enable_sanitizers(tinylsm_lab_http)

add_executable(tinylsm_lab_server lab/tinylsm_lab_server.cpp)
target_link_libraries(tinylsm_lab_server PRIVATE tinylsm_lab_http)
target_compile_features(tinylsm_lab_server PRIVATE cxx_std_20)
tinylsm_set_warnings(tinylsm_lab_server)
tinylsm_enable_sanitizers(tinylsm_lab_server)
