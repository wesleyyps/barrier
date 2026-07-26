if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=fuzzer,address -fno-omit-frame-pointer")

    add_executable(fuzz_protocol_util ProtocolUtilFuzzer.cpp)

    target_include_directories(fuzz_protocol_util PRIVATE
        ${CMAKE_SOURCE_DIR}/src/lib
        ${CMAKE_SOURCE_DIR}/src/test
    )

    target_link_libraries(fuzz_protocol_util
        synlib
        base
        arch
        common
        net
        ipc
        io
        mt
    )
else()
    message(WARNING "Fuzzing targets require standard Clang (Apple Clang omits the libFuzzer runtime). Fuzzing targets will not be built.")
endif()
