// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors

#include "ttl/TtlSyncCore.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ttl {
namespace {

constexpr size_t kMaxString = 2048;
constexpr int kMaxDepth = 8;
constexpr size_t kMaxFilename = 120;

bool isUnreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
         c == '_' || c == '~';
}

bool isTokenChar(char c) { return isUnreserved(c); }

std::string percentEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (isUnreserved(static_cast<char>(c))) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

void clipUtf8(std::string& value, size_t maxBytes) {
  if (value.size() <= maxBytes) return;
  size_t end = maxBytes;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) --end;
  value.resize(end);
}

std::string trim(const std::string& value) {
  size_t begin = 0;
  while (begin < value.size() &&
         (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\n' || value[begin] == '\r')) {
    ++begin;
  }
  size_t end = value.size();
  while (end > begin &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\n' || value[end - 1] == '\r')) {
    --end;
  }
  return value.substr(begin, end - begin);
}

bool hasInteriorWhitespace(const std::string& value) {
  for (char c : value) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return true;
  }
  return false;
}

bool startsWith(const std::string& value, const char* prefix) {
  const size_t n = std::strlen(prefix);
  return value.size() >= n && value.compare(0, n, prefix) == 0;
}

bool endsWithIgnoreCase(const std::string& value, const char* suffix) {
  const size_t n = std::strlen(suffix);
  if (value.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    char a = value[value.size() - n + i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

std::string jsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 2);
  for (unsigned char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04X", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

std::string oneLine(std::string value) {
  for (char& c : value) {
    if (c == '\n' || c == '\r') c = ' ';
  }
  return value;
}

bool isReservedStem(const std::string& stem) {
  std::string upper;
  upper.reserve(stem.size());
  for (char c : stem) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    upper.push_back(c);
  }
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") return true;
  if (upper.size() == 4 && (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0) && upper[3] >= '1' &&
      upper[3] <= '9') {
    return true;
  }
  return false;
}

struct Fields {
  std::string id;
  std::string title;
  std::string author;
  std::string filename;
  std::string updatedAt;
  std::string downloadUrl;
  std::string token;
  std::string errorCode;
  std::string message;
  bool sawId = false;
  bool hasFileSize = false;
  uint64_t fileSize = 0;
  bool sawOk = false;
  bool ok = false;
};

class Parser {
 public:
  explicit Parser(const std::string& in) : in_(in) {}

  bool parseManifest(std::vector<TtlBook>& books, std::string& error) {
    i_ = 0;
    if (in_.size() >= 3 && static_cast<unsigned char>(in_[0]) == 0xEF && static_cast<unsigned char>(in_[1]) == 0xBB &&
        static_cast<unsigned char>(in_[2]) == 0xBF) {
      i_ = 3;
    }
    if (!consume('[')) return fail(error, "Bad book list");
    skipWs();
    if (consume(']')) {
      skipWs();
      if (i_ != in_.size()) return fail(error, "Bad book list");
      return true;
    }
    while (true) {
      if (books.size() >= kMaxBooks) return fail(error, "Too many books");
      Fields fields;
      if (!parseObject(fields, error)) return false;
      if (!fields.sawId || fields.id.empty()) return fail(error, "Book missing id");
      TtlBook book;
      book.id = std::move(fields.id);
      book.title = std::move(fields.title);
      book.author = std::move(fields.author);
      book.filename = std::move(fields.filename);
      book.updatedAt = std::move(fields.updatedAt);
      book.downloadUrl = std::move(fields.downloadUrl);
      book.fileSize = fields.fileSize;
      book.hasFileSize = fields.hasFileSize;
      books.push_back(std::move(book));
      skipWs();
      if (consume(']')) break;
      if (!consume(',')) return fail(error, "Bad book list");
    }
    skipWs();
    if (i_ != in_.size()) return fail(error, "Bad book list");
    return true;
  }

  bool parseLoginObject(Fields& fields, std::string& error) {
    i_ = 0;
    if (!parseObject(fields, error)) return false;
    skipWs();
    if (i_ != in_.size()) return fail(error, "Bad sign-in response");
    return true;
  }

 private:
  const std::string& in_;
  size_t i_ = 0;
  int depth_ = 0;

  bool fail(std::string& error, const char* message) {
    error = message;
    return false;
  }

  void skipWs() {
    while (i_ < in_.size()) {
      const char c = in_[i_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
      ++i_;
    }
  }

  bool consume(char expected) {
    skipWs();
    if (i_ < in_.size() && in_[i_] == expected) {
      ++i_;
      return true;
    }
    return false;
  }

  bool parseObject(Fields& fields, std::string& error) {
    if (depth_ >= kMaxDepth) return fail(error, "Bad book list");
    if (!consume('{')) return fail(error, "Bad book list");
    ++depth_;
    skipWs();
    if (consume('}')) {
      --depth_;
      return true;
    }
    while (true) {
      std::string key;
      if (!parseString(key, error)) return false;
      if (!consume(':')) return fail(error, "Bad book list");
      if (key == "id" || key == "title" || key == "author" || key == "filename" || key == "updated_at" ||
          key == "download_url" || key == "token" || key == "error" || key == "message") {
        std::string value;
        if (!parseString(value, error)) return false;
        if (key == "id") {
          fields.id = std::move(value);
          fields.sawId = true;
        } else if (key == "title") {
          fields.title = std::move(value);
        } else if (key == "author") {
          fields.author = std::move(value);
        } else if (key == "filename") {
          fields.filename = std::move(value);
        } else if (key == "updated_at") {
          fields.updatedAt = std::move(value);
        } else if (key == "download_url") {
          fields.downloadUrl = std::move(value);
        } else if (key == "token") {
          fields.token = std::move(value);
        } else if (key == "error") {
          fields.errorCode = std::move(value);
        } else {
          fields.message = std::move(value);
        }
      } else if (key == "file_size") {
        if (!parseUint(fields.fileSize, error)) return false;
        fields.hasFileSize = true;
      } else if (key == "ok") {
        if (!parseBool(fields.ok, error)) return false;
        fields.sawOk = true;
      } else if (!skipValue(error)) {
        return false;
      }
      skipWs();
      if (consume('}')) break;
      if (!consume(',')) return fail(error, "Bad book list");
    }
    --depth_;
    return true;
  }

  bool parseString(std::string& out, std::string& error) {
    skipWs();
    if (i_ >= in_.size() || in_[i_] != '"') return fail(error, "Bad book list");
    ++i_;
    out.clear();
    while (i_ < in_.size()) {
      const unsigned char c = static_cast<unsigned char>(in_[i_++]);
      if (c == '"') {
        if (out.size() > kMaxString) return fail(error, "Field too long");
        return true;
      }
      if (c == '\\') {
        if (i_ >= in_.size()) return fail(error, "Bad book list");
        const char esc = in_[i_++];
        switch (esc) {
          case '"':
          case '\\':
          case '/':
            out.push_back(esc);
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u':
            if (!parseUnicodeEscape(out, error)) return false;
            break;
          default:
            return fail(error, "Bad book list");
        }
      } else if (c < 0x20) {
        return fail(error, "Bad book list");
      } else {
        out.push_back(static_cast<char>(c));
      }
      if (out.size() > kMaxString) return fail(error, "Field too long");
    }
    return fail(error, "Bad book list");
  }

  bool parseHex4(uint32_t& cp, std::string& error) {
    cp = 0;
    if (i_ + 4 > in_.size()) return fail(error, "Bad book list");
    for (int n = 0; n < 4; ++n) {
      const char c = in_[i_++];
      cp <<= 4;
      if (c >= '0' && c <= '9')
        cp += static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        cp += static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        cp += static_cast<uint32_t>(c - 'A' + 10);
      else
        return fail(error, "Bad book list");
    }
    return true;
  }

  bool parseUnicodeEscape(std::string& out, std::string& error) {
    uint32_t cp = 0;
    if (!parseHex4(cp, error)) return false;
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if (i_ + 2 <= in_.size() && in_[i_] == '\\' && in_[i_ + 1] == 'u') {
        i_ += 2;
        uint32_t low = 0;
        if (!parseHex4(low, error)) return false;
        if (low >= 0xDC00 && low <= 0xDFFF) {
          cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
        } else {
          cp = 0xFFFD;
        }
      } else {
        cp = 0xFFFD;
      }
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    appendUtf8(out, cp);
    return true;
  }

  bool parseUint(uint64_t& out, std::string& error) {
    skipWs();
    if (i_ >= in_.size() || in_[i_] < '0' || in_[i_] > '9') return fail(error, "Bad book list");
    uint64_t value = 0;
    while (i_ < in_.size() && in_[i_] >= '0' && in_[i_] <= '9') {
      const uint64_t digit = static_cast<uint64_t>(in_[i_] - '0');
      if (value > (UINT64_MAX - digit) / 10) return fail(error, "Bad book list");
      value = value * 10 + digit;
      ++i_;
    }
    if (i_ < in_.size()) {
      const char c = in_[i_];
      if (c == '.' || c == 'e' || c == 'E') return fail(error, "Bad book list");
    }
    out = value;
    return true;
  }

  bool parseBool(bool& out, std::string& error) {
    skipWs();
    if (in_.compare(i_, 4, "true") == 0) {
      i_ += 4;
      out = true;
      return true;
    }
    if (in_.compare(i_, 5, "false") == 0) {
      i_ += 5;
      out = false;
      return true;
    }
    return fail(error, "Bad book list");
  }

  bool skipValue(std::string& error) {
    skipWs();
    if (i_ >= in_.size()) return fail(error, "Bad book list");
    const char c = in_[i_];
    if (c == '"') {
      std::string ignored;
      return parseString(ignored, error);
    }
    if (c == '{') {
      if (depth_ >= kMaxDepth) return fail(error, "Bad book list");
      ++i_;
      ++depth_;
      skipWs();
      if (i_ < in_.size() && in_[i_] == '}') {
        ++i_;
        --depth_;
        return true;
      }
      while (true) {
        std::string ignored;
        if (!parseString(ignored, error)) return false;
        if (!consume(':')) return fail(error, "Bad book list");
        if (!skipValue(error)) return false;
        skipWs();
        if (consume('}')) break;
        if (!consume(',')) return fail(error, "Bad book list");
      }
      --depth_;
      return true;
    }
    if (c == '[') {
      if (depth_ >= kMaxDepth) return fail(error, "Bad book list");
      ++i_;
      ++depth_;
      skipWs();
      if (i_ < in_.size() && in_[i_] == ']') {
        ++i_;
        --depth_;
        return true;
      }
      while (true) {
        if (!skipValue(error)) return false;
        skipWs();
        if (consume(']')) break;
        if (!consume(',')) return fail(error, "Bad book list");
      }
      --depth_;
      return true;
    }
    if (c == 't' && in_.compare(i_, 4, "true") == 0) {
      i_ += 4;
      return true;
    }
    if (c == 'f' && in_.compare(i_, 5, "false") == 0) {
      i_ += 5;
      return true;
    }
    if (c == 'n' && in_.compare(i_, 4, "null") == 0) {
      i_ += 4;
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      if (c == '-') ++i_;
      if (i_ >= in_.size() || in_[i_] < '0' || in_[i_] > '9') return fail(error, "Bad book list");
      while (i_ < in_.size()) {
        const char n = in_[i_];
        if ((n >= '0' && n <= '9') || n == '.' || n == 'e' || n == 'E' || n == '+' || n == '-') {
          ++i_;
          continue;
        }
        break;
      }
      return true;
    }
    return fail(error, "Bad book list");
  }
};

bool acceptableHttpUrl(const std::string& url, std::string& error) {
  if (!(startsWith(url, "https://") || startsWith(url, "http://"))) {
    error = "Bad URL";
    return false;
  }
  if (url.find('?') != std::string::npos || url.find('#') != std::string::npos || hasInteriorWhitespace(url)) {
    error = "Bad URL";
    return false;
  }
  const size_t scheme = url.find("://");
  const size_t host = scheme + 3;
  const size_t slash = url.find('/', host);
  const size_t hostEnd = slash == std::string::npos ? url.size() : slash;
  if (url.find('@', host) != std::string::npos && url.find('@', host) < hostEnd) {
    error = "Bad URL";
    return false;
  }
  if (hostEnd == host) {
    error = "Bad URL";
    return false;
  }
  return true;
}

}  // namespace

bool ttlNormalizeServerUrl(const std::string& input, std::string& out, std::string& error) {
  out = trim(input);
  if (out.empty()) {
    out = kDefaultServerUrl;
    error.clear();
    return true;
  }
  if (out.size() > kMaxServerUrl || !acceptableHttpUrl(out, error)) {
    if (out.size() > kMaxServerUrl) error = "Bad URL";
    out.clear();
    return false;
  }
  const auto api = out.find("/api/sync");
  if (api != std::string::npos) out.erase(api);
  while (!out.empty() && out.back() == '/') out.pop_back();
  if (!acceptableHttpUrl(out, error)) {
    out.clear();
    return false;
  }
  error.clear();
  return true;
}

bool ttlNormalizeEmail(const std::string& input, std::string& out, std::string& error) {
  out = trim(input);
  if (out.empty()) {
    error.clear();
    return true;
  }
  if (out.size() > kMaxEmail || out.find('@') == std::string::npos || hasInteriorWhitespace(out)) {
    error = "Bad email";
    out.clear();
    return false;
  }
  error.clear();
  return true;
}

std::string ttlLoginUrl(const std::string& server) { return server + "/api/sync/login"; }

std::string ttlManifestUrl(const std::string& server) { return server + "/api/sync/manifest"; }

bool ttlBookDownloadUrl(const std::string& server, const TtlBook& book, std::string& out, std::string& error) {
  if (!book.downloadUrl.empty()) {
    if (!acceptableHttpUrl(book.downloadUrl, error)) {
      out.clear();
      return false;
    }
    out = book.downloadUrl;
    error.clear();
    return true;
  }
  if (book.id.empty()) {
    error = "Book missing id";
    out.clear();
    return false;
  }
  out = server + "/api/sync/download/" + percentEncode(book.id);
  error.clear();
  return true;
}

std::string ttlBuildLoginJson(const std::string& email, const std::string& password) {
  std::string safeEmail = email.size() > kMaxEmail ? email.substr(0, kMaxEmail) : email;
  std::string safePassword = password.size() > kMaxPassword ? password.substr(0, kMaxPassword) : password;
  return std::string("{\"email\":\"") + jsonEscape(safeEmail) + "\",\"password\":\"" + jsonEscape(safePassword) + "\"}";
}

bool ttlParseManifest(const std::string& json, std::vector<TtlBook>& books, std::string& error) {
  books.clear();
  if (json.size() > kMaxManifestBytes) {
    error = "Book list too large";
    return false;
  }
  Parser parser(json);
  return parser.parseManifest(books, error);
}

TtlLoginResponse ttlParseLoginResponse(int httpStatus, const std::string& body) {
  TtlLoginResponse result;
  Fields fields;
  std::string error;
  Parser parser(body);
  const bool parsed = !body.empty() && parser.parseLoginObject(fields, error);
  if (parsed) {
    result.token = fields.token;
    result.errorCode = fields.errorCode;
    result.message = fields.message;
  }
  bool tokenOk = !result.token.empty() && result.token.size() >= 20 && result.token.size() <= kMaxToken;
  if (tokenOk) {
    for (char c : result.token) {
      if (!isTokenChar(c)) tokenOk = false;
    }
  }
  result.ok = httpStatus >= 200 && httpStatus < 300 && tokenOk;
  if (!result.ok) result.token.clear();
  return result;
}

std::string ttlSyncErrorText(int httpStatus, const std::string& errorCode, const std::string& message) {
  if (httpStatus == 0) return "Network error";
  if (errorCode == "invalid_login" || (httpStatus == 401 && errorCode.empty())) return "Wrong email or password";
  if (errorCode == "unconfirmed" || httpStatus == 403) return "Confirm email first";
  if (errorCode == "unauthorized") return "Sign in again";
  if (errorCode == "not_found" || httpStatus == 404) return "Book missing";
  const std::string line = oneLine(message);
  if (!line.empty() && line.size() <= 48) return line;
  return "Sync failed";
}

TtlBookAction ttlPlanDownload(bool exists, uint64_t existingSize, bool hasFileSize, uint64_t fileSize) {
  if (exists && hasFileSize && existingSize == fileSize) return TtlBookAction::Skip;
  return TtlBookAction::Download;
}

std::string ttlSanitizeFilename(const std::string& filename, const std::string& idFallback) {
  auto clean = [](std::string name) {
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    for (char& c : name) {
      const unsigned char u = static_cast<unsigned char>(c);
      if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' ||
          u < 0x20 || u == 0x7F) {
        c = '_';
      }
    }
    size_t begin = 0;
    while (begin < name.size() && (name[begin] == '.' || name[begin] == ' ')) ++begin;
    size_t end = name.size();
    while (end > begin && (name[end - 1] == '.' || name[end - 1] == ' ')) --end;
    return name.substr(begin, end - begin);
  };

  std::string base = clean(filename);
  if (base.empty()) base = clean(idFallback);
  if (base.empty()) base = "book";
  if (endsWithIgnoreCase(base, ".epub")) {
    base.erase(base.size() - 5);
  }
  if (base.empty()) base = "book";
  if (isReservedStem(base)) base.insert(base.begin(), '_');
  clipUtf8(base, kMaxFilename - 5);
  if (base.empty()) base = "book";
  return base + ".epub";
}

std::string ttlBookLabel(const TtlBook& book) {
  if (!book.title.empty()) return oneLine(book.title);
  if (!book.filename.empty()) return oneLine(book.filename);
  if (!book.id.empty()) return oneLine(book.id);
  return "book";
}

std::string ttlSyncSummary(int saved, int skipped, int failed) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "Saved %d, skipped %d, failed %d", saved, skipped, failed);
  return buf;
}

std::string ttlFormatLogLine(const std::string& stamp, const std::string& message) {
  return stamp + " " + oneLine(message);
}

}  // namespace ttl
