#include "chronos/journal/inspection.hpp"

#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: chronos_journal_inspect JOURNAL MANIFEST\n";
        return 2;
    }
    std::ifstream journal(argv[1], std::ios::binary), manifest(argv[2], std::ios::binary);
    if (!journal || !manifest) {
        std::cerr << "Cannot open journal or manifest\n";
        return 1;
    }
    const auto result = chronos::journal::inspect(journal, manifest);
    if (!result.ok) {
        std::cerr << "Invalid journal envelope: " << result.error << '\n';
        return 1;
    }
    std::cout << "Structurally valid prefix: LSN " << result.durable_lsn << ", terminal offset "
              << result.terminal_offset << ", unverified tail bytes " << result.tail_bytes << '\n';
    std::cout << "Outcomes and exchange state were not verified or recovered.\n";
    return 0;
}
