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

  add_executable(playerbots_social_content_tests
    tests/cpp/PlayerbotSocialContentTest.cpp
    src/Bot/Social/PlayerbotSocialContent.cpp)
  target_include_directories(playerbots_social_content_tests PRIVATE src)
  target_compile_options(playerbots_social_content_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME playerbots_social_content_tests COMMAND playerbots_social_content_tests)
else()
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/Bot/PlayerbotAI.h")
    message(FATAL_ERROR "mod-playerbots-social requires mod-playerbots")
  endif()
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbots-personality/src/Bot/Personality/PlayerbotPersonality.h")
    message(FATAL_ERROR "mod-playerbots-social requires mod-playerbots-personality")
  endif()
  AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src")
  include("${CMAKE_CURRENT_LIST_DIR}/mod-playerbots-social.cmake")
endif()
