// pkgmerge.cpp : Defines the entry point for the console application.
// Version 1.1 by xZenithy forked from Tustin master repo
// Improved performance speed from bigger parts files

#include "stdafx.h"
#include <stdio.h>
#include <string>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <map>
#include <list>
#include <assert.h>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;
using std::string;
using std::map;
using std::vector;

struct Package {
	int					part;
	fs::path			file;
	vector<Package>		parts;
	Package*			sc_part;		// Special _sc file
	string				output_name;	// Custom output name if _sc file exists
	bool operator < (const Package& rhs) const {
		return part < rhs.part;
	}
	Package() : sc_part(nullptr), part(0) {}
};

const char PKG_MAGIC[4] = { 0x7F, 0x43, 0x4E, 0x54 };

// Helper function to convert string to lowercase for case-insensitive comparison
string toLower(const string& str) {
	string result = str;
	std::transform(result.begin(), result.end(), result.begin(),
		[](unsigned char c) { return std::tolower(c); });
	return result;
}

// Helper function to check if string ends with a specific suffix
bool endsWith(const string& str, const string& suffix) {
	if (suffix.length() > str.length()) return false;
	return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
}

// Helper function to remove leading and trailing quotes from path strings
string cleanPathString(const string& path) {
	string result = path;
	
	// Remove leading quote
	if (!result.empty() && result.front() == '\"') {
		result.erase(0, 1);
	}
	
	// Remove trailing quote
	if (!result.empty() && result.back() == '\"') {
		result.erase(result.length() - 1, 1);
	}
	
	return result;
}

// Helper function to split merged arguments when trailing backslash causes quote escaping
bool splitMergedArguments(const string& merged, string& source, string& target) {
	// Look for the pattern: path" path (quote in the middle with space)
	size_t quotePos = merged.find('\"');
	
	if (quotePos != string::npos && quotePos < merged.length() - 1) {
		// Found a quote that's not at the end
		// Check if there's a space after it
		if (quotePos + 1 < merged.length() && merged[quotePos + 1] == ' ') {
			// Split at this position
			source = merged.substr(0, quotePos);
			target = merged.substr(quotePos + 2); // Skip quote and space
			return true;
		}
	}
	
	return false;
}

