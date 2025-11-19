#include "config.h"
#include "glog/file_pack.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <codecvt>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
# include <io.h>
# include <sys/stat.h>
#else
# include <sys/stat.h>
# include <sys/types.h>
# include <unistd.h>
#endif

namespace {

#if defined(_WIN32)
using StatInfo = struct _stat64;
#else
using StatInfo = struct stat;
#endif

constexpr size_t kTarBlockSize = 512;

struct TarHeader {
  char name[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char size[12];
  char mtime[12];
  char chksum[8];
  char typeflag;
  char linkname[100];
  char magic[6];
  char version[2];
  char uname[32];
  char gname[32];
  char devmajor[8];
  char devminor[8];
  char prefix[155];
  char padding[12];
};

static_assert(sizeof(TarHeader) == kTarBlockSize,
              "TarHeader must be 512 bytes.");

class FileStream {
 public:
  FileStream() : file_(nullptr) {}
  ~FileStream() { Close(); }

  bool OpenForRead(const std::wstring& path) {
    return Open(path, L"rb", "rb");
  }

  bool OpenForWrite(const std::wstring& path) {
    return Open(path, L"wb", "wb");
  }

  size_t Read(void* buffer, size_t size) {
    if (!file_ || size == 0) return 0;
    return std::fread(buffer, 1, size, file_);
  }

  bool Write(const void* buffer, size_t size) {
    if (!file_) return false;
    if (size == 0) return true;
    return std::fwrite(buffer, 1, size, file_) == size;
  }

  bool Flush() {
    if (!file_) return false;
    return std::fflush(file_) == 0;
  }

  bool Eof() const {
    return file_ && std::feof(file_) != 0;
  }

  bool Error() const {
    return file_ && std::ferror(file_) != 0;
  }

  void Close() {
    if (file_) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

 private:
  bool Open(const std::wstring& path,
            const wchar_t* mode_w,
            const char* mode_narrow) {
    Close();
#if defined(_WIN32)
    file_ = _wfopen(path.c_str(), mode_w);
#else
    file_ = std::fopen(WideToUtf8(path).c_str(), mode_narrow);
#endif
    return file_ != nullptr;
  }

  static std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) return std::string();
    try {
      std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
      return conv.to_bytes(input);
    } catch (const std::range_error&) {
      std::string fallback;
      fallback.reserve(input.size());
      for (wchar_t ch : input) {
        if (static_cast<unsigned>(ch) < 0x80) {
          fallback.push_back(static_cast<char>(ch));
        } else {
          fallback.push_back('?');
        }
      }
      return fallback;
    }
  }

