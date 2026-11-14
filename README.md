# weather-service

Маленький сервис погоды на Си. Получает название города, спрашивает погоду
у бесплатного сайта [Open-Meteo](https://open-meteo.com) (ключ не нужен)
и отвечает температурой и ветром. Всё, что происходит, пишет в журнал:
на экран, в файл и в базу PostgreSQL.

На компьютере нужен только Docker: сборка, тесты и запуск идут внутри него.
Программа собирается в один файл, которому не нужны библиотеки системы.

## Запуск

```sh
echo "POSTGRES_PASSWORD=$(tr -dc a-z0-9 </dev/urandom | head -c 24)" > .env   # один раз: пароль базы
docker compose up -d --build
curl -d '{"city":"Київ"}' http://localhost:8080/weather
docker compose logs app       # журнал сервиса
docker compose down           # остановить; записи журнала в базе сохраняются
```

Пароль базы лежит в файле `.env`, в git он не попадает. Сервис и база видны
только с этого компьютера (адрес 127.0.0.1).

Сборка идёт внутри Docker, в Alpine Linux — маленькой системе с инструментами
сборки. Скрипт `scripts/build-static.sh` скачивает там исходники библиотек,
сверяет их контрольные суммы, собирает библиотеки, потом сервис, и запускает
тесты. Собранные библиотеки Docker запоминает и в следующий раз не собирает заново.

Образ сервиса пустой: в нём лежит только файл программы — ни оболочки,
ни библиотек. Так видно, что одного файла программе достаточно.

## Один файл программы

```sh
docker build --target file --output out .
```

Результат — `out/weather_service`. В нём есть всё: библиотека языка Си (musl),
шифрование (LibreSSL), curl, libmicrohttpd, cJSON, клиент PostgreSQL (libpq)
и список центров сертификации — по нему программа проверяет, что говорит
с настоящим погодным сайтом. Этот файл можно скопировать на любой Linux
с таким же процессором (здесь x86-64) и запустить — ставить там ничего не нужно.

## Тесты

```sh
docker compose run --rm --build test
```

Все четыре теста идут внутри Docker. Тест `db` работает с базой из `compose.yaml`:
заводит себе отдельную базу и потом удаляет её, записи журнала не трогает.
В интернет тесты не ходят: вместо Open-Meteo отвечает подставной сайт внутри теста.

| Тест | Что проверяет |
|---|---|
| `config` — `tests/test_config.c` | настройки: значения по умолчанию, файл, переменные окружения, опечатки |
| `service` — `tests/test_service.c` | ответы сервера на настоящие запросы, журнал в файле, параллельную работу, быструю остановку |
| `main` — `tests/test_main.c` | программу целиком: запуск, остановку сигналом, ошибки при старте |
| `db` — `tests/test_db.c` | журнал в PostgreSQL: строки доходят до базы как есть, журнал переживает перезапуск базы |

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

Сначала берутся значения по умолчанию, потом файл `config.env`, потом переменные
окружения с теми же именами. В Docker настройки сервиса задаются в `compose.yaml`,
в разделе `environment` сервиса `app`.

| Настройка | По умолчанию | Что это |
|---|---|---|
| `PORT` | `8080` | порт, на котором сервис принимает запросы (1–65535) |
| `LOG_PATH` | `service.log` | файл журнала; пусто — без файла (так в Docker) |
| `DB_URL` | пусто | адрес базы PostgreSQL для журнала (таблица `logs`); пусто — без базы. Пароль — в переменной `PGPASSWORD`, не в адресе |
| `WEATHER_URL` | `https://api.open-meteo.com/v1/forecast` | адрес прогноза |
| `GEO_URL` | `https://geocoding-api.open-meteo.com/v1/search` | адрес поиска города |
| `GEO_LANGUAGE` | `uk` | язык поиска и названий в ответе: `uk`, `ru`, `en`… Кириллицу сайт находит только на её языке: «Київ» — с `uk`, «Киев» — с `ru` |

Опечатка в файле настроек (неизвестная настройка, строка без `=`, плохое
значение) останавливает запуск с номером строки. Файл, указанный явно, обязан
существовать. Без аргумента и без `config.env` сервис работает на значениях
по умолчанию и говорит об этом.

## Журнал

```sh
docker compose logs app
docker compose exec db psql -U weather -d weather -c "SELECT ts, level, message FROM logs ORDER BY id"
```

Если связь с базой оборвалась, журнал переподключается сам. Пока базы нет,
строки идут только на экран (и в файл, если он задан).

## Без Docker

Тот же скрипт работает и прямо в системе, если в ней есть инструменты сборки:
bash, gcc, make, cmake, pkg-config, curl, tar, gzip.

```sh
scripts/build-static.sh       # результат — build-static/weather_service
```

Или сборка с библиотеками системы — их ставят один раз. Fedora:

```sh
sudo dnf install gcc cmake pkgconf-pkg-config libmicrohttpd-devel libcurl-devel libpq-devel cjson-devel
```

Ubuntu / Debian:

```sh
sudo apt install build-essential cmake pkgconf libmicrohttpd-dev libcurl4-openssl-dev libpq-dev libcjson-dev
```

Сборка, тесты и запуск — из папки проекта (там лежит `config.env`):

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/weather_service       # остановка — Ctrl+C; другой файл настроек: ./build/weather_service my.env
```

Без базы тест `db` пропускается и ctest пишет `Skipped`. С базой из Docker:

```sh
docker compose up -d db
export PGPASSWORD=$(sed -n 's/^POSTGRES_PASSWORD=//p' .env)
TEST_DB_URL=postgresql://weather@127.0.0.1:5432/weather ctest --test-dir build --output-on-failure
DB_URL=postgresql://weather@127.0.0.1:5432/weather ./build/weather_service    # журнал в базу
```

Так запущенный сервис принимает запросы со всех сетевых адресов компьютера,
пароля нет. Брандмауэр Fedora Workstation по умолчанию пропускает порты выше
1024, поэтому сервис видят другие устройства в той же сети.

### Проверка памяти

Сборка с AddressSanitizer и UBSan: они ловят выход за границы памяти, утечки
и неопределённое поведение. Нужен clang (Fedora: `sudo dnf install clang compiler-rt`).

```sh
CC=clang cmake -B build-check -DSANITIZE=ON
cmake --build build-check
ctest --test-dir build-check --output-on-failure
```

Можно и с gcc (без `CC=clang`), но тогда на Fedora нужны пакеты `libasan` и `libubsan`.
В Docker этой проверки нет: с библиотекой musl она не работает. Её делает GitHub.

## Автоматические тесты на GitHub

При каждой отправке изменений на GitHub (`git push`) там сами запускаются
проверки из файла `.github/workflows/tests.yml`:

- сборка в Docker, как выше: все тесты вместе с тестом базы, потом сервис с базой,
  настоящий запрос погоды и запись журнала в базу;
- сборка с библиотеками Ubuntu (те же пакеты, что выше) и все тесты;
- та же сборка с проверкой памяти.

Сайт погоды машинам GitHub отвечает не всегда. Если он молчит и самой машине,
проверка не проваливается, а оставляет жёлтое предупреждение. Красный крестик —
только когда машине сайт отвечает, а сервису нет.

Результаты — на вкладке [Actions](https://github.com/cultivateweb/weather-service/actions).
Там же, в разделе Artifacts, лежит готовый файл программы `weather_service`.
