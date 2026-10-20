// test_party.cpp -- the trade emulator's party store (payloads/pokemon.cpp),
// alone, under ASan and UBSan. test_gblink.py runs it.
//
// PATCHES.md P9: LINKCMD_READY_TO_TRADE carries the slot to trade in a word
// from the GBA, and tradePkmnAtIndex() used it unchecked; any index past 5
// read past the partner's party and wrote past ours.
#include <cstdio>

#include "payloads/pokemon.hpp"

int main()
{
    for (unsigned i = 0; i < 256; i++) party::tradePkmnAtIndex(static_cast<uint8_t>(i));
    std::puts("party: every index from the link stays inside the party");
    return 0;
}