  FILE* file_;
};

std::wstring GetBasename(const std::wstring& path) {
  const std::wstring::size_type pos = path.find_last_of(L"/\\");
  if (pos == std::wstring::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

std::string NarrowFromWide(const std::wstring& value) {
  if (value.empty()) return std::string();
  try {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.to_bytes(value);
  } catch (const std::range_error&) {
    std::string fallback;
    fallback.reserve(value.size());
    for (wchar_t ch : value) {
      if (static_cast<unsigned>(ch) < 0x80) {
        fallback.push_back(static_cast<char>(ch));
      } else {
        fallback.push_back('?');
      }
    }
    return fallback;
  }
}

std::string NormalizeTarPath(const std::wstring& path) {
  std::string normalized = NarrowFromWide(path);
  if (normalized.empty()) {
    normalized = NarrowFromWide(GetBasename(path));
  }
  if (normalized.empty()) {
    normalized = "file";
  }
  for (char& ch : normalized) {
    if (ch == '\\') {
      ch = '/';
    }
  }
  if (normalized.size() >= 2 && normalized[1] == ':') {
    normalized.erase(0, 2);
  }
  while (!normalized.empty() && normalized.front() == '/') {
    normalized.erase(normalized.begin());
  }
  if (normalized.empty()) {
    normalized = "file";
  }
  return normalized;
}

bool AssignTarPath(const std::string& path, TarHeader* header) {
  if (path.size() <= sizeof(header->name)) {
    std::memcpy(header->name, path.c_str(), path.size());
    return true;
  }

  const std::string::size_type slash = path.rfind('/');
  if (slash == std::string::npos) {
    return false;
  }

  const std::string prefix = path.substr(0, slash);
  const std::string filename = path.substr(slash + 1);

  if (filename.size() > sizeof(header->name) ||
      prefix.size() > sizeof(header->prefix)) {
    return false;
  }

  std::memcpy(header->prefix, prefix.c_str(), prefix.size());
  std::memcpy(header->name, filename.c_str(), filename.size());
  return true;
}

template <size_t N>
bool FormatOctal(uint64_t value, char (&buffer)[N]) {
  const size_t digits = N - 1;
  const uint64_t max_value =
      digits >= 22 ? std::numeric_limits<uint64_t>::max()
                   : ((uint64_t{1} << (digits * 3)) - 1);
  if (value > max_value) {
    return false;
  }
  const int required = static_cast<int>(digits);
#if defined(_MSC_VER)
# pragma warning(push)
# pragma warning(disable: 4996)
#endif
  std::snprintf(buffer, N, "%0*llo",
                required,
                static_cast<unsigned long long>(value));
#if defined(_MSC_VER)
# pragma warning(pop)
#endif
  return true;
}

#if defined(_WIN32)
unsigned int ModeFromStat(const StatInfo& info) {
  unsigned int user = 0;
  if (info.st_mode & _S_IREAD) {
    user |= 0400;
  }
  if (info.st_mode & _S_IWRITE) {
    user |= 0200;
  }
  if (info.st_mode & _S_IEXEC) {
    user |= 0100;
  }
  unsigned int mode = user;
  mode |= (user >> 3);
  mode |= (user >> 6);
  return mode;
}
#else
unsigned int ModeFromStat(const StatInfo& info) {
  return static_cast<unsigned int>(info.st_mode & 0777);
}
#endif

bool IsRegularFile(const StatInfo& info) {
#if defined(_WIN32)
  return (info.st_mode & _S_IFREG) != 0;
#else
  return S_ISREG(info.st_mode);
#endif
}

bool GetFileInfo(const std::wstring& path, StatInfo* info) {
#if defined(_WIN32)
  return _wstat64(path.c_str(), info) == 0;
#else
  return ::stat(NarrowFromWide(path).c_str(), info) == 0;
#endif
}

void SetChecksum(TarHeader* header) {
  std::memset(header->chksum, ' ', sizeof(header->chksum));
  unsigned int sum = 0;
  const unsigned char* bytes =
      reinterpret_cast<const unsigned char*>(header);
  for (size_t i = 0; i < sizeof(TarHeader); ++i) {
    sum += bytes[i];
  }
#if defined(_MSC_VER)
# pragma warning(push)
# pragma warning(disable: 4996)
#endif
  std::snprintf(header->chksum, sizeof(header->chksum), "%06o",
                sum);
#if defined(_MSC_VER)
# pragma warning(pop)
#endif
  header->chksum[6] = '\0';
  header->chksum[7] = ' ';
}

bool BuildTarHeader(const std::wstring& path,
                    const StatInfo& info,
                    TarHeader* header) {
  std::memset(header, 0, sizeof(TarHeader));
  const std::string tar_path = NormalizeTarPath(path);
  if (!AssignTarPath(tar_path, header)) {
    return false;
  }
  if (info.st_size < 0) {
    return false;
  }
  const uint64_t file_size = static_cast<uint64_t>(info.st_size);
  if (!FormatOctal(file_size, header->size) ||
      !FormatOctal(static_cast<uint64_t>(info.st_mtime >= 0
                                               ? info.st_mtime
                                               : 0),
                   header->mtime)) {
    return false;
  }
  if (!FormatOctal(ModeFromStat(info), header->mode)) {
    return false;
  }
#if defined(_WIN32)
  std::memset(header->uid, '0', sizeof(header->uid) - 1);
  std::memset(header->gid, '0', sizeof(header->gid) - 1);
#else
  if (!FormatOctal(info.st_uid, header->uid) ||
      !FormatOctal(info.st_gid, header->gid)) {
    return false;
  }
#endif
  header->typeflag = '0';
  std::memcpy(header->magic, "ustar", 5);
  header->magic[5] = '\0';
  header->version[0] = '0';
  header->version[1] = '0';
  SetChecksum(header);
  return true;
}

bool CopyFileToTar(const std::wstring& source,
                   uint64_t expected_size,
                   FileStream& tar) {
  FileStream input;
  if (!input.OpenForRead(source)) {
    return false;
  }

  std::vector<char> buffer(64 * 1024);
  uint64_t remaining = expected_size;
  while (remaining > 0) {
    const size_t chunk = static_cast<size_t>(
        std::min<uint64_t>(remaining, buffer.size()));
    const size_t read = input.Read(buffer.data(), chunk);
    if (read == 0) {
      return false;
    }
    if (!tar.Write(buffer.data(), read)) {
      return false;
    }
    remaining -= read;
  }

  const size_t pad =
      static_cast<size_t>((kTarBlockSize - (expected_size % kTarBlockSize)) %
                          kTarBlockSize);
  if (pad != 0) {
    char zeros[kTarBlockSize] = {0};
    if (!tar.Write(zeros, pad)) {
      return false;
    }
  }
  return true;
}

const uint32_t* Crc32Table() {
  static uint32_t table[256];
  static bool initialized = false;
  if (!initialized) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (int j = 0; j < 8; ++j) {
        if (crc & 1U) {
          crc = (crc >> 1) ^ 0xEDB88320u;
        } else {
          crc >>= 1;
        }
      }
      table[i] = crc;
    }
    initialized = true;
  }
  return table;
}

uint32_t UpdateCrc32(uint32_t crc,
                     const unsigned char* data,
                     size_t length) {
  const uint32_t* table = Crc32Table();
  uint32_t value = crc;
  for (size_t i = 0; i < length; ++i) {
    value = (value >> 8) ^
            table[(value ^ data[i]) & 0xFF];
  }
  return value;
}

struct DeflateWriter {
  explicit DeflateWriter(FileStream* file)
      : file(file), bit_buffer(0), bit_count(0) {}

