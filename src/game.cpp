#include "werewolf/game.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <random>
#include <thread>
#include <utility>

#include "werewolf/types.h"

namespace werewolf {

// cleanup scope guard
Game::CleanupGuard::CleanupGuard(Game& game) : game(game) {}

Game::CleanupGuard::~CleanupGuard() {
  game.running_.store(false);
  if (is_initialized && game.comm_) {
    game.comm_->shutdown();
  }
  if (game.game_log_.is_open()) {
    game.game_log_.flush();
  }
  if (game.moderator_log_.is_open()) {
    game.moderator_log_.flush();
  }
}

// ctor & dtor
Game::Game(std::unique_ptr<IServerCommunication> comm, GameConfig cfg)
    : comm_(std::move(comm)), cfg_(std::move(cfg)), round_cnt(0) {}

Game::~Game() { request_stop(); }

// public APIs

void Game::request_stop() { running_.store(false); }

// responsible for acquire and release comm object.
void Game::run() {
  running_.store(true);
  round_cnt = 0;

  // handle game state and communication object lifecycle automatically
  CleanupGuard scope_guard(*this);

  game_log_.open(cfg_.game_log, std::ios::app);
  moderator_log_.open(cfg_.moderator_log, std::ios::app);

  if (!comm_) {
    std::cerr << "Game has no communication backend.\n";
    return;
  }

  if (!comm_->initialize(cfg_.max_players)) {
    std::cerr << "Failed to initialize communication when game runs.\n";
    return;
  }
  scope_guard.is_initialized = true;

  log(">>> Werewolf game starting <<<");

  lobby_phase();

  Winner winner = Winner::TBD;
  while (running_) {
    log_rounds();
    night_phase();
    winner = check_win();
    if (winner != Winner::TBD) {
      log_winner(winner);
      break;
    }
    day_phase();
    winner = check_win();
    if (winner != Winner::TBD) {
      log_winner(winner);
      break;
    }
  }

  broadcast_to_slots(">>> Werewolf game ended <<<", connected_slots());
  broadcast_to_slots("close", connected_slots());
}

// initialize_players:
// instantiate player object by @p names, and store to players_ list.
void Game::initialize_players(const std::vector<std::string>& names) {
  // TODO: role assignment, connection status, liveness.
  std::lock_guard<std::mutex> lock(players_mutex_);

  players_.clear();
  players_.reserve(names.size());

  for (int i = 0; i < static_cast<int>(names.size()); ++i) {
    Player p;
    p.slot = i;
    p.name = names[i];
    players_.push_back(std::move(p));
  }
}

// get_player_info:
// get player object by querying the slot
std::optional<Game::Player> Game::get_player_info(int slot) const {
  std::lock_guard<std::mutex> lock(players_mutex_);
  for (const Player& player : players_) {
    if (player.slot == slot) {
      return player;
    }
  }
  return std::nullopt;
}

std::optional<Game::Player> Game::get_player_info(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(players_mutex_);
  for (const Player& player : players_) {
    if (player.name == name) {
      return player;
    }
  }
  return std::nullopt;
}

// alive_slots:
// alive slots is defined as slots from players who is_alive and is_connected.
// return list of alive slots.
std::vector<int> Game::alive_slots() const {
  std::lock_guard<std::mutex> lock(players_mutex_);
  std::vector<int> slots;
  for (const Player& player : players_) {
    if (player.is_connected && player.is_alive && player.slot >= 0) {
      slots.push_back(player.slot);
    }
  }
  return slots;
}

// mark_connected:
// mark the given slot as connected.
bool Game::mark_connected(int slot) {
  std::lock_guard<std::mutex> lock(players_mutex_);

  if (slot < 0 || slot >= static_cast<int>(players_.size())) {
    return false;
  }
  if (!players_[slot].is_alive) {
    return false;
  }
  players_[slot].is_connected = true;
  return true;
}

// connected_slots:
// returns a list of connected slots.
std::vector<int> Game::connected_slots() const {
  std::lock_guard<std::mutex> lock(players_mutex_);

  std::vector<int> slots;
  slots.reserve(players_.size());
  for (const Player& p : players_) {
    if (p.is_connected && p.slot >= 0) {
      slots.push_back(p.slot);
    }
  }
  return slots;
}

int Game::connected_player_count() const {
  std::lock_guard<std::mutex> lock(players_mutex_);

  int cnt = 0;
  for (const Player& p : players_) {
    if (p.is_connected) cnt++;
  }
  return cnt;
}

std::string Game::role_name(Role r) const {
  switch (r) {
    case Role::Townperson:
      return "Townperson";
    case Role::Witch:
      return "Witch";
    case Role::Wolf:
      return "Wolf";
  }
  return "Unknown";
}

VoteResult Game::conduct_night_vote() {
  std::vector<int> wolves = alive_slots_with_role(Role::Wolf);
  if (wolves.empty()) {
    log("Night: no wolves alive.");
    return VoteResult();
  }

  std::vector<int> witch = alive_slots_with_role(Role::Witch);
  std::vector<int> victims = alive_slots_with_role(Role::Townperson);
  victims.insert(victims.end(), witch.begin(), witch.end());

  if (victims.empty()) {
    log("Night: no valid victim.");
    return VoteResult();
  }

  if (cfg_.deterministic_vote) {
    sort(victims.begin(), victims.end());
    return choose_night_victim(wolves, victims);
  }

  return collect_votes(wolves, victims, cfg_.vote_duration);
}

VoteResult Game::conduct_day_vote() {
  std::vector<int> slots = alive_slots();

  if (slots.empty()) {
    log("Day: no valid lynch target.");
    return VoteResult();
  }

  if (cfg_.deterministic_vote) {
    sort(slots.begin(), slots.end());
    return choose_day_target(slots, slots);
  }

  return collect_votes(slots, slots, cfg_.vote_duration);
}

// witch_magic_power
// Given a vote result, returns if witch healed, poisoned, or skip.
// Note. witch action is independent to wolves vote.
// Only excpetion is witch can only heal a player when it is killed by wolves.
WitchAction Game::witch_magic_power(const VoteResult result) {
  // return object
  WitchAction action = WitchAction();
  bool has_victim = result.status == VoteStatus::Decided;

  // get copy of witch player
  auto witch_players = alive_slots_with_role(Role::Witch);
  if (witch_players.empty()) return action;
  auto witch = get_player_info(witch_players.front());

  // msg
  std::string msg;
  if (auto victim = get_player_info(result.target); victim) {
    msg += "The wolves chose " + victim->name + " tonight.\n";
  } else {
    msg += "The wolves did not kill anyone tonight.\n";
  }
  msg += "Witch, decide one of your actions:\n+ skip";
  if (witch->power.heal_power > 0 && has_victim) {
    msg += "\n+ heal";
  }
  if (witch->power.poison_power > 0) {
    msg += "\n+ poison: <name>";
  }

  // send msg to witch
  send_to_slot(witch->slot, msg);

  // get msg from witch
  auto timeout = std::chrono::steady_clock().now() +
                 std::chrono::seconds(cfg_.witch_decide_seconds);
  while (std::chrono::steady_clock().now() < timeout) {
    if (auto spell = recv_from_slot(witch->slot); spell && *spell != "skip") {
      if (*spell == "heal" && has_victim && witch->power.heal_power > 0) {
        witch->power.heal_power--;
        action.magic = WitchAction::Magic::Healed;
        break;
      } else if (spell->rfind(cfg_.poison_prefix, 0) == 0 &&
                 witch->power.poison_power > 0) {
        std::string poison_target = spell->substr(cfg_.poison_prefix.size());
        if (auto target = get_player_info(poison_target);
            target && target->is_alive) {
          witch->power.poison_power--;
          action.magic = WitchAction::Magic::Poisoned;
          action.poison_target = target->slot;
          break;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.delay_ms));
  }

  // mutate actual witch player
  {
    std::lock_guard<std::mutex> lock(players_mutex_);
    players_[witch_players.front()].power = witch->power;
  }

  return action;
}

// collect_votes:
// caller ensures voters and candidates are all connected slots
VoteResult Game::collect_votes(const std::vector<int>& voters,
                               const std::vector<int>& candidates,
                               int duration) {
  broadcast_to_slots("[vote] starts", voters);
  VoteResult vr;

  auto timeout =
      std::chrono::steady_clock().now() + std::chrono::seconds(duration);
  std::vector<bool> voted(cfg_.max_players,
                          false);  // max player space to ensure valid indexing
  std::vector<int> votes(cfg_.max_players,
                         0);  // max player space to ensure valid indexing

  while (std::chrono::steady_clock().now() < timeout) {
    for (auto v : voters) {
      if (voted[v]) continue;
      auto msg = recv_from_slot(v);
      if (!msg) continue;

      // vote string should be prefixed with "vote<colon><space>"
      if (msg->rfind(cfg_.vote_prefix, 0) != 0) continue;
      std::string target_name = msg->substr(cfg_.vote_prefix.size());
      auto p = get_player_info(target_name);
      if (p == std::nullopt) {
        send_to_slot(v, "[Invalid vote] Unknown player: " + target_name);
        continue;
      }
      if (std::find(candidates.begin(), candidates.end(), p->slot) ==
          candidates.end()) {
        send_to_slot(v, "[Invalid vote] Unallowed target: " + target_name);
        continue;
      }
      votes[p->slot]++;
      voted[v] = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.delay_ms));
  }

  int target = -1, max_vote = 0;
  bool tie = false;
  for (int cand : candidates) {
    if (votes[cand] > max_vote) {
      target = cand;
      max_vote = votes[cand];
      tie = false;
    } else if (max_vote > 0 && votes[cand] == max_vote)
      tie = true;
  }

  if (max_vote == 0)
    vr.status = VoteStatus::NoDecision;
  else if (tie)
    vr.status = VoteStatus::Tie;
  else {
    vr.status = VoteStatus::Decided;
    vr.target = target;
  }

  broadcast_to_slots("[vote] ends", voters);
  return vr;
}

// assign_roles:
// if deterministic_vote flag is set in the config, we apply the following
// roles to the role assignment:
// - assign # of wolves
// - assign witch
// rest are townperson
// if stochastic: all roles are assigned randomly.
void Game::assign_roles() {
  std::vector<int> slots = connected_slots();
  if (slots.empty()) return;
  if (!valid_assign_config(slots)) return;

  {
    std::lock_guard<std::mutex> lock(
        players_mutex_);  // connected slot also acquire lock
                          // clear all role assignments
    for (auto s : slots) {
      players_[s].role = Role::Townperson;
    }
    // assign wolves
    if (!cfg_.deterministic_assign) {
      std::random_device rd;
      std::mt19937 rng(rd());
      std::shuffle(slots.begin(), slots.end(), rng);
    }

    int i = 0;
    for (; i < cfg_.wolf_count; ++i) {
      players_[slots[i]].role = Role::Wolf;
    }
    // assign witch
    if (cfg_.has_witch) {
      players_[slots[i]].role = Role::Witch;
      players_[slots[i]].power.heal_power = cfg_.heal_power;
      players_[slots[i]].power.poison_power = cfg_.poison_power;
    }
  }

  log_assigned_slots(slots);
}

void Game::notify_roles() {
  std::vector<int> participants = connected_slots();
  std::vector<int> wolves = alive_slots_with_role(Role::Wolf);
  for (int p : participants) {
    auto player = get_player_info(p);
    if (!player) continue;
    std::string msg = "Your role is " + role_name(player->role);
    if (player->role == Role::Wolf) {
      msg += ". Fellow wolves are: ";
      for (int wolf : wolves) {
        if (auto fellow = get_player_info(wolf); fellow && wolf != p) {
          msg += " " + fellow->name;
        }
      }
    }
    send_to_slot(p, msg);
  }
}

std::vector<int> Game::alive_slots_with_role(Role r) const {
  std::vector<int> alives = alive_slots();
  std::vector<int> target;
  {
    std::lock_guard<std::mutex> lock(players_mutex_);
    for (int s : alives) {
      if (players_[s].is_connected && players_[s].role == r) {
        target.push_back(s);
      }
    }
  }
  return target;
}

VoteResult Game::choose_night_victim(const std::vector<int>& voters,
                                     const std::vector<int>& slots) {
  VoteResult vr;

  if (cfg_.deterministic_vote) {
    vr.status = VoteStatus::Decided;
    vr.target = slots.front();
    return vr;
  }

  // discuss

  return vr;
}

VoteResult Game::choose_day_target(const std::vector<int>& voters,
                                   const std::vector<int>& slots) {
  VoteResult vr;

  if (cfg_.deterministic_vote) {
    vr.status = VoteStatus::Decided;
    vr.target = slots.front();
    return vr;
  }

  return vr;
}

bool Game::kill_player(const int slot) {
  std::lock_guard<std::mutex> lock(players_mutex_);
  if (slot < 0 || slot >= static_cast<int>(players_.size())) {
    return false;
  }
  if (!players_[slot].is_alive) {
    return false;
  }
  players_[slot].is_alive = false;
  return true;
}

void Game::handle_night_vote_result(const WitchAction& action,
                                    const VoteResult& result) {
  std::vector<std::pair<int, std::string>> deaths;

  auto collect = [&](const VoteResult& r, const std::string& phase) {
    switch (r.status) {
      case VoteStatus::Decided:
        deaths.emplace_back(r.target, phase);
        break;
      case VoteStatus::Tie:
        broadcast_to_slots("Night: vote tied. Nobody was killed.",
                           alive_slots());
        break;
      case VoteStatus::NoDecision:
        broadcast_to_slots("Night: no decision was made.", alive_slots());
        break;
    }
  };

  switch (action.magic) {
    case WitchAction::Magic::Healed:
      log("Witch healed " + get_player_info(result.target)->name);
      return;

    case WitchAction::Magic::Poisoned:
      collect(result, "night");
      collect({VoteStatus::Decided, action.poison_target}, "poison");
      break;

    case WitchAction::Magic::Skip:
      collect(result, "night");
      break;
  }

  // dedup: same person die once per night
  uint32_t killed = 0;
  std::vector<std::pair<int, std::string>> unique_deaths;
  for (auto& [target, phase] : deaths) {
    if (killed & (1u << target)) continue;
    unique_deaths.emplace_back(target, phase);
    killed |= (1u << target);
  }

  // uniform updates
  for (auto& [target, phase] : unique_deaths) kill_player(target);

  for (auto& [target, phase] : unique_deaths) announce_death(target, phase);

  for (auto& [target, phase] : unique_deaths) dead_phase(target);
}

void Game::handle_day_vote_result(const VoteResult& result) {
  switch (result.status) {
    case VoteStatus::Decided:
      if (kill_player(result.target)) {
        announce_death(result.target, "day");
        dead_phase(result.target);
      }
      break;
    case VoteStatus::Tie:
      log("Day: vote tied. Nobody was targeted.");
      broadcast_to_slots("The vote was inconclusive, nobody was lynched.",
                         alive_slots());
      break;
    case VoteStatus::NoDecision:
      log("Day: no decision was made.");
      broadcast_to_slots("No valid votes were cast.", alive_slots());
      break;
  }
}

// connect_lobby_players:
// within wait seconds, scan all players, if recv msg : `connected`, mark it as
// connected, send greeting msg to the player.
void Game::connect_lobby_players() {
  log("Connecting players...");
  auto timeout = std::chrono::steady_clock().now() +
                 std::chrono::seconds(cfg_.lobby_wait_seconds);
  while (std::chrono::steady_clock().now() < timeout && running_) {
    for (const Player& p : players_) {
      if (p.is_connected) continue;
      if (auto msg = recv_from_slot(p.slot); msg && *msg == "connect") {
        mark_connected(p.slot);
        log(p.name + " is connected.");
        send_to_slot(
            p.slot,
            "Greeting from the game. You are connected, your slot is: " +
                std::to_string(p.slot));
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.delay_ms));
  }
}

// load_names:
// If cfg_.names_file is readable, load non-empty trimmed lines from it.
// The returned list is not padded to cfg_.max_players.
// If the file cannot be opened, fall back to load_default_names().
std::vector<std::string> Game::load_names() const {
  std::vector<std::string> names;
  names.reserve(cfg_.max_players);
  std::ifstream fd(cfg_.names_file);
  if (!fd) {
    return load_default_names();
  }

  std::string line;
  while (getline(fd, line)) {
    // trim trailing whitespaces
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    if (!line.empty()) names.push_back(line);
    if (static_cast<int>(names.size()) >= cfg_.max_players) break;
  }

  if (names.empty()) return load_default_names();

  return names;
}

// load_default_names:
// The returne names list has size cfg_.max_players
std::vector<std::string> Game::load_default_names() const {
  std::vector<std::string> names;
  names.reserve(cfg_.max_players);

  for (int i = 0; i < cfg_.max_players; ++i) {
    names.push_back("player" + std::to_string(i));
  }
  return names;
}

// private methods
bool Game::send_to_slot(int slot, const std::string& msg) {
  if (!comm_) {
    log("send_to_slot failed: no communication backend", true, true, true);
    return false;
  }

  if (!comm_->send(slot, msg)) {
    log("send_to_slot failed for slot " + std::to_string(slot), true, true,
        true);
    return false;
  }
  log("Game sent message to " + get_player_info(slot)->name + ": " + msg);
  return true;
}

void Game::broadcast_to_slots(const std::string& msg,
                              const std::vector<int>& slots) {
  if (!comm_) {
    log("broadcast_to_slots failed: no communication backend", true, true,
        true);
    return;
  }
  comm_->broadcast(msg, slots);
  log("Game broadcast: " + msg);
}

std::optional<std::string> Game::recv_from_slot(int slot) {
  if (!comm_) {
    log("recv_from_slot failed: no communication backend", true, true, true);
    return std::nullopt;
  }
  return comm_->recv(slot);
}

void Game::announce_death(int slot, const std::string& when) {
  auto info = get_player_info(slot);
  if (!info) return;

  std::string msg;
  if (when == "night") {
    msg = "Night: " + info->name + " was killed.";
  } else if (when == "day") {
    msg = "Day: " + info->name + " was lynched.";
  } else if (when == "poison") {
    msg = "Night: " + info->name + " was poisoned.";
  } else {
    msg = info->name + " died.";
  }
  log(msg);
  broadcast_to_slots(msg, alive_slots());
}

// phase: players waiting in the lobby

// lobby_phase:
// load names from txt files or fallback to default
// instantiate players_ list object from the names.
// connect the players.
// assign roles to the `connected` players.
void Game::lobby_phase() {
  auto names = load_names();
  initialize_players(names);
  log("Lobby initialized with " + std::to_string(names.size()) + " players.");
  connect_lobby_players();
  if (!valid_player_numbers()) {
    request_stop();
    return;
  }
  broadcast_to_slots("Lobby Ready", connected_slots());
  assign_roles();
  notify_roles();
}

// night_phase:
// get wolves sets
// get victim sets
// choose smallest slot as victim (deterministic_vote)
void Game::night_phase() {
  broadcast_to_slots("Night starts", alive_slots());

  chat_phase(alive_slots_with_role(Role::Wolf));
  VoteResult result = conduct_night_vote();

  // witch
  auto action = witch_magic_power(result);
  handle_night_vote_result(action, result);

  broadcast_to_slots("Night ends", alive_slots());
}

void Game::day_phase() {
  broadcast_to_slots("Day starts", alive_slots());

  chat_phase(alive_slots());
  VoteResult result = conduct_day_vote();
  handle_day_vote_result(result);

  broadcast_to_slots("Day ends", alive_slots());
}

// return immediately after recving the first dead note.
void Game::dead_phase(int slot) {
  auto info = get_player_info(slot);
  if (!info) return;

  std::string dead_announcement =
      std::to_string(slot) + " is dead. Final words...";
  broadcast_to_slots(dead_announcement, connected_slots());

  auto timeout = std::chrono::steady_clock().now() +
                 std::chrono::seconds(cfg_.death_speech_seconds);
  while (std::chrono::steady_clock().now() < timeout) {
    if (auto msg = recv_from_slot(slot); msg && !msg->empty()) {
      std::string final_words = "Final words from " + info->name + ": " + *msg;
      log(final_words);
      broadcast_to_slots(final_words, alive_slots());
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.delay_ms));
  }
}

