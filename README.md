> **Work in progress**

This project is not ready for installation or use. It provides no deployment or compatibility guarantee.

# Playerbots Social

Playerbots Social is an AzerothCore module for grounded Playerbot conversations. It owns conversation
coordination, relationships, biographies, privacy scoped memory, routing, runtime controls, persistence,
configuration, and telemetry.

The module consumes stable personality profiles from `mod-playerbot-personality`. It accepts generated
proposals through the provider interface implemented by `mod-playerbot-llm`. The worldserver remains
authoritative for admission, grounding, privacy, delivery, and silence.

## Repository layout

`src/Bot/Social` contains the domain and runtime implementation.

`conf/mod_playerbots_social.conf.dist` documents every Social setting.

`data/sql/db_playerbot/updates` contains migrations for the Playerbots database.

`tests/cpp` contains behavioral tests. `tests/python` checks the public repository contract.

## Standalone checks

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 -m unittest tests/python/test_check_repository.py
python3 tools/check_repository.py
```

## AzerothCore integration

Place this repository at `modules/mod-playerbots-social`. Place the public Playerbots fork at
`modules/mod-playerbots`, Personality at `modules/mod-playerbot-personality`, and LLM at
`modules/mod-playerbot-llm`. Configure and build AzerothCore with static modules. The manual integration
workflow records the exact commands used by continuous integration.
