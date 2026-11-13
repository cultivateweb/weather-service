# weather-service

Маленький сервис погоды на Си. Получает название города, спрашивает погоду
у бесплатного сайта [Open-Meteo](https://open-meteo.com) (ключ не нужен)
и отвечает температурой и ветром. Всё, что происходит, пишет в журнал:
на экран, в файл `service.log` и в базу SQLite `service.db`.

## Библиотеки (поставить один раз)

Fedora:

```sh
sudo dnf install gcc cmake pkgconf-pkg-config libmicrohttpd-devel libcurl-devel sqlite-devel cjson-devel
```

Ubuntu / Debian:

```sh
sudo apt install build-essential cmake pkgconf libmicrohttpd-dev libcurl4-openssl-dev libsqlite3-dev libcjson-dev sqlite3
```

## Сборка и тесты

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Тесты в интернет не ходят: вместо Open-Meteo отвечает подставной сайт внутри теста.

| Тест | Что проверяет |
|---|---|
| `config` — `tests/test_config.c` | настройки: значения по умолчанию, файл, переменные окружения, опечатки |
| `service` — `tests/test_service.c` | ответы сервера на настоящие запросы, журнал в файле и в базе, параллельную работу, быструю остановку |
| `main` — `tests/test_main.c` | программу целиком: запуск, остановку сигналом, ошибки при старте |

## Запуск

Из папки проекта (там лежит `config.env`):

```sh
./build/weather_service
```

Остановка — Ctrl+C. Другой файл настроек: `./build/weather_service my.env`.

## Запросы

```sh
curl http://localhost:8080/health
curl -d '{"city":"Київ"}' http://localhost:8080/weather
```

| Запрос | Ответ |
|---|---|
| `GET /health` (или `HEAD`) | `200` `{"status":"ok"}` |
| `POST /weather` с `{"city":"Київ"}` | `200` `{"city":"Київ","country":"Україна","temperature_c":9.9,"wind_kmh":7.9}` |
| город не найден | `404` `{"error":"city not found"}` |
| нет поля `city`, оно пустое или длиннее 100 байт, битый JSON | `400` |
| тело запроса больше 2 КБ | `413` |
| погодный сайт не ответил или ответил ерунду | `502` |
| не тот метод, например `GET /weather` | `405` и заголовок `Allow` с нужным методом |
| другой адрес | `404` `{"error":"not found"}` |

Поле `country` показывает, какой именно город нашёлся: на `Kiev` сайт находит
деревню в России, и это сразу видно по ответу.

## Настройки

Сначала берутся значения по умолчанию, потом `config.env`, потом переменные
окружения с теми же именами: `PORT=8081 ./build/weather_service`.

| Настройка | По умолчанию | Что это |
|---|---|---|
| `PORT` | `8080` | порт, на котором сервис принимает запросы (1–65535) |
| `DB_PATH` | `service.db` | база SQLite с журналом (таблица `logs`) |
| `LOG_PATH` | `service.log` | файл журнала |
| `WEATHER_URL` | `https://api.open-meteo.com/v1/forecast` | адрес прогноза |
| `GEO_URL` | `https://geocoding-api.open-meteo.com/v1/search` | адрес поиска города |
| `GEO_LANGUAGE` | `uk` | язык поиска и названий в ответе: `uk`, `ru`, `en`… Кириллицу сайт находит только на её языке: «Київ» — с `uk`, «Киев» — с `ru` |

Опечатка в файле настроек (неизвестная настройка, строка без `=`, плохое
значение) останавливает запуск с номером строки. Файл, указанный явно, обязан
существовать. Без аргумента и без `config.env` сервис работает на значениях
по умолчанию и говорит об этом.

## Журнал

```sh
cat service.log
sqlite3 service.db "SELECT * FROM logs;"
```

## Проверка памяти

Сборка с AddressSanitizer и UBSan: они ловят выход за границы памяти, утечки
и неопределённое поведение. Нужен clang (Fedora: `sudo dnf install clang compiler-rt`).

```sh
CC=clang cmake -B build-check -DSANITIZE=ON
cmake --build build-check
ctest --test-dir build-check --output-on-failure
```

Можно и с gcc (без `CC=clang`), но тогда на Fedora нужны пакеты `libasan` и `libubsan`.

## Безопасность

Сервис принимает запросы со всех сетевых адресов компьютера, пароля нет.
Брандмауэр Fedora Workstation по умолчанию пропускает порты выше 1024, поэтому
запущенный сервис видят другие устройства в той же сети.
