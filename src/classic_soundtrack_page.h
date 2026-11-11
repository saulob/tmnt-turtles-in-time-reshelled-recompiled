#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "classic_soundtrack/importer.h"

namespace tmnt {

enum class ClassicSoundtrackStatus { kNotImported, kReady, kImporting, kReimportRequired };

// Launcher text, in English: the launcher translates it.
const char* ClassicStatusLabel(ClassicSoundtrackStatus status);
const char* ClassicFailureLabel(classic::ImportFailure failure);
const char* ClassicFailureText(classic::ImportFailure failure);
const char* ClassicImportedMessage(const std::string& region);

ClassicSoundtrackStatus ClassicStatusFromQuery(classic::SoundtrackStatus status);
inline bool ClassicCanEnable(ClassicSoundtrackStatus status) { return status == ClassicSoundtrackStatus::kReady; }
inline bool ClassicCanRemove(ClassicSoundtrackStatus status) {
  return status == ClassicSoundtrackStatus::kReady || status == ClassicSoundtrackStatus::kReimportRequired;
}
// The Classic soundtrack setting only takes effect with a Ready import.
inline bool ClassicEffectiveSetting(bool stored, ClassicSoundtrackStatus status) {
  return stored && ClassicCanEnable(status);
}

// How the Gameplay toggle reaches settings.toml: save writes the value now, read_back reads it from the file.
struct ClassicSettingStore {
  std::function<bool(const std::string& value)> save;
  std::function<std::string()> read_back;
};

struct ClassicToggleOutcome {
  bool value = false;      // what the setting is after the change
  bool persisted = false;  // settings.toml holds exactly that value
};

// Writes the Classic soundtrack choice at once and checks the file. On is refused without a Ready import.
ClassicToggleOutcome CommitClassicToggle(bool requested, ClassicSoundtrackStatus status, const ClassicSettingStore& store);

// The Gameplay page's state for the imported soundtrack. Reading the import touches the disk, so it happens on
// construction and after an import or removal, never per frame.
class ClassicSoundtrackModel {
 public:
  explicit ClassicSoundtrackModel(std::filesystem::path user_data_root);

  void Refresh();
  void BeginImport();
  void FinishImport(const classic::ImportResult& result);
  bool Remove(std::string& error);

  ClassicSoundtrackStatus status() const { return importing_ ? ClassicSoundtrackStatus::kImporting : status_; }
  const std::string& region() const { return region_; }
  classic::ImportFailure last_failure() const { return last_failure_; }
  const std::filesystem::path& user_data_root() const { return root_; }

 private:
  std::filesystem::path root_;
  ClassicSoundtrackStatus status_ = ClassicSoundtrackStatus::kNotImported;
  std::string region_;
  classic::ImportFailure last_failure_ = classic::ImportFailure::kNone;
  bool importing_ = false;
};

}  // namespace tmnt