  bool WriteStoredData(const unsigned char* data,
                       size_t length,
                       bool final_block) {
    if (length == 0) {
      return WriteStoredBlock(nullptr, 0, final_block);
    }
    size_t offset = 0;
    while (offset < length) {
      const size_t chunk = std::min(length - offset, kMaxStoredBlockSize);
      const bool is_final = final_block && (offset + chunk == length);
      if (!WriteStoredBlock(data + offset, chunk, is_final)) {
        return false;
      }
      offset += chunk;
    }
    return true;
  }

  bool Finish(uint32_t crc32, uint64_t input_size) {
    if (!FlushBits()) {
      return false;
    }
    const uint32_t final_crc = crc32 ^ 0xFFFFFFFFu;
    if (!WriteUint32(final_crc) ||
        !WriteUint32(static_cast<uint32_t>(input_size & 0xFFFFFFFFu))) {
      return false;
    }
    return file->Flush();
  }

  static constexpr size_t kMaxStoredBlockSize = 0xFFFF;

 private:
  bool WriteStoredBlock(const unsigned char* data,
                        size_t length,
                        bool final) {
    if (length > kMaxStoredBlockSize) {
      return false;
    }
    if (!WriteBits(final ? 1u : 0u, 1) ||
        !WriteBits(0u, 2u) ||
        !FlushBits()) {
      return false;
    }
    const uint16_t len16 = static_cast<uint16_t>(length);
    const uint16_t nlen16 = static_cast<uint16_t>(~len16);
    if (!WriteUint16(len16) || !WriteUint16(nlen16)) {
      return false;
    }
    if (length > 0 && !file->Write(data, length)) {
      return false;
    }
    return true;
  }

  bool WriteBits(uint32_t bits, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
      bit_buffer |= ((bits >> i) & 1u) << bit_count;
      ++bit_count;
      if (bit_count == 8) {
        if (!WriteByte(static_cast<unsigned char>(bit_buffer))) {
          return false;
        }
        bit_buffer = 0;
        bit_count = 0;
      }
    }
    return true;
  }

