# Образ сервиса, в котором нет ничего, кроме одного файла программы.
#
# Стадия build — Alpine Linux (маленькая система) с инструментами сборки.
# Скрипт scripts/build-static.sh скачивает исходники библиотек, собирает их
# и сервис в один файл и запускает тесты. Итоговый образ собран FROM scratch,
# то есть из пустоты: в нём нет ни оболочки, ни библиотек — он меньше даже Alpine.
# Программе они не нужны — это и есть проверка «пустой ОС».
#
# На компьютере нужен только Docker:
#   docker compose up -d --build                   сервис вместе с базой (compose.yaml)
#   docker build --target file --output out .      только файл программы — в папку out

FROM alpine:3.24 AS build
# bash — на нём написан скрипт сборки; linux-headers — описание ядра Linux,
# нужное curl и PostgreSQL; остальное — компилятор и инструменты сборки.
RUN apk add --no-cache bash gcc make cmake pkgconf curl linux-headers
WORKDIR /src
# Сначала только скрипт: пока он не менялся, Docker берёт собранные библиотеки
# из своего запаса (кэша) и не собирает их заново.
COPY scripts/build-static.sh scripts/
RUN scripts/build-static.sh deps
COPY . .
RUN scripts/build-static.sh app

# Только файл программы: для docker build --target file --output out .
FROM scratch AS file
COPY --from=build /src/build-static/weather_service /

FROM scratch
COPY --from=build /src/build-static/weather_service /weather_service
# Журнал — на экран (его показывает docker logs) и в базу, без файла.
ENV LOG_PATH=
# Пользователь без прав: даже взломав сервис, в контейнере ничего не поменять.
USER 65534:65534
EXPOSE 8080
ENTRYPOINT ["/weather_service"]
