/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file fios.cpp
 * This file contains functions for building file lists for the save/load dialogs.
 */

#include "stdafx.h"
#include "3rdparty/md5/md5.h"
#include "fileio_func.h"
#include "fios.h"
#include "network/network_content.h"
#include "screenshot.h"
#include "string_func.h"
#include "strings_func.h"
#include "tar_type.h"
#include <sys/stat.h>
#include <charconv>
#include <filesystem>

#include "table/strings.h"

#include "safeguards.h"

/* Variables to display file lists */
static std::string *_fios_path = nullptr;
SortingBits _savegame_sort_order = SORT_BY_DATE | SORT_DESCENDING;

/* OS-specific functions are taken from their respective files (win32/unix .c) */
extern bool FiosIsRoot(const std::string &path);
extern bool FiosIsHiddenFile(const std::filesystem::path &path);
extern void FiosGetDrives(FileList &file_list);

/* get the name of an oldstyle savegame */
extern std::string GetOldSaveGameName(const std::string &file);

/**
 * Compare two FiosItem's. Used with sort when sorting the file list.
 * @param other The FiosItem to compare to.
 * @return for ascending order: returns true if da < db. Vice versa for descending order.
 */
bool FiosItem::operator< (const FiosItem &other) const
{
	int r = false;

	if ((_savegame_sort_order & SORT_BY_NAME) == 0 && (*this).mtime != other.mtime) {
		r = ClampTo<int32_t>(this->mtime - other.mtime);
	} else {
		r = StrNaturalCompare((*this).title, other.title);
	}
	if (r == 0) return false;
	return (_savegame_sort_order & SORT_DESCENDING) ? r > 0 : r < 0;
}

/**
 * Construct a file list with the given kind of files, for the stated purpose.
 * @param abstract_filetype Kind of files to collect.
 * @param fop Purpose of the collection, either #SLO_LOAD or #SLO_SAVE.
 * @param show_dirs Whether to show directories.
 */
void FileList::BuildFileList(AbstractFileType abstract_filetype, SaveLoadOperation fop, bool show_dirs)
{
	this->clear();

	assert(fop == SLO_LOAD || fop == SLO_SAVE);
	switch (abstract_filetype) {
		case FT_NONE:
			break;

		case FT_SAVEGAME:
			FiosGetSavegameList(fop, show_dirs, *this);
			break;

		case FT_SCENARIO:
			FiosGetScenarioList(fop, show_dirs, *this);
			break;

		case FT_HEIGHTMAP:
			FiosGetHeightmapList(fop, show_dirs, *this);
			break;

		case FT_TOWN_DATA:
			FiosGetTownDataList(fop, show_dirs, *this);
			break;

		default:
			NOT_REACHED();
	}
}

/**
 * Find file information of a file by its name from the file list.
 * @param file The filename to return information about. Can be the actual name
 *             or a numbered entry into the filename list.
 * @return The information on the file, or \c nullptr if the file is not available.
 */
const FiosItem *FileList::FindItem(const std::string_view file)
{
	for (const auto &it : *this) {
		const FiosItem *item = &it;
		if (file == item->name) return item;
		if (file == item->title) return item;
	}

	/* If no name matches, try to parse it as number */
	int i;
	const char *endptr = std::from_chars(file.data(), file.data() + file.size(), i, 10).ptr;
	if (file.data() == endptr || endptr != file.data() + file.size()) i = -1;

	if (IsInsideMM(i, 0, this->size())) return &this->at(i);

	/* As a last effort assume it is an OpenTTD savegame and
	 * that the ".sav" part was not given. */
	std::string long_file(file);
	long_file += ".sav";
	for (const auto &it : *this) {
		const FiosItem *item = &it;
		if (long_file == item->name) return item;
		if (long_file == item->title) return item;
	}

	return nullptr;
}

/**
 * Get the current path/working directory.
 */
std::string FiosGetCurrentPath()
{
	return *_fios_path;
}

/**
 * Browse to a new path based on the passed \a item, starting at #_fios_path.
 * @param *item Item telling us what to do.
 * @return \c true when the path got changed.
 */
