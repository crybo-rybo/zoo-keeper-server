# Repository Guidelines

## Project Structure & Module Organization
Core server code lives in `src/`. The HTTP/bootstrap path is split between `src/main.cpp` and `src/server/` (`config`, `runtime`, `health`). Tests live in `tests/` and are compiled as small standalone executables registered with CTest. Example runtime configuration lives in `config/server.example.json`. Design notes and API planning live in `docs/`. Third-party code is vendored under `extern/zoo-keeper`; treat that directory as an upstream submodule and keep local changes there intentional and isolated.

## Build, Test, and Development Commands
Initialize dependencies after clone:

```bash
git submodule update --init --recursive
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run the server with a config file:

```bash
./build/zoo_keeper_server config/server.example.json
```

Optional smoke checks:

```bash
./build/zoo_keeper_link_smoke
./build/zoo_keeper_live_smoke /absolute/path/to/model.gguf
```

Linux CI expects `libjsoncpp-dev`; macOS uses `brew install jsoncpp`.

## Coding Style & Naming Conventions
Use C++23 and follow the existing style: 4-space indentation, attached braces, 100-column limit, and left-aligned pointers, matching `extern/zoo-keeper/.clang-format`. Keep files and functions in `snake_case`, types in `PascalCase`, and namespaces under `zks::server`. Prefer narrow, testable helpers over large controller-style functions. When adding includes, keep them grouped and sorted consistently with existing files.

## Testing Guidelines
Add or update CTest-backed coverage for behavior changes. Place new tests in `tests/` with names ending in `_test.cpp`, then register the executable in the top-level `CMakeLists.txt`. Current coverage focuses on config parsing, `/healthz`, startup failure paths, and smoke validation. There is no numeric coverage gate, but changes should keep the default `ctest` suite green.

## Commit & Pull Request Guidelines
Recent commits use short imperative subjects such as `Add Drogon bootstrap and health endpoint`. Follow that pattern and keep each commit scoped to one concern. PRs should include a concise summary, linked issue or task when applicable, the exact local commands run, and any config or model-path assumptions. Screenshots are unnecessary unless a docs change benefits from them.

## Security & Configuration Tips
Do not commit real model files, absolute local model paths, or machine-specific secrets. Start from `config/server.example.json`, and keep repository examples portable.