  bool FlushBits() {
    if (bit_count == 0) {
      return true;
    }
    if (!WriteByte(static_cast<unsigned char>(bit_buffer))) {
      return false;
    }
    bit_buffer = 0;
    bit_count = 0;
    return true;
  }

  bool WriteByte(unsigned char byte) {
    return file->Write(&byte, 1);
  }

  bool WriteUint16(uint16_t value) {
    unsigned char bytes[2];
    bytes[0] = static_cast<unsigned char>(value & 0xFF);
    bytes[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
    return file->Write(bytes, sizeof(bytes));
  }

  bool WriteUint32(uint32_t value) {
    unsigned char bytes[4];
    bytes[0] = static_cast<unsigned char>(value & 0xFF);
    bytes[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
    bytes[2] = static_cast<unsigned char>((value >> 16) & 0xFF);
    bytes[3] = static_cast<unsigned char>((value >> 24) & 0xFF);
    return file->Write(bytes, sizeof(bytes));
  }

  FileStream* file;
  uint32_t bit_buffer;
  uint32_t bit_count;
};

bool WriteGzipStream(FileStream& input, FileStream& output) {
  static constexpr unsigned char kGzipHeader[] = {
      0x1f, 0x8b, 0x08, 0x00,
      0x00, 0x00, 0x00, 0x00,
      0x02, 0x03};
  if (!output.Write(kGzipHeader, sizeof(kGzipHeader))) {
    return false;
  }

  DeflateWriter writer(&output);
  std::vector<unsigned char> buffer(DeflateWriter::kMaxStoredBlockSize);
  std::vector<unsigned char> pending;
  pending.reserve(DeflateWriter::kMaxStoredBlockSize);

  uint32_t crc = 0xFFFFFFFFu;
  uint64_t total_bytes = 0;

  while (true) {
    const size_t bytes_read = input.Read(buffer.data(), buffer.size());
    if (bytes_read == 0) {
      if (input.Error()) {
        return false;
      }
      if (!pending.empty()) {
        if (!writer.WriteStoredData(pending.data(), pending.size(), true)) {
          return false;
        }
        pending.clear();
      } else {
        if (!writer.WriteStoredData(nullptr, 0, true)) {
          return false;
        }
      }
      break;
    }

    crc = UpdateCrc32(crc, buffer.data(), bytes_read);
    total_bytes += bytes_read;

    if (!pending.empty()) {
      if (!writer.WriteStoredData(pending.data(), pending.size(), false)) {
        return false;
      }
      pending.clear();
    }
    pending.assign(buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(bytes_read));
  }

  if (!writer.Finish(crc, total_bytes)) {
    return false;
  }
  return true;
}

}  // namespace

bool packFilesToTar(const std::vector<std::wstring>& vecFiles,
                    const std::wstring& targetFile) {
  if (targetFile.empty()) {
    return false;
  }

  FileStream output;
  if (!output.OpenForWrite(targetFile)) {
    return false;
  }

  for (const auto& path : vecFiles) {
    if (path.empty()) {
      return false;
    }
    StatInfo info{};
    if (!GetFileInfo(path, &info) || !IsRegularFile(info)) {
      return false;
    }
    TarHeader header{};
    if (!BuildTarHeader(path, info, &header)) {
      return false;
    }
    if (!output.Write(&header, sizeof(header))) {
      return false;
    }
    if (!CopyFileToTar(path, static_cast<uint64_t>(info.st_size), output)) {
      return false;
    }
  }

  const char zero_block[kTarBlockSize] = {0};
  return output.Write(zero_block, sizeof(zero_block)) &&
         output.Write(zero_block, sizeof(zero_block)) &&
         output.Flush();
}

bool compressgzfile(const std::wstring& filePath,
                    const std::wstring& outfilePath) {
  if (filePath.empty() || outfilePath.empty()) {
    return false;
  }

  FileStream input;
  FileStream output;
  if (!input.OpenForRead(filePath) || !output.OpenForWrite(outfilePath)) {
    return false;
  }
  return WriteGzipStream(input, output);
}
