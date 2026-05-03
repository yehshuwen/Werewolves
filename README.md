# Werewolf -- Terminal-Based Party Game

A terminal-based implementation of the Werewolf (Mafia) party game written in C++17, featuring a pluggable communication backend architecture. 
Each backend implements the same abstract interface so the game logic is completely decoupled from the IPC mechanism.

## Group Members & Primitives

| Member      | Concurrency Primitive | Communication Library File                     |
|-------------|-----------------------|------------------------------------------------|
| Megan Kuo  | Named Pipes (FIFO)    | `backends/pipe/pipe_communication.cpp`         |
| Megan Kuo   | **Shared Memory**     | `backends/shm/shm_communication.cpp`           |
|  Timo Lin  |  RPC  | |
| Gene Wang | Async IO     | |
| Shu-Wen Yeh | **POSIX Message Queue** | `backends/mqueue/mqueue_communication.cpp` |

> **My individual contribution:** POSIX Message Queue communication backend (`backends/mqueue/mqueue_communication.cpp` and `backends/mqueue/mqueue_communication.h`).

## Project Structure

```
.
├── include/werewolf/           # Public API headers
│   ├── server_communication.h  #   IServerCommunication interface
│   ├── client_communication.h  #   IClientCommunication interface
│   ├── game.h                  #   Game engine header
│   └── types.h                 #   Shared types (Role, GameConfig, etc.)
│
├── src/
│   └── game.cpp                # Game engine implementation
│
├── backends/                   # Communication libraries (one per group member)
│   ├── mqueue/                 #   ★ POSIX Message Queue backend (MY WORK)
│   │   ├── mqueue_communication.h
│   │   └── mqueue_communication.cpp
│   ├── shm/                    #   Shared memory backend (Megan)
│   │   ├── shm_communication.h
│   │   └── shm_communication.cpp
│   ├── pipe/                   #   Named-pipe (FIFO) backend
│   │   ├── pipe_communication.h
│   │   └── pipe_communication.cpp
│   └── template/               #   Skeleton for additional backends
│
├── frontends/                  # Executables that wire a backend to the game
│   ├── shm/                    #   Server + client using shared memory
│   └── pipes/                  #   Server + client using named pipes
│       ├── mqueue_server.cpp   #   ★ POSIX MQ server entry point (MY WORK)
│       ├── mqueue_client.cpp   #   ★ POSIX MQ client entry point (MY WORK)
│       ├── pipe_server.cpp
│       └── pipe_client.cpp
│
├── tests/                      # Unit tests & end-to-end scripts
│   ├── mqueue/                 #   ★ POSIX MQ backend tests (MY WORK)
│   │   ├── test_mqueue.cpp
│   │   └── e2e/
│   │       └── test_mqueue_e2e.sh
│   ├── shm/                    #   Shared memory backend tests
│   ├── pipe/                   #   Pipe backend tests
│   └── game/                   #   Game logic tests
│
├── docs/                       # Design documentation
│   ├── communication_contract.md
│   ├── game.md
│   └── shm_backend.md
│
├── CMakeLists.txt
└── README.md                   # ← You are here
```

## Files Written by Me (independently)

```
backends/mqueue/mqueue_communication.h        ← my library header
backends/mqueue/mqueue_communication.cpp      ← my library implementation
backends/mqueue/CMakeLists.txt                ← my build config
frontends/pipes/mqueue_server.cpp             ← my server entry point
frontends/pipes/mqueue_client.cpp             ← my client entry point
tests/mqueue/test_mqueue.cpp                  ← my unit tests
tests/mqueue/e2e/test_mqueue_e2e.sh           ← my end-to-end test
```

## Communication Library API

Every backend implements two abstract interfaces defined in `include/werewolf/`:

### `IServerCommunication` (server side)

```cpp
bool initialize(int num_slots);                       // Prepare resources for N player slots
void shutdown();                                      // Release all resources
bool send(int slot, const std::string& msg);          // Send to one player (Unicast)
std::optional<std::string> recv(int slot);            // Non-blocking receive from one player
void broadcast(const std::string& msg,
               const std::vector<int>& slots);        // Send to multiple players (Broadcast / Multicast)
```

