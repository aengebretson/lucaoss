#include "benchmark.hpp"
#include "synthetic_data.hpp"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <thread>

#ifdef __linux__
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifndef LUCA_BENCHMARK_BUILD_TYPE
#define LUCA_BENCHMARK_BUILD_TYPE "unknown"
#endif
#ifndef LUCA_BENCHMARK_COMPILER
#define LUCA_BENCHMARK_COMPILER "unknown"
#endif

namespace luca::bench {
namespace {

std::string operating_system() {
#ifdef __linux__
  utsname value{};
  return uname(&value) == 0 ? std::string{value.sysname} + " " + value.release : "Linux";
#elif defined(_WIN32)
  return "Windows";
#elif defined(__APPLE__)
  return "macOS";
#else
  return "unknown";
#endif
}

std::string architecture() {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#else
  return "unknown";
#endif
}

std::size_t resident_bytes() {
#ifdef __linux__
  std::ifstream statm{"/proc/self/statm"};
  std::size_t total_pages{}, resident_pages{};
  if (statm >> total_pages >> resident_pages)
    return resident_pages * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
  return 0;
}

std::size_t parse_size(std::string_view value) {
  std::size_t offset = 0;
  const auto parsed = std::stoull(std::string{value}, &offset);
  if (offset != value.size() || (parsed != 10'000 && parsed != 100'000 && parsed != 1'000'000))
    throw std::invalid_argument("--size must be 10000, 100000, or 1000000");
  return parsed;
}

Configuration arguments(int argc, char** argv) {
  Configuration configuration;
  if (const char* environment_size = std::getenv("LUCA_BENCHMARK_SIZE"))
    configuration.events = parse_size(environment_size);
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument{argv[i]};
    const auto value = [&](std::string_view option) -> std::string_view {
      if (i + 1 == argc) throw std::invalid_argument(std::string{option} + " needs a value");
      return argv[++i];
    };
    if (argument == "--size") configuration.events = parse_size(value(argument));
    else if (argument == "--repetitions") configuration.repetitions = std::stoull(std::string{value(argument)});
    else if (argument == "--format") configuration.format = value(argument);
    else if (argument == "--filter") configuration.filter = value(argument);
    else if (argument == "--help") {
      std::cout << "Usage: pacioli_benchmarks [--size 10000|100000|1000000] "
                   "[--repetitions N] [--filter substring] [--format text|json|csv]\n";
      std::exit(0);
    } else throw std::invalid_argument("unknown argument: " + std::string{argument});
  }
  if (configuration.repetitions == 0) throw std::invalid_argument("repetitions must be positive");
  if (configuration.format != "text" && configuration.format != "json" && configuration.format != "csv")
    throw std::invalid_argument("format must be text, json, or csv");
  return configuration;
}

void print_measurements(const Configuration& configuration,
                        const std::vector<Measurement>& measurements,
                        std::size_t rss_bytes) {
  const auto rate = [](const Measurement& value) {
    return value.median_seconds == 0 ? 0.0 : value.input_count / value.median_seconds;
  };
  if (configuration.format == "json") {
    std::cout << "{\n  \"metadata\": {\"compiler\": \"" << LUCA_BENCHMARK_COMPILER
              << "\", \"build_type\": \"" << LUCA_BENCHMARK_BUILD_TYPE
              << "\", \"os\": \"" << operating_system() << "\", \"architecture\": \""
              << architecture() << "\", \"dataset_size\": " << configuration.events
              << ", \"repetitions\": " << configuration.repetitions << "},\n"
              << "  \"memory\": {\"EconomicEvent\": " << sizeof(EconomicEvent)
              << ", \"LedgerEntry\": " << sizeof(LedgerEntry) << ", \"Position\": " << sizeof(Position)
              << ", \"CashBalance\": " << sizeof(CashBalance) << ", \"SettlementObligation\": " << sizeof(SettlementObligation)
              << ", \"PositionObservation\": " << sizeof(PositionObservation) << ", \"CashObservation\": " << sizeof(CashObservation)
              << ", \"linux_process_rss_bytes\": " << rss_bytes << "},\n  \"benchmarks\": [\n";
    for (std::size_t i = 0; i < measurements.size(); ++i) {
      const auto& value = measurements[i];
      std::cout << "    {\"name\": \"" << value.name << "\", \"input_count\": " << value.input_count
                << ", \"matching_or_result_count\": " << value.result_count << ", \"unit\": \"" << value.unit
                << "\", \"median_seconds\": " << value.median_seconds << ", \"min_seconds\": " << value.min_seconds
                << ", \"max_seconds\": " << value.max_seconds << ", \"per_second\": " << rate(value) << "}"
                << (i + 1 == measurements.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
  } else if (configuration.format == "csv") {
    std::cout << "name,input_count,matching_or_result_count,unit,median_seconds,min_seconds,max_seconds,per_second\n";
    for (const auto& value : measurements)
      std::cout << value.name << ',' << value.input_count << ',' << value.result_count << ',' << value.unit << ','
                << value.median_seconds << ',' << value.min_seconds << ',' << value.max_seconds << ',' << rate(value) << '\n';
  } else {
    std::cout << "LUCA benchmark | compiler=" << LUCA_BENCHMARK_COMPILER << " | build=" << LUCA_BENCHMARK_BUILD_TYPE
              << " | os=" << operating_system() << " | arch=" << architecture() << " | events=" << configuration.events
              << " | repetitions=" << configuration.repetitions << "\n";
    std::cout << "Static bytes: EconomicEvent=" << sizeof(EconomicEvent) << " LedgerEntry=" << sizeof(LedgerEntry)
              << " Position=" << sizeof(Position) << " CashBalance=" << sizeof(CashBalance)
              << " SettlementObligation=" << sizeof(SettlementObligation) << " PositionObservation=" << sizeof(PositionObservation)
              << " CashObservation=" << sizeof(CashObservation) << " | Linux current RSS=" << rss_bytes << "\n";
    std::cout << std::left << std::setw(42) << "benchmark" << std::right << std::setw(12) << "input" << std::setw(12)
              << "result" << std::setw(15) << "median(s)" << std::setw(15) << "min(s)" << std::setw(15) << "max(s)"
              << std::setw(18) << "per_second" << '\n';
    for (const auto& value : measurements)
      std::cout << std::left << std::setw(42) << value.name << std::right << std::setw(12) << value.input_count
                << std::setw(12) << value.result_count << std::setw(15) << value.median_seconds << std::setw(15)
                << value.min_seconds << std::setw(15) << value.max_seconds << std::setw(18) << rate(value) << '\n';
  }
}

}  // namespace
}  // namespace luca::bench

int main(int argc, char** argv) {
  try {
    const auto configuration = luca::bench::arguments(argc, argv);
    std::vector<luca::bench::Benchmark> benchmarks;
    luca::bench::add_ledger_benchmarks(benchmarks, configuration);
    luca::bench::add_projection_benchmarks(benchmarks, configuration);
    luca::bench::add_reconciliation_benchmarks(benchmarks, configuration);
    luca::bench::add_journal_balance_benchmarks(benchmarks, configuration);
    std::vector<luca::bench::Measurement> measurements;
    for (const auto& benchmark : benchmarks)
      if (configuration.filter.empty() || benchmark.name.find(configuration.filter) != std::string::npos)
        measurements.push_back(luca::bench::measure(benchmark, configuration.repetitions));
    luca::bench::print_measurements(configuration, measurements, luca::bench::resident_bytes());
  } catch (const std::exception& error) {
    std::cerr << "benchmark error: " << error.what() << '\n';
    return 1;
  }
}
