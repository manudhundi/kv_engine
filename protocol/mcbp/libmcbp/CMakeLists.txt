add_library(mcbp STATIC
            ${Memcached_SOURCE_DIR}/include/mcbp/mcbp.h
            ${Memcached_SOURCE_DIR}/include/mcbp/protocol/datatype.h
            ${Memcached_SOURCE_DIR}/include/mcbp/protocol/magic.h
            ${Memcached_SOURCE_DIR}/include/mcbp/protocol/opcode.h
            ${Memcached_SOURCE_DIR}/include/mcbp/protocol/request.h
            ${Memcached_SOURCE_DIR}/include/mcbp/protocol/response.h
            ${Memcached_SOURCE_DIR}/include/mcbp/protocol/status.h
            dump.cc
            feature.cc
            gdb_dump_parser.cc
            lldb_dump_parser.cc
            opcode.cc
            status_to_string.cc
            )
target_link_libraries(mcbp mcd_util)

add_executable(mcbp_dump_parser_test mcbp_dump_parser_test.cc)
target_link_libraries(mcbp_dump_parser_test mcbp gtest gtest_main)
add_test(NAME mcbp_dump_parser_test
         WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
         COMMAND mcbp_dump_parser_test)

add_executable(mcbp_packet_printer mcbp_packet_printer.cc)
target_link_libraries(mcbp_packet_printer mcbp dirutils)
install(TARGETS mcbp_packet_printer RUNTIME DESTINATION bin)

add_executable(mcbp_opcode_test opcode_test.cc)
target_link_libraries(mcbp_opcode_test mcbp gtest gtest_main)
add_test(NAME mcbp_opcode_test
         WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
         COMMAND mcbp_opcode_test)

add_executable(mcbp_feature_test feature_test.cc)
target_link_libraries(mcbp_feature_test mcbp gtest gtest_main)
add_test(NAME mcbp_feature_test
         WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
         COMMAND mcbp_feature_test)
