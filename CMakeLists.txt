cmake_minimum_required(VERSION 3.16)
project(weather_service C)

set(CMAKE_C_STANDARD 11)

# Строгая сборка: любое предупреждение компилятора считается ошибкой.
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Werror)

# Проверка памяти: cmake -B build-check -DSANITIZE=ON
option(SANITIZE "Build with AddressSanitizer and UBSan" OFF)
if(SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Внешние библиотеки (на Ubuntu/Debian ставятся через apt).
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET libmicrohttpd libcurl sqlite3 libcjson)
find_package(Threads REQUIRED)    # потоки: сервер обслуживает запросы параллельно

# Все модули собираются в одну библиотеку core.
add_library(core
  src/config.c
  src/logger.c
  src/server.c
  src/weather.c)
target_include_directories(core PUBLIC src)
target_link_libraries(core PUBLIC PkgConfig::DEPS Threads::Threads)

# Программа: только main.c плюс библиотека.
add_executable(weather_service src/main.c)
target_link_libraries(weather_service PRIVATE core)

# Тесты берут ту же библиотеку: ctest --test-dir build
enable_testing()
add_executable(test_config tests/test_config.c)
target_link_libraries(test_config PRIVATE core)
add_test(NAME config COMMAND test_config)

# Сквозной тест: настоящий сервер + подставной погодный сайт, без интернета.
add_executable(test_service tests/test_service.c)
target_link_libraries(test_service PRIVATE core)
add_test(NAME service COMMAND test_service)
set_tests_properties(service PROPERTIES TIMEOUT 60)

# Тест программы целиком: запуск, остановка, ошибки при старте.
add_executable(test_main tests/test_main.c)
target_link_libraries(test_main PRIVATE PkgConfig::DEPS)
add_test(NAME main COMMAND test_main $<TARGET_FILE:weather_service>
                                     ${CMAKE_CURRENT_SOURCE_DIR}/config.env)
set_tests_properties(main PROPERTIES TIMEOUT 60)
