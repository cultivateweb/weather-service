#!/usr/bin/env bash
#
# Собирает weather_service в один файл, в который зашито всё нужное для работы:
# библиотека языка Си (musl), шифрование (LibreSSL), curl, libmicrohttpd, cJSON,
# клиент PostgreSQL (libpq) и список центров сертификации, которым программа верит.
# Такой файл можно скопировать на любой Linux с тем же типом процессора и запустить,
# даже если там нет ни одной библиотеки.
#
# Ставить библиотеки в систему не нужно: скрипт скачивает их исходники, сверяет
# контрольные суммы и собирает всё здесь же. Нужны только инструменты:
# gcc, make, cmake, pkg-config, curl, tar, gzip.
#
#   scripts/build-static.sh        всё: сначала библиотеки, потом сервис
#   scripts/build-static.sh deps   только библиотеки (долго, но нужно один раз)
#   scripts/build-static.sh app    только сервис и его тесты
#
# Результат: build-static/weather_service. Подробный вывод шагов: build-static/logs/.
# Собрать в другую папку: OUT=/путь/к/папке scripts/build-static.sh
#
# Почему musl, а не обычная библиотека Си системы (glibc): glibc нельзя целиком
# зашить в программу — поиск адресов в сети (DNS) всё равно ищет её части в системе.
# musl зашивается полностью.

set -euo pipefail

# Версии и контрольные суммы архивов (SHA-256). Суммы проверены по подписям
# авторов, где авторы их публикуют. После скачивания скрипт сверяет сумму:
# подменённый или битый архив собран не будет.
MUSL_VERSION=1.2.6
MUSL_SHA256=d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
LIBRESSL_VERSION=4.4.1
LIBRESSL_SHA256=290cdf1b53c086255dc302f118b98602a444e9ae6d11e9bfdd8c9b1b60aa3a22
CURL_VERSION=8.23.0
CURL_SHA256=e3de77cfa6367083c776a26647fc9f633b538c06c3baa73d931a252ac8f72b49
MHD_VERSION=1.0.10
MHD_SHA256=04bfe8ef75db7d629a33de767599765cecadc56274a39822d5d081030d577685
CJSON_VERSION=1.7.19
CJSON_SHA256=7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562
# libpq берём из PostgreSQL 16: начиная с 17-й версии архив без bison, flex
# и perl не собирается. Клиент 16 работает и с сервером 18 — язык общения тот же.
PG_VERSION=16.16
PG_SHA256=3e930f3366f675ab26e873d16201201f8e9b6d9bc13686e4465380271145ea2d
# Список центров сертификации: по нему программа проверяет, что говорит
# с настоящим погодным сайтом. Его собирают авторы curl из списка Mozilla.
CA_DATE=2026-11-05
CA_SHA256=427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285

ROOT=$(cd "$(dirname "$0")/.." && pwd)      # папка проекта
OUT=${OUT:-$ROOT/build-static}
DL=$OUT/downloads                           # скачанные архивы
SRC=$OUT/src                                # распакованные исходники
PREFIX=$OUT/prefix                          # собранные библиотеки и их заголовки
LOGS=$OUT/logs                              # подробный вывод каждого шага
JOBS=${JOBS:-$(nproc)}                      # сколько файлов компилировать одновременно
MUSL_GCC=$PREFIX/bin/musl-gcc               # gcc, который собирает под musl

# pkg-config подсказывает сборке, где лежат библиотеки. Пусть смотрит только
# в нашу папку: так в программу не попадёт ничего из системы.
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig
export PKG_CONFIG_PATH=

# ---------- Помощники ----------

# run <шаг> <команда...>: выполняет команду, а её вывод пишет в $LOGS/<шаг>.log.
# Если команда упала — показывает конец журнала и останавливает сборку.
run() {
    local name=$1 log=$LOGS/$1.log status
    shift
    mkdir -p "$LOGS"
    set +e
    ( set -e; "$@" ) >"$log" 2>&1
    status=$?
    set -e
    if [ "$status" -ne 0 ]; then
        echo "Ошибка на шаге «$name». Конец журнала $log:" >&2
        tail -n 30 "$log" >&2
        exit 1
    fi
}

