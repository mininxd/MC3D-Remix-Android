#pragma once

#include <cstdint>
#include <string>

namespace game::cheats {

enum class CheatId : int {
    Cash5000 = 0,     // AMSSRBLHGSHARK (clickable +$5,000)
    FullThrottle = 1, // FULLTHROTTLE (infinite NOS)
    SecretDealer = 2, // SECRETDEALER (unlock REMIX/SpecialEdition cars)
    NoTraffic = 3,    // GHOSTTOWN / GENOSIDA (no traffic)
    ZeroCost = 4,     // ZEROCOST (free shop/upgrades)
    Arcade = 5,       // ARCADE (powerup pickups in all races)
    Delivery = 6,     // DELIVERY (unlock all cities)
    Count = 7,
};

void OnFrameTick();
void ReloadCheatDb();

bool HasAnyUnlockedCheat();
bool IsCheatUnlocked(CheatId id);
bool IsCheatActive(CheatId id);
bool ToggleCheat(CheatId id);
bool TriggerCashCheat();
int32_t GetCurrentCash();
std::string GetActiveSaveName();

const char* GetCheatCodeName(CheatId id);
const char* GetCheatMenuLabel(CheatId id);

}  // namespace game::cheats
