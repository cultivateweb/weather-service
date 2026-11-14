cmake_minimum_required(VERSION 3.16)
project(weather_service C)

set(CMAKE_C_STANDARD 11)

# Строгая сборка: любое предупреждение компилятора считается ошибкой.
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Werror)

# Проверка памяти: cmake -B build-check -DSANITIZE=ON
# (с gcc на Fedora нужны пакеты libasan и libubsan; с clang — ничего: CC=clang cmake ...)
option(SANITIZE "Build with AddressSanitizer and UBSan" OFF)
if(SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Один файл со всем внутри, без библиотек системы: cmake -DSTATIC=ON.
# Нужны библиотеки в виде .a — scripts/build-static.sh собирает их из исходников.
option(STATIC "Link everything into one self-contained program" OFF)

# Файл со списком центров сертификации (PEM), который зашивается в программу.
# Пусто — программа берёт такой список из системы.
set(CA_BUNDLE "" CACHE FILEPATH "PEM file with CA certificates to embed into the program")

# Внешние библиотеки. Как поставить их на Fedora и на Ubuntu/Debian — в README.md.
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)    # потоки: сервер обслуживает запросы параллельно
set(DEPS_MODULES libmicrohttpd libcurl libpq libcjson)
add_library(deps INTERFACE)
if(STATIC)
  # --static: pkg-config называет и библиотеки, нужные самим библиотекам.
  pkg_check_modules(DEPS REQUIRED ${DEPS_MODULES})
  target_include_directories(deps INTERFACE ${DEPS_STATIC_INCLUDE_DIRS})
  target_compile_options(deps INTERFACE ${DEPS_STATIC_CFLAGS_OTHER})
  target_link_libraries(deps INTERFACE ${DEPS_STATIC_LDFLAGS})
  # -no-pie: gcc в Alpine при -static делает «перемещаемую» программу (static-pie),
  # а musl-gcc к такой не готов — она падает при запуске. Обычная работает везде.
  target_link_options(deps INTERFACE -static -no-pie)
else()
  pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET ${DEPS_MODULES})
  target_link_libraries(deps INTERFACE PkgConfig::DEPS)
endif()

# Все модули собираются в одну библиотеку core.
add_library(core
  src/config.c
  src/logger.c
  src/server.c
  src/weather.c)
target_include_directories(core PUBLIC src)
target_link_libraries(core PUBLIC deps Threads::Threads)

if(CA_BUNDLE)
  # Список центров сертификации становится массивом байт в программе.
  file(READ "${CA_BUNDLE}" ca_hex HEX)
  string(REGEX REPLACE "(..)" "0x\\1," ca_bytes "${ca_hex}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ca_bundle.c"
       "#include <stddef.h>\n"
       "const unsigned char ca_bundle[] = {${ca_bytes}};\n"
       "const size_t ca_bundle_size = sizeof ca_bundle;\n")
  target_sources(core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ca_bundle.c")
  target_compile_definitions(core PRIVATE HAVE_CA_BUNDLE)
endif()

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
target_link_libraries(test_main PRIVATE deps)
add_test(NAME main COMMAND test_main $<TARGET_FILE:weather_service>
                                     ${CMAKE_CURRENT_SOURCE_DIR}/config.env)
set_tests_properties(main PROPERTIES TIMEOUT 60)

# Журнал в PostgreSQL. Нужна настоящая база: TEST_DB_URL=postgresql://... ctest
# Без TEST_DB_URL тест возвращает 77, и ctest показывает его как Skipped.
add_executable(test_db tests/test_db.c)
target_link_libraries(test_db PRIVATE core)
add_test(NAME db COMMAND test_db)
set_tests_properties(db PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
