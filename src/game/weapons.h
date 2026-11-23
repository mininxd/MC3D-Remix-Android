#pragma once
#include <string>
#include <vector>

namespace weapons {
struct Choice { int type = 0; bool held = false; std::string label; };
struct View { bool in_level = false; std::vector<Choice> choices; std::string status; };
inline View Current() { return {}; }
inline void Give(int) {}
inline void FillAmmo() {}
inline void FillMedkits() {}
}