bool FiosBrowseTo(const FiosItem *item)
{
	switch (item->type) {
		case FIOS_TYPE_DRIVE:
#if defined(_WIN32)
			assert(_fios_path != nullptr);
			*_fios_path = std::string{ item->title, 0, 1 } + ":" PATHSEP;
#endif
			break;

		case FIOS_TYPE_INVALID:
			break;

		case FIOS_TYPE_PARENT: {
			assert(_fios_path != nullptr);
			auto s = _fios_path->find_last_of(PATHSEPCHAR);
			if (s != std::string::npos && s != 0) {
				_fios_path->erase(s); // Remove last path separator character, so we can go up one level.
			}

			s = _fios_path->find_last_of(PATHSEPCHAR);
			if (s != std::string::npos) {
				_fios_path->erase(s + 1); // go up a directory
			}
			break;
		}

		case FIOS_TYPE_DIR:
			assert(_fios_path != nullptr);
			*_fios_path += item->name;
			*_fios_path += PATHSEP;
			break;

		case FIOS_TYPE_DIRECT:
			assert(_fios_path != nullptr);
			*_fios_path = item->name;
			break;

		case FIOS_TYPE_FILE:
		case FIOS_TYPE_OLDFILE:
		case FIOS_TYPE_SCENARIO:
		case FIOS_TYPE_OLD_SCENARIO:
		case FIOS_TYPE_PNG:
		case FIOS_TYPE_BMP:
		case FIOS_TYPE_JSON:
			return false;
	}

	return true;
}

/**
 * Construct a filename from its components in destination buffer \a buf.
 * @param path Directory path, may be \c nullptr.
 * @param name Filename.
 * @param ext Filename extension (use \c "" for no extension).
 * @return The completed filename.
 */
static std::string FiosMakeFilename(const std::string *path, const char *name, const char *ext)
{
	std::string buf;

	if (path != nullptr) {
		buf = *path;
		/* Remove trailing path separator, if present */
		if (!buf.empty() && buf.back() == PATHSEPCHAR) buf.pop_back();
	}

	/* Don't append the extension if it is already there */
	const char *period = strrchr(name, '.');
	if (period != nullptr && StrEqualsIgnoreCase(period, ext)) ext = "";

	return buf + PATHSEP + name + ext;
}

/**
 * Make a save game or scenario filename from a name.
 * @param name Name of the file.
 * @return The completed filename.
 */
std::string FiosMakeSavegameName(const char *name)
{
	const char *extension = (_game_mode == GM_EDITOR) ? ".scn" : ".sav";

	return FiosMakeFilename(_fios_path, name, extension);
}

/**
 * Construct a filename for a height map.
 * @param name Filename.
 * @return The completed filename.
 */
std::string FiosMakeHeightmapName(const char *name)
{
	std::string ext(".");
	ext += GetCurrentScreenshotExtension();

	return FiosMakeFilename(_fios_path, name, ext.c_str());
}

/**
 * Delete a file.
 * @param name Filename to delete.
 * @return Whether the file deletion was successful.
 */
bool FiosDelete(const char *name)
{
	return FioRemove(FiosMakeSavegameName(name));
}

typedef std::tuple<FiosType, std::string> FiosGetTypeAndNameProc(SaveLoadOperation fop, const std::string &filename, const std::string_view ext);

/**
 * Scanner to scan for a particular type of FIOS file.
 */
class FiosFileScanner : public FileScanner {
	SaveLoadOperation fop;   ///< The kind of file we are looking for.
	FiosGetTypeAndNameProc *callback_proc; ///< Callback to check whether the file may be added
	FileList &file_list;     ///< Destination of the found files.
public:
	/**
	 * Create the scanner
	 * @param fop Purpose of collecting the list.
	 * @param callback_proc The function that is called where you need to do the filtering.
	 * @param file_list Destination of the found files.
	 */
	FiosFileScanner(SaveLoadOperation fop, FiosGetTypeAndNameProc *callback_proc, FileList &file_list) :
			fop(fop), callback_proc(callback_proc), file_list(file_list)
	{}

	bool AddFile(const std::string &filename, size_t basepath_length, const std::string &tar_filename) override;
};

/**
 * Try to add a fios item set with the given filename.
 * @param filename        the full path to the file to read
 * @return true if the file is added.
 */
