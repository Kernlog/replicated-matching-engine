rme_add_test(engine_test
  SOURCES types_test.cpp order_book_test.cpp order_index_test.cpp price_ladder_test.cpp
          differential_test.cpp
  LIBS rme_engine rme_testing)

if(NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize")
  rme_add_test(engine_alloc_test SOURCES alloc_test.cpp LIBS rme_engine rme_testing)
endif()
