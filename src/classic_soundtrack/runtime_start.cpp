#include "classic_soundtrack/runtime_start.h"

#include "classic_soundtrack/importer.h"

namespace tmnt::classic {

RuntimeStartResult PrepareRuntimeStart(bool enabled, const std::filesystem::path& user_data_root) {
  RuntimeStartResult result;
  if (!enabled) {
    result.message = "Classic soundtrack: Off";
    return result;
  }
  const StatusResult status = QuerySoundtrackStatus(user_data_root, &result.set);
  if (status.status != SoundtrackStatus::kReady) {
    result.start = RuntimeStart::kFallback;
    result.message = std::string("Classic soundtrack: the original music plays because the import is ") +
                     (status.status == SoundtrackStatus::kNotImported ? "missing" : "not usable: " + status.detail);
    result.set = {};
    return result;
  }
  result.start = RuntimeStart::kReady;
  result.region = status.manifest.source_region;
  result.message = "Classic soundtrack: Ready, source " + result.region;
  return result;
}

}  // namespace tmnt::classic
