# Included by AzerothCore's module configuration to register Social integration tests.

if(BUILD_TESTING)
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotSocialControlTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotSocialCoordinatorTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotSocialDeliveryTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotSocialExtractionTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotSocialPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotSocialRepositoryTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotSocialRouteTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_SOURCE_DIR}/modules/mod-playerbots-personality/src")
endif()
