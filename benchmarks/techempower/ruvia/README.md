# Ruvia TechEmpower benchmark

This directory contains the Ruvia implementation for the TechEmpower Framework Benchmarks test suite.

Implemented endpoints:

- `/json`
- `/plaintext`
- `/db`
- `/queries?queries=N`
- `/fortunes`
- `/updates?queries=N`
- `/cached-worlds?count=N`
- `/cached-queries?count=N`

Local build from the Ruvia repository:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRUVIA_BUILD_TECHEMPOWER=ON
cmake --build build --config Release --target ruvia_techempower
```

Runtime configuration is read from environment variables. Ruvia-specific names take precedence, then common TechEmpower-style names, then defaults:

| Setting | Variables | Default |
| --- | --- | --- |
| host | `RUVIA_DB_HOST`, `DBHOST`, `DATABASE_HOST` | `tfb-database` |
| port | `RUVIA_DB_PORT`, `DBPORT`, `DATABASE_PORT` | `3306` |
| user | `RUVIA_DB_USER`, `DBUSER`, `DATABASE_USER` | `benchmarkdbuser` |
| password | `RUVIA_DB_PASSWORD`, `DBPASSWORD`, `DATABASE_PASSWORD` | `benchmarkdbpass` |
| database | `RUVIA_DB_DATABASE`, `DBNAME`, `DATABASE_NAME` | `hello_world` |
| pool size per worker | `RUVIA_DB_POOL_SIZE` | `4` |
| HTTP threads | `RUVIA_THREADS`, `MAX_THREADS` | hardware concurrency or `2` |

Set `RUVIA_TFB_NO_DB=1` only for local smoke testing of `/json` and `/plaintext` without a database. Do not set it for TechEmpower verification or benchmark runs.

The database test uses the standard TechEmpower tables `World`, `Fortune`, and `CachedWorld`.