bool FiosFileScanner::AddFile(const std::string &filename, size_t, const std::string &)
{
	auto sep = filename.rfind('.');
	if (sep == std::string::npos) return false;
	std::string ext = filename.substr(sep);

	auto [type, title] = this->callback_proc(this->fop, filename, ext);
	if (type == FIOS_TYPE_INVALID) return false;

	for (const auto &fios : file_list) {
		if (filename == fios.name) return false;
	}

	FiosItem *fios = &file_list.emplace_back();

	std::error_code error_code;
	auto write_time = std::filesystem::last_write_time(OTTD2FS(filename), error_code);
	if (error_code) {
		fios->mtime = 0;
	} else {
		fios->mtime = std::chrono::duration_cast<std::chrono::milliseconds>(write_time.time_since_epoch()).count();
	}

	fios->type = type;
	fios->name = filename;

	/* If the file doesn't have a title, use its filename */
	if (title.empty()) {
		auto ps = filename.rfind(PATHSEPCHAR);
		fios->title = StrMakeValid(filename.substr((ps == std::string::npos ? 0 : ps + 1)));
	} else {
		fios->title = StrMakeValid(title);
	};

	return true;
}


/**
 * Fill the list of the files in a directory, according to some arbitrary rule.
 * @param fop Purpose of collecting the list.
 * @param show_dirs Whether to list directories.
 * @param callback_proc The function that is called where you need to do the filtering.
 * @param subdir The directory from where to start (global) searching.
 * @param file_list Destination of the found files.
 */
static void FiosGetFileList(SaveLoadOperation fop, bool show_dirs, FiosGetTypeAndNameProc *callback_proc, Subdirectory subdir, FileList &file_list)
{
	size_t sort_start = 0;

	file_list.clear();

	assert(_fios_path != nullptr);

	if (show_dirs) {
		/* A parent directory link exists if we are not in the root directory */
		if (!FiosIsRoot(*_fios_path)) {
			FiosItem &fios = file_list.emplace_back();
			fios.type = FIOS_TYPE_PARENT;
			fios.mtime = 0;
			fios.name = "..";
			fios.title = GetString(STR_SAVELOAD_PARENT_DIRECTORY, "..");
			sort_start = file_list.size();
		}

		/* Show subdirectories */
		std::error_code error_code;
		for (const auto &dir_entry : std::filesystem::directory_iterator(OTTD2FS(*_fios_path), error_code)) {
			if (!dir_entry.is_directory()) continue;
			if (FiosIsHiddenFile(dir_entry) && dir_entry.path().filename() != PERSONAL_DIR) continue;

			FiosItem &fios = file_list.emplace_back();
			fios.type = FIOS_TYPE_DIR;
			fios.mtime = 0;
			fios.name = FS2OTTD(dir_entry.path().filename());
			fios.title = GetString(STR_SAVELOAD_DIRECTORY, fios.name + PATHSEP);
		}

		/* Sort the subdirs always by name, ascending, remember user-sorting order */
		SortingBits order = _savegame_sort_order;
		_savegame_sort_order = SORT_BY_NAME | SORT_ASCENDING;
		std::sort(file_list.begin() + sort_start, file_list.end());
		_savegame_sort_order = order;
	}

	/* This is where to start sorting for the filenames */
	sort_start = file_list.size();

	/* Show files */
	FiosFileScanner scanner(fop, callback_proc, file_list);
	if (subdir == NO_DIRECTORY) {
		scanner.Scan({}, *_fios_path, false);
	} else {
		scanner.Scan({}, subdir, true, true);
	}

	std::sort(file_list.begin() + sort_start, file_list.end());

	/* Show drives */
	FiosGetDrives(file_list);
}

/**
 * Get the title of a file, which (if exists) is stored in a file named
 * the same as the data file but with '.title' added to it.
 * @param file filename to get the title for
 * @param subdir the sub directory to search in
 * @return The file title.
 */
static std::string GetFileTitle(const std::string &file, Subdirectory subdir)
{
	auto f = FioFOpenFile(file + ".title", "r", subdir);
	if (!f.has_value()) return {};

	char title[80];
	size_t read = fread(title, 1, lengthof(title), *f);

	assert(read <= lengthof(title));
	return StrMakeValid(std::string_view{title, read});
}

/**
 * Callback for FiosGetFileList. It tells if a file is a savegame or not.
 * @param fop Purpose of collecting the list.
 * @param file Name of the file to check.
 * @param ext A pointer to the extension identifier inside file
 * @return a FIOS_TYPE_* type of the found file, FIOS_TYPE_INVALID if not a savegame, and the title of the file (if any).
 * @see FiosGetFileList
 * @see FiosGetSavegameList
 */
