# Integration tests

End-to-end checks that start the real server. Run them with
`ctest --preset debug -L integration --output-on-failure`.

`ctest --preset debug` runs:

- `redis_simple_unit_<SuiteName>`: unit tests compiled into
  `redis_simple_tests` and registered in CTest by GoogleTest suite.
- `redis_simple_integration_tcp`: TCP client/server integration check.
- `redis_simple_integration_server_lifecycle`: command-line option, nondefault
  bind, and graceful shutdown checks.
- `redis_simple_integration_aof`: production-server append, background rewrite,
  compaction, shutdown, restart, expiration, and cross-data-type recovery checks.
- `redis_simple_integration_aof_io_threads`: the same against a server with
  `--io-threads 4`, so rewrites fork while I/O threads run.
- `redis_simple_integration_aof_command_base`: the same with
  `--aof-use-rdb-preamble no`, so the base file holds commands.
- `redis_simple_integration_command_connection`: connection command integration
  checks.
- `redis_simple_integration_command_key`: generic key command integration
  checks.
- `redis_simple_integration_command_string`: string command integration checks.
- `redis_simple_integration_command_set`: set command integration checks.
- `redis_simple_integration_command_list`: list command integration checks.
- `redis_simple_integration_command_zset`: sorted-set command integration
  checks.
- `redis_simple_integration_command_hash`: hash command integration checks.
- `redis_simple_integration_command_transaction`: `MULTI`/`EXEC` and `WATCH`
  integration checks across two connections.
- `redis_simple_integration_command_*_io_threads`: each command family again,
  against a server started with `--io-threads 4`.
- `redis_simple_integration_concurrent_clients_plain` and
  `..._io_threads`: 32 concurrent pipelined clients, with replies, totals,
  and `INFO stats` checked. The threaded variant requires batches shared
  across the I/O threads.

For debugging a single executable directly:

```sh
./build/debug/redis_simple_tests
./build/debug/redis_simple_integration_set_client
```

For debugging one unit-test suite directly:

```sh
./build/debug/redis_simple_tests --gtest_filter=ListpackTest.*
```

The integration clients return nonzero on failed expectations, so they are safe
to run in CI instead of relying on manual log inspection.
