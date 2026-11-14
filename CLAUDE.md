# weather-service — notes for Claude

Learning project: an HTTP weather service written entirely in C (the owner wants
everything in C, tests included). README.md (Russian) is the user guide.

## Build and test

- Docker only — the owner wants nothing but Docker on the host (build stage: Alpine 3.24):
  `docker compose up -d --build` (service + database, password in `.env`),
  `docker compose run --rm --build test` (all 4 tests, db included),
  `docker build --target file --output out .` (just the single static file).
- Without Docker: `scripts/build-static.sh` (the same one-file build: musl, LibreSSL,
  curl, libmicrohttpd, cJSON, libpq, CA list → `build-static/weather_service`), or system
  libraries: `cmake -B build && cmake --build build && ctest --test-dir build`.
  Memory check: `-DSANITIZE=ON` (clang works on Fedora as is; gcc needs libasan libubsan;
  ASan does not work with musl, so it is not in Docker).
- The `db` test needs `TEST_DB_URL` (a role allowed to CREATE DATABASE); without it
  ctest reports Skipped. The compose `test` service sets it.
- CI: `.github/workflows/tests.yml` — Docker build, all tests via compose, smoke test
  with a real Open-Meteo call; system build + ASan with a PostgreSQL service.

## Commits

- Author: `cultivateweb <30661271+cultivateweb@users.noreply.github.com>` — set in this
  repo's git config; the personal email must never get into a commit (the repo is public).
- No `Co-Authored-By` trailer: the owner asked to keep commit messages without it.
- One commit per change, conventional subject in lowercase English.

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
- The Docker build stage is Alpine: busybox `sha256sum` has no `--quiet`; bash and
  linux-headers come from apk. Its gcc already targets musl, so musl skips the musl-gcc
  wrapper unless `--enable-wrapper=gcc`; it links `-lssp_nonshared` into every program
  (build-static.sh makes its own) and turns `-static` into static-pie, which crashes
  with musl-gcc's Scrt1.o — hence `-no-pie` in CMakeLists.txt.
- postgres:18 images keep data under `/var/lib/postgresql` — mount that, not `.../data`.
- In shell checks start a service directly (`prog & PID=$!`), not `cd dir && prog &`:
  then `$!` is a subshell, and SIGINT never reaches the service.
- Open-Meteo often times out from GitHub runners (3 of 5 runs on 2026-11-13; the runner
  itself could not reach it either). The CI smoke step then asks the site from the runner
  and only warns; red means the runner got an answer and the image did not.
