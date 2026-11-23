add_library(rme_engine STATIC order_book.cpp snapshot.cpp)
target_link_libraries(rme_engine PUBLIC rme_core)