### `IClientCommunication` (client side)

```cpp
bool initialize(int slot_num);            // Attach to the game at a given slot
void shutdown();                          // Release resources
bool send(const std::string& msg);        // Send a message to the server
std::optional<std::string> recv();        // Non-blocking receive from the server
```

Key contract points (full specification in `docs/communication_contract.md`):
- Per-slot isolation: messages for slot N never leak to slot M.
- Message boundary preservation: one `send()` ↔ one `recv()`.
- In-order delivery within each (slot, direction) pair.
- `recv()` is always **non-blocking** — returns `std::nullopt` when nothing is available.

## My Backend: POSIX Message Queue

Each player slot owns two POSIX MQs:
- `/ww_s2p_{slot}` — server → player
- `/ww_p2s_{slot}` — player → server

The kernel guarantees atomic `mq_send()` and FIFO ordering. `O_NONBLOCK` ensures `recv()` never blocks. Broadcast and Multicast are both implemented via a loop over `mq_send()` — passing `alive_slots()` produces a broadcast, passing wolf-only slots produces a multicast.

**Build note:** links with `-lrt` (handled automatically by CMakeLists.txt).  
**Platform note:** POSIX MQ is Linux-only. Use Docker for macOS development.

## Requirements

- Linux (tested on Ubuntu 22.04 / 24.04)
- CMake >= 3.14
- A C++17-capable compiler (GCC >= 9 or Clang >= 10)
- POSIX shared-memory support (`/dev/shm`)
- POSIX real-time library (`librt`, included in glibc on Linux)

## Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Run (POSIX Message Queue Backend)

Open **one terminal** for the server and **one terminal per player** for the clients.

### Server

```bash
./build/frontends/pipes/werewolf_mqueue_server \
    --players 5 \
    --wolves 1 \
    --lobby 60 \
    --chat 30 \
    --vote 30 \
    --speech 15 \
    --witch 15 \
    --det-assign
```

Server options:
- `--players N` — Maximum player slots (default 16)
- `--wolves N` — Number of wolves (default 2)
- `--lobby SEC` — Lobby wait time
- `--chat SEC` — Chat phase duration
- `--vote SEC` — Vote phase duration
- `--speech SEC` — Death speech duration
- `--witch SEC` — Witch decision duration
- `--det-assign` — Deterministic role assignment (for testing)
- `--no-randomize` — Use default player names (player0, player1, ...)

### Clients

Each client needs a unique **slot number** starting from 0:

```bash
# Terminal 1
./build/frontends/pipes/werewolf_mqueue_client 0

# Terminal 2
./build/frontends/pipes/werewolf_mqueue_client 1

# Terminal 3
./build/frontends/pipes/werewolf_mqueue_client 2

# ... up to --players - 1
```

### Verify POSIX MQs are active

```bash
ls /dev/mqueue/
# Expected: ww_s2p_0  ww_p2s_0  ww_s2p_1  ww_p2s_1 ...
```

### In-Game Commands

| Phase          | Command                   | Example                |
|----------------|---------------------------|------------------------|
| Chat           | `chat: <message>`         | `chat: I suspect Bob`  |
| Vote           | `vote: <player_name>`     | `vote: player3`        |
| Witch (heal)   | `heal`                    | `heal`                 |
| Witch (poison) | `poison: <player_name>`   | `poison: player2`      |
| Witch (skip)   | `skip`                    | `skip`                 |

## Tests

```bash
cd build

# Run all tests
ctest

# POSIX MQ unit tests (7 tests)
./tests/test_mqueue

# POSIX MQ end-to-end test (runs a full automated game)
ctest -R test_mqueue_e2e -V
```

## Game Rules

Werewolf is a social deduction game with the following phases each round:

1. **Night Phase** — Wolves privately vote to kill a villager; the Witch may heal or poison.
2. **Day Phase** — All living players discuss and vote to lynch a suspect.
3. **Win Conditions** — Village wins when all wolves are eliminated; Wolves win when they outnumber the villagers.

Roles: Townperson, Wolf, Witch (with heal and poison powers).

## Cleanup

If the server crashes without running `shutdown()`, remove stale MQs manually:

```bash
rm /dev/mqueue/ww_*
```
