# weather-service — notes for Claude

Learning project: an HTTP weather service written entirely in C (the owner wants
everything in C, tests included). README.md (Russian) is the user guide.

## Build and test

- System libraries: `cmake -B build && cmake --build build && ctest --test-dir build`.
  Memory check: `-DSANITIZE=ON` (clang works on Fedora as is; gcc needs libasan libubsan).
- One static file from sources: `scripts/build-static.sh` (musl, LibreSSL, curl,
  libmicrohttpd, cJSON, libpq, CA list) → `build-static/weather_service`.
- The `db` test needs `TEST_DB_URL` (a role allowed to CREATE DATABASE); without it
  ctest reports Skipped. `docker compose up -d db` gives one, password in `.env`.
- CI: `.github/workflows/tests.yml` — system build + ASan with a PostgreSQL service;
  docker build + compose smoke test with a real Open-Meteo call.

## Traps

- The language setting is `GEO_LANGUAGE`, not `LANGUAGE`: every setting can come from
  the environment, and `LANGUAGE` is a gettext variable often set on Ubuntu.
- `build-static.sh` takes libpq from PostgreSQL 16 on purpose: 17+ needs bison, flex
  and perl to build from the tarball.
- LibreSSL, not OpenSSL: OpenSSL's Configure needs Perl modules (IPC::Cmd, FindBin)
  that Fedora does not install by default.
- musl words `strerror()` differently from glibc ("Address in use"); tests must not
  hard-code glibc texts.
- The static binary trusts only the embedded CA list (curl is built without CA paths);
  bump `CA_DATE`/`CA_SHA256` in `build-static.sh` now and then.
- cJSON's CMakeLists needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` with CMake 4.
- postgres:18 images keep data under `/var/lib/postgresql` — mount that, not `.../data`.
- In shell checks start a service directly (`prog & PID=$!`), not `cd dir && prog &`:
  then `$!` is a subshell, and SIGINT never reaches the service.
- Open-Meteo often times out from GitHub runners (3 of 5 runs on 2026-11-13; the runner
  itself could not reach it either). The CI smoke step then asks the site from the runner
  and only warns; red means the runner got an answer and the image did not.
