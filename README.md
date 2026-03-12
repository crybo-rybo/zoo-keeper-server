# zoo-keeper-server

Pre-MVP bootstrap for a companion server project built around the upstream
[`zoo-keeper`](https://github.com/crybo-rybo/zoo-keeper) library.

## Baseline

The current baseline only verifies that this repo can:

- vendor `zoo-keeper` as `extern/zoo-keeper`
- initialize nested submodules recursively, including `llama.cpp`
- configure a top-level CMake build around `ZooKeeper::zoo`
- build and run a zero-model smoke executable that proves the library links

## Bootstrap

If you cloned without submodules, initialize them first:

```bash
git submodule update --init --recursive
```

Configure the project:

```bash
cmake -S . -B build
```

Acceleration defaults are set explicitly by this repo:

- macOS: `ZOO_ENABLE_METAL=ON`
- non-macOS: `ZOO_ENABLE_METAL=OFF`
- all platforms: `ZOO_ENABLE_CUDA=OFF`

You can still override them at configure time, for example:

```bash
cmake -S . -B build -DZOO_ENABLE_METAL=OFF
```

Build and run the smoke test:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

The smoke executable intentionally runs without a GGUF model. It succeeds only
when `ZooKeeper::zoo` is linked correctly and `zoo::Agent::create(zoo::Config{})`
fails with the expected `InvalidModelPath` validation error.
