#pragma once
#include "ChiakiBackend.h"
#include <filesystem>
#include <optional>
namespace veyra::remoteplay {
// macOS port: <AppLocalData>/remoteplay. Profiles are written owner-only (0600)
// in Veyra's binary layout; DPAPI has no macOS equivalent bound to a user account
// that survives without prompts, so the file permission is the protection.
std::filesystem::path profileDirectory();
bool saveProfile(const std::filesystem::path&, const NativeConnectRequest&);
enum class ProfileLoadError { None, Missing, Read, Decrypt, Format };
std::optional<NativeConnectRequest> loadProfile(const std::filesystem::path&,ProfileLoadError* error=nullptr);
}