std::tuple<FiosType, std::string> FiosGetSavegameListCallback(SaveLoadOperation fop, const std::string &file, const std::string_view ext)
{
	/* Show savegame files
	 * .SAV OpenTTD saved game
	 * .SS1 Transport Tycoon Deluxe preset game
	 * .SV1 Transport Tycoon Deluxe (Patch) saved game
	 * .SV2 Transport Tycoon Deluxe (Patch) saved 2-player game */

	if (StrEqualsIgnoreCase(ext, ".sav")) {
		return { FIOS_TYPE_FILE, GetFileTitle(file, SAVE_DIR) };
	}

	if (fop == SLO_LOAD) {
		if (StrEqualsIgnoreCase(ext, ".ss1") || StrEqualsIgnoreCase(ext, ".sv1") ||
				StrEqualsIgnoreCase(ext, ".sv2")) {
			return { FIOS_TYPE_OLDFILE, GetOldSaveGameName(file) };
		}
	}

	return { FIOS_TYPE_INVALID, {} };
}

/**
 * Get a list of savegames.
 * @param fop Purpose of collecting the list.
 * @param show_dirs Whether to show directories.
 * @param file_list Destination of the found files.
 * @see FiosGetFileList
 */
void FiosGetSavegameList(SaveLoadOperation fop, bool show_dirs, FileList &file_list)
{
	static std::optional<std::string> fios_save_path;

	if (!fios_save_path) fios_save_path = FioFindDirectory(SAVE_DIR);

	_fios_path = &(*fios_save_path);

	FiosGetFileList(fop, show_dirs, &FiosGetSavegameListCallback, NO_DIRECTORY, file_list);
}

/**
 * Callback for FiosGetFileList. It tells if a file is a scenario or not.
 * @param fop Purpose of collecting the list.
 * @param file Name of the file to check.
 * @param ext A pointer to the extension identifier inside file
 * @return a FIOS_TYPE_* type of the found file, FIOS_TYPE_INVALID if not a scenario and the title of the file (if any).
 * @see FiosGetFileList
 * @see FiosGetScenarioList
 */
std::tuple<FiosType, std::string> FiosGetScenarioListCallback(SaveLoadOperation fop, const std::string &file, const std::string_view ext)
{
	/* Show scenario files
	 * .SCN OpenTTD style scenario file
	 * .SV0 Transport Tycoon Deluxe (Patch) scenario
	 * .SS0 Transport Tycoon Deluxe preset scenario */
	if (StrEqualsIgnoreCase(ext, ".scn")) {
		return { FIOS_TYPE_SCENARIO, GetFileTitle(file, SCENARIO_DIR) };

	}

	if (fop == SLO_LOAD) {
		if (StrEqualsIgnoreCase(ext, ".sv0") || StrEqualsIgnoreCase(ext, ".ss0")) {
			return { FIOS_TYPE_OLD_SCENARIO, GetOldSaveGameName(file) };
		}
	}

	return { FIOS_TYPE_INVALID, {} };
}

/**
 * Get a list of scenarios.
 * @param fop Purpose of collecting the list.
 * @param show_dirs Whether to show directories.
 * @param file_list Destination of the found files.
 * @see FiosGetFileList
 */
void FiosGetScenarioList(SaveLoadOperation fop, bool show_dirs, FileList &file_list)
{
	static std::optional<std::string> fios_scn_path;

	/* Copy the default path on first run or on 'New Game' */
	if (!fios_scn_path) fios_scn_path = FioFindDirectory(SCENARIO_DIR);

	_fios_path = &(*fios_scn_path);

	std::string base_path = FioFindDirectory(SCENARIO_DIR);
	Subdirectory subdir = (fop == SLO_LOAD && base_path == *_fios_path) ? SCENARIO_DIR : NO_DIRECTORY;
	FiosGetFileList(fop, show_dirs, &FiosGetScenarioListCallback, subdir, file_list);
}

std::tuple<FiosType, std::string> FiosGetHeightmapListCallback(SaveLoadOperation, const std::string &file, const std::string_view ext)
{
	/* Show heightmap files
	 * .PNG PNG Based heightmap files
	 * .BMP BMP Based heightmap files
	 */

	FiosType type = FIOS_TYPE_INVALID;

#ifdef WITH_PNG
	if (StrEqualsIgnoreCase(ext, ".png")) type = FIOS_TYPE_PNG;
#endif /* WITH_PNG */

	if (StrEqualsIgnoreCase(ext, ".bmp")) type = FIOS_TYPE_BMP;

	if (type == FIOS_TYPE_INVALID) return { FIOS_TYPE_INVALID, {} };

	TarFileList::iterator it = _tar_filelist[SCENARIO_DIR].find(file);
	if (it != _tar_filelist[SCENARIO_DIR].end()) {
		/* If the file is in a tar and that tar is not in a heightmap
		 * directory we are for sure not supposed to see it.
		 * Examples of this are pngs part of documentation within
		 * collections of NewGRFs or 32 bpp graphics replacement PNGs.
		 */
		bool match = false;
		for (Searchpath sp : _valid_searchpaths) {
			std::string buf = FioGetDirectory(sp, HEIGHTMAP_DIR);

			if (buf.compare(0, buf.size(), it->second.tar_filename, 0, buf.size()) == 0) {
				match = true;
				break;
			}
		}

		if (!match) return { FIOS_TYPE_INVALID, {} };
	}

	return { type, GetFileTitle(file, HEIGHTMAP_DIR) };
}

