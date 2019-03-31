#include <ESOData/Filesystem/Archive.h>
#include <ESOData/Filesystem/DataFileHeader.h>
#include <ESOData/Filesystem/FileSignature.h>

#include <ESOData/IO/IOUtilities.h>

#include <ESOData/Serialization/InputSerializationStream.h>
#include <ESOData/Serialization/DeflatedSegment.h>

#include <ESOData/Cryptography/CNGAlgorithmProvider.h>
#include <ESOData/Cryptography/CNGKey.h>
#include <ESOData/Cryptography/CNGHash.h>

#include <archiveparse/EncodingUtilities.h>
#include <archiveparse/WindowsError.h>

#include <sstream>
#include <array>

#include <zlib.h>

namespace esodata {
	Archive::Archive(const std::string &manifestFilename) {
		{
			auto data = readWholeFile(manifestFilename);
			InputSerializationStream stream(data.data(), data.data() + data.size());

			stream >> m_manifest;
		}

		m_files.reserve(m_manifest.dataFileCount());

		for (size_t index = 0, count = m_manifest.dataFileCount(); index < count; index++) {
			std::stringstream name;

			auto delim = manifestFilename.find_last_of('.');
			name << manifestFilename.substr(0, delim);
			name.width(4);
			name.fill('0');
			name << index;
			name << ".dat";

			auto rawHandle = CreateFile(archiveparse::utf8ToWide(name.str()).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
			if (rawHandle == INVALID_HANDLE_VALUE)
				throw archiveparse::WindowsError();

			archiveparse::WindowsHandle handle(rawHandle);

			std::array<unsigned char, 14> headerData;
			DWORD bytesRead;
			OVERLAPPED overlapped;
			ZeroMemory(&overlapped, sizeof(overlapped));

			if(!ReadFile(handle.get(), headerData.data(), headerData.size(), &bytesRead, &overlapped))
				throw archiveparse::WindowsError();

			if (bytesRead != headerData.size())
				throw std::runtime_error("short read");

			InputSerializationStream stream(headerData.data(), headerData.data() + headerData.size());

			DataFileHeader header;
			stream >> header;

			m_files.emplace_back(std::move(handle));
		}
	}

	Archive::~Archive() = default;

	bool Archive::readFileByKey(uint64_t key, std::vector<unsigned char> &data) {
		auto it = m_manifest.body.data.files.find(key);
		if (it == m_manifest.body.data.files.end())
			return false;

		const auto &entry = (*it).second;

		OVERLAPPED overlapped;
		ZeroMemory(&overlapped, sizeof(overlapped));
		overlapped.Offset = entry.fileOffset;

		data.resize(entry.compressedSize);

		DWORD bytesRead;
		if(!ReadFile(m_files[entry.archiveIndex].get(), data.data(), data.size(), &bytesRead, &overlapped))
			throw archiveparse::WindowsError();

		if (bytesRead != data.size())
			throw std::runtime_error("short read");

		switch (entry.compressionType) {
		case FileCompressionType::None:
			if (entry.compressedSize != entry.uncompressedSize)
				throw std::logic_error("compressed/uncompressed size mismatch");

			break;

		case FileCompressionType::Deflate:
		{
			std::vector<unsigned char> uncompressedData(entry.uncompressedSize);
			zlibUncompress(data.data(), data.size(), uncompressedData.data(), uncompressedData.size());
			data = std::move(uncompressedData);
			break;
		}

		default:
			throw std::logic_error("unsupported compression type");
		}

		auto checksum = ~crc32(0xffffffff, data.data(), data.size());

		if (checksum != entry.fileCRC32) {
			std::stringstream error;
			error << "CRC32 mismatch for " << std::hex << key << ": expectected " << entry.fileCRC32 << ", got " << checksum;
			throw std::runtime_error(error.str());
		}

		if (m_manifest.hasFileSignatures()) {

			InputSerializationStream stream(data.data(), data.data() + data.size());
			stream.setSwapEndian(true);

			FileSignature signature;
			stream >> signature;

			data.erase(data.begin(), data.begin() + stream.getCurrentPosition());

			CNGKey key = CNGKey::importDERPublicKey(signature.publicKey.data);
			CNGAlgorithmProvider sha1Provider(BCRYPT_SHA1_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
			CNGHash hash(sha1Provider, nullptr, 0, 0);
			hash.hashData(data.data(), data.size());
			std::vector<uint8_t> digest;
			hash.finish(digest);

			if (!key.verifySignature(nullptr, digest.data(), digest.size(), signature.signature.data.data(), signature.signature.data.size(), 0)) {
				std::stringstream sstream;
				sstream << "Signature mismatch for " << std::hex << key;
				throw std::runtime_error(sstream.str());
			}
		}

		return true;
	}

	void Archive::enumerateFiles(std::function<void(uint64_t, size_t)> &&enumerator) {
		for (auto it = m_manifest.body.data.files.begin(); it != m_manifest.body.data.files.end(); it++) {
			auto &info = (*it).second;

			enumerator((*it).first, info.uncompressedSize);
		}
	}
}
