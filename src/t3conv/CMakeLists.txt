add_executable(t3conv
    args_parser.cpp
    batch_runner.cpp
    config_loader.cpp
    tbatsave_runtime_patch.cpp
    main.cpp
    process_mgr.cpp
    ../common/utils.cpp
)

target_include_directories(t3conv PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(t3conv PRIVATE
    ole32
    oleaut32
)
