# Configuration

Spark reads `config.toml` when the server starts. On Endstone it is in the
plugin data directory; on LeviLamina it is in the mod configuration directory.
When the file is missing, Spark creates it with the defaults below. The file is
user-owned: Spark does not rewrite it during normal operation. Missing fields use
defaults and unknown fields are ignored.

If TOML is malformed, a field has the wrong type, or a value is invalid, Spark
reports the error and uses defaults for that startup without changing the file.
Environment overrides apply only in memory.

## Options

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `viewerUrl` | string | `"https://spark.lucko.me/"` | HTTP(S) base URL for the spark viewer. |
| `bytebinUrl` | string | `"https://spark-usercontent.lucko.me/"` | HTTP(S) base URL for profile and health uploads. |
| `bytesocksHost` | string | `"spark-usersockets.lucko.me"` | Live-viewer WebSocket host with an optional port. |
| `backgroundProfiler` | boolean | `true` | Enable the automatic background execution profiler. |
| `backgroundProfilerInterval` | integer | `10` | Background sampling interval in milliseconds, from `1` through `1000`. |
| `backgroundProfilerThreadGrouper` | string | `"by-pool"` | Group background threads by `by-pool`, `by-name`, or `as-one`. |
| `backgroundProfilerThreadDumper` | string | `"default"` | Profile the server thread (`default`) or all process threads (`all`). |
| `allocationRateMetrics` | boolean | `true` | Keep count-only native allocation-rate metrics active outside allocation profiles. |
| `serverPropertiesAdditionalKeys` | string | `""` | Comma-separated reviewed `server.properties` keys for profile metadata. |
| `disableResponseBroadcast` | boolean | `false` | Restrict result notifications to the requesting player. |

Valid foreground profiles pause the background profiler. Stopping and exporting a
foreground profile restarts it; a cancelled or timed-out profile leaves it
paused until Spark reloads.

`serverPropertiesAdditionalKeys` accepts at most 64 unique keys of up to 128
characters using letters, digits, `-`, `_`, and `.`. Sensitive names and names
containing `password`, `passcode`, `token`, `secret`, `credential`, or
`private-key` remain blocked.

## Environment overrides

These Java-compatible environment variables override matching TOML values for
the current process:

| Environment variable | TOML key |
| --- | --- |
| `SPARK_VIEWERURL` | `viewerUrl` |
| `SPARK_BYTEBINURL` | `bytebinUrl` |
| `SPARK_BYTESOCKSHOST` | `bytesocksHost` |
| `SPARK_BACKGROUNDPROFILER` | `backgroundProfiler` |
| `SPARK_BACKGROUNDPROFILERINTERVAL` | `backgroundProfilerInterval` |
| `SPARK_BACKGROUNDPROFILERTHREADGROUPER` | `backgroundProfilerThreadGrouper` |
| `SPARK_BACKGROUNDPROFILERTHREADDUMPER` | `backgroundProfilerThreadDumper` |
| `SPARK_ALLOCATIONRATEMETRICS` | `allocationRateMetrics` |
| `SPARK_DISABLERESPONSEBROADCAST` | `disableResponseBroadcast` |

Only case-insensitive `true` enables a boolean variable. Invalid intervals,
endpoints, or thread modes cause Spark to use defaults for that startup.

`SPARK_PYTHON_ATTRIBUTION_MODE` accepts `auto` (default), `off`, or
`shadow-only`. It applies only to Endstone's Python integration.

## Trusted live-viewer clients

Approved public keys are stored outside `config.toml` in `trusted-viewers.json`.
The `/spark profiler trust-viewer --id <client id>` and
`/spark health trust-viewer --id <client id>` commands add pending clients. The
same trust list is used for the profiler live viewer and health dashboard.
