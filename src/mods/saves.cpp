#include "mods/lp2.h"

#include "core/log.h"
#include "core/utils.h"

#include <windows.h>
#include <shlobj.h>

#include <string>

namespace xls::mods {

// Under GFWL the save lived in the GFWL gamertag's folder; ours is named after the Steam persona,
// so a player switching over would start fresh. When our folder has no save yet, the newest save
// of any other folder is copied in (the original is left alone). The file is not bound to the profile.
void SavesImport(const std::string& gamertag)
{
	static const wchar_t kSave[] = L"Lostplanet2.Lostplanet2Save-capcom";
	wchar_t docs[MAX_PATH];
	if (gamertag.empty() || FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
		return;
	}
	std::wstring root = std::wstring(docs) + L"\\CAPCOM\\LOST PLANET 2\\";
	std::wstring mine = root + Utf8ToWide(gamertag.c_str());
	if (GetFileAttributesW((mine + L"\\" + kSave).c_str()) != INVALID_FILE_ATTRIBUTES) {
		return;
	}
	WIN32_FIND_DATAW find;
	HANDLE it = FindFirstFileW((root + L"*").c_str(), &find);
	if (it == INVALID_HANDLE_VALUE) {
		return;
	}
	std::wstring newest;
	FILETIME newestTime = {};
	do {
		std::wstring name = find.cFileName;
		if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == L"." || name == L".." || _wcsicmp(name.c_str(), Utf8ToWide(gamertag.c_str()).c_str()) == 0) {
			continue;
		}
		WIN32_FILE_ATTRIBUTE_DATA save;
		if (GetFileAttributesExW((root + name + L"\\" + kSave).c_str(), GetFileExInfoStandard, &save) && CompareFileTime(&save.ftLastWriteTime, &newestTime) > 0) {
			newest = name;
			newestTime = save.ftLastWriteTime;
		}
	} while (FindNextFileW(it, &find));
	FindClose(it);
	if (newest.empty()) {
		return;
	}
	CreateDirectoryW(mine.c_str(), nullptr);
	if (CopyFileW((root + newest + L"\\" + kSave).c_str(), (mine + L"\\" + kSave).c_str(), TRUE)) {
		XLS_LOG_INFO("mods: saves: imported the save of %s into %s (the original is kept).", WideToUtf8(newest.c_str()).c_str(), gamertag.c_str());
	} else {
		XLS_LOG_WARN("mods: saves: could not copy the save of %s into %s (%lu).", WideToUtf8(newest.c_str()).c_str(), gamertag.c_str(), GetLastError());
	}
}

}