vector<string> merge(map<string, Package> packages, const fs::path& target_dir) {
	vector<string> created_files;

	// Calculate optimal buffer size based on available files
	// Start with 512 KB minimum, but scale up for large files
	size_t max_file_size = 0;
	for (auto & root : packages) {
		auto pkg = root.second;
		// Check all parts for the largest file
		for (auto & part : pkg.parts) {
			size_t part_size = fs::file_size(part.file);
			if (part_size > max_file_size) {
				max_file_size = part_size;
			}
		}
		// Check _sc file if exists
		if (pkg.sc_part != nullptr) {
			size_t sc_size = fs::file_size(pkg.sc_part->file);
			if (sc_size > max_file_size) {
				max_file_size = sc_size;
			}
		}
	}

	// Adaptive buffer sizing strategy
	size_t BUFFER_SIZE;
	if (max_file_size < 200 * 1024 * 1024) {
		BUFFER_SIZE = 512 * 1024;  // 512 KB
		printf("[Performance info] Using 512 KB buffer for small files\n");
	} else if (max_file_size < 1024 * 1024 * 1024) {
		BUFFER_SIZE = 2 * 1024 * 1024;  // 2 MB
		printf("[Performance info] Using 2 MB buffer for medium files\n");
	} else if (max_file_size < 4ULL * 1024 * 1024 * 1024) {
		BUFFER_SIZE = 4 * 1024 * 1024;  // 4 MB
		printf("[Performance info] Using 4 MB buffer for large files\n");
	} else {
		BUFFER_SIZE = 8 * 1024 * 1024;  // 8 MB
		printf("[Performance info] Using 8 MB buffer for huge files (>4GB)\n");
	}

	// Allocate buffer ONCE on heap, reuse for all files
	char* buffer = new char[BUFFER_SIZE];

	for (auto & root : packages) {
		auto pkg = root.second;

		// Before we start, we need to sort the lists properly
		std::sort(pkg.parts.begin(), pkg.parts.end());

		size_t pieces = pkg.parts.size();
		
		// Add _sc file to the count if it exists
		if (pkg.sc_part != nullptr) {
			pieces++;
		}
		
		auto title_id = root.first.c_str();

		printf("[work] beginning to merge %d %s for package %s...\n", pieces, pieces == 1 ? "piece" : "pieces", title_id);

		// Use custom output name if _sc file exists, otherwise use title_id
		string merged_file_name;
		if (!pkg.output_name.empty()) {
			merged_file_name = pkg.output_name + "-merged.pkg";
			printf("[info] using custom output name from _sc file: %s\n", merged_file_name.c_str());
		} else {
			merged_file_name = root.first + "-merged.pkg";
		}
		
		string full_merged_file = target_dir.string() + "\\" + merged_file_name;

		if (fs::exists(full_merged_file)) {
			fs::remove(full_merged_file);
		}

		printf("\t[work] copying root package file to new file...");
		auto merged_file = fs::path(full_merged_file);

		// Deal with root file first
		fs::copy_file(pkg.file, merged_file, fs::copy_options::update_existing);
		printf("done\n");

		// Using C API from here on because it just works and is fast
		FILE *merged = fopen(full_merged_file.c_str(), "a+b");

		// Now all the regular pieces...
		for (auto & part : pkg.parts) {
			FILE *to_merge = fopen(part.file.string().c_str(), "r+b");

			auto total_size = fs::file_size(part.file);
			assert(total_size != 0);
			uintmax_t copied = 0;

			size_t read_data;
			while ((read_data = fread(buffer, 1, BUFFER_SIZE, to_merge)) > 0)
			{
				fwrite(buffer, 1, read_data, merged);
				copied += read_data;
				auto percentage = ((double)copied / (double)total_size) * 100;
				printf("\r\t[work] merged %llu/%llu bytes (%.0lf%%) for part %d...", copied, total_size, percentage, part.part);
			}
			fclose(to_merge);

			printf("done\n");
		}

		// Now merge the _sc file as the last part if it exists
		if (pkg.sc_part != nullptr) {
			FILE *to_merge = fopen(pkg.sc_part->file.string().c_str(), "r+b");

			auto total_size = fs::file_size(pkg.sc_part->file);
			assert(total_size != 0);
			uintmax_t copied = 0;

			size_t read_data;
			while ((read_data = fread(buffer, 1, BUFFER_SIZE, to_merge)) > 0)
			{
				fwrite(buffer, 1, read_data, merged);
				copied += read_data;
				auto percentage = ((double)copied / (double)total_size) * 100;
				printf("\r\t[work] merged %llu/%llu bytes (%.0lf%%) for _sc part (final)...", copied, total_size, percentage);
			}
			fclose(to_merge);

			printf("done\n");
		}

		fclose(merged);

		// Add the created file to the list
		created_files.push_back(full_merged_file);
	}

	// Free buffer once at the end
	delete[] buffer;

	return created_files;
}