/**
 * Get a list of heightmaps.
 * @param fop Purpose of collecting the list.
 * @param show_dirs Whether to show directories.
 * @param file_list Destination of the found files.
 */
void FiosGetHeightmapList(SaveLoadOperation fop, bool show_dirs, FileList &file_list)
{
	static std::optional<std::string> fios_hmap_path;

	if (!fios_hmap_path) fios_hmap_path = FioFindDirectory(HEIGHTMAP_DIR);

	_fios_path = &(*fios_hmap_path);

	std::string base_path = FioFindDirectory(HEIGHTMAP_DIR);
	Subdirectory subdir = base_path == *_fios_path ? HEIGHTMAP_DIR : NO_DIRECTORY;
	FiosGetFileList(fop, show_dirs, &FiosGetHeightmapListCallback, subdir, file_list);
}

/**
 * Callback for FiosGetTownDataList.
 * @param fop Purpose of collecting the list.
 * @param file Name of the file to check.
 * @return a FIOS_TYPE_JSON type of the found file, FIOS_TYPE_INVALID if not a valid JSON file, and the title of the file (if any).
 */
static std::tuple<FiosType, std::string> FiosGetTownDataListCallback(SaveLoadOperation fop, const std::string &file, const std::string_view ext)
{
	if (fop == SLO_LOAD) {
		if (StrEqualsIgnoreCase(ext, ".json")) {
			return { FIOS_TYPE_JSON, GetFileTitle(file, SAVE_DIR) };
		}
	}

	return { FIOS_TYPE_INVALID, {} };
}

/**
 * Get a list of town data files.
 * @param fop Purpose of collecting the list.
 * @param show_dirs Whether to show directories.
 * @param file_list Destination of the found files.
 */
void FiosGetTownDataList(SaveLoadOperation fop, bool show_dirs, FileList &file_list)
{
	static std::optional<std::string> fios_town_data_path;

	if (!fios_town_data_path) fios_town_data_path = FioFindDirectory(HEIGHTMAP_DIR);

	_fios_path = &(*fios_town_data_path);

	std::string base_path = FioFindDirectory(HEIGHTMAP_DIR);
	Subdirectory subdir = base_path == *_fios_path ? HEIGHTMAP_DIR : NO_DIRECTORY;
	FiosGetFileList(fop, show_dirs, &FiosGetTownDataListCallback, subdir, file_list);
}

/**
 * Get the directory for screenshots.
 * @return path to screenshots
 */
const char *FiosGetScreenshotDir()
{
	static std::optional<std::string> fios_screenshot_path;

	if (!fios_screenshot_path) fios_screenshot_path = FioFindDirectory(SCREENSHOT_DIR);

	return fios_screenshot_path->c_str();
}

/** Basic data to distinguish a scenario. Used in the server list window */
struct ScenarioIdentifier {
	uint32_t scenid;           ///< ID for the scenario (generated by content).
	MD5Hash md5sum;          ///< MD5 checksum of file.
	std::string filename;    ///< filename of the file.

	bool operator == (const ScenarioIdentifier &other) const
	{
		return this->scenid == other.scenid && this->md5sum == other.md5sum;
	}
};

/**
 * Scanner to find the unique IDs of scenarios
 */
class ScenarioScanner : protected FileScanner, public std::vector<ScenarioIdentifier> {
	bool scanned; ///< Whether we've already scanned
public:
	/** Initialise */
	ScenarioScanner() : scanned(false) {}

	/**
	 * Scan, but only if it's needed.
	 * @param rescan whether to force scanning even when it's not necessary
	 */
	void Scan(bool rescan)
	{
		if (this->scanned && !rescan) return;

		this->FileScanner::Scan(".id", SCENARIO_DIR, true, true);
		this->scanned = true;
	}

