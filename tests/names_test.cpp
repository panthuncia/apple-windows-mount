// SPDX-License-Identifier: GPL-3.0-or-later
#include "names.h"
#include <stdexcept>
#include <iostream>
void require(bool condition) { if (!condition) throw std::runtime_error("Filename regression"); }
int main() {
    try {
        for (const auto& raw : {std::string("CON"), std::string("name."), std::string("a:b"), std::string("~escape"), std::string("back\\slash")}) {
            auto names = awm::directory_names({raw});
            require(names.size() == 1 && awm::decode_name(names[0].visible) == raw && names[0].visible.front() == L'~');
        }
        auto collision = awm::directory_names({"File", "file", "normal", "r\xC3\xA9sum\xC3\xA9"});
        require(collision.size() == 4);
        for (size_t first = 0; first < collision.size(); ++first) {
            require(awm::decode_name(collision[first].visible) == collision[first].original);
            for (size_t second = first + 1; second < collision.size(); ++second)
                require(!awm::equal_name(collision[first].visible, collision[second].visible));
        }
        for (const auto& bad : {L"~2F", L"~00", L"~2E2E", L"~ZZ"}) {
            bool rejected = false;
            try { awm::decode_name(bad); } catch (...) { rejected = true; }
            require(rejected);
        }
        bool traversal = false;
        try { awm::path_components(L"\\Media\\..\\Apps"); } catch (...) { traversal = true; }
        require(traversal);
        auto long_name = awm::directory_names({std::string(200, 'x') + ':'});
        require(long_name[0].visible.size() <= 255 && long_name[0].original == std::string(200, 'x') + ':');
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
