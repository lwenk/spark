# Spark for Bedrock

Spark is a native profiler for Minecraft Bedrock Dedicated Server. It samples
native execution and allocation call stacks, then displays the results in the
[spark viewer](https://spark.lucko.me/). It supports Endstone and a Windows x64
LeviLamina 26.51 module.

Spark uses the profile format, protocol, and viewer from
[lucko/spark](https://github.com/lucko/spark). Credit for those belongs to the
upstream spark project.

## Install

For Endstone, download `endstone_spark.dll` on Windows or `endstone_spark.so`
on Linux from the [latest release](https://github.com/EndstoneMC/spark/releases/latest),
place it in the server `plugins/` directory, then fully restart the server.

For LeviLamina, use the `levilamina_spark.dll` module together with its
`manifest.json` in the LeviLamina mods directory. It is built for BDS 1.26.51.x
and LeviLamina 26.51.x.

## Quick start

Run these commands in the server console or in game as an operator:

```text
/spark profiler start
/spark profiler info
/spark profiler stop
```

The profiler runs until stopped. `stop` uploads the profile and prints a viewer
link. Use `/spark profiler start --alloc` for allocation profiling.

## Features

- Sample native server execution, including work outside plugin code.
- Profile native allocations with `/spark profiler start --alloc`.
- View rolling server statistics with `/spark tps` and resource reports with
  `/spark health show`.
- Open a live spark viewer while profiling with `/spark profiler open`.

## Documentation

For server owners and operators:

- [Using Spark](docs/using-spark.md)
- [Configuration](docs/configuration.md)

For developers and contributors:

- [Development](docs/development.md)
- [Architecture](docs/architecture.md)
- [Behavior pack metadata](docs/behavior-pack-metadata.md)
- [Python function attribution](docs/python-function-attribution.md)

## License

Spark for Bedrock is licensed under the GNU General Public License v3.0. See
[LICENSE](LICENSE).