	bool AddFile(const std::string &filename, size_t, const std::string &) override
	{
		auto f = FioFOpenFile(filename, "r", SCENARIO_DIR);
		if (!f.has_value()) return false;

		ScenarioIdentifier id;
		int fret = fscanf(*f, "%u", &id.scenid);
		if (fret != 1) return false;
		id.filename = filename;

		Md5 checksum;
		uint8_t buffer[1024];
		size_t len, size;

		/* open the scenario file, but first get the name.
		 * This is safe as we check on extension which
		 * must always exist. */
		f = FioFOpenFile(filename.substr(0, filename.rfind('.')), "rb", SCENARIO_DIR, &size);
		if (!f.has_value()) return false;

		/* calculate md5sum */
		while ((len = fread(buffer, 1, (size > sizeof(buffer)) ? sizeof(buffer) : size, *f)) != 0 && size != 0) {
			size -= len;
			checksum.Append(buffer, len);
		}
		checksum.Finish(id.md5sum);

		include(*this, id);
		return true;
	}
};

/** Scanner for scenarios */
static ScenarioScanner _scanner;

/**
 * Find a given scenario based on its unique ID.
 * @param ci The content info to compare it to.
 * @param md5sum Whether to look at the md5sum or the id.
 * @return The filename of the file, else \c nullptr.
 */
const char *FindScenario(const ContentInfo &ci, bool md5sum)
{
	_scanner.Scan(false);

	for (ScenarioIdentifier &id : _scanner) {
		if (md5sum ? (id.md5sum == ci.md5sum)
		           : (id.scenid == ci.unique_id)) {
			return id.filename.c_str();
		}
	}

	return nullptr;
}

/**
 * Check whether we've got a given scenario based on its unique ID.
 * @param ci The content info to compare it to.
 * @param md5sum Whether to look at the md5sum or the id.
 * @return True iff we've got the scenario.
 */
bool HasScenario(const ContentInfo &ci, bool md5sum)
{
	return (FindScenario(ci, md5sum) != nullptr);
}

/**
 * Force a (re)scan of the scenarios.
 */
void ScanScenarios()
{
	_scanner.Scan(true);
}

/**
 * Constructs FiosNumberedSaveName. Initial number is the most recent save, or -1 if not found.
 * @param prefix The prefix to use to generate a filename.
 */
FiosNumberedSaveName::FiosNumberedSaveName(const std::string &prefix) : prefix(prefix), number(-1)
{
	static std::optional<std::string> _autosave_path;
	if (!_autosave_path) _autosave_path = FioFindDirectory(AUTOSAVE_DIR);

	static std::string _prefix; ///< Static as the lambda needs access to it.

	/* Callback for FiosFileScanner. */
	static FiosGetTypeAndNameProc *proc = [](SaveLoadOperation, const std::string &file, const std::string_view ext) {
		if (StrEqualsIgnoreCase(ext, ".sav") && file.starts_with(_prefix)) return std::tuple(FIOS_TYPE_FILE, std::string{});
		return std::tuple(FIOS_TYPE_INVALID, std::string{});
	};

	/* Prefix to check in the callback. */
	_prefix = *_autosave_path + this->prefix;

	/* Get the save list. */
	FileList list;
	FiosFileScanner scanner(SLO_SAVE, proc, list);
	scanner.Scan(".sav", *_autosave_path, false);

	/* Find the number for the most recent save, if any. */
	if (list.begin() != list.end()) {
		SortingBits order = _savegame_sort_order;
		_savegame_sort_order = SORT_BY_DATE | SORT_DESCENDING;
		std::sort(list.begin(), list.end());
		_savegame_sort_order = order;

		std::string_view name = list.begin()->title;
		std::from_chars(name.data() + this->prefix.size(), name.data() + name.size(), this->number);
	}
}

/**
 * Generate a savegame name and number according to _settings_client.gui.max_num_autosaves.
 * @return A filename in format "<prefix><number>.sav".
 */
std::string FiosNumberedSaveName::Filename()
{
	if (++this->number >= _settings_client.gui.max_num_autosaves) this->number = 0;
	return fmt::format("{}{}.sav", this->prefix, this->number);
}

/**
 * Generate an extension for a savegame name.
 * @return An extension in format "-<prefix>.sav".
 */
std::string FiosNumberedSaveName::Extension()
{
	return fmt::format("-{}.sav", this->prefix);
}
