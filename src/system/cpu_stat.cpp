#include "system/cpu_stat.h"

#include "util/sys_utils.h"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <numeric>
#include <sstream>

// FreeBSD specific includes

#include <sys/resource.h>
#include <sys/sysctl.h>

namespace noctalia::system::cpu_stat {

  namespace {

    // The label is everything before the first space: "cpu" for the aggregate row, "cpuN" for a
    // core. Returns an empty view for a row with no space at all.
    [[nodiscard]] std::string_view labelOf(const std::string& line) {
      const auto space = line.find(' ');
      return space == std::string::npos ? std::string_view{} : std::string_view{line}.substr(0, space);
    }

    [[nodiscard]] bool isCoreLabel(std::string_view label) { return SysUtils::isCpuN(label); }

  } // namespace

  std::optional<Totals> parseLine(const std::string& line, std::string_view expectedLabel) {
    std::istringstream iss{line};
    std::string cpuLabel;
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
    std::uint64_t steal = 0;

    // user/nice/system/idle are mandatory, so a row with a plausible label but unreadable counters
    // is rejected rather than read as all-zero. The trailing fields are optional: a kernel
    // predating one leaves it zero-initialised rather than poisoning the sum.
    if (!(iss >> cpuLabel >> user >> nice >> system >> idle)) {
      return std::nullopt;
    }
    if (cpuLabel != expectedLabel) {
      return std::nullopt;
    }
    iss >> iowait >> irq >> softirq >> steal;

    Totals totals{};
    totals.idle = idle + iowait;
    totals.total = user + nice + system + idle + iowait + irq + softirq + steal;
    return totals;
  }

  std::optional<double> usageBetween(const Totals& prev, const Totals& current) {
    if (current.total <= prev.total) {
      return std::nullopt;
    }
    const std::uint64_t totalDelta = current.total - prev.total;
    const std::uint64_t idleDelta = current.idle >= prev.idle ? current.idle - prev.idle : 0;
    const double busy = 1.0 - (static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
    return std::clamp(100.0 * busy, 0.0, 100.0);
  }

  std::optional<size_t> getCpuCount() {
    size_t ncpu = 0;
    size_t len = sizeof(size_t);
    int ncpuMib[2] = {CTL_HW, HW_NCPU};
    int success = sysctl(ncpuMib, 2, &ncpu, &len, nullptr, 0);
    if (success < 0) {
      return std::nullopt;
    }
    return std::optional{ncpu};
  }

  constexpr unsigned CP_TIME_MIB_LEN = 2;

  struct CpTimeMib {
    int mib[CP_TIME_MIB_LEN];
  };

  std::optional<CpTimeMib> getCpTimeMib() {
    size_t len;
    CpTimeMib result;
    int success = sysctlnametomib("kern.cp_time", result.mib, &len);
    if (success < 0) {
      return {};
    }
    return result;
  }


  // We cache the CP time mib. According to `man 3 sysctl`, sysctlbyname takes approximately 3x longer than sysctl.
  static const std::optional<CpTimeMib> cpTimeMib = getCpTimeMib();

  std::optional<Totals> readTotals(const std::filesystem::path& /*unused*/) {
    if (!cpTimeMib.has_value())
      return {};
    std::array<size_t, CPUSTATES> cpuValues = {0};
    size_t len = sizeof(cpuValues);
    int success = sysctl(cpTimeMib->mib, CP_TIME_MIB_LEN, &cpuValues, &len, nullptr, 0);
    if (success < 0) {
      return {};
    }

    return Totals{.total = std::accumulate(cpuValues.begin(), cpuValues.end(), 0U), .idle = cpuValues[CP_IDLE]};
  }

  std::optional<CpTimeMib> getAllCpTimeMib() {
    size_t len;
    CpTimeMib result;
    int success = sysctlnametomib("kern.cp_times", result.mib, &len);
    if (success < 0) {
      return {};
    }
    return result;
  }

  // Same caching idea to avoid sysctlbyname.
  static const std::optional<CpTimeMib> allCpTimesMib = getAllCpTimeMib();

  std::optional<std::vector<Totals>> readCoreTotals(const std::filesystem::path& /*unused*/) {
    if (!allCpTimesMib.has_value())
      return {};
    auto ncpu = getCpuCount();
    if (!ncpu.has_value())
      return {};
    std::vector<std::array<size_t, CPUSTATES>> cpu_times(*ncpu);
    std::memset(cpu_times.data(), 0, cpu_times.size());
    size_t len = sizeof(size_t) * CPUSTATES * (*ncpu);
    int success = sysctl(allCpTimesMib->mib, CP_TIME_MIB_LEN, cpu_times.data(), &len, nullptr, 0);
    if (success < 0) {
      return {};
    }

    std::vector<Totals> totals(*ncpu);
    for (size_t i = 0; i < *ncpu; ++i) {
      totals[i].idle = cpu_times[i][CP_IDLE];
      totals[i].total = std::accumulate(cpu_times[i].begin(), cpu_times[i].end(), 0U);
    }
    return totals;
  }

} // namespace noctalia::system::cpu_stat
