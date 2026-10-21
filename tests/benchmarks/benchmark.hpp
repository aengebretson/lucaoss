#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace luca::bench {

struct Configuration {
  std::size_t events = 100'000;
  std::size_t repetitions = 5;
  std::string format = "text";
  std::string filter;
};

struct Measurement {
  std::string name;
  std::size_t input_count{};
  std::size_t result_count{};
  std::string unit;
  double median_seconds{};
  double min_seconds{};
  double max_seconds{};
};

struct Benchmark {
  std::string name;
  std::size_t input_count{};
  std::string unit;
  std::function<std::size_t()> validate;
  std::function<std::uint64_t()> run;
};

inline volatile std::uint64_t consumed_result = 0;

inline Measurement measure(const Benchmark& benchmark, std::size_t repetitions) {
  const auto result_count = benchmark.validate();
  consumed_result = benchmark.run();  // untimed warm-up
  std::vector<double> samples;
  samples.reserve(repetitions);
  for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
    const auto start = std::chrono::steady_clock::now();
    const auto value = benchmark.run();
    const auto stop = std::chrono::steady_clock::now();
    consumed_result = value;
    samples.push_back(std::chrono::duration<double>(stop - start).count());
  }
  std::sort(samples.begin(), samples.end());
  return {benchmark.name, benchmark.input_count, result_count, benchmark.unit,
          samples[samples.size() / 2], samples.front(), samples.back()};
}

void add_ledger_benchmarks(std::vector<Benchmark>&, const Configuration&);
void add_projection_benchmarks(std::vector<Benchmark>&, const Configuration&);
void add_reconciliation_benchmarks(std::vector<Benchmark>&, const Configuration&);
void add_journal_balance_benchmarks(std::vector<Benchmark>&, const Configuration&);

}  // namespace luca::bench
