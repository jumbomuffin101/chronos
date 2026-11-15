#include "chronos/eventlog/event_log.hpp"
#include "chronos/simulation/simulation.hpp"

#include <charconv>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <streambuf>
#include <string_view>

using namespace chronos::simulation;
namespace {
struct FileCloser {
    void operator()(std::FILE *file) const noexcept {
        if (file)
            std::fclose(file);
    }
};
class FileBuffer final : public std::streambuf {
  public:
    explicit FileBuffer(std::FILE *file) : file_(file) {}

  protected:
    std::streamsize xsputn(const char *data, std::streamsize size) override {
        return static_cast<std::streamsize>(
            std::fwrite(data, 1, static_cast<std::size_t>(size), file_));
    }
    int sync() override { return std::fflush(file_); }

  private:
    std::FILE *file_;
};
std::uint64_t number(std::string_view s) {
    std::uint64_t n = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || end != s.data() + s.size())
        throw std::invalid_argument("invalid integer");
    return n;
}
} // namespace
int main(int argc, char **argv) {
    try {
        ScenarioConfig c;
        bool has_symbol = false;
        std::string record_path;
        for (int i = 1; i < argc; ++i) {
            const std::string_view key{argv[i]};
            if (key == "--help") {
                std::cout << "chronos_sim [--scenario "
                             "balanced|high-volume|volatile|liquidity-sweep|mixed] [--seed N] "
                             "[--participants N] [--commands N] [--symbol N] [--reference N] "
                             "[--variation N] [--min-quantity N] [--max-quantity N] [--buy-weight "
                             "N] [--cancel-percent N] [--max-orders N] [--max-requests N] "
                             "[--record FILE]\n";
                return 0;
            }
            if (++i >= argc)
                throw std::invalid_argument("missing option value");
            const std::string_view value{argv[i]};
            if (key == "--record") {
                if (!record_path.empty() || value.empty())
                    throw std::invalid_argument("invalid or repeated record path");
                record_path = value;
            } else if (key == "--scenario") {
                if (value == "balanced")
                    c.scenario = Scenario::Balanced;
                else if (value == "high-volume")
                    c.scenario = Scenario::HighVolume;
                else if (value == "volatile")
                    c.scenario = Scenario::Volatile;
                else if (value == "liquidity-sweep")
                    c.scenario = Scenario::LiquiditySweep;
                else if (value == "mixed")
                    c.scenario = Scenario::Mixed;
                else
                    throw std::invalid_argument("unknown scenario");
            } else {
                const auto n = number(value);
                if (key == "--seed")
                    c.seed = n;
                else if (key == "--participants") {
                    if (n > 16)
                        throw std::invalid_argument("participants out of range");
                    c.participants = static_cast<std::size_t>(n);
                } else if (key == "--commands") {
                    if (n > 1024)
                        throw std::invalid_argument("commands out of range");
                    c.max_commands = static_cast<std::size_t>(n);
                } else if (key == "--symbol") {
                    if (n > UINT32_MAX)
                        throw std::invalid_argument("symbol out of range");
                    if (!has_symbol) {
                        c.symbols.clear();
                        has_symbol = true;
                    }
                    c.symbols.push_back(static_cast<std::uint32_t>(n));
                } else if (key == "--max-orders" || key == "--max-requests") {
                    if (n > 1024)
                        throw std::invalid_argument("capacity out of range");
                    if (key == "--max-orders")
                        c.max_order_history = static_cast<std::size_t>(n);
                    else
                        c.max_requests_per_session = static_cast<std::size_t>(n);
                } else if (key == "--reference" || key == "--variation") {
                    if (n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                        throw std::invalid_argument("price out of range");
                    if (key == "--reference")
                        c.reference_price = static_cast<std::int64_t>(n);
                    else
                        c.price_variation = static_cast<std::int64_t>(n);
                } else if (key == "--min-quantity")
                    c.min_quantity = n;
                else if (key == "--max-quantity")
                    c.max_quantity = n;
                else if (key == "--buy-weight") {
                    if (n > 100)
                        throw std::invalid_argument("weight out of range");
                    c.buy_weight = static_cast<std::uint32_t>(n);
                } else if (key == "--cancel-percent") {
                    if (n > 100)
                        throw std::invalid_argument("weight out of range");
                    c.cancel_percent = static_cast<std::uint32_t>(n);
                } else
                    throw std::invalid_argument("unknown option");
            }
        }
        const auto config_error = SimulationRunner::validate(c);
        if (!config_error.empty())
            throw std::invalid_argument(config_error);
        std::unique_ptr<std::FILE, FileCloser> file;
        std::unique_ptr<FileBuffer> buffer;
        std::unique_ptr<std::ostream> stream;
        std::unique_ptr<chronos::eventlog::EventRecorder> recorder;
        if (!record_path.empty()) {
            file.reset(std::fopen(record_path.c_str(), "wbx"));
            if (!file)
                throw std::runtime_error("cannot create new log file (it may already exist)");
            buffer = std::make_unique<FileBuffer>(file.get());
            stream = std::make_unique<std::ostream>(buffer.get());
            recorder = std::make_unique<chronos::eventlog::EventRecorder>(*stream);
        }
        SimulationRunner runner(c, recorder.get());
        runner.start();
        runner.run_batch(c.max_commands);
        const auto &r = runner.result();
        if (r.status.state != Lifecycle::Completed) {
            std::cerr << "Simulation failed: " << r.status.error << " (generated " << r.generated
                      << ", admitted " << r.admitted << ", accepted orders " << r.accepted_orders
                      << ", cancellations " << r.cancelled << ", committed trades " << r.trades
                      << ", aggregated trades " << r.aggregated_trades
                      << "); committed operations were not rolled back.\n";
            if (recorder && !recorder->error().empty())
                std::cerr << "Log error: " << recorder->error() << '\n';
            return 1;
        }
        if (file && std::fclose(file.release()) != 0) {
            std::cerr
                << "Simulation finished, but closing the log failed; recording is uncertain.\n";
            return 1;
        }
        std::cout << "Scenario: " << scenario_name(c.scenario) << " seed: " << c.seed
                  << " generator: " << SimulationRunner::generator_version
                  << " participants: " << c.participants << " symbols:";
        for (auto s : c.symbols)
            std::cout << ' ' << s;
        std::cout << "\nGenerated: " << r.generated << " admitted: " << r.admitted
                  << " accepted orders: " << r.accepted_orders
                  << " rejected orders: " << r.rejected_orders << " cancellations: " << r.cancelled
                  << " rejected cancellations: " << r.rejected_cancels << "\nTrades: " << r.trades
                  << " quantity: " << r.executed_quantity << " steps: " << r.status.steps << '\n';
        for (const auto &b : r.books)
            std::cout << "Symbol " << b.symbol << " bid " << b.bid << " x " << b.bid_quantity
                      << " ask " << b.ask << " x " << b.ask_quantity << '\n';
        std::cout << "Digest: " << std::hex << std::setfill('0') << std::setw(16) << r.digest
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Simulation error: " << e.what() << '\n';
        return 1;
    }
}