// chat_phase:
// loop through given slot vector, check if msgs are prefixed with
// chat<colon><space> if the condition is true, we get the actual text from the
// msg and broadcast to everyone other than the slot itself.
void Game::chat_phase(const std::vector<int>& slots) {
  broadcast_to_slots("Chat starts", alive_slots());
  auto timeout = std::chrono::steady_clock().now() +
                 std::chrono::seconds(cfg_.chat_duration);
  while (std::chrono::steady_clock().now() < timeout) {
    for (int slot : slots) {
      if (auto msg = recv_from_slot(slot);
          msg && msg->rfind(cfg_.chat_prefix, 0) == 0) {
        std::string text = msg->substr(cfg_.chat_prefix.size());
        if (text.empty()) continue;

        auto p = get_player_info(slot);
        if (!p) continue;

        std::string content = p->name + ": " + text;
        log(content);

        std::vector<int> others = slots;
        others.erase(std::remove(others.begin(), others.end(), slot),
                     others.end());
        broadcast_to_slots(content, others);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.delay_ms));
  }
  broadcast_to_slots("Chat ends", alive_slots());
}

// rule
Winner Game::check_win() const {
  int wolves_cnt = static_cast<int>(alive_slots_with_role(Role::Wolf).size());
  int village_cnt = static_cast<int>(alive_slots().size()) - wolves_cnt;

  if (wolves_cnt == 0) return Winner::Village;
  if (wolves_cnt >= village_cnt) return Winner::Wolf;
  return Winner::TBD;
}

