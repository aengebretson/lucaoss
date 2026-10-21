add_executable(luca-ingestion-engine main.cpp)
target_link_libraries(luca-ingestion-engine PRIVATE luca::adapters)
install(TARGETS luca-ingestion-engine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
