# Building

The repository builds the shared profiler and application layers on their own,
the Endstone plugin on Windows or Linux, and an isolated LeviLamina module on
Windows x64. The normal build uses CMake, Ninja, Conan 2, and Clang. The first
configure may fetch the public Endstone API branch and pinned PlaceholderAPI headers,
so network access is required unless those sources are supplied locally.

For host setup, follow Endstone's [installation guide](https://endstone.dev/latest/getting-started/installation/)
and its [C++ project setup](https://next.endstone.dev/docs/api/cpp/getting-started/setup).
Those pages explain the MSVC environment on Windows and the Clang/libc++ ABI
requirement on Linux.

## Prerequisites

- CMake 3.29 or newer.
- Ninja and Conan 2 (`python -m pip install "conan>=2,<3"`).
- Clang 18 or newer. Use `clang-cl` with the MSVC frontend on Windows; use
  Clang with libc++ and libc++abi on Linux.
- Windows: Visual Studio Build Tools with the Windows SDK and Clang tools.
- Linux: libc++, libc++abi, and the static `libc++.a` and `libc++abi.a` archives
  used by the default symbol fixture.
- Python 3.12 or newer for release tooling and native Python runtime tests.

The checked-in `.conan2/profiles/default` selects the expected compiler. Do not
run `conan profile detect` over it.

## Endstone plugin and shared libraries

Install dependencies and configure a `RelWithDebInfo` build:

```shell
python -m pip install "conan>=2,<3"
conan install . --build=missing
cmake -S . -B build -G Ninja \
  "-DCMAKE_TOOLCHAIN_FILE=build/RelWithDebInfo/generators/conan_toolchain.cmake" \
  "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
cmake --build build
```

On Windows, run from an environment where `clang-cl` can find the MSVC
toolchain and Windows SDK. The plugin build fetches the Endstone `v0.11` branch
for its public API and Endstone PAPI headers v0.1.0. It produces
`endstone_spark.dll` on Windows and `endstone_spark.so` on Linux.

The equivalent generated-preset commands are:

```shell
cmake --preset conan-relwithdebinfo
cmake --build --preset conan-relwithdebinfo
```

If only the common profiler, application, native, protobuf, and network layers
are needed, disable the host plugin and its fetched host APIs:

```shell
cmake -S . -B build -G Ninja \
  "-DCMAKE_TOOLCHAIN_FILE=build/RelWithDebInfo/generators/conan_toolchain.cmake" \
  "-DCMAKE_BUILD_TYPE=RelWithDebInfo" \
  -DENDSTONE_SPARK_BUILD_PLUGIN=OFF
cmake --build build
```

Set `-DENDSTONE_SPARK_BUILD_SELFTEST=OFF` when building only the plugin. The
plugin and self-test options are independent: `ENDSTONE_SPARK_BUILD_PLUGIN=OFF`
avoids fetching Endstone and PAPI, while `ENDSTONE_SPARK_BUILD_SELFTEST=OFF`
omits the offline test tools.

## Tests

Run the offline suite after building:

```shell
ctest --test-dir build --output-on-failure
```

On Linux, leave `ENDSTONE_SPARK_GATEWAY_SAMPLER_TESTS=ON` (the default) for the
production-sampler, unload, and legacy gateway tests. Set it to `OFF` only when
those isolated tests are not needed. The focused tools are:

```text
build/spark_selftest --seconds=1
build/spark_selftest --statistics-only
build/spark_selftest --allocation-only
```

Use `.exe` paths on Windows. Python runtime tests need the matching shared
Python library through `SPARK_TEST_LIBPYTHON` or
`-DSPARK_TEST_LIBPYTHON:FILEPATH=...`. Linux may additionally run the optional
CPython 3.11 fallback with `SPARK_TEST_LIBPYTHON_311`; it does not replace the
required 3.12-or-newer runtime for Python attribution.

## LeviLamina target

`SPARK_BUILD_LEVILAMINA` is disabled by default. It is a source-built Windows
x64 target for BDS 1.26.51.x with LeviLamina 26.51.6
inputs. It builds one native module (`levilamina_spark.dll`, with matching
`levilamina_spark.pdb` for Windows symbols). The module supplies server and
native-mod metadata, aggregate player ping, uptime, TPS/MSPT, and health data,
plus validated world, region, and chunk metadata for the three vanilla dimensions,
including entity-type data and entity/loaded-chunk gauges. Tile/block-entity counts
and gamerules remain unavailable. Active behavior-pack metadata uses the shared
selected-pack discovery rules. Uptime follows the BDS process lifetime and is not
reset by module unload/load. The raw `/spark` command requires LeviLamina's
`GameDirectors` permission level.

Chunk discard callbacks and snapshot reconciliation remove stale LL observations;
each scan prunes expired dimension references. Module unload follows the same
cleanup boundary: Spark first closes callback admission, then disconnects world
subscriptions, waits for admitted callbacks to quiesce, and releases world
access.

CMake does not download or prepare the LeviLamina SDK, runtime, BDS data, or
prelink tool. The repository provides a public pinned-input bootstrap at
[`tools/levilamina/prepare-sdk.ps1`](../tools/levilamina/prepare-sdk.ps1) and
the corresponding [`runtime-lock.json`](../tools/levilamina/runtime-lock.json)
and [`sdk-lock.json`](../tools/levilamina/sdk-lock.json). It downloads only the
locked public runtime, prelink, SymbolProvider, and Bedrock runtime-data
archives, plus the pinned LeviLamina source tag; it does not download a BDS server archive. Keep its output outside
the repository. An optional `-CacheRoot <absolute-path>` may reuse existing
archives, which are rehashed before use. Supply every resulting build input
explicitly:

- a pinned LeviLamina 26.51.6 SDK root;
- the matching `LeviLamina.dll` and PDB;
- Bedrock runtime data for prelink;
- pinned prelink 0.8.6;
- the pinned `SymbolProvider.cpp` source;
- `llvm-dlltool`; and
- the repository's `tools/levilamina/spark-levilamina-imports.json` allowlist.

Run the bootstrap self-test and preparation step from PowerShell. The receipt
contains the exact paths used below:

```powershell
$llRoot = Join-Path $env:TEMP ('spark-ll-levilamina-' + [Guid]::NewGuid().ToString('N'))
pwsh -NoProfile -File tools/levilamina/prepare-sdk.ps1 -SelfTest -OutputRoot $llRoot
pwsh -NoProfile -File tools/levilamina/prepare-sdk.ps1 -OutputRoot $llRoot
$receipt = Get-Content -Raw -LiteralPath (Join-Path $llRoot 'setup-receipt.json') | ConvertFrom-Json
$sdk = $receipt.paths.sdk_root
$runtimeDll = $receipt.paths.runtime_dll
$runtimePdb = $receipt.paths.runtime_pdb
$runtimeData = $receipt.paths.runtime_data
$prelink = $receipt.paths.prelink
$symbolProvider = $receipt.paths.symbolprovider_source
```

The SDK must contain the LeviLamina headers and dependency headers checked by
`cmake/LeviLamina.cmake`. CMake verifies the expected-lite header and
SymbolProvider source hashes, checks every required path, and fails closed when
an input is absent or does not match the pinned source.

From a Windows x64 `clang-cl` environment, configure the target with those
receipt paths:

```powershell
$allowlist = (Resolve-Path 'tools/levilamina/spark-levilamina-imports.json').Path

conan install . --build=missing -of build-ll
$configureArgs = @(
  "-DCMAKE_TOOLCHAIN_FILE=build-ll/build/RelWithDebInfo/generators/conan_toolchain.cmake",
  "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
  "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
  "-DENDSTONE_SPARK_BUILD_PLUGIN=OFF",
  "-DENDSTONE_SPARK_BUILD_SELFTEST=OFF",
  "-DSPARK_BUILD_LEVILAMINA=ON",
  "-DSPARK_LL_SDK_ROOT=$sdk",
  "-DSPARK_LL_RUNTIME_DLL=$runtimeDll",
  "-DSPARK_LL_RUNTIME_PDB=$runtimePdb",
  "-DSPARK_LL_RUNTIME_DATA=$runtimeData",
  "-DSPARK_LL_PRELINK=$prelink",
  "-DSPARK_LL_SYMBOLPROVIDER_SOURCE=$symbolProvider",
  "-DSPARK_LL_IMPORT_ALLOWLIST=$allowlist"
)
cmake -S . -B build-ll -G Ninja $configureArgs
cmake --build build-ll --config RelWithDebInfo
ctest --test-dir build-ll -C RelWithDebInfo --output-on-failure
```

The target synthesizes a named import library from the runtime DLL/PDB and
allowlist, runs prelink against the Bedrock runtime data, and writes the native
module to `build-ll/bin/spark/levilamina_spark.dll` with matching
`levilamina_spark.pdb` and `manifest.json` beside it. The manifest entry names
the DLL basename. The LL loader supports quiescent `unload`, `load`, `reload`,
and `reactivate` operations for this module. Before an unload, Spark stops its
application, drains callbacks, closes world and chunk subscriptions, and removes
the command registration; a refused unload leaves the module loaded. Stop and
save a profile first when its data must be preserved because unload does not
export profiles automatically. The generated import library, receipt, prelink
output, map, and PDB remain in the build tree.

The LeviLamina CTest entries include the import-generator self-test, callback
protocol test, world metadata and gauge tests, and cleanup-deadline test.

The [`Build`](../.github/workflows/build.yml) workflow runs the LL
preparation and build on `windows-latest` with clang-cl 20 and uploads the DLL,
PDB, and manifest listed above as the `levilamina-spark-<run-id>` LL build
artifact. The [`Release`](../.github/workflows/release.yml) workflow
publishes the same three LL files as separate versioned release assets together
with the Endstone artifacts; it does not package them as a combined archive.

## ABI and local source reuse

Endstone plugins execute inside the BDS process. Match the host's compiler ABI,
C++ standard library, and runtime: use `clang-cl` with the MSVC ABI on Windows,
and Clang with an ABI-compatible libc++ on Linux. Do not pass STL objects across
the plugin boundary from an incompatible runtime.

For local Endstone iteration, CMake accepts
`-DFETCHCONTENT_SOURCE_DIR_ENDSTONE=<path>` to reuse an existing checkout. Keep
the fetched API at the version expected by this source tree.
