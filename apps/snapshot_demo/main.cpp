#include "chronos/eventlog/event_log.hpp"
#include "chronos/simulation/simulation.hpp"
#include "chronos/snapshot/checkpoint.hpp"
#include "chronos/snapshot/restorable.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>

int main(int argc, char **argv) {
    using namespace chronos;
    if (argc != 3 || std::string_view(argv[1]) != "--image") {
        std::cerr << "Usage: chronos_snapshot_demo --image NEW_FILE\n";
        return 2;
    }
    try {
        simulation::ScenarioConfig config;
        config.scenario = simulation::Scenario::LiquiditySweep;
        config.max_commands = 5;
        config.min_quantity = 2;
        config.max_quantity = 3;
        std::ostringstream log(std::ios::binary);
        eventlog::EventRecorder recorder(log);
        simulation::SimulationRunner runner(config, &recorder, true);
        runner.start();
        runner.run_batch(config.max_commands);
        auto *original = runner.completed_isolated_exchange();
        if (!original) {
            std::cerr << "Simulation did not complete\n";
            return 1;
        }
        const auto diagnostic = snapshot::capture(*original, 1);
        std::ostringstream diagnostic_bytes(std::ios::binary);
        if (!snapshot::write(diagnostic_bytes, diagnostic).ok) {
            std::cerr << "Diagnostic checkpoint write failed\n";
            return 1;
        }
        std::istringstream diagnostic_input(diagnostic_bytes.str(), std::ios::binary);
        snapshot::Checkpoint diagnostic_copy;
        if (!snapshot::read(diagnostic_input, diagnostic_copy).ok ||
            !snapshot::compare(diagnostic_copy, *original).ok) {
            std::cerr << "Diagnostic checkpoint comparison failed\n";
            return 1;
        }
        std::ostringstream output(std::ios::binary);
        const auto written = snapshot::write_image(output, log.str(), *original, 1);
        if (!written.ok) {
            std::cerr << "Image capture failed: "
                      << (written.allocation_failure ? "allocation failure" : written.error)
                      << '\n';
            return 1;
        }
        const auto bytes = output.str();
        std::FILE *file = std::fopen(argv[2], "wbx");
        if (!file) {
            std::cerr << "Cannot create new image file\n";
            return 1;
        }
        const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
        const bool flushed = std::fflush(file) == 0;
        const bool closed = std::fclose(file) == 0;
        if (!wrote || !flushed || !closed) {
            std::cerr << "Image write failed or is uncertain\n";
            return 1;
        }
        std::ifstream input(argv[2], std::ios::binary);
        std::unique_ptr<exchange::Exchange> candidate;
        const auto loaded = snapshot::restore_image(input, candidate);
        if (!loaded.ok || !candidate) {
            std::cerr << "Image restore failed: "
                      << (loaded.allocation_failure ? "allocation failure" : loaded.error) << '\n';
            return 1;
        }
        if (original->isolated_continuation_state(1024 * 1024) !=
            candidate->isolated_continuation_state(1024 * 1024)) {
            std::cerr << "Initial exact state mismatch\n";
            return 1;
        }
        for (auto *book : {original, candidate.get()}) {
            if (book->submit(1, 1000, 1000, core::SymbolId{2}, core::Side::Sell,
                             core::OrderType::Limit, 110, 5) != exchange::Result::Accepted ||
                book->submit(2, 1000, 1000, core::SymbolId{2}, core::Side::Buy,
                             core::OrderType::Limit, 110, 2) != exchange::Result::Accepted ||
                book->cancel(1, 1001, 1000) != exchange::Result::Accepted) {
                std::cerr << "Follow-on command failed\n";
                return 1;
            }
        }
        if (original->isolated_continuation_state(1024 * 1024) !=
            candidate->isolated_continuation_state(1024 * 1024)) {
            std::cerr << "Follow-on state mismatch\n";
            return 1;
        }
        std::cout << "Diagnostic CHROVIEW comparison passed.\n";
        std::cout << "Verified isolated image: generation " << loaded.generation << ", ingress "
                  << loaded.ingress << ", bytes " << bytes.size()
                  << "; exact state and follow-on commands match.\n";
        std::cout << "No live publication or crash durability is provided.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Snapshot demo failed: " << e.what() << '\n';
        return 1;
    }
}
