#include "classic_soundtrack_page.h"

#include <utility>

namespace tmnt {

const char* ClassicStatusLabel(ClassicSoundtrackStatus status) {
  switch (status) {
    case ClassicSoundtrackStatus::kNotImported: return "Not imported";
    case ClassicSoundtrackStatus::kReady: return "Ready";
    case ClassicSoundtrackStatus::kImporting: return "Importing";
    case ClassicSoundtrackStatus::kReimportRequired: return "Re-import required";
  }
  return "Not imported";
}

const char* ClassicFailureLabel(classic::ImportFailure failure) {
  switch (failure) {
    case classic::ImportFailure::kInvalidRom: return "Invalid ROM";
    case classic::ImportFailure::kUnsupportedRom: return "Unsupported ROM";
    case classic::ImportFailure::kUnsupportedSoundtrack: return "Unsupported soundtrack";
    case classic::ImportFailure::kCorruptSoundtrack: return "Corrupt soundtrack data";
    case classic::ImportFailure::kNone:
    case classic::ImportFailure::kImportFailed: break;
  }
  return "Import failed";
}

const char* ClassicFailureText(classic::ImportFailure failure) {
  switch (failure) {
    case classic::ImportFailure::kInvalidRom:
      return "The file is not a SNES ROM the importer can read. Choose an unmodified .sfc, .smc or .zip dump.";
    case classic::ImportFailure::kUnsupportedRom:
      return "This SNES ROM is not Teenage Mutant Ninja Turtles IV: Turtles in Time.";
    case classic::ImportFailure::kUnsupportedSoundtrack:
      return "The music in this ROM differs from the known USA, Europe and Australia versions.";
    case classic::ImportFailure::kCorruptSoundtrack:
      return "The music data in this ROM is damaged. Try another dump.";
    case classic::ImportFailure::kNone:
    case classic::ImportFailure::kImportFailed: break;
  }
  return "The imported music could not be saved. Check that the user data folder can be written.";
}

const char* ClassicImportedMessage(const std::string& region) {
  if (region == "USA") return "SNES soundtrack imported from the USA ROM. Switch Classic soundtrack On to use it.";
  if (region == "Europe") return "SNES soundtrack imported from the Europe ROM. Switch Classic soundtrack On to use it.";
  if (region == "Australia") return "SNES soundtrack imported from the Australia ROM. Switch Classic soundtrack On to use it.";
  return "SNES soundtrack imported from a compatible ROM. Switch Classic soundtrack On to use it.";
}

ClassicSoundtrackStatus ClassicStatusFromQuery(classic::SoundtrackStatus status) {
  switch (status) {
    case classic::SoundtrackStatus::kReady: return ClassicSoundtrackStatus::kReady;
    case classic::SoundtrackStatus::kNotImported: return ClassicSoundtrackStatus::kNotImported;
    case classic::SoundtrackStatus::kUnsupportedRom:
    case classic::SoundtrackStatus::kImportFailed: break;
  }
  return ClassicSoundtrackStatus::kReimportRequired;
}

ClassicToggleOutcome CommitClassicToggle(bool requested, ClassicSoundtrackStatus status, const ClassicSettingStore& store) {
  ClassicToggleOutcome outcome;
  outcome.value = ClassicEffectiveSetting(requested, status);
  const std::string value = outcome.value ? "true" : "false";
  outcome.persisted = store.save && store.read_back && store.save(value) && store.read_back() == value;
  return outcome;
}

ClassicSoundtrackModel::ClassicSoundtrackModel(std::filesystem::path user_data_root) : root_(std::move(user_data_root)) {
  Refresh();
}

void ClassicSoundtrackModel::Refresh() {
  const classic::StatusResult result = classic::QuerySoundtrackStatus(root_);
  status_ = ClassicStatusFromQuery(result.status);
  region_ = status_ == ClassicSoundtrackStatus::kReady ? result.manifest.source_region : std::string();
}

void ClassicSoundtrackModel::BeginImport() {
  importing_ = true;
  last_failure_ = classic::ImportFailure::kNone;
}

void ClassicSoundtrackModel::FinishImport(const classic::ImportResult& result) {
  importing_ = false;
  last_failure_ = result.ok ? classic::ImportFailure::kNone : result.failure;
  if (!result.ok && last_failure_ == classic::ImportFailure::kNone) {
    last_failure_ = classic::ImportFailure::kImportFailed;
  }
  Refresh();
}

bool ClassicSoundtrackModel::Remove(std::string& error) {
  const bool removed = classic::RemoveSoundtrack(root_, error);
  last_failure_ = classic::ImportFailure::kNone;
  Refresh();
  return removed;
}

}  // namespace tmnt