int main(int argc, char *argv[])
{
	string source_dir;
	string target_dir;
	string mode = "-single";  // Default mode

	std::cout << "PKG-merge version 1.1 by xZenithy forked from Tustin master repo" << std::endl;
	std::cout << std::endl;


#ifndef _DEBUG
	// Check if arguments were merged due to trailing backslash before quote
	if (argc == 2) {
		string merged_arg = argv[1];
		string potential_source, potential_target;
		
		// Try to split the merged argument
		if (splitMergedArguments(merged_arg, potential_source, potential_target)) {
			std::cout << "[warn] Detected merged arguments due to trailing backslash before quote" << std::endl;
			std::cout << "[warn] Parsed as: Source='" << potential_source << "' Target='" << potential_target << "'" << std::endl;
			std::cout << "[info] To avoid this issue, don't end paths with backslash when using quotes" << std::endl;
			std::cout << "[info] Use \"C:\\Path\" instead of \"C:\\Path\\\"" << std::endl;
			std::cout << std::endl;
			
			source_dir = potential_source;
			target_dir = potential_target;
		} else {
			std::cout << "[error] Target folder argument is missing" << std::endl;
			std::cout << "Usage: pkg-merge.exe \"Source Folder\" \"Target Folder\" [mode]" << std::endl;
			std::cout << "\nNote: Use quotes around paths that contain spaces" << std::endl;
			std::cout << "Important: Do NOT end paths with backslash when using quotes" << std::endl;
			std::cout << "  Correct:   pkg-merge.exe \"C:\\My Documents\\PKGs\" \"C:\\Output Folder\"" << std::endl;
			std::cout << "  Incorrect: pkg-merge.exe \"C:\\My Documents\\PKGs\\\" \"C:\\Output Folder\\\"" << std::endl;
			return 1;
		}
	} else if (argc < 3 || argc > 4) {
		if (argc == 1) {
			std::cout << "Usage: pkg-merge.exe \"Source Folder\" \"Target Folder\" [mode]" << std::endl;
			std::cout << "\nArguments:" << std::endl;
			std::cout << "  Source Folder : Path to folder containing PKG files to merge (required)" << std::endl;
			std::cout << "  Target Folder : Path to folder where merged files will be created (required)" << std::endl;
			std::cout << "                  Use \".\" for current directory" << std::endl;
			std::cout << "  mode          : Merge mode - \"-single\" or \"-multiple\" (optional, default: -single)" << std::endl;
			std::cout << "\nMerge Modes:" << std::endl;
			std::cout << "  Single   : Merges all PKG files into one output file" << std::endl;
			std::cout << "             - If file ending with _sc exists, uses its name for output" << std::endl;
			std::cout << "             - Example: game_1.pkg, game_2.pkg, Title_sc.pkg -> Title-merged.pkg" << std::endl;
			std::cout << "             - Only ONE _sc file allowed (aborts if multiple found)" << std::endl;
			std::cout << "\n  Multiple : Merges multiple PKG sets independently by their base name" << std::endl;
			std::cout << "             - Groups files by prefix (before _number)" << std::endl;
			std::cout << "             - Example: file_1.pkg, file_2.pkg, file_sc.pkg -> file-merged.pkg" << std::endl;
			std::cout << "                        other_1.pkg, other_2.pkg, other_sc.pkg -> other-merged.pkg" << std::endl;
			std::cout << "             - Multiple _sc files allowed (one per group)" << std::endl;
			std::cout << "\nExamples:" << std::endl;
			std::cout << "  pkg-merge.exe \"C:\\My Documents\\PKGs\" \"C:\\Output Folder\"" << std::endl;
			std::cout << "  pkg-merge.exe C:\\PKGs . -single" << std::endl;
			std::cout << "  pkg-merge.exe C:\\PKGs C:\\Output -multiple" << std::endl;
			std::cout << "\nNote: Use quotes around paths that contain spaces" << std::endl;
			std::cout << "Important: Do NOT end paths with backslash when using quotes" << std::endl;
		}
		else {
			std::cout << "[error] Invalid number of arguments" << std::endl;
			std::cout << "Usage: pkg-merge.exe \"Source Folder\" \"Target Folder\" [mode]" << std::endl;
		}
		return 1;
	} else {
		// Normal case: argc == 3 or 4
		source_dir = cleanPathString(argv[1]);
		target_dir = cleanPathString(argv[2]);
		
		// Check for optional third argument (mode)
		if (argc == 4) {
			mode = toLower(cleanPathString(argv[3]));
			if (mode != "-single" && mode != "-multiple") {
				std::cout << "[error] Invalid mode '" << argv[3] << "'. Must be '-single' or '-multiple'" << std::endl;
				return 1;
			}
		}
	}
#else
	source_dir = "c:\\Users\\Public\\Sources\\repos\\pkg-merge-master\\x64\\Debug\\pkgs";
	target_dir = "c:\\Users\\Public\\Sources\\repos\\pkg-merge-master\\x64\\Debug\\output";
	mode = "-single";  // Can be changed for debugging
#endif // !_DEBUG

	printf("[info] Merge mode: %s\n", mode.c_str());

	// Handle "." for current directory
	if (target_dir == ".") {
		target_dir = fs::current_path().string();
	}

	// std::filesystem::path handles paths with spaces correctly
	fs::path source_path = fs::path(source_dir);
	fs::path target_path = fs::path(target_dir);

	// Validate source directory
	if (!fs::exists(source_path)) {
		printf("[error] source directory '%s' does not exist\n", source_dir.c_str());
		return 1;
	}

	if (!fs::is_directory(source_path)) {
		printf("[error] source argument '%s' is not a directory\n", source_dir.c_str());
		return 1;
	}

	// Validate target directory
	if (!fs::exists(target_path)) {
		printf("[error] target directory '%s' does not exist\n", target_dir.c_str());
		return 1;
	}

	if (!fs::is_directory(target_path)) {
		printf("[error] target argument '%s' is not a directory\n", target_dir.c_str());
		return 1;
	}

	// Count _sc files
	int sc_file_count = 0;
	for (auto & file : fs::directory_iterator(source_path)) {
		string file_name = file.path().filename().string();
		
		if (file.path().extension() != ".pkg") continue;
		if (file_name.find("-merged") != string::npos) continue;
		
		// Check for _sc.pkg files
		if (endsWith(file_name, "_sc.pkg")) {
			sc_file_count++;
		}
	}

	// Check mode-specific constraints
	if (mode == "-single" && sc_file_count > 1) {
		printf("[error] Have been detected more than 1 file ended with '_sc'. Merge process aborted!\n");
		printf("[info] Use mode '-multiple' to process multiple PKG groups independently\n");
		return 1;
	}

	if (sc_file_count > 0) {
		if (mode == "-single") {
			printf("[info] Detected 1 file ending with '_sc' - will be merged as the last part\n");
		} else {
			printf("[info] Detected %d file(s) ending with '_sc' - will process multiple PKG groups\n", sc_file_count);
		}
	}

	// After the mode validation, replace the file processing section with this:

	map<string, Package> packages;
	
	// In SINGLE mode, we need two passes to avoid the _sc file being processed first
	if (mode == "-single") {
		// FIRST PASS: Process all NON-_sc files
		for (auto & file : fs::directory_iterator(source_path)) {
			string file_name = file.path().filename().string();

			if (file.path().extension() != ".pkg") {
				printf("[warn] '%s' is not a PKG file. skipping...\n", file_name.c_str());
				continue;
			}

			if (file_name.find("-merged") != string::npos) continue;
			
			// Skip _sc files in first pass
			if (endsWith(file_name, "_sc.pkg")) {
				continue;
			}

			size_t found_part_begin = file_name.find_last_of("_") + 1;
			size_t found_part_end = file_name.find_first_of(".");
			string part = file_name.substr(found_part_begin, found_part_end - found_part_begin);
			string title_id = file_name.substr(0, found_part_begin - 1);
			char* ptr = NULL;
			auto pkg_piece = strtol(part.c_str(), &ptr, 10);
			if (ptr == NULL) {
				printf("[warn] '%s' is not a valid piece (fails integer conversion). skipping...\n", part.c_str());
				continue;
			}

			//Check if package exists
			auto it = packages.find(title_id);
			if (it != packages.end()) {
				//Exists, so add this as a piece
				auto pkg = &it->second;
				auto piece = Package();
				piece.file = file.path();
				piece.part = pkg_piece;
				pkg->parts.push_back(piece);
				printf("[success] found piece %d for PKG file %s\n", pkg_piece, title_id.c_str());
				continue;
			}

			//Wasn't found, so let's try to see if it's a root PKG file.
			std::ifstream ifs(file.path().string(), std::ios::binary);
			char magic[4];
			ifs.read(magic, sizeof(magic));
			ifs.close();

			if (memcmp(magic, PKG_MAGIC, sizeof(PKG_MAGIC) != 0)) {
				printf("[warn] assumed root PKG file '%s' doesn't match PKG magic (is %x, wants %x). skipping...\n", file_name.c_str(), magic, PKG_MAGIC);
				continue;
			}

			auto package = Package();
			package.part = 0;
			package.file = file.path();
			packages.insert(std::pair<string, Package>(title_id, package));
			printf("[success] found root PKG file for %s\n", title_id.c_str());
		}
		
		// SECOND PASS: Process _sc file and attach to first package found
		for (auto & file : fs::directory_iterator(source_path)) {
			string file_name = file.path().filename().string();

			if (file.path().extension() != ".pkg") continue;
			if (file_name.find("-merged") != string::npos) continue;
			
			// Only process _sc files in second pass
			if (endsWith(file_name, "_sc.pkg")) {
				// Extract base name (everything before _sc.pkg)
				string base_name = file_name.substr(0, file_name.length() - 7); // Remove "_sc.pkg"
				
				// In single mode, attach to the first (and should be only) package
				if (packages.empty()) {
					// No packages found, create one with the _sc file as root
					auto package = Package();
					package.part = 0;
					package.file = file.path();
					package.output_name = base_name;
					
					auto sc_package = new Package();
					sc_package->file = file.path();
					sc_package->part = 9999;
					package.sc_part = sc_package;
					
					packages.insert(std::pair<string, Package>(base_name, package));
					printf("[success] found _sc PKG file for %s (will be merged as last part)\n", base_name.c_str());
				} else {
					// Attach to the first package found
					auto it = packages.begin();
					auto pkg = &it->second;
					
					auto sc_package = new Package();
					sc_package->file = file.path();
					sc_package->part = 9999;
					pkg->sc_part = sc_package;
					pkg->output_name = base_name;
					printf("[success] found _sc PKG file for %s (will be merged as last part)\n", base_name.c_str());
				}
			}
		}
	} else {
		// MULTIPLE MODE: Original single-pass logic works fine
		for (auto & file : fs::directory_iterator(source_path)) {
			string file_name = file.path().filename().string();

			if (file.path().extension() != ".pkg") {
				printf("[warn] '%s' is not a PKG file. skipping...\n", file_name.c_str());
				continue;
			}

			if (file_name.find("-merged") != string::npos) continue;

			// Check if this is a _sc file
			if (endsWith(file_name, "_sc.pkg")) {
				// Extract base name (everything before _sc.pkg)
				string base_name = file_name.substr(0, file_name.length() - 7); // Remove "_sc.pkg"
				string title_id = base_name;
				
				auto it = packages.find(title_id);
				
				if (it == packages.end()) {
					// No matching package found, create new one
					auto package = Package();
					package.part = 0;
					package.file = file.path();
					package.output_name = base_name;
					
					auto sc_package = new Package();
					sc_package->file = file.path();
					sc_package->part = 9999;
					package.sc_part = sc_package;
					
					packages.insert(std::pair<string, Package>(title_id, package));
					printf("[success] found _sc PKG file for %s (will be merged as last part)\n", base_name.c_str());
				} else {
					// Found matching package, add _sc as the last part
					auto pkg = &it->second;
					auto sc_package = new Package();
					sc_package->file = file.path();
					sc_package->part = 9999;
					pkg->sc_part = sc_package;
					pkg->output_name = base_name;
					printf("[success] found _sc PKG file for %s (will be merged as last part)\n", base_name.c_str());
				}
				
				continue;
			}

			size_t found_part_begin = file_name.find_last_of("_") + 1;
			size_t found_part_end = file_name.find_first_of(".");
			string part = file_name.substr(found_part_begin, found_part_end - found_part_begin);
			string title_id = file_name.substr(0, found_part_begin - 1);
			char* ptr = NULL;
			auto pkg_piece = strtol(part.c_str(), &ptr, 10);
			if (ptr == NULL) {
				printf("[warn] '%s' is not a valid piece (fails integer conversion). skipping...\n", part.c_str());
				continue;
			}

			//Check if package exists
			auto it = packages.find(title_id);
			if (it != packages.end()) {
				//Exists, so add this as a piece
				auto pkg = &it->second;
				auto piece = Package();
				piece.file = file.path();
				piece.part = pkg_piece;
				pkg->parts.push_back(piece);
				printf("[success] found piece %d for PKG file %s\n", pkg_piece, title_id.c_str());
				continue;
			}

			//Wasn't found, so let's try to see if it's a root PKG file.
			std::ifstream ifs(file.path().string(), std::ios::binary);
			char magic[4];
			ifs.read(magic, sizeof(magic));
			ifs.close();

			if (memcmp(magic, PKG_MAGIC, sizeof(PKG_MAGIC) != 0)) {
				printf("[warn] assumed root PKG file '%s' doesn't match PKG magic (is %x, wants %x). skipping...\n", file_name.c_str(), magic, PKG_MAGIC);
				continue;
			}

			auto package = Package();
			package.part = 0;
			package.file = file.path();
			packages.insert(std::pair<string, Package>(title_id, package));
			printf("[success] found root PKG file for %s\n", title_id.c_str());
		}
	}


	vector<string> created_files = merge(packages, target_path);

	printf("\n[success] completed\n");

	// Display all created files
	for (const auto& file : created_files) {
		printf("The file was created: %s\n", file.c_str());
	}

	return 0;
}


