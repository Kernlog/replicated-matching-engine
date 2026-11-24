rme_add_test(node_test SOURCES node_test.cpp LIBS rme_node rme_gateway rme_testing)
add_test(NAME node_sigterm COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/sigterm_test.sh $<TARGET_FILE:rme_node_bin>)
