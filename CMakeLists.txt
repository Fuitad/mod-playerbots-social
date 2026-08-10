if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(mod_playerbots_social LANGUAGES CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  include(CTest)

  add_executable(playerbots_social_tests
    tests/cpp/PlayerbotSocialConfigTest.cpp
    src/Bot/Social/PlayerbotSocialConfig.cpp)
  target_include_directories(playerbots_social_tests PRIVATE
    tests/standalone
    src)
  target_compile_definitions(playerbots_social_tests PRIVATE PLAYERBOTS_SOCIAL_STANDALONE)
  target_compile_options(playerbots_social_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME playerbots_social_tests COMMAND playerbots_social_tests)
else()
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/Bot/PlayerbotAI.h")
    message(FATAL_ERROR "mod-playerbots-social requires mod-playerbots")
  endif()
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbot-personality/src/Bot/Personality/PlayerbotPersonality.h")
    message(FATAL_ERROR "mod-playerbots-social requires mod-playerbot-personality")
  endif()
  AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src")
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
      "${CMAKE_SOURCE_DIR}/modules/mod-playerbot-personality/src")
  endif()
endif()
