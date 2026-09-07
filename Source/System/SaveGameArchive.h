#pragma once

#ifdef SYSTEM_MINIZIP
#include <minizip/unzip.h>
#else
#include "unzip.h"
#endif

#include <array>
#include <stdexcept>
#include <string>

namespace RTE {

	/// Reads complete, checksummed entries from a saved game.
	class SaveGameArchive {
	public:
		explicit SaveGameArchive(const std::string& filePath) : m_File(unzOpen(filePath.c_str())) {
			if (!m_File) throw std::runtime_error("could not open the save archive");
		}
		~SaveGameArchive() { unzClose(m_File); }
		SaveGameArchive(const SaveGameArchive&) = delete;
		SaveGameArchive& operator=(const SaveGameArchive&) = delete;

		/// Returns false only when an optional entry is absent; invalid entries throw.
		bool ReadEntry(const std::string& name, std::string& data, bool required = true) {
			const int found = unzLocateFile(m_File, name.c_str(), NULL);
			if (!required && found == UNZ_END_OF_LIST_OF_FILE) return false;
			if (found != UNZ_OK) throw std::runtime_error("missing or unreadable " + name);
			unz_file_info64 info{};
			if (unzGetCurrentFileInfo64(m_File, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK ||
			    info.uncompressed_size > data.max_size() || unzOpenCurrentFile(m_File) != UNZ_OK) {
				throw std::runtime_error("could not open " + name);
			}
			struct Entry {
				unzFile file;
				~Entry() { if (file) unzCloseCurrentFile(file); }
			} entry{m_File};
			data.clear();
			std::array<char, 65536> chunk;
			int count;
			while ((count = unzReadCurrentFile(m_File, chunk.data(), chunk.size())) > 0) {
				if (data.size() + count > info.uncompressed_size) throw std::runtime_error("incorrect size for " + name);
				data.append(chunk.data(), count);
			}
			const int closed = unzCloseCurrentFile(m_File);
			entry.file = nullptr;
			if (count < 0 || closed != UNZ_OK || data.size() != info.uncompressed_size) {
				throw std::runtime_error("incomplete or corrupt " + name);
			}
			return true;
		}

	private:
		unzFile m_File;
	};
}