# step <библиотека> <версия> <функция>: собирает библиотеку, если она ещё не
# собрана. Пометка в $PREFIX/.built помнит версию и сам рецепт сборки:
# поменялось что-то из них — библиотека соберётся заново.
step() {
    local name=$1 id stamp=$PREFIX/.built/$1
    id="$2 $(declare -f "$3" | sha256sum | cut -c1-16)"
    if [ "$(cat "$stamp" 2>/dev/null)" = "$id" ]; then
        echo "== $name $2: уже собрана"
        return
    fi
    echo "== $name $2: собираю"
    run "$name" "$3"
    mkdir -p "$PREFIX/.built"
    echo "$id" >"$stamp"
}

# fetch <адрес> <sha256> [имя файла]: скачивает архив (если его ещё нет) и сверяет сумму.
fetch() {
    local url=$1 sum=$2 file=$DL/${3:-${1##*/}}
    mkdir -p "$DL"
    if [ ! -f "$file" ]; then
        curl -fsSL --retry 3 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    if ! echo "$sum  $file" | sha256sum -c --quiet -; then
        echo "Контрольная сумма не совпала: $file. Удаляю его." >&2
        rm -f "$file"
        return 1
    fi
}

# unpack <архив из $DL> <папка>: распаковывает начисто в $SRC/<папка>.
unpack() {
    rm -rf "${SRC:?}/$2"
    mkdir -p "$SRC/$2"
    tar -xzf "$DL/$1" -C "$SRC/$2" --strip-components=1
}

# Заголовки ядра Linux (linux/*.h, asm/*.h) — это описание ядра, а не библиотека.
# В musl их нет, а curl и PostgreSQL ими пользуются. Берём системные.
link_kernel_headers() {
    local sub dir
    for sub in linux asm asm-generic; do
        for dir in /usr/include "/usr/include/$(gcc -dumpmachine)"; do
            if [ -d "$dir/$sub" ]; then
                ln -sfn "$dir/$sub" "$PREFIX/include/$sub"
                break
            fi
        done
    done
}

# ---------- Библиотеки ----------

build_musl() {
    fetch "https://musl.libc.org/releases/musl-$MUSL_VERSION.tar.gz" "$MUSL_SHA256"
    unpack "musl-$MUSL_VERSION.tar.gz" musl
    cd "$SRC/musl"
    # syslibdir — куда положить загрузчик программ: в нашу папку, а не в /lib.
    ./configure --prefix="$PREFIX" --syslibdir="$PREFIX/lib"
    make -j"$JOBS"
    make install
    link_kernel_headers
}

build_libressl() {
    fetch "https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/libressl-$LIBRESSL_VERSION.tar.gz" "$LIBRESSL_SHA256"
    unpack "libressl-$LIBRESSL_VERSION.tar.gz" libressl
    cd "$SRC/libressl"
    ./configure CC="$MUSL_GCC" --prefix="$PREFIX" --disable-shared --disable-tests
    make -j"$JOBS"
    make install
}

build_curl() {
    fetch "https://curl.se/download/curl-$CURL_VERSION.tar.gz" "$CURL_SHA256"
    unpack "curl-$CURL_VERSION.tar.gz" curl
    cd "$SRC/curl"
    # Сервису нужен только HTTP и HTTPS, остальное выключено: меньше кода — меньше дыр.
    # Где искать сертификаты, curl не знает: их список зашивается в саму программу.
    ./configure CC="$MUSL_GCC" --prefix="$PREFIX" --disable-shared \
        --with-openssl="$PREFIX" \
        --without-ca-bundle --without-ca-path --without-ca-fallback \
        --without-libpsl --without-zlib --without-brotli --without-zstd \
        --without-libidn2 --without-nghttp2 \
        --disable-ftp --disable-file --disable-ipfs --disable-ldap --disable-ldaps \
        --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 \
        --disable-imap --disable-smtp --disable-gopher --disable-mqtt \
        --disable-websockets --disable-ntlm --disable-manual --disable-docs
    make -j"$JOBS"
    make install
}

build_mhd() {
    fetch "https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-$MHD_VERSION.tar.gz" "$MHD_SHA256"
    unpack "libmicrohttpd-$MHD_VERSION.tar.gz" libmicrohttpd
    cd "$SRC/libmicrohttpd"
    # Шифрованный сервер (HTTPS) не нужен: потребовал бы ещё одну библиотеку, GnuTLS.
    ./configure CC="$MUSL_GCC" --prefix="$PREFIX" --disable-shared \
        --enable-https=no --disable-doc --disable-examples --disable-tools --disable-curl
    make -j"$JOBS"
    make install
}

build_cjson() {
    fetch "https://github.com/DaveGamble/cJSON/archive/refs/tags/v$CJSON_VERSION.tar.gz" \
          "$CJSON_SHA256" "cJSON-$CJSON_VERSION.tar.gz"
    unpack "cJSON-$CJSON_VERSION.tar.gz" cjson
    # CMAKE_POLICY_VERSION_MINIMUM: файлы сборки cJSON написаны для старого CMake,
    # новый без этой подсказки их не примет.
    cmake -S "$SRC/cjson" -B "$SRC/cjson/build" \
        -DCMAKE_C_COMPILER="$MUSL_GCC" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF -DENABLE_CJSON_UTILS=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build "$SRC/cjson/build" -j"$JOBS"
    cmake --install "$SRC/cjson/build"
}

build_libpq() {
    fetch "https://ftp.postgresql.org/pub/source/v$PG_VERSION/postgresql-$PG_VERSION.tar.gz" "$PG_SHA256"
    unpack "postgresql-$PG_VERSION.tar.gz" postgresql
    cd "$SRC/postgresql"
    ./configure CC="$MUSL_GCC" --prefix="$PREFIX" --with-ssl=openssl \
        --without-readline --without-zlib --without-icu \
        --with-includes="$PREFIX/include" --with-libraries="$PREFIX/lib"
    # Из всего PostgreSQL нужен только клиент libpq, и только в виде .a —
    # библиотеки, которую можно зашить в программу. Вместе с ней собираются
    # две вспомогательные: pgcommon и pgport.
    make -C src/interfaces/libpq -j"$JOBS" all-static-lib libpq.pc
    mkdir -p "$PREFIX/lib/pkgconfig"
    cp src/interfaces/libpq/libpq.a src/common/libpgcommon.a src/port/libpgport.a "$PREFIX/lib/"
    cp src/interfaces/libpq/libpq.pc "$PREFIX/lib/pkgconfig/"
    cp src/interfaces/libpq/libpq-fe.h src/interfaces/libpq/libpq-events.h \
       src/include/postgres_ext.h src/include/pg_config_ext.h "$PREFIX/include/"
}

build_ca() {
    fetch "https://curl.se/ca/cacert-$CA_DATE.pem" "$CA_SHA256"
    mkdir -p "$PREFIX/share"
    cp "$DL/cacert-$CA_DATE.pem" "$PREFIX/share/cacert.pem"
}

deps() {
    step musl          "$MUSL_VERSION"     build_musl
    step libressl      "$LIBRESSL_VERSION" build_libressl
    step curl          "$CURL_VERSION"     build_curl
    step libmicrohttpd "$MHD_VERSION"      build_mhd
    step cjson         "$CJSON_VERSION"    build_cjson
    step libpq         "$PG_VERSION"       build_libpq
    step ca-bundle     "$CA_DATE"          build_ca
}

# ---------- Сервис ----------

configure_app() {
    cmake -S "$ROOT" -B "$OUT/app" -DCMAKE_C_COMPILER="$MUSL_GCC" -DCMAKE_BUILD_TYPE=Release \
        -DSTATIC=ON -DCA_BUNDLE="$PREFIX/share/cacert.pem"
    cmake --build "$OUT/app" -j"$JOBS"
}

app() {
    if [ ! -x "$MUSL_GCC" ]; then
        echo "Библиотеки ещё не собраны: сначала scripts/build-static.sh deps" >&2
        exit 1
    fi
    echo "== сервис: собираю"
    run app configure_app
    echo "== сервис: тесты"
    ctest --test-dir "$OUT/app" --output-on-failure
    cp "$OUT/app/weather_service" "$OUT/weather_service"
    strip "$OUT/weather_service"
    echo
    echo "Готово: $OUT/weather_service ($(du -h "$OUT/weather_service" | cut -f1))"
}

for tool in gcc make cmake pkg-config curl tar gzip sha256sum; do
    command -v "$tool" >/dev/null || { echo "Нет инструмента: $tool" >&2; exit 1; }
done

case ${1:-all} in
    deps) deps ;;
    app)  app ;;
    all)  deps; app ;;
    *)    echo "Как запускать: $0 [deps|app]" >&2; exit 2 ;;
esac