// methods
bool Game::valid_assign_config(std::vector<int>& slots) {
  if (slots.empty()) return false;
  int n = static_cast<int>(slots.size());
  if (n < cfg_.wolf_count) {
    log("Not enough players to play wolves.");
    return false;
  }
  if (n < cfg_.wolf_count + int(cfg_.has_witch)) {
    log("Not enough players to play witch.");
    return false;
  }
  return true;
}

bool Game::valid_player_numbers() {
  if (connected_player_count() < 4) {
    broadcast_to_slots("Not enough players (need at least 4). Game cancelled",
                       connected_slots());
    broadcast_to_slots("close", connected_slots());
    return false;
  }
  return true;
}

void Game::log_assigned_slots(std::vector<int>& slots) {
  // log
  for (int s : slots) {
    Player p = players_[s];
    log("player " + std::to_string(p.slot) + " is " + role_name(p.role));
  }
}

void Game::log_winner(Winner winner) {
  std::string msg;
  switch (winner) {
    case Winner::Village:
      msg = "--- Village Win ---";
      break;
    case Winner::Wolf:
      msg = "--- Wolves Win ---";
      break;
    case Winner::TBD:
      log("--- Game continues ---");
      break;
  }
  broadcast_to_slots(msg, connected_slots());
  log(msg);
}

void Game::log_rounds() {
  log("=== Round " + std::to_string(round_cnt) + " ===");
  round_cnt++;
}

void Game::log(const std::string& msg, bool to_stdout, bool to_game_log,
               bool to_moderator_log) {
  const auto now = std::time(nullptr);
  const std::string stamped = "(" + std::to_string(now) + "): " + msg;

  std::lock_guard<std::mutex> lock(log_mutex_);

  if (to_stdout) {
    std::cout << stamped << std::endl;
  }
  if (to_game_log && game_log_.is_open()) {
    game_log_ << stamped << '\n';
    game_log_.flush();
  }
  if (to_moderator_log && moderator_log_.is_open()) {
    moderator_log_ << stamped << '\n';
    moderator_log_.flush();
  }
}

}  // namespace werewolf
