#include "stdafx.h"
#include <RimeWithWeasel.h>
#include <StringAlgorithm.hpp>
#include <WeaselConstants.h>
#include <WeaselStyleColor.h>
#include <WeaselUtility.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <system_error>
#include <utility>
#include <vector>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <rime_api.h>

#define TRANSPARENT_COLOR 0x00000000

using namespace weasel;

static RimeApi* rime_api;
WeaselSessionId _GenerateNewWeaselSessionId(SessionStatusMap sm, DWORD pid) {
  if (sm.empty())
    return (WeaselSessionId)(pid + 1);
  return (WeaselSessionId)(sm.rbegin()->first + 1);
}

int expand_ibus_modifier(int m) {
  return (m & 0xff) | ((m & 0xff00) << 16);
}

namespace {

constexpr size_t kClipboardHistoryLimit = 100;
constexpr size_t kClipboardVisibleLimit = 8;
constexpr size_t kClipboardDisplayLength = 56;
constexpr size_t kClipboardMaxTextLength = 256 * 1024;
constexpr uint32_t kClipboardMaxStoredEntry = 4u << 20;
constexpr size_t kClipboardMaxTotalChars = 32u << 20;
constexpr size_t kClipboardMaxFiles = 100;
constexpr size_t kClipboardMaxFileListChars = 32 * 1024;
constexpr size_t kClipboardMaxImages = 10;
constexpr size_t kClipboardMaxImageDimension = 4096;
constexpr size_t kClipboardMaxImageSourceBytes = 64 * 1024 * 1024;
constexpr size_t kClipboardMaxImageFileBytes = 24 * 1024 * 1024;
const wchar_t kClipboardWindowClass[] = L"WeaselClipboardMonitor";
// File entries are stored in the same string history as text. The control
// characters cannot appear in Windows paths, so they keep the encoding
// unambiguous without changing the on-disk format.
const wchar_t kFileEntryPrefix[] = L"\x01FILES\x01";
const wchar_t kImageEntryPrefix[] = L"\x01IMAGE\x01";
const wchar_t kFileEntrySeparator = L'\x1F';

// Same layout as DROPFILES from shellapi.h. Defining it locally keeps the
// clipboard code independent of shell header include ordering.
struct ClipboardDropFiles {
  DWORD pFiles;
  POINT pt;
  BOOL fNC;
  BOOL fWide;
};

std::wstring ClipboardHistoryPath() {
  return (WeaselUserDataPath() / L"weasel-clipboard-history.dat").wstring();
}

std::wstring ClipboardFavoritesPath() {
  return (WeaselUserDataPath() / L"weasel-clipboard-favorites.dat").wstring();
}

std::wstring SanitizeClipboardText(const std::wstring& text) {
  std::wstring result;
  result.reserve((std::min)(text.size(), kClipboardDisplayLength + 1));

  for (wchar_t c : text) {
    if (result.size() >= kClipboardDisplayLength) {
      result += L"…";
      break;
    }
    if (c == L'\r' || c == L'\n' || c == L'\t')
      result.push_back(L' ');
    else
      result.push_back(c);
  }

  if (result.empty())
    result = L"(空)";
  return result;
}

bool IsFileEntry(const std::wstring& entry) {
  return entry.rfind(kFileEntryPrefix, 0) == 0;
}

std::wstring EncodeFileEntry(const std::vector<std::wstring>& files) {
  std::wstring result = kFileEntryPrefix;
  for (size_t i = 0; i < files.size(); ++i) {
    if (i > 0)
      result.push_back(kFileEntrySeparator);
    result += files[i];
  }
  return result;
}

std::vector<std::wstring> DecodeFileEntry(const std::wstring& entry) {
  std::vector<std::wstring> files;
  size_t start = std::wcslen(kFileEntryPrefix);
  while (start < entry.size()) {
    size_t end = entry.find(kFileEntrySeparator, start);
    if (end == std::wstring::npos)
      end = entry.size();
    if (end > start)
      files.emplace_back(entry, start, end - start);
    start = end + 1;
  }
  return files;
}

struct ImageEntry {
  std::wstring path;
  int width = 0;
  int height = 0;
};

bool IsImageEntry(const std::wstring& entry) {
  return entry.rfind(kImageEntryPrefix, 0) == 0;
}

std::wstring EncodeImageEntry(const ImageEntry& image) {
  return std::wstring(kImageEntryPrefix) + image.path + kFileEntrySeparator +
         std::to_wstring(image.width) + kFileEntrySeparator +
         std::to_wstring(image.height);
}

bool DecodeImageEntry(const std::wstring& entry, ImageEntry* image) {
  if (!image || !IsImageEntry(entry))
    return false;

  std::vector<std::wstring> parts;
  size_t start = std::wcslen(kImageEntryPrefix);
  while (start <= entry.size()) {
    size_t end = entry.find(kFileEntrySeparator, start);
    if (end == std::wstring::npos)
      end = entry.size();
    parts.emplace_back(entry, start, end - start);
    if (end == entry.size())
      break;
    start = end + 1;
  }

  if (parts.size() != 3 || parts[0].empty())
    return false;

  try {
    image->path = parts[0];
    image->width = std::stoi(parts[1]);
    image->height = std::stoi(parts[2]);
  } catch (...) {
    return false;
  }
  return image->width > 0 && image->height > 0;
}

std::wstring ClipboardImageDirectory() {
  return (WeaselUserDataPath() / L"weasel-clipboard-images").wstring();
}

bool EnsureClipboardImageDirectory() {
  const std::wstring directory = ClipboardImageDirectory();
  if (directory.empty())
    return false;
  if (CreateDirectoryW(directory.c_str(), nullptr))
    return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring MakeClipboardImagePath() {
  static std::atomic<unsigned long long> counter{0};
  const unsigned long long id = GetTickCount64();
  const std::wstring filename =
      std::to_wstring(id) + L"-" + std::to_wstring(++counter) + L".png";
  return (std::filesystem::path(ClipboardImageDirectory()) / filename)
      .wstring();
}

bool GetPngEncoderClsid(CLSID* clsid) {
  UINT count = 0;
  UINT size = 0;
  if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok ||
      size == 0) {
    return false;
  }

  std::vector<BYTE> buffer(size);
  auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
  if (Gdiplus::GetImageEncoders(count, size, codecs) != Gdiplus::Ok)
    return false;

  for (UINT i = 0; i < count; ++i) {
    if (codecs[i].MimeType &&
        std::wcscmp(codecs[i].MimeType, L"image/png") == 0) {
      *clsid = codecs[i].Clsid;
      return true;
    }
  }
  return false;
}

std::wstring ClipboardEntryDisplay(const std::wstring& entry) {
  if (IsImageEntry(entry)) {
    ImageEntry image;
    if (!DecodeImageEntry(entry, &image))
      return L"[图片]";
    return L"[图片 " + std::to_wstring(image.width) + L"×" +
           std::to_wstring(image.height) + L"]";
  }

  if (!IsFileEntry(entry))
    return SanitizeClipboardText(entry);

  const std::vector<std::wstring> files = DecodeFileEntry(entry);
  if (files.empty())
    return L"[文件] (空)";

  std::wstring name = files.front();
  try {
    std::wstring filename =
        std::filesystem::path(files.front()).filename().wstring();
    if (!filename.empty())
      name = std::move(filename);
  } catch (...) {
  }

  std::wstring display = L"[文件] " + name;
  if (files.size() > 1) {
    display += L" 等 " +
               std::to_wstring(static_cast<unsigned long long>(files.size())) +
               L" 个文件";
  }
  return SanitizeClipboardText(display);
}

bool ContainsCaseInsensitive(const std::wstring& haystack,
                             const std::wstring& needle) {
  if (needle.empty())
    return true;

  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(),
                        needle.end(), [](wchar_t a, wchar_t b) {
                          return std::towlower(a) == std::towlower(b);
                        });
  return it != haystack.end();
}

std::wstring ReadClipboardText(HWND owner) {
  std::wstring result;
  if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
    return result;

  if (!OpenClipboard(owner))
    return result;

  HANDLE data = GetClipboardData(CF_UNICODETEXT);
  if (data) {
    auto text = static_cast<const wchar_t*>(GlobalLock(data));
    if (text) {
      result = text;
      GlobalUnlock(data);
    }
  }

  CloseClipboard();
  return result;
}

std::vector<std::wstring> ReadClipboardFiles(HWND owner) {
  std::vector<std::wstring> files;
  if (!IsClipboardFormatAvailable(CF_HDROP))
    return files;
  if (!OpenClipboard(owner))
    return files;

  HANDLE data = GetClipboardData(CF_HDROP);
  if (data) {
    const SIZE_T size = GlobalSize(data);
    const auto* base = static_cast<const BYTE*>(GlobalLock(data));
    if (base && size >= sizeof(ClipboardDropFiles)) {
      const auto* drop = reinterpret_cast<const ClipboardDropFiles*>(base);
      if (drop->pFiles >= sizeof(ClipboardDropFiles) && drop->pFiles < size) {
        const BYTE* p = base + drop->pFiles;
        const BYTE* end = base + size;
        size_t total_chars = 0;

        if (drop->fWide) {
          std::wstring current;
          while (p + sizeof(wchar_t) <= end) {
            wchar_t c = L'\0';
            memcpy(&c, p, sizeof(c));
            p += sizeof(wchar_t);
            if (c == L'\0') {
              if (current.empty())
                break;
              total_chars += current.size();
              if (total_chars > kClipboardMaxFileListChars)
                break;
              files.push_back(current);
              current.clear();
              if (files.size() >= kClipboardMaxFiles)
                break;
            } else {
              current.push_back(c);
            }
          }
        } else {
          std::string current;
          while (p < end) {
            const char c = static_cast<char>(*p++);
            if (c == '\0') {
              if (current.empty())
                break;
              const int wlen = MultiByteToWideChar(
                  CP_ACP, 0, current.data(), static_cast<int>(current.size()),
                  nullptr, 0);
              if (wlen > 0) {
                std::wstring wide(wlen, L'\0');
                MultiByteToWideChar(CP_ACP, 0, current.data(),
                                    static_cast<int>(current.size()),
                                    wide.data(), wlen);
                total_chars += wide.size();
                if (total_chars > kClipboardMaxFileListChars)
                  break;
                files.push_back(std::move(wide));
              }
              current.clear();
              if (files.size() >= kClipboardMaxFiles)
                break;
            } else {
              current.push_back(c);
            }
          }
        }
      }
    }
    if (base)
      GlobalUnlock(data);
  }

  CloseClipboard();
  return files;
}

bool ClipboardContentExcluded(HWND owner) {
  if (!OpenClipboard(owner))
    return false;

  bool excluded = false;
  UINT exclude_format =
      RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
  if (exclude_format && IsClipboardFormatAvailable(exclude_format)) {
    excluded = true;
  } else {
    UINT include_format =
        RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (include_format && IsClipboardFormatAvailable(include_format)) {
      HANDLE data = GetClipboardData(include_format);
      if (data && GlobalSize(data) >= sizeof(DWORD)) {
        const DWORD* value = static_cast<const DWORD*>(GlobalLock(data));
        if (value) {
          excluded = (*value == 0);
          GlobalUnlock(data);
        }
      }
    }
  }

  CloseClipboard();
  return excluded;
}

bool CommitTempFile(const std::wstring& temp_path,
                    const std::wstring& final_path) {
  if (MoveFileExW(temp_path.c_str(), final_path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  DeleteFileW(temp_path.c_str());
  return false;
}

template <typename Container>
bool SaveClipboardEntries(const std::wstring& path, const Container& entries) {
  const std::wstring temp_path = path + L".tmp";
  FILE* file = nullptr;
  if (_wfopen_s(&file, temp_path.c_str(), L"wb") != 0 || !file)
    return false;

  bool write_ok = true;
  uint32_t count = static_cast<uint32_t>(entries.size());
  if (fwrite(&count, sizeof(count), 1, file) != 1)
    write_ok = false;

  for (const auto& text : entries) {
    if (!write_ok)
      break;
    uint32_t length = static_cast<uint32_t>(text.size());
    if (fwrite(&length, sizeof(length), 1, file) != 1) {
      write_ok = false;
      break;
    }
    if (length > 0 &&
        fwrite(text.data(), sizeof(wchar_t), length, file) != length) {
      write_ok = false;
    }
  }

  write_ok = write_ok && !ferror(file);
  const bool close_ok = fclose(file) == 0;
  if (!write_ok || !close_ok) {
    DeleteFileW(temp_path.c_str());
    return false;
  }
  return CommitTempFile(temp_path, path);
}

bool EqualsIgnoreCase(const std::string& text, const char* expected) {
  size_t i = 0;
  for (; i < text.size() && expected[i] != '\0'; ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) !=
        std::tolower(static_cast<unsigned char>(expected[i])))
      return false;
  }
  return i == text.size() && expected[i] == '\0';
}

std::string TrimAscii(const std::string& text) {
  const size_t begin = text.find_first_not_of(" \t");
  if (begin == std::string::npos)
    return std::string();
  const size_t end = text.find_last_not_of(" \t");
  return text.substr(begin, end - begin + 1);
}

bool ParseClipboardHotkey(const std::string& text,
                          UINT* modifiers,
                          int* keycode) {
  if (!modifiers || !keycode)
    return false;

  UINT mods = 0;
  int key = 0;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('+', start);
    const std::string token = TrimAscii(text.substr(
        start, end == std::string::npos ? std::string::npos : end - start));
    if (token.empty())
      return false;

    if (EqualsIgnoreCase(token, "control") || EqualsIgnoreCase(token, "ctrl")) {
      mods |= ibus::Modifier::CONTROL_MASK;
    } else if (EqualsIgnoreCase(token, "shift")) {
      mods |= ibus::Modifier::SHIFT_MASK;
    } else if (EqualsIgnoreCase(token, "alt")) {
      mods |= ibus::Modifier::ALT_MASK;
    } else if (key == 0 && token.size() == 1 &&
               std::isalnum(static_cast<unsigned char>(token[0]))) {
      key = std::toupper(static_cast<unsigned char>(token[0]));
    } else if (key == 0 && EqualsIgnoreCase(token, "space")) {
      key = 0x20;
    } else if (key == 0 && EqualsIgnoreCase(token, "tab")) {
      key = ibus::Tab;
    } else if (key == 0 && EqualsIgnoreCase(token, "enter")) {
      key = ibus::Return;
    } else {
      return false;
    }

    if (end == std::string::npos)
      break;
    start = end + 1;
  }

  if (key == 0 || mods == 0)
    return false;

  *modifiers = mods;
  *keycode = key;
  return true;
}

bool WriteClipboardText(HWND owner, const std::wstring& text) {
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!data)
    return false;

  void* buffer = GlobalLock(data);
  if (!buffer) {
    GlobalFree(data);
    return false;
  }
  memcpy(buffer, text.c_str(), bytes);
  GlobalUnlock(data);

  if (!OpenClipboard(owner)) {
    GlobalFree(data);
    return false;
  }

  bool ok = false;
  EmptyClipboard();
  if (SetClipboardData(CF_UNICODETEXT, data)) {
    data = nullptr;
    ok = true;
  }
  if (data)
    GlobalFree(data);

  CloseClipboard();
  return ok;
}

bool WriteClipboardFiles(HWND owner, const std::vector<std::wstring>& files) {
  if (files.empty())
    return false;

  size_t chars = 1;
  for (const auto& file : files)
    chars += file.size() + 1;

  const size_t bytes = sizeof(ClipboardDropFiles) + chars * sizeof(wchar_t);
  HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (!data)
    return false;

  auto* drop = static_cast<ClipboardDropFiles*>(GlobalLock(data));
  if (!drop) {
    GlobalFree(data);
    return false;
  }

  drop->pFiles = sizeof(ClipboardDropFiles);
  drop->fWide = TRUE;
  auto* p = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop) +
                                       sizeof(ClipboardDropFiles));
  for (const auto& file : files) {
    memcpy(p, file.c_str(), (file.size() + 1) * sizeof(wchar_t));
    p += file.size() + 1;
  }
  *p = L'\0';
  GlobalUnlock(data);

  if (!OpenClipboard(owner)) {
    GlobalFree(data);
    return false;
  }

  bool ok = false;
  EmptyClipboard();
  if (SetClipboardData(CF_HDROP, data)) {
    data = nullptr;
    ok = true;
  }
  if (data)
    GlobalFree(data);

  CloseClipboard();
  return ok;
}

}  // namespace

RimeWithWeaselHandler::RimeWithWeaselHandler(UI* ui)
    : m_ui(ui),
      m_active_session(0),
      m_disabled(true),
      m_current_dark_mode(false),
      m_global_ascii_mode(false),
      m_show_notifications_time(1200),
      _UpdateUICallback(NULL) {
  LOG(INFO) << "constructing RimeWithWeaselHandler";
  Gdiplus::GdiplusStartupInput gdiplus_input;
  if (Gdiplus::GdiplusStartup(&m_gdiplus_token, &gdiplus_input, nullptr) !=
      Gdiplus::Ok) {
    m_gdiplus_token = 0;
  }
  m_ui->InServer() = true;
  rime_api = rime_get_api();
  assert(rime_api);
  m_pid = GetCurrentProcessId();
  uint16_t msbit = 0;
  for (auto i = 31; i >= 0; i--) {
    if (m_pid & (1 << i)) {
      msbit = i;
      break;
    }
  }
  m_pid = (m_pid << (31 - msbit));
  LOG(INFO) << "setting up Rime traits";
  _Setup();
  LOG(INFO) << "RimeWithWeaselHandler constructed";
}

RimeWithWeaselHandler::~RimeWithWeaselHandler() {
  m_show_notifications.clear();
  m_session_status_map.clear();
  m_app_options.clear();
  if (m_gdiplus_token) {
    Gdiplus::GdiplusShutdown(m_gdiplus_token);
    m_gdiplus_token = 0;
  }
}

bool add_session = false;
void _UpdateUIStyle(RimeConfig* config, UI* ui, bool initialize);
bool _UpdateUIStyleColor(RimeConfig* config,
                         UIStyle& style,
                         const std::string& color = std::string());
void _LoadAppOptions(RimeConfig* config, AppOptionsByAppName& app_options);

void _RefreshTrayIcon(const RimeSessionId session_id,
                      const std::function<void()> _UpdateUICallback) {
  // Dangerous, don't touch
  static char app_name[256] = {0};
  auto ret = rime_api->get_property(session_id, "client_app", app_name,
                                    sizeof(app_name) - 1);
  if (!ret || u8tow(app_name) == std::wstring(L"explorer.exe"))
    boost::thread th([=]() {
      ::Sleep(100);
      if (_UpdateUICallback)
        _UpdateUICallback();
    });
  else if (_UpdateUICallback)
    _UpdateUICallback();
}

void RimeWithWeaselHandler::_Setup() {
  RIME_STRUCT(RimeTraits, weasel_traits);
  std::string shared_dir = wtou8(WeaselSharedDataPath().wstring());
  std::string user_dir = wtou8(WeaselUserDataPath().wstring());
  weasel_traits.shared_data_dir = shared_dir.c_str();
  weasel_traits.user_data_dir = user_dir.c_str();
  weasel_traits.prebuilt_data_dir = weasel_traits.shared_data_dir;
  std::string distribution_name = wtou8(get_weasel_ime_name());
  weasel_traits.distribution_name = distribution_name.c_str();
  weasel_traits.distribution_code_name = WEASEL_CODE_NAME;
  weasel_traits.distribution_version = WEASEL_PRODUCT_VERSION;
  weasel_traits.app_name = "rime.weasel";
  std::string log_dir = WeaselLogPath().u8string();
  weasel_traits.log_dir = log_dir.c_str();
  rime_api->setup(&weasel_traits);
  rime_api->set_notification_handler(&RimeWithWeaselHandler::OnNotify, this);
}

void RimeWithWeaselHandler::Initialize() {
  m_disabled = _IsDeployerRunning();
  if (m_disabled) {
    return;
  }

  LOG(INFO) << "Initializing la rime.";
  LOG(INFO) << "calling rime_api->initialize";
  rime_api->initialize(NULL);
  LOG(INFO) << "starting Rime maintenance";
  if (rime_api->start_maintenance(/*full_check = */ False)) {
    m_disabled = true;
    LOG(INFO) << "waiting for Rime maintenance";
    rime_api->join_maintenance_thread();
  }

  _LoadClipboardSettings(nullptr);

  RimeConfig config = {NULL};
  if (rime_api->config_open("weasel", &config)) {
    if (m_ui) {
      _UpdateUIStyle(&config, m_ui, true);
      _UpdateShowNotifications(&config, true);
      m_current_dark_mode = IsUserDarkMode();
      if (m_current_dark_mode) {
        const int BUF_SIZE = 255;
        char buffer[BUF_SIZE + 1] = {0};
        if (rime_api->config_get_string(&config, "style/color_scheme_dark",
                                        buffer, BUF_SIZE)) {
          std::string color_name(buffer);
          _UpdateUIStyleColor(&config, m_ui->style(), color_name);
        }
      }
      m_base_style = m_ui->style();
      // Warm up UI resources with finalized style to avoid first-show latency.
      m_ui->Refresh();
      m_ui->Hide();
    }
    Bool global_ascii = false;
    if (rime_api->config_get_bool(&config, "global_ascii", &global_ascii))
      m_global_ascii_mode = !!global_ascii;
    if (!rime_api->config_get_int(&config, "show_notifications_time",
                                  &m_show_notifications_time))
      m_show_notifications_time = 1200;
    _LoadAppOptions(&config, m_app_options);
    _LoadClipboardSettings(&config);
    rime_api->config_close(&config);
  }

  if (!m_clipboard_enabled)
    _TeardownClipboardMonitor();

  _LoadClipboardHistory();
  _LoadClipboardFavorites();
  _CleanupClipboardImages();
  _SetupClipboardMonitor();
  m_last_schema_id.clear();
}

void RimeWithWeaselHandler::Finalize() {
  _HideClipboardPanel();
  _SaveClipboardHistory();
  _SaveClipboardFavorites();
  _CleanupClipboardImages();
  _TeardownClipboardMonitor();
  m_active_session = 0;
  m_disabled = true;
  m_session_status_map.clear();
  LOG(INFO) << "Finalizing la rime.";
  rime_api->finalize();
}

DWORD RimeWithWeaselHandler::FindSession(WeaselSessionId ipc_id) {
  if (m_disabled)
    return 0;
  Bool found = rime_api->find_session(to_session_id(ipc_id));
  DLOG(INFO) << "Find session: session_id = " << to_session_id(ipc_id)
             << ", found = " << found;
  return found ? (ipc_id) : 0;
}

DWORD RimeWithWeaselHandler::AddSession(LPWSTR buffer, EatLine eat) {
  if (m_disabled) {
    DLOG(INFO) << "Trying to resume service.";
    EndMaintenance();
    if (m_disabled)
      return 0;
  }
  RimeSessionId session_id = (RimeSessionId)rime_api->create_session();
  if (m_global_ascii_mode) {
    for (const auto& pair : m_session_status_map) {
      if (pair.first) {
        rime_api->set_option(session_id, "ascii_mode",
                             !!pair.second.status.is_ascii_mode);
        break;
      }
    }
  }

  WeaselSessionId ipc_id =
      _GenerateNewWeaselSessionId(m_session_status_map, m_pid);
  DLOG(INFO) << "Add session: created session_id = " << session_id
             << ", ipc_id = " << ipc_id;
  SessionStatus& session_status = new_session_status(ipc_id);
  session_status.style = m_base_style;
  session_status.session_id = session_id;
  _ReadClientInfo(ipc_id, buffer);

  RIME_STRUCT(RimeStatus, status);
  if (rime_api->get_status(session_id, &status)) {
    std::string schema_id = status.schema_id;
    m_last_schema_id = schema_id;
    _LoadSchemaSpecificSettings(ipc_id, schema_id);
    _LoadAppInlinePreeditSet(ipc_id, true);
    _UpdateInlinePreeditStatus(ipc_id);
    _RefreshTrayIcon(session_id, _UpdateUICallback);
    session_status.status = status;
    session_status.__synced = false;
    rime_api->free_status(&status);
  }
  m_ui->style() = session_status.style;
  // show session's welcome message :-) if any
  if (eat) {
    _Respond(ipc_id, eat);
  }
  add_session = true;
  _UpdateUI(ipc_id);
  add_session = false;
  m_active_session = ipc_id;
  return ipc_id;
}

DWORD RimeWithWeaselHandler::RemoveSession(WeaselSessionId ipc_id) {
  if (m_ui)
    m_ui->Hide();
  if (m_disabled)
    return 0;
  DLOG(INFO) << "Remove session: session_id = " << to_session_id(ipc_id);
  // TODO: force committing? otherwise current composition would be lost
  rime_api->destroy_session(to_session_id(ipc_id));
  m_session_status_map.erase(ipc_id);
  m_active_session = 0;
  return 0;
}

void RimeWithWeaselHandler::UpdateColorTheme(BOOL darkMode) {
  RimeConfig config = {NULL};
  if (rime_api->config_open("weasel", &config)) {
    if (m_ui) {
      _UpdateUIStyle(&config, m_ui, true);
      m_current_dark_mode = darkMode;
      if (darkMode) {
        const int BUF_SIZE = 255;
        char buffer[BUF_SIZE + 1] = {0};
        if (rime_api->config_get_string(&config, "style/color_scheme_dark",
                                        buffer, BUF_SIZE)) {
          std::string color_name(buffer);
          _UpdateUIStyleColor(&config, m_ui->style(), color_name);
        }
      }
      m_base_style = m_ui->style();
    }
    rime_api->config_close(&config);
  }

  for (auto& pair : m_session_status_map) {
    RIME_STRUCT(RimeStatus, status);
    if (rime_api->get_status(to_session_id(pair.first), &status)) {
      _LoadSchemaSpecificSettings(pair.first, std::string(status.schema_id));
      _LoadAppInlinePreeditSet(pair.first, true);
      _UpdateInlinePreeditStatus(pair.first);
      pair.second.status = status;
      pair.second.__synced = false;
      rime_api->free_status(&status);
    }
  }
  m_ui->style() = get_session_status(m_active_session).style;
}

BOOL RimeWithWeaselHandler::ProcessKeyEvent(KeyEvent keyEvent,
                                            WeaselSessionId ipc_id,
                                            EatLine eat) {
  DLOG(INFO) << "Process key event: keycode = " << keyEvent.keycode
             << ", mask = " << keyEvent.mask << ", ipc_id = " << ipc_id;
  if ((keyEvent.mask & ibus::Modifier::RELEASE_MASK) &&
      _IsClipboardHotkeyKey(keyEvent.keycode)) {
    m_clipboard_hotkey_down = false;
  }
  if (m_disabled)
    return FALSE;

  if (m_clipboard_mode)
    return _HandleClipboardKey(keyEvent, ipc_id) ? TRUE : FALSE;

  if (_IsClipboardHotkey(keyEvent)) {
    if (!m_clipboard_hotkey_down) {
      m_clipboard_hotkey_down = true;
      _ShowClipboardPanel(ipc_id);
    }
    return TRUE;
  }

  RimeSessionId session_id = to_session_id(ipc_id);
  Bool handled = rime_api->process_key(session_id, keyEvent.keycode,
                                       expand_ibus_modifier(keyEvent.mask));
  // vim_mode when keydown only
  if (!handled && !(keyEvent.mask & ibus::Modifier::RELEASE_MASK)) {
    bool isVimBackInCommandMode =
        (keyEvent.keycode == ibus::Keycode::Escape) ||
        ((keyEvent.mask & (1 << 2)) &&
         (keyEvent.keycode == ibus::Keycode::XK_c ||
          keyEvent.keycode == ibus::Keycode::XK_C ||
          keyEvent.keycode == ibus::Keycode::XK_bracketleft));
    if (isVimBackInCommandMode &&
        rime_api->get_option(session_id, "vim_mode") &&
        !rime_api->get_option(session_id, "ascii_mode")) {
      rime_api->set_option(session_id, "ascii_mode", True);
    }
  }
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
  return (BOOL)handled;
}

void RimeWithWeaselHandler::CommitComposition(WeaselSessionId ipc_id) {
  DLOG(INFO) << "Commit composition: ipc_id = " << ipc_id;
  if (m_disabled)
    return;
  rime_api->commit_composition(to_session_id(ipc_id));
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
}

void RimeWithWeaselHandler::ClearComposition(WeaselSessionId ipc_id) {
  DLOG(INFO) << "Clear composition: ipc_id = " << ipc_id;
  if (m_disabled)
    return;
  rime_api->clear_composition(to_session_id(ipc_id));
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
}

void RimeWithWeaselHandler::SelectCandidateOnCurrentPage(
    size_t index,
    WeaselSessionId ipc_id) {
  DLOG(INFO) << "select candidate on current page, ipc_id = " << ipc_id
             << ", index = " << index;
  if (m_clipboard_mode) {
    _PasteClipboardEntry(ipc_id, static_cast<int>(index));
    return;
  }
  if (m_disabled)
    return;
  rime_api->select_candidate_on_current_page(to_session_id(ipc_id), index);
}

bool RimeWithWeaselHandler::HighlightCandidateOnCurrentPage(
    size_t index,
    WeaselSessionId ipc_id,
    EatLine eat) {
  DLOG(INFO) << "highlight candidate on current page, ipc_id = " << ipc_id
             << ", index = " << index;
  if (m_clipboard_mode) {
    m_clipboard_highlight = static_cast<int>(index);
    _RefreshClipboardPanel();
    return true;
  }
  bool res = rime_api->highlight_candidate_on_current_page(
      to_session_id(ipc_id), index);
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  return res;
}

bool RimeWithWeaselHandler::ChangePage(bool backward,
                                       WeaselSessionId ipc_id,
                                       EatLine eat) {
  DLOG(INFO) << "change page, ipc_id = " << ipc_id
             << (backward ? "backward" : "foreward");
  if (m_clipboard_mode) {
    _ChangeClipboardPage(backward);
    _RefreshClipboardPanel();
    return true;
  }
  bool res = rime_api->change_page(to_session_id(ipc_id), backward);
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  return res;
}

void RimeWithWeaselHandler::FocusIn(DWORD client_caps, WeaselSessionId ipc_id) {
  DLOG(INFO) << "Focus in: ipc_id = " << ipc_id
             << ", client_caps = " << client_caps;
  if (m_disabled)
    return;
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
}

void RimeWithWeaselHandler::FocusOut(DWORD param, WeaselSessionId ipc_id) {
  DLOG(INFO) << "Focus out: ipc_id = " << ipc_id;
  if (m_clipboard_mode)
    _HideClipboardPanel();
  m_clipboard_hotkey_down = false;
  if (m_ui)
    m_ui->Hide();
  m_active_session = 0;
  if (!m_disabled && RIME_API_AVAILABLE(rime_api, cleanup_stale_sessions))
    rime_api->cleanup_stale_sessions();
}

void RimeWithWeaselHandler::UpdateInputPosition(RECT const& rc,
                                                WeaselSessionId ipc_id) {
  DLOG(INFO) << "Update input position: (" << rc.left << ", " << rc.top
             << "), ipc_id = " << ipc_id
             << ", m_active_session = " << m_active_session;
  if (m_ui)
    m_ui->UpdateInputPosition(rc);
  if (m_disabled)
    return;
  if (m_active_session != ipc_id) {
    _UpdateUI(ipc_id);
    m_active_session = ipc_id;
  }
}

std::string RimeWithWeaselHandler::m_message_type;
std::string RimeWithWeaselHandler::m_message_value;
std::string RimeWithWeaselHandler::m_message_label;
std::string RimeWithWeaselHandler::m_option_name;
std::mutex RimeWithWeaselHandler::m_notifier_mutex;

void RimeWithWeaselHandler::OnNotify(void* context_object,
                                     uintptr_t session_id,
                                     const char* message_type,
                                     const char* message_value) {
  // may be running in a thread when deploying rime
  RimeWithWeaselHandler* self =
      reinterpret_cast<RimeWithWeaselHandler*>(context_object);
  if (!self || !message_type || !message_value)
    return;
  std::lock_guard<std::mutex> lock(m_notifier_mutex);
  m_message_type = message_type;
  m_message_value = message_value;
  if (RIME_API_AVAILABLE(rime_api, get_state_label) &&
      !strcmp(message_type, "option")) {
    Bool state = message_value[0] != '!';
    const char* option_name = message_value + !state;
    m_option_name = option_name;
    const char* state_label =
        rime_api->get_state_label(session_id, option_name, state);
    if (state_label) {
      m_message_label = std::string(state_label);
    }
  }
}

void RimeWithWeaselHandler::_ReadClientInfo(WeaselSessionId ipc_id,
                                            LPWSTR buffer) {
  std::string app_name;
  // parse request text
  wbufferstream bs(buffer, WEASEL_IPC_BUFFER_LENGTH);
  std::wstring line;
  while (bs.good()) {
    std::getline(bs, line);
    if (!bs.good())
      break;
    // file ends
    if (line == L".")
      break;
    const std::wstring kClientAppKey = L"session.client_app=";
    if (starts_with(line, kClientAppKey)) {
      std::wstring lwr = line;
      to_lower(lwr);
      app_name = wtou8(lwr.substr(kClientAppKey.length()));
    }
  }
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  // set app specific options
  if (!app_name.empty()) {
    rime_api->set_property(session_id, "client_app", app_name.c_str());

    auto it = m_app_options.find(app_name);
    if (it != m_app_options.end()) {
      AppOptions& options(m_app_options[it->first]);
      for (const auto& pair : options) {
        DLOG(INFO) << "set app option: " << pair.first << " = " << pair.second;
        rime_api->set_option(session_id, pair.first.c_str(), Bool(pair.second));
      }
    }
  }
  // inline preedit
  bool inline_preedit = session_status.style.inline_preedit;
  rime_api->set_option(session_id, "inline_preedit", Bool(inline_preedit));
  // show soft cursor on weasel panel but not inline
  rime_api->set_option(session_id, "soft_cursor", Bool(!inline_preedit));
}

void RimeWithWeaselHandler::_GetCandidateInfo(CandidateInfo& cinfo,
                                              RimeContext& ctx) {
  cinfo.candies.resize(ctx.menu.num_candidates);
  cinfo.comments.resize(ctx.menu.num_candidates);
  cinfo.labels.resize(ctx.menu.num_candidates);
  for (int i = 0; i < ctx.menu.num_candidates; ++i) {
    cinfo.candies[i].str = escape_string(u8tow(ctx.menu.candidates[i].text));
    if (ctx.menu.candidates[i].comment) {
      cinfo.comments[i].str =
          escape_string(u8tow(ctx.menu.candidates[i].comment));
    }
    if (RIME_STRUCT_HAS_MEMBER(ctx, ctx.select_labels) && ctx.select_labels) {
      cinfo.labels[i].str = escape_string(u8tow(ctx.select_labels[i]));
    } else if (ctx.menu.select_keys) {
      cinfo.labels[i].str =
          escape_string(std::wstring(1, ctx.menu.select_keys[i]));
    } else {
      cinfo.labels[i].str = std::to_wstring((i + 1) % 10);
    }
  }
  cinfo.highlighted = ctx.menu.highlighted_candidate_index;
  cinfo.currentPage = ctx.menu.page_no;
  cinfo.is_last_page = ctx.menu.is_last_page;
}

void RimeWithWeaselHandler::StartMaintenance() {
  m_session_status_map.clear();
  Finalize();
  _UpdateUI(0);
}

void RimeWithWeaselHandler::EndMaintenance() {
  if (m_disabled) {
    Initialize();
    _UpdateUI(0);
  }
  m_session_status_map.clear();
}

void RimeWithWeaselHandler::SetOption(WeaselSessionId ipc_id,
                                      const std::string& opt,
                                      bool val) {
  // from no-session client, not actual typing session
  if (!ipc_id) {
    if (m_global_ascii_mode && opt == "ascii_mode") {
      for (auto& pair : m_session_status_map)
        rime_api->set_option(to_session_id(pair.first), "ascii_mode", val);
    } else {
      rime_api->set_option(to_session_id(m_active_session), opt.c_str(), val);
    }
  } else {
    rime_api->set_option(to_session_id(ipc_id), opt.c_str(), val);
  }
  // refresh UI (and tray icon) so the option change takes effect immediately,
  // e.g. when toggling ascii_mode from the TSF language bar
  _UpdateUI(ipc_id ? ipc_id : m_active_session);
}

void RimeWithWeaselHandler::OnUpdateUI(std::function<void()> const& cb) {
  _UpdateUICallback = cb;
}

bool RimeWithWeaselHandler::_IsDeployerRunning() {
  HANDLE hMutex = CreateMutex(NULL, TRUE, L"WeaselDeployerMutex");
  bool deployer_detected = hMutex && GetLastError() == ERROR_ALREADY_EXISTS;
  if (hMutex) {
    CloseHandle(hMutex);
  }
  return deployer_detected;
}

void RimeWithWeaselHandler::_SetupClipboardMonitor() {
  if (!m_clipboard_enabled || m_clipboard_hwnd)
    return;

  WNDCLASSEX wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &RimeWithWeaselHandler::ClipboardWndProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = kClipboardWindowClass;
  RegisterClassEx(&wc);

  m_clipboard_hwnd =
      CreateWindowEx(0, kClipboardWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                     nullptr, wc.hInstance, this);

  if (m_clipboard_hwnd) {
    AddClipboardFormatListener(m_clipboard_hwnd);
    LOG(INFO) << "clipboard monitor started.";
  } else {
    LOG(ERROR) << "failed to create clipboard monitor window.";
  }
}

void RimeWithWeaselHandler::_TeardownClipboardMonitor() {
  if (!m_clipboard_hwnd)
    return;

  RemoveClipboardFormatListener(m_clipboard_hwnd);
  DestroyWindow(m_clipboard_hwnd);
  m_clipboard_hwnd = nullptr;
}

LRESULT CALLBACK RimeWithWeaselHandler::ClipboardWndProc(HWND hwnd,
                                                         UINT msg,
                                                         WPARAM wParam,
                                                         LPARAM lParam) {
  auto* self = reinterpret_cast<RimeWithWeaselHandler*>(
      GetWindowLongPtr(hwnd, GWLP_USERDATA));

  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCT*>(lParam);
    self = reinterpret_cast<RimeWithWeaselHandler*>(create->lpCreateParams);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else if (msg == WM_CLIPBOARDUPDATE && self) {
    self->_CaptureClipboardText();
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

void RimeWithWeaselHandler::_LoadClipboardSettings(RimeConfig* config) {
  auto clamp_int = [](int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
  };

  m_clipboard_enabled = true;
  m_clipboard_history_limit = kClipboardHistoryLimit;
  m_clipboard_page_size = kClipboardVisibleLimit;
  m_clipboard_max_text_length = kClipboardMaxTextLength;
  m_clipboard_hotkey_modifiers =
      ibus::Modifier::CONTROL_MASK | ibus::Modifier::SHIFT_MASK;
  m_clipboard_hotkey_keycode = 'V';
  m_clipboard_hotkey_down = false;
  m_clipboard_clear_confirm = false;
  m_clipboard_clear_key_down = false;

  if (!config)
    return;

  Bool enabled = 0;
  if (rime_api->config_get_bool(config, "clipboard/enabled", &enabled))
    m_clipboard_enabled = enabled != 0;

  int value = 0;
  if (rime_api->config_get_int(config, "clipboard/history_limit", &value))
    m_clipboard_history_limit = static_cast<size_t>(clamp_int(value, 1, 1000));
  if (rime_api->config_get_int(config, "clipboard/page_size", &value)) {
    m_clipboard_page_size = static_cast<size_t>(
        clamp_int(value, 1, static_cast<int>(kClipboardVisibleLimit)));
  }
  if (rime_api->config_get_int(config, "clipboard/max_text_length", &value))
    m_clipboard_max_text_length = static_cast<size_t>(
        clamp_int(value, 1024, static_cast<int>(kClipboardMaxStoredEntry)));

  char hotkey[128] = {0};
  if (rime_api->config_get_string(config, "clipboard/hotkey", hotkey,
                                  static_cast<int>(sizeof(hotkey) - 1))) {
    UINT modifiers = 0;
    int keycode = 0;
    if (ParseClipboardHotkey(hotkey, &modifiers, &keycode)) {
      m_clipboard_hotkey_modifiers = modifiers;
      m_clipboard_hotkey_keycode = keycode;
    }
  }
}

void RimeWithWeaselHandler::_LoadClipboardHistory() {
  std::lock_guard<std::mutex> lock(m_clipboard_mutex);
  m_clipboard_entries.clear();

  FILE* file = nullptr;
  if (_wfopen_s(&file, ClipboardHistoryPath().c_str(), L"rb") != 0 || !file)
    return;

  uint32_t count = 0;
  if (fread(&count, sizeof(count), 1, file) == 1) {
    count = (std::min)(count, static_cast<uint32_t>(m_clipboard_history_limit));
    size_t total_chars = 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t length = 0;
      if (fread(&length, sizeof(length), 1, file) != 1 ||
          length > kClipboardMaxStoredEntry ||
          total_chars + length > kClipboardMaxTotalChars) {
        break;
      }
      total_chars += length;

      std::wstring text(length, L'\0');
      if (length > 0 &&
          fread(text.data(), sizeof(wchar_t), length, file) != length) {
        break;
      }

      if (!text.empty()) {
        if (IsImageEntry(text)) {
          ImageEntry image;
          if (!DecodeImageEntry(text, &image) ||
              GetFileAttributesW(image.path.c_str()) ==
                  INVALID_FILE_ATTRIBUTES) {
            continue;
          }
        }
        m_clipboard_entries.push_back(std::move(text));
      }
    }

    size_t image_count = 0;
    for (auto it = m_clipboard_entries.begin();
         it != m_clipboard_entries.end();) {
      if (IsImageEntry(*it) && ++image_count > kClipboardMaxImages) {
        it = m_clipboard_entries.erase(it);
        continue;
      }
      ++it;
    }
  }

  fclose(file);
}

void RimeWithWeaselHandler::_SaveClipboardHistory() {
  std::lock_guard<std::mutex> lock(m_clipboard_mutex);
  if (!SaveClipboardEntries(ClipboardHistoryPath(), m_clipboard_entries))
    LOG(WARNING) << "failed to save clipboard history";
}

void RimeWithWeaselHandler::_LoadClipboardFavorites() {
  std::lock_guard<std::mutex> lock(m_clipboard_mutex);
  m_clipboard_favorites.clear();

  FILE* file = nullptr;
  if (_wfopen_s(&file, ClipboardFavoritesPath().c_str(), L"rb") != 0 || !file)
    return;

  uint32_t count = 0;
  if (fread(&count, sizeof(count), 1, file) == 1) {
    count = (std::min)(count, 1000u);
    size_t total_chars = 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t length = 0;
      if (fread(&length, sizeof(length), 1, file) != 1 ||
          length > kClipboardMaxStoredEntry ||
          total_chars + length > kClipboardMaxTotalChars) {
        break;
      }
      total_chars += length;

      std::wstring text(length, L'\0');
      if (length > 0 &&
          fread(text.data(), sizeof(wchar_t), length, file) != length) {
        break;
      }

      if (!text.empty())
        m_clipboard_favorites.insert(std::move(text));
    }
  }

  fclose(file);
}

void RimeWithWeaselHandler::_SaveClipboardFavorites() {
  std::lock_guard<std::mutex> lock(m_clipboard_mutex);
  if (!SaveClipboardEntries(ClipboardFavoritesPath(), m_clipboard_favorites))
    LOG(WARNING) << "failed to save clipboard favorites";
}

void RimeWithWeaselHandler::_CleanupClipboardImages() {
  std::set<std::wstring> referenced;
  {
    std::lock_guard<std::mutex> lock(m_clipboard_mutex);
    for (const auto& entry : m_clipboard_entries) {
      ImageEntry image;
      if (DecodeImageEntry(entry, &image))
        referenced.insert(image.path);
    }
  }

  const std::wstring directory = ClipboardImageDirectory();
  if (directory.empty())
    return;

  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec) || ec)
    return;

  for (const auto& item : std::filesystem::directory_iterator(directory, ec)) {
    if (ec)
      break;
    if (!item.is_regular_file(ec)) {
      if (ec)
        break;
      continue;
    }
    const std::wstring path = item.path().wstring();
    if (referenced.find(path) == referenced.end())
      DeleteFileW(path.c_str());
  }
}

void RimeWithWeaselHandler::_CaptureClipboardText() {
  if (ClipboardContentExcluded(m_clipboard_hwnd))
    return;

  std::wstring entry;
  const std::vector<std::wstring> files = ReadClipboardFiles(m_clipboard_hwnd);
  if (!files.empty()) {
    entry = EncodeFileEntry(files);
    if (entry.size() > kClipboardMaxFileListChars)
      return;
  } else {
    std::wstring text = ReadClipboardText(m_clipboard_hwnd);
    if (!text.empty() && text.size() <= m_clipboard_max_text_length) {
      entry = std::move(text);
    } else {
      std::wstring image_path;
      int width = 0;
      int height = 0;
      if (!_CaptureClipboardImage(&image_path, &width, &height))
        return;
      entry = EncodeImageEntry(ImageEntry{image_path, width, height});
    }
  }

  {
    std::lock_guard<std::mutex> lock(m_clipboard_mutex);
    auto it = std::find(m_clipboard_entries.begin(), m_clipboard_entries.end(),
                        entry);
    if (it != m_clipboard_entries.end())
      m_clipboard_entries.erase(it);

    m_clipboard_entries.insert(m_clipboard_entries.begin(), entry);
    if (m_clipboard_entries.size() > m_clipboard_history_limit)
      m_clipboard_entries.resize(m_clipboard_history_limit);

    if (IsImageEntry(entry)) {
      size_t image_count = 0;
      for (auto image_it = m_clipboard_entries.begin();
           image_it != m_clipboard_entries.end();) {
        if (IsImageEntry(*image_it)) {
          if (++image_count > kClipboardMaxImages) {
            m_clipboard_favorites.erase(*image_it);
            image_it = m_clipboard_entries.erase(image_it);
            continue;
          }
        }
        ++image_it;
      }
    }

    size_t total_chars = 0;
    for (const auto& item : m_clipboard_entries)
      total_chars += item.size();
    while (m_clipboard_entries.size() > 1 &&
           total_chars > kClipboardMaxTotalChars) {
      total_chars -= m_clipboard_entries.back().size();
      m_clipboard_entries.pop_back();
    }
  }

  _SaveClipboardHistory();
  _CleanupClipboardImages();
  m_clipboard_dirty = true;
}

bool RimeWithWeaselHandler::_IsClipboardHotkey(
    const weasel::KeyEvent& keyEvent) const {
  if (!m_clipboard_enabled || (keyEvent.mask & ibus::Modifier::RELEASE_MASK)) {
    return false;
  }

  const UINT relevant_modifiers = ibus::Modifier::CONTROL_MASK |
                                  ibus::Modifier::SHIFT_MASK |
                                  ibus::Modifier::ALT_MASK;
  if ((keyEvent.mask & relevant_modifiers) != m_clipboard_hotkey_modifiers)
    return false;

  return _IsClipboardHotkeyKey(keyEvent.keycode);
}

bool RimeWithWeaselHandler::_IsClipboardHotkeyKey(int keycode) const {
  if (keycode >= 'a' && keycode <= 'z')
    keycode = std::toupper(static_cast<unsigned char>(keycode));
  int expected = m_clipboard_hotkey_keycode;
  if (expected >= 'a' && expected <= 'z')
    expected = std::toupper(static_cast<unsigned char>(expected));
  return keycode == expected;
}

void RimeWithWeaselHandler::_ShowClipboardPanel(WeaselSessionId ipc_id) {
  if (!m_clipboard_enabled || !m_ui)
    return;

  m_clipboard_mode = true;
  m_clipboard_query.clear();
  m_clipboard_highlight = 0;
  m_clipboard_page = 0;
  m_clipboard_favorites_only = false;
  m_clipboard_clear_confirm = false;
  m_clipboard_clear_key_down = false;
  m_active_session = ipc_id;

  bool history_empty = false;
  {
    std::lock_guard<std::mutex> lock(m_clipboard_mutex);
    history_empty = m_clipboard_entries.empty();
  }
  if (history_empty)
    _CaptureClipboardText();

  if (!m_disabled) {
    RimeSessionId session_id = to_session_id(ipc_id);
    if (session_id)
      rime_api->clear_composition(session_id);
  }

  _RefreshClipboardPanel();
  m_ui->Show();
}

void RimeWithWeaselHandler::_HideClipboardPanel() {
  m_clipboard_mode = false;
  m_clipboard_query.clear();
  m_clipboard_visible.clear();
  m_clipboard_highlight = 0;
  m_clipboard_page = 0;
  m_clipboard_total_pages = 1;
  m_clipboard_favorites_only = false;
  m_clipboard_clear_confirm = false;
  m_clipboard_clear_key_down = false;

  if (m_ui)
    m_ui->Hide();
}

void RimeWithWeaselHandler::_ChangeClipboardPage(bool backward) {
  if (m_clipboard_total_pages <= 1)
    return;
  m_clipboard_page =
      (m_clipboard_page + (backward ? -1 : 1) + m_clipboard_total_pages) %
      m_clipboard_total_pages;
  m_clipboard_highlight = 0;
}

void RimeWithWeaselHandler::_RefreshClipboardPanel() {
  if (!m_ui)
    return;

  struct Item {
    size_t index;
    bool favorite;
  };

  std::vector<Item> filtered;
  std::vector<bool> visible_favorites;
  {
    std::lock_guard<std::mutex> lock(m_clipboard_mutex);
    filtered.reserve(m_clipboard_entries.size());
    for (size_t i = 0; i < m_clipboard_entries.size(); ++i) {
      const auto& entry = m_clipboard_entries[i];
      const bool favorite =
          m_clipboard_favorites.find(entry) != m_clipboard_favorites.end();
      if (m_clipboard_favorites_only && !favorite)
        continue;
      if (!ContainsCaseInsensitive(entry, m_clipboard_query))
        continue;
      filtered.push_back(Item{i, favorite});
    }

    std::stable_sort(
        filtered.begin(), filtered.end(),
        [](const Item& a, const Item& b) { return a.favorite && !b.favorite; });

    const size_t page_size =
        (std::max)(m_clipboard_page_size, static_cast<size_t>(1));
    size_t total_pages = (filtered.size() + page_size - 1) / page_size;
    if (total_pages == 0)
      total_pages = 1;

    m_clipboard_total_pages = static_cast<int>(total_pages);
    if (m_clipboard_page < 0)
      m_clipboard_page = 0;
    if (m_clipboard_page >= m_clipboard_total_pages)
      m_clipboard_page = m_clipboard_total_pages - 1;

    const size_t begin = static_cast<size_t>(m_clipboard_page) * page_size;
    const size_t end = (std::min)(begin + page_size, filtered.size());

    m_clipboard_visible.clear();
    visible_favorites.clear();
    visible_favorites.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
      m_clipboard_visible.push_back(m_clipboard_entries[filtered[i].index]);
      visible_favorites.push_back(filtered[i].favorite);
    }
  }

  if (m_clipboard_visible.empty() || m_clipboard_highlight < 0) {
    m_clipboard_highlight = 0;
  } else if (m_clipboard_highlight >=
             static_cast<int>(m_clipboard_visible.size())) {
    m_clipboard_highlight = static_cast<int>(m_clipboard_visible.size()) - 1;
  }

  Context ctx;
  std::wstring title = L"剪贴板";
  if (m_clipboard_clear_confirm)
    title += L" [确认清空]";
  if (m_clipboard_favorites_only)
    title += L" [收藏]";
  title += L" " +
           std::to_wstring(static_cast<unsigned long long>(filtered.size())) +
           L"条";
  if (m_clipboard_total_pages > 1) {
    title += L" " + std::to_wstring(m_clipboard_page + 1) + L"/" +
             std::to_wstring(m_clipboard_total_pages);
  }
  if (!m_clipboard_query.empty())
    title += L"  搜索:" + m_clipboard_query;

  ctx.preedit = Text(title);
  ctx.aux = Text(m_clipboard_clear_confirm
                     ? L"再按一次 Ctrl+Shift+Delete 清空全部历史  Esc取消"
                     : L"↑↓选择  PgUp/PgDn翻页  Enter粘贴  Del删除  "
                       L"Tab收藏  Esc关闭");

  if (m_clipboard_visible.empty()) {
    ctx.cinfo.candies.push_back(Text(L"（没有匹配的剪贴板历史）"));
    ctx.cinfo.labels.push_back(Text(L""));
    ctx.cinfo.comments.push_back(Text(L""));
  } else {
    for (size_t i = 0; i < m_clipboard_visible.size(); ++i) {
      const auto& text = m_clipboard_visible[i];
      const bool favorite = visible_favorites[i];
      ctx.cinfo.labels.push_back(Text(std::to_wstring(i + 1)));
      ctx.cinfo.candies.push_back(Text(ClipboardEntryDisplay(text)));
      const wchar_t* kind = IsImageEntry(text)
                                ? L"图片"
                                : (IsFileEntry(text) ? L"文件" : L"历史");
      ctx.cinfo.comments.push_back(Text(favorite ? L"★ 收藏" : kind));
    }
  }

  // The panel renders its own page counter in the title. Keeping the
  // candidate window on page 0/1 disables its page arrows, which have no
  // mouse handler in the server UI.
  ctx.cinfo.currentPage = 0;
  ctx.cinfo.totalPages = 1;
  ctx.cinfo.is_last_page = true;
  ctx.cinfo.highlighted = m_clipboard_highlight;

  Status status = m_ui->status();
  status.composing = true;
  m_ui->Update(ctx, status);
  m_ui->Refresh();
}

bool RimeWithWeaselHandler::_HandleClipboardKey(
    const weasel::KeyEvent& keyEvent,
    WeaselSessionId ipc_id) {
  if (keyEvent.mask & ibus::Modifier::RELEASE_MASK) {
    if (_IsClipboardHotkeyKey(keyEvent.keycode))
      m_clipboard_hotkey_down = false;
    if (keyEvent.keycode == ibus::Delete ||
        keyEvent.keycode == ibus::KP_Delete) {
      m_clipboard_clear_key_down = false;
    }
    return true;
  }

  if (m_clipboard_dirty.exchange(false))
    _RefreshClipboardPanel();

  if (_IsClipboardHotkey(keyEvent)) {
    if (!m_clipboard_hotkey_down) {
      m_clipboard_hotkey_down = true;
      _HideClipboardPanel();
    }
    return true;
  }

  const bool clear_request =
      (keyEvent.mask & ibus::Modifier::CONTROL_MASK) &&
      (keyEvent.mask & ibus::Modifier::SHIFT_MASK) &&
      !(keyEvent.mask & ibus::Modifier::ALT_MASK) &&
      (keyEvent.keycode == ibus::Delete || keyEvent.keycode == ibus::KP_Delete);
  if (clear_request) {
    if (m_clipboard_clear_key_down)
      return true;
    m_clipboard_clear_key_down = true;

    if (!m_clipboard_clear_confirm) {
      m_clipboard_clear_confirm = true;
      _RefreshClipboardPanel();
    } else {
      {
        std::lock_guard<std::mutex> lock(m_clipboard_mutex);
        m_clipboard_entries.clear();
        m_clipboard_favorites.clear();
      }
      _SaveClipboardHistory();
      _SaveClipboardFavorites();
      m_clipboard_clear_confirm = false;
      m_clipboard_query.clear();
      m_clipboard_page = 0;
      m_clipboard_highlight = 0;
      m_clipboard_favorites_only = false;
      _RefreshClipboardPanel();
    }
    return true;
  }

  const bool was_confirming_clear = m_clipboard_clear_confirm;
  m_clipboard_clear_confirm = false;
  if (was_confirming_clear)
    _RefreshClipboardPanel();

  switch (keyEvent.keycode) {
    case ibus::Escape:
      _HideClipboardPanel();
      return true;

    case ibus::Up:
    case ibus::KP_Up:
      if (!m_clipboard_visible.empty()) {
        m_clipboard_highlight = (m_clipboard_highlight - 1 +
                                 static_cast<int>(m_clipboard_visible.size())) %
                                static_cast<int>(m_clipboard_visible.size());
      }
      _RefreshClipboardPanel();
      return true;

    case ibus::Down:
    case ibus::KP_Down:
      if (!m_clipboard_visible.empty()) {
        m_clipboard_highlight = (m_clipboard_highlight + 1) %
                                static_cast<int>(m_clipboard_visible.size());
      }
      _RefreshClipboardPanel();
      return true;

    case ibus::Page_Up:
    case ibus::KP_Page_Up:
      _ChangeClipboardPage(true);
      _RefreshClipboardPanel();
      return true;

    case ibus::Page_Down:
    case ibus::KP_Page_Down:
      _ChangeClipboardPage(false);
      _RefreshClipboardPanel();
      return true;

    case ibus::Home:
    case ibus::KP_Home:
      m_clipboard_page = 0;
      m_clipboard_highlight = 0;
      _RefreshClipboardPanel();
      return true;

    case ibus::End:
    case ibus::KP_End:
      m_clipboard_page = m_clipboard_total_pages - 1;
      m_clipboard_highlight = 0;
      _RefreshClipboardPanel();
      return true;

    case ibus::Tab:
      m_clipboard_favorites_only = !m_clipboard_favorites_only;
      m_clipboard_page = 0;
      m_clipboard_highlight = 0;
      _RefreshClipboardPanel();
      return true;

    case ibus::Return:
    case ibus::KP_Enter:
      _PasteClipboardEntry(ipc_id, m_clipboard_highlight);
      return true;

    case ibus::BackSpace:
      if (!m_clipboard_query.empty()) {
        m_clipboard_query.pop_back();
        m_clipboard_page = 0;
        m_clipboard_highlight = 0;
        _RefreshClipboardPanel();
      } else {
        _HideClipboardPanel();
      }
      return true;

    case ibus::Delete: {
      std::wstring selected;
      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(m_clipboard_mutex);
        if (m_clipboard_highlight >= 0 &&
            m_clipboard_highlight <
                static_cast<int>(m_clipboard_visible.size())) {
          selected = m_clipboard_visible[m_clipboard_highlight];
          auto it = std::find(m_clipboard_entries.begin(),
                              m_clipboard_entries.end(), selected);
          if (it != m_clipboard_entries.end()) {
            m_clipboard_entries.erase(it);
            changed = true;
          }
        }
      }

      if (changed) {
        {
          std::lock_guard<std::mutex> lock(m_clipboard_mutex);
          m_clipboard_favorites.erase(selected);
        }
        _SaveClipboardHistory();
        _SaveClipboardFavorites();
        _RefreshClipboardPanel();
      }
      return true;
    }
  }

  const bool ctrl_only = (keyEvent.mask & ibus::Modifier::CONTROL_MASK) &&
                         !(keyEvent.mask & ibus::Modifier::ALT_MASK) &&
                         !(keyEvent.mask & ibus::Modifier::SHIFT_MASK);

  if (ctrl_only && (keyEvent.keycode == 'D' || keyEvent.keycode == 'd')) {
    _ToggleClipboardFavorite();
    return true;
  }

  if (ctrl_only && (keyEvent.keycode == 'F' || keyEvent.keycode == 'f')) {
    m_clipboard_favorites_only = !m_clipboard_favorites_only;
    m_clipboard_page = 0;
    m_clipboard_highlight = 0;
    _RefreshClipboardPanel();
    return true;
  }

  if (ctrl_only && (keyEvent.keycode == 'U' || keyEvent.keycode == 'u')) {
    if (!m_clipboard_query.empty()) {
      m_clipboard_query.clear();
      m_clipboard_page = 0;
      m_clipboard_highlight = 0;
      _RefreshClipboardPanel();
    }
    return true;
  }

  const int max_number_key = '1' + static_cast<int>(m_clipboard_page_size) - 1;
  if (m_clipboard_query.empty() &&
      !(keyEvent.mask &
        (ibus::Modifier::CONTROL_MASK | ibus::Modifier::ALT_MASK)) &&
      keyEvent.keycode >= '1' && keyEvent.keycode <= max_number_key) {
    _PasteClipboardEntry(ipc_id, keyEvent.keycode - '1');
    return true;
  }

  if (!(keyEvent.mask &
        (ibus::Modifier::CONTROL_MASK | ibus::Modifier::ALT_MASK)) &&
      keyEvent.keycode >= 0x20 && keyEvent.keycode < 0x7f) {
    wchar_t c = static_cast<wchar_t>(keyEvent.keycode);
    if (std::iswprint(static_cast<wint_t>(c))) {
      m_clipboard_query.push_back(static_cast<wchar_t>(std::towlower(c)));
      m_clipboard_page = 0;
      m_clipboard_highlight = 0;
      _RefreshClipboardPanel();
    }
  }

  return true;
}

void RimeWithWeaselHandler::_ToggleClipboardFavorite() {
  if (m_clipboard_highlight < 0 ||
      m_clipboard_highlight >= static_cast<int>(m_clipboard_visible.size())) {
    return;
  }

  std::wstring selected = m_clipboard_visible[m_clipboard_highlight];
  {
    std::lock_guard<std::mutex> lock(m_clipboard_mutex);
    auto it = m_clipboard_favorites.find(selected);
    if (it == m_clipboard_favorites.end())
      m_clipboard_favorites.insert(selected);
    else
      m_clipboard_favorites.erase(it);
  }

  _SaveClipboardFavorites();
  m_clipboard_page = 0;
  m_clipboard_highlight = 0;
  _RefreshClipboardPanel();
}

void RimeWithWeaselHandler::_PasteClipboardEntry(WeaselSessionId ipc_id,
                                                 int index) {
  if (index < 0 || index >= static_cast<int>(m_clipboard_visible.size())) {
    return;
  }

  std::wstring entry = m_clipboard_visible[index];
  _HideClipboardPanel();
  Sleep(30);
  if (!_SetClipboardEntry(entry)) {
    LOG(WARNING) << "failed to set clipboard entry";
    return;
  }
  _SendPasteKey();
  m_active_session = ipc_id;
}

bool RimeWithWeaselHandler::_SetClipboardEntry(const std::wstring& entry) {
  if (IsImageEntry(entry)) {
    ImageEntry image;
    return DecodeImageEntry(entry, &image) && _SetClipboardImage(image.path);
  }
  if (IsFileEntry(entry))
    return WriteClipboardFiles(m_clipboard_hwnd, DecodeFileEntry(entry));
  return WriteClipboardText(m_clipboard_hwnd, entry);
}

bool RimeWithWeaselHandler::_SetClipboardImage(const std::wstring& path) {
  if (!m_gdiplus_token || path.empty())
    return false;

  std::error_code ec;
  const auto file_size = std::filesystem::file_size(path, ec);
  if (ec || file_size > kClipboardMaxImageFileBytes)
    return false;

  Gdiplus::Bitmap* bitmap = Gdiplus::Bitmap::FromFile(path.c_str());
  if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
    delete bitmap;
    return false;
  }

  const UINT width = bitmap->GetWidth();
  const UINT height = bitmap->GetHeight();
  if (width == 0 || height == 0 ||
      width > static_cast<UINT>(kClipboardMaxImageDimension) ||
      height > static_cast<UINT>(kClipboardMaxImageDimension)) {
    delete bitmap;
    return false;
  }

  Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
  Gdiplus::BitmapData data = {};
  if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB,
                       &data) != Gdiplus::Ok) {
    delete bitmap;
    return false;
  }

  const size_t row_bytes = static_cast<size_t>(width) * 4;
  const size_t image_size = row_bytes * height;
  const size_t total = sizeof(BITMAPINFOHEADER) + image_size;
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, total);
  if (!memory) {
    bitmap->UnlockBits(&data);
    delete bitmap;
    return false;
  }

  auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(memory));
  if (!header) {
    GlobalFree(memory);
    bitmap->UnlockBits(&data);
    delete bitmap;
    return false;
  }

  header->biSize = sizeof(BITMAPINFOHEADER);
  header->biWidth = static_cast<LONG>(width);
  header->biHeight = static_cast<LONG>(height);
  header->biPlanes = 1;
  header->biBitCount = 32;
  header->biCompression = BI_RGB;
  header->biSizeImage = static_cast<DWORD>(image_size);
  header->biXPelsPerMeter = 0;
  header->biYPelsPerMeter = 0;
  header->biClrUsed = 0;
  header->biClrImportant = 0;

  auto* destination = reinterpret_cast<BYTE*>(header + 1);
  const auto* source = static_cast<const BYTE*>(data.Scan0);
  const std::ptrdiff_t stride = data.Stride;
  for (UINT y = 0; y < height; ++y) {
    const BYTE* source_row =
        stride >= 0
            ? source + static_cast<std::ptrdiff_t>(y) * stride
            : source + static_cast<std::ptrdiff_t>(height - 1 - y) * -stride;
    memcpy(destination + static_cast<size_t>(height - 1 - y) * row_bytes,
           source_row, row_bytes);
  }

  GlobalUnlock(memory);
  bitmap->UnlockBits(&data);
  delete bitmap;

  if (!OpenClipboard(m_clipboard_hwnd)) {
    GlobalFree(memory);
    return false;
  }

  bool ok = false;
  EmptyClipboard();
  if (SetClipboardData(CF_DIB, memory)) {
    memory = nullptr;
    ok = true;
  }
  if (memory)
    GlobalFree(memory);
  CloseClipboard();
  return ok;
}

bool RimeWithWeaselHandler::_CaptureClipboardImage(std::wstring* path,
                                                   int* width,
                                                   int* height) {
  if (!m_gdiplus_token || !path || !width || !height)
    return false;
  if (!EnsureClipboardImageDirectory())
    return false;
  if (!OpenClipboard(m_clipboard_hwnd))
    return false;

  Gdiplus::Bitmap* bitmap = nullptr;
  HBITMAP owned_bitmap = nullptr;

  HBITMAP clipboard_bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
  if (clipboard_bitmap) {
    bitmap = Gdiplus::Bitmap::FromHBITMAP(clipboard_bitmap, nullptr);
    if (bitmap && bitmap->GetLastStatus() != Gdiplus::Ok) {
      delete bitmap;
      bitmap = nullptr;
    }
  }

  if (!bitmap) {
    HANDLE dib = GetClipboardData(CF_DIB);
    if (!dib)
      dib = GetClipboardData(CF_DIBV5);
    if (dib) {
      const SIZE_T size = GlobalSize(dib);
      auto* dib_data = static_cast<BYTE*>(GlobalLock(dib));
      if (dib_data && size >= sizeof(BITMAPINFOHEADER) &&
          size <= kClipboardMaxImageSourceBytes) {
        auto* header = reinterpret_cast<BITMAPINFOHEADER*>(dib_data);
        const LONG w = header->biWidth;
        const LONG h = header->biHeight;
        if (header->biSize >= sizeof(BITMAPINFOHEADER) && w > 0 && h != 0 &&
            w <= static_cast<LONG>(kClipboardMaxImageDimension) &&
            h >= -static_cast<LONG>(kClipboardMaxImageDimension) &&
            h <= static_cast<LONG>(kClipboardMaxImageDimension) &&
            header->biBitCount > 0 && header->biBitCount <= 32) {
          const size_t height_px =
              h < 0 ? static_cast<size_t>(-h) : static_cast<size_t>(h);
          size_t palette_size = 0;
          if (header->biBitCount <= 8) {
            const size_t colors = header->biClrUsed
                                      ? header->biClrUsed
                                      : (size_t(1) << header->biBitCount);
            palette_size = colors * sizeof(RGBQUAD);
          }

          size_t masks_size = 0;
          if (header->biSize == sizeof(BITMAPINFOHEADER) &&
              header->biCompression == BI_BITFIELDS) {
            masks_size = 3 * sizeof(DWORD);
          }

          size_t image_size = header->biSizeImage;
          if (image_size == 0) {
            const size_t row_bytes =
                ((static_cast<size_t>(w) * header->biBitCount + 31) / 32) * 4;
            image_size = row_bytes * height_px;
          }

          const size_t offset = header->biSize + masks_size + palette_size;
          if (image_size <= kClipboardMaxImageSourceBytes && offset <= size &&
              image_size <= size - offset) {
            HDC hdc = GetDC(nullptr);
            if (hdc) {
              HBITMAP hbmp = CreateDIBitmap(
                  hdc, header, CBM_INIT, dib_data + offset,
                  reinterpret_cast<BITMAPINFO*>(dib_data), DIB_RGB_COLORS);
              if (hbmp) {
                bitmap = Gdiplus::Bitmap::FromHBITMAP(hbmp, nullptr);
                if (bitmap && bitmap->GetLastStatus() != Gdiplus::Ok) {
                  delete bitmap;
                  bitmap = nullptr;
                }
                if (bitmap)
                  owned_bitmap = hbmp;
                else
                  DeleteObject(hbmp);
              }
              ReleaseDC(nullptr, hdc);
            }
          }
        }
      }
      if (dib_data)
        GlobalUnlock(dib);
    }
  }

  bool ok = false;
  if (bitmap) {
    const UINT w = bitmap->GetWidth();
    const UINT h = bitmap->GetHeight();
    if (w > 0 && h > 0 && w <= static_cast<UINT>(kClipboardMaxImageDimension) &&
        h <= static_cast<UINT>(kClipboardMaxImageDimension)) {
      CLSID png_clsid = {};
      if (GetPngEncoderClsid(&png_clsid)) {
        const std::wstring file_path = MakeClipboardImagePath();
        if (bitmap->Save(file_path.c_str(), &png_clsid, nullptr) ==
            Gdiplus::Ok) {
          std::error_code ec;
          const auto file_size = std::filesystem::file_size(file_path, ec);
          if (!ec && file_size <= kClipboardMaxImageFileBytes) {
            *path = file_path;
            *width = static_cast<int>(w);
            *height = static_cast<int>(h);
            ok = true;
          } else {
            DeleteFileW(file_path.c_str());
          }
        } else {
          DeleteFileW(file_path.c_str());
        }
      }
    }
  }

  delete bitmap;
  if (owned_bitmap)
    DeleteObject(owned_bitmap);
  CloseClipboard();
  return ok;
}

void RimeWithWeaselHandler::_SendPasteKey() {
  INPUT inputs[4] = {};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = VK_CONTROL;
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = 'V';
  inputs[2].type = INPUT_KEYBOARD;
  inputs[2].ki.wVk = 'V';
  inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
  inputs[3].type = INPUT_KEYBOARD;
  inputs[3].ki.wVk = VK_CONTROL;
  inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(4, inputs, sizeof(INPUT));
}

void RimeWithWeaselHandler::_UpdateUI(WeaselSessionId ipc_id) {
  // if m_ui nullptr, _UpdateUI meaningless
  if (!m_ui)
    return;

  if (m_clipboard_mode)
    return;

  Status& weasel_status = m_ui->status();
  Context weasel_context;

  RimeSessionId session_id = to_session_id(ipc_id);

  if (ipc_id == 0)
    weasel_status.disabled = m_disabled;

  _GetStatus(weasel_status, ipc_id, weasel_context);

  SessionStatus& session_status = get_session_status(ipc_id);
  if (rime_api->get_option(session_id, "inline_preedit"))
    session_status.style.client_caps |= INLINE_PREEDIT_CAPABLE;
  else
    session_status.style.client_caps &= ~INLINE_PREEDIT_CAPABLE;

  if (!_ShowMessage(weasel_context, weasel_status)) {
    m_ui->Hide();
    m_ui->Update(weasel_context, weasel_status);
  }

  _RefreshTrayIcon(session_id, _UpdateUICallback);

  {
    std::lock_guard<std::mutex> lock(m_notifier_mutex);
    m_message_type.clear();
    m_message_value.clear();
    m_message_label.clear();
    m_option_name.clear();
  }
}

void RimeWithWeaselHandler::_LoadSchemaSpecificSettings(
    WeaselSessionId ipc_id,
    const std::string& schema_id) {
  if (!m_ui)
    return;
  RimeConfig config;
  if (!rime_api->schema_open(schema_id.c_str(), &config))
    return;
  _UpdateShowNotifications(&config);
  m_ui->style() = m_base_style;
  _UpdateUIStyle(&config, m_ui, false);
  SessionStatus& session_status = get_session_status(ipc_id);
  session_status.style = m_ui->style();
  UIStyle& style = session_status.style;
  // load schema color style config
  const int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  const auto update_color_scheme = [&]() {
    std::string color_name(buffer);
    RimeConfigIterator preset = {0};
    if (rime_api->config_begin_map(
            &preset, &config, ("preset_color_schemes/" + color_name).c_str())) {
      _UpdateUIStyleColor(&config, style, color_name);
      rime_api->config_end(&preset);
    } else {
      RimeConfig weaselconfig;
      if (rime_api->config_open("weasel", &weaselconfig)) {
        _UpdateUIStyleColor(&weaselconfig, style, color_name);
        rime_api->config_close(&weaselconfig);
      }
    }
  };
  const char* key =
      m_current_dark_mode ? "style/color_scheme_dark" : "style/color_scheme";
  if (rime_api->config_get_string(&config, key, buffer, BUF_SIZE))
    update_color_scheme();
  // load schema icon start
  {
    const auto load_icon = [](RimeConfig& config, const char* key1,
                              const char* key2) {
      const auto user_dir = WeaselUserDataPath();
      const auto shared_dir = WeaselSharedDataPath();
      const int BUF_SIZE = 255;
      char buffer[BUF_SIZE + 1] = {0};
      if (rime_api->config_get_string(&config, key1, buffer, BUF_SIZE) ||
          (key2 != NULL &&
           rime_api->config_get_string(&config, key2, buffer, BUF_SIZE))) {
        auto resource = u8tow(buffer);
        if (fs::is_regular_file(user_dir / resource))
          return (user_dir / resource).wstring();
        else if (fs::is_regular_file(shared_dir / resource))
          return (shared_dir / resource).wstring();
      }
      return std::wstring();
    };
    style.current_zhung_icon =
        load_icon(config, "schema/icon", "schema/zhung_icon");
    style.current_ascii_icon = load_icon(config, "schema/ascii_icon", NULL);
    style.current_full_icon = load_icon(config, "schema/full_icon", NULL);
    style.current_half_icon = load_icon(config, "schema/half_icon", NULL);
  }
  // load schema icon end
  rime_api->config_close(&config);
}

void RimeWithWeaselHandler::_LoadAppInlinePreeditSet(WeaselSessionId ipc_id,
                                                     bool ignore_app_name) {
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  static char _app_name[50];
  rime_api->get_property(session_id, "client_app", _app_name,
                         sizeof(_app_name) - 1);
  std::string app_name(_app_name);
  if (!ignore_app_name && m_last_app_name == app_name)
    return;
  m_last_app_name = app_name;
  bool inline_preedit = session_status.style.inline_preedit;
  bool found = false;
  if (!app_name.empty()) {
    auto it = m_app_options.find(app_name);
    if (it != m_app_options.end()) {
      AppOptions& options(m_app_options[it->first]);
      for (const auto& pair : options) {
        if (pair.first == "inline_preedit") {
          rime_api->set_option(session_id, pair.first.c_str(),
                               Bool(pair.second));
          session_status.style.inline_preedit = Bool(pair.second);
          found = true;
          break;
        }
      }
    }
  }
  if (!found) {
    session_status.style.inline_preedit = m_base_style.inline_preedit;
    // load from schema.
    RIME_STRUCT(RimeStatus, status);
    if (rime_api->get_status(session_id, &status)) {
      std::string schema_id = status.schema_id;
      RimeConfig config;
      if (rime_api->schema_open(schema_id.c_str(), &config)) {
        Bool value = False;
        if (rime_api->config_get_bool(&config, "style/inline_preedit",
                                      &value)) {
          session_status.style.inline_preedit = value;
        }
        rime_api->config_close(&config);
      }
      rime_api->free_status(&status);
    }
  }
  if (session_status.style.inline_preedit != inline_preedit)
    _UpdateInlinePreeditStatus(ipc_id);
}

bool RimeWithWeaselHandler::_ShowMessage(Context& ctx, Status& status) {
  std::lock_guard<std::mutex> lock(m_notifier_mutex);
  if (m_message_type.empty() || m_message_value.empty())
    return m_ui->IsCountingDown();
  // show as auxiliary string
  std::wstring& tips(ctx.aux.str);
  bool show_icon = false;
  if (m_message_type == "deploy") {
    if (m_message_value == "start")
      if (GetThreadUILanguage() == MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US))
        tips = L"Deploying RIME";
      else
        tips = L"正在部署 RIME";
    else if (m_message_value == "success")
      if (GetThreadUILanguage() == MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US))
        tips = L"Deployed";
      else
        tips = L"部署完成";
    else if (m_message_value == "failure") {
      if (GetThreadUILanguage() ==
          MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL))
        tips = L"有錯誤，請查看日誌 %TEMP%\\rime.weasel\\rime.weasel.*.INFO";
      else if (GetThreadUILanguage() ==
               MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED))
        tips = L"有错误，请查看日志 %TEMP%\\rime.weasel\\rime.weasel.*.INFO";
      else
        tips =
            L"There is an error, please check the logs "
            L"%TEMP%\\rime.weasel\\rime.weasel.*.INFO";
    }
  } else if (m_message_type == "schema") {
    tips = /*L"【" + */ status.schema_name /* + L"】"*/;
  } else if (m_message_type == "option") {
    status.type = SCHEMA;
    if (m_message_value == "!ascii_mode") {
      show_icon = true;
    } else if (m_message_value == "ascii_mode") {
      show_icon = true;
    } else
      tips = u8tow(m_message_label);

    if (m_message_value == "full_shape" || m_message_value == "!full_shape")
      status.type = FULL_SHAPE;
  }
  auto counter = m_ui->IsCountingDown();
  if (!show_icon && counter)
    return counter;
  auto foption = m_show_notifications.find(m_option_name);
  auto falways = m_show_notifications.find("always");
  if ((!add_session && (foption != m_show_notifications.end() ||
                        falways != m_show_notifications.end())) ||
      m_message_type == "deploy") {
    m_ui->Update(ctx, status);
    // For icon-only notifications, status/context may be unchanged and Update()
    // can early-return; force a refresh before showing timeout tip.
    m_ui->Refresh();
    if (m_show_notifications_time)
      m_ui->ShowWithTimeout(m_show_notifications_time);
    return true;
  } else {
    return m_ui->IsCountingDown();
  }
}
inline std::string _GetLabelText(const std::vector<Text>& labels,
                                 int id,
                                 const wchar_t* format) {
  wchar_t buffer[128];
  swprintf_s<128>(buffer, format, labels.at(id).str.c_str());
  return wtou8(std::wstring(buffer));
}

bool RimeWithWeaselHandler::_Respond(WeaselSessionId ipc_id, EatLine eat) {
  std::wstring body;
  body.reserve(4096);
  std::vector<const char*> actions;
  actions.reserve(8);

  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  RIME_STRUCT(RimeCommit, commit);
  if (rime_api->get_commit(session_id, &commit)) {
    actions.push_back("commit");
    std::wstring commit_text_w = escape_string(u8tow(commit.text));
    body.append(L"commit=").append(commit_text_w).append(L"\n");
    rime_api->free_commit(&commit);
  }

  bool is_composing = false;
  RIME_STRUCT(RimeStatus, status);
  static const std::wstring Bool_wstring[] = {L"0", L"1"};
  if (rime_api->get_status(session_id, &status)) {
    is_composing = !!status.is_composing;
    actions.push_back("status");
    body.append(L"status.ascii_mode=")
        .append(Bool_wstring[!!status.is_ascii_mode])
        .append(L"\n")
        .append(L"status.composing=")
        .append(Bool_wstring[!!status.is_composing])
        .append(L"\n")
        .append(L"status.disabled=")
        .append(Bool_wstring[!!status.is_disabled])
        .append(L"\n")
        .append(L"status.full_shape=")
        .append(Bool_wstring[!!status.is_full_shape])
        .append(L"\n")
        .append(L"status.schema_id=")
        .append(status.schema_id ? u8tow(status.schema_id) : std::wstring())
        .append(L"\n");
    if (m_global_ascii_mode &&
        (session_status.status.is_ascii_mode != status.is_ascii_mode)) {
      for (auto& pair : m_session_status_map) {
        if (pair.first != ipc_id)
          rime_api->set_option(to_session_id(pair.first), "ascii_mode",
                               !!status.is_ascii_mode);
      }
    }
    session_status.status = status;
    rime_api->free_status(&status);
  }

  RIME_STRUCT(RimeContext, ctx);
  if (rime_api->get_context(session_id, &ctx)) {
    bool has_candidates = ctx.menu.num_candidates > 0;
    CandidateInfo cinfo;
    if (has_candidates) {
      _GetCandidateInfo(cinfo, ctx);
    }
    if (is_composing) {
      const auto& preedit = ctx.composition.preedit;
      const auto& start = ctx.composition.sel_start;
      const auto& end = ctx.composition.sel_end;
      const auto& cursor = ctx.composition.cursor_pos;
      static const auto u8towstring = [](const char* u8str, int len = 0) {
        return std::to_wstring(utf8towcslen(u8str, len));
      };
      actions.push_back("ctx");
      switch (session_status.style.preedit_type) {
        case UIStyle::PREVIEW: {
          if (ctx.commit_text_preview) {
            const char* first_utf8 = ctx.commit_text_preview;
            const size_t first_len = std::strlen(first_utf8);
            const std::wstring first_w = escape_string(u8tow(first_utf8));
            const std::wstring tmp = u8towstring(first_utf8, (int)first_len);
            body.append(L"ctx.preedit=")
                .append(first_w)
                .append(L"\n")
                .append(L"ctx.preedit.cursor=")
                .append(u8towstring(first_utf8, 0))
                .append(L",")
                .append(tmp)
                .append(L",")
                .append(tmp)
                .append(L"\n");
            break;
          }
          // no preview, fall back to composition
        }
        case UIStyle::COMPOSITION: {
          body.append(L"ctx.preedit=")
              .append(escape_string(u8tow(preedit)))
              .append(L"\n");
          if (start <= end) {
            body.append(L"ctx.preedit.cursor=")
                .append(u8towstring(preedit, start))
                .append(L",")
                .append(u8towstring(preedit, end))
                .append(L",")
                .append(u8towstring(preedit, cursor))
                .append(L"\n");
          }
          break;
        }
        case UIStyle::PREVIEW_ALL: {
          body.append(L"ctx.preedit=")
              .append(escape_string(u8tow(preedit)))
              .append(L"  [");
          auto label_valid = session_status.style.label_font_point > 0;
          auto comment_valid = session_status.style.comment_font_point > 0;
          const std::wstring mark_text_w =
              session_status.style.mark_text.empty()
                  ? std::wstring(L"*")
                  : session_status.style.mark_text;
          for (auto i = 0; i < ctx.menu.num_candidates; i++) {
            std::wstring label_w;
            if (label_valid) {
              wchar_t buf_lbl[128];
              swprintf_s<128>(buf_lbl,
                              session_status.style.label_text_format.c_str(),
                              cinfo.labels.at(i).str.c_str());
              label_w = std::wstring(buf_lbl);
            }
            std::wstring comment_w =
                comment_valid ? cinfo.comments.at(i).str : std::wstring();
            std::wstring prefix_w = (i != ctx.menu.highlighted_candidate_index)
                                        ? std::wstring()
                                        : mark_text_w;
            body.append(L" ")
                .append(prefix_w)
                .append(escape_string(label_w))
                .append(escape_string(u8tow(ctx.menu.candidates[i].text)))
                .append(L" ")
                .append(escape_string(comment_w));
          }
          body.append(L" ]\n");
          if (start <= end) {
            body.append(L"ctx.preedit.cursor=")
                .append(u8towstring(preedit, start))
                .append(L",")
                .append(u8towstring(preedit, end))
                .append(L",")
                .append(u8towstring(preedit, cursor))
                .append(L"\n");
          }
          break;
        }
      }
    }
    if (has_candidates) {
      std::wstringstream ss;
      boost::archive::text_woarchive oa(ss);

      oa << cinfo;

      auto s = ss.str();
      body.append(L"ctx.cand=").append(std::move(s)).append(L"\n");
    }
    rime_api->free_context(&ctx);
  }

  // configuration information
  actions.push_back("config");
  body.append(L"config.inline_preedit=")
      .append(std::to_wstring((int)session_status.style.inline_preedit))
      .append(L"\n");

  // style
  if (!session_status.__synced) {
    std::wstringstream ss;
    boost::archive::text_woarchive oa(ss);
    oa << session_status.style;

    actions.push_back("style");
    body.append(L"style=").append(ss.str()).append(L"\n");
    session_status.__synced = true;
  }

  // summarize: send header first to avoid vector head-insert cost
  std::wstring header;
  if (actions.empty()) {
    header = L"action=noop\n";
  } else {
    std::string actionList;
    actionList.reserve(64);
    for (size_t i = 0; i < actions.size(); ++i) {
      if (i > 0)
        actionList += ',';
      actionList += actions[i];
    }
    header = std::wstring(L"action=") + u8tow(actionList) + L"\n";
  }
  if (!eat(header))
    return false;

  body.append(L".\n");
  if (!eat(body))
    return false;

  return true;
}

// Blend foreground and background ARGB colors taking alpha into account.
// Returns an ABGR COLORREF with premultiplied alpha blended result.
static inline COLORREF blend_colors(COLORREF fcolor, COLORREF bcolor) {
  // Extract ARGB channels from both colors.
  BYTE fA = (fcolor >> 24) & 0xFF;
  BYTE fB = (fcolor >> 16) & 0xFF;
  BYTE fG = (fcolor >> 8) & 0xFF;
  BYTE fR = fcolor & 0xFF;
  BYTE bA = (bcolor >> 24) & 0xFF;
  BYTE bB = (bcolor >> 16) & 0xFF;
  BYTE bG = (bcolor >> 8) & 0xFF;
  BYTE bR = bcolor & 0xFF;
  // Convert alpha to [0,1]
  float fAlpha = fA / 255.0f;
  float bAlpha = bA / 255.0f;
  // Result alpha
  float retAlpha = fAlpha + (1 - fAlpha) * bAlpha;
  if (retAlpha <= 1e-6f) {
    // Fully transparent result — return background unchanged as fallback.
    return bcolor;
  }
  auto mix = [&](float fc, float bc) -> BYTE {
    return static_cast<BYTE>((fc * fAlpha + bc * bAlpha * (1 - fAlpha)) /
                             retAlpha);
  };
  BYTE retR = mix(fR, bR);
  BYTE retG = mix(fG, bG);
  BYTE retB = mix(fB, bB);
  BYTE outA = static_cast<BYTE>(retAlpha * 255.0f);
  return (static_cast<COLORREF>(outA) << 24) | (retB << 16) | (retG << 8) |
         retR;
}
// parse color value, with fallback value
static Bool _RimeGetColor(RimeConfig* config,
                          const std::string& key,
                          int& value,
                          const ColorFormat& fmt,
                          const unsigned int& fallback) {
  char color[256] = {0};
  if (!rime_api->config_get_string(config, key.c_str(), color, 256)) {
    value = fallback;
    return False;
  }
  unsigned int parsed = 0;
  if (!ParseColorValue(std::string(color), fmt, &parsed)) {
    value = fallback;
    return False;
  }
  value = static_cast<int>(parsed);
  return True;
}

template <typename T, size_t N>
using Array = std::array<std::pair<const char*, T>, N>;

// parset bool type configuration to T type value trueValue / falseValue
template <typename T>
void _RimeGetBool(RimeConfig* config,
                  const char* key,
                  bool cond,
                  T& value,
                  const T& trueValue = true,
                  const T& falseValue = false) {
  Bool tempb = False;
  if (rime_api->config_get_bool(config, key, &tempb) || cond)
    value = (!!tempb) ? trueValue : falseValue;
}
// parse string option to T type value, with fallback
template <typename T, size_t N>
void _RimeParseStringOptWithFallback(RimeConfig* config,
                                     const char* key,
                                     T& value,
                                     const Array<T, N>& arr,
                                     const T& fallback) {
  char str_buff[256] = {0};
  if (rime_api->config_get_string(config, key, str_buff, 255)) {
    for (size_t i = 0; i < N; ++i) {
      if (strcmp(arr[i].first, str_buff) == 0) {
        value = arr[i].second;
        return;
      }
    }
  }
  value = fallback;
}

template <typename T>
void _RimeGetIntStr(RimeConfig* config,
                    const char* key,
                    T& value,
                    const char* fb_key = nullptr,
                    const void* fb_value = nullptr,
                    const std::function<void(T&)>& func = nullptr) {
  if constexpr (std::is_same<T, int>::value) {
    if (!rime_api->config_get_int(config, key, &value) && fb_key != 0)
      rime_api->config_get_int(config, fb_key, &value);
  } else if constexpr (std::is_same<T, std::wstring>::value) {
    const int BUF_SIZE = 2047;
    char buffer[BUF_SIZE + 1] = {0};
    if (rime_api->config_get_string(config, key, buffer, BUF_SIZE) ||
        rime_api->config_get_string(config, fb_key, buffer, BUF_SIZE)) {
      value = u8tow(buffer);
    } else if (fb_value) {
      value = *(T*)fb_value;
    }
  }
  if (func)
    func(value);
}

// Helper to iterate a Rime map and invoke callback with key/path
static void ForEachRimeMap(
    RimeConfig* config,
    const std::string& path,
    const std::function<void(const char* key, const char* child_path)>& cb) {
  RimeConfigIterator iter;
  if (!rime_api->config_begin_map(&iter, config, path.c_str()))
    return;
  while (rime_api->config_next(&iter)) {
    cb(iter.key, iter.path);
  }
  rime_api->config_end(&iter);
}

// Helper to iterate a Rime list and invoke callback with item path
static void ForEachRimeList(
    RimeConfig* config,
    const std::string& path,
    const std::function<void(const char* item_path)>& cb) {
  RimeConfigIterator iter;
  if (!rime_api->config_begin_list(&iter, config, path.c_str()))
    return;
  while (rime_api->config_next(&iter)) {
    cb(iter.path);
  }
  rime_api->config_end(&iter);
}

void RimeWithWeaselHandler::_UpdateShowNotifications(RimeConfig* config,
                                                     bool initialize) {
  Bool show_notifications = true;
  if (initialize)
    m_show_notifications_base.clear();
  m_show_notifications.clear();

  if (rime_api->config_get_bool(config, "show_notifications",
                                &show_notifications)) {
    // config read as bool, for global all on or off
    if (show_notifications)
      m_show_notifications["always"] = true;
    if (initialize)
      m_show_notifications_base = m_show_notifications;
  } else {
    // read as list using helper
    ForEachRimeList(config, "show_notifications", [&](const char* item_path) {
      char buffer[256] = {0};
      if (rime_api->config_get_string(config, item_path, buffer, 256))
        m_show_notifications[std::string(buffer)] = true;
    });
    if (initialize)
      m_show_notifications_base = m_show_notifications;
    if (m_show_notifications.empty()) {
      // not configured, or incorrect type
      if (initialize)
        m_show_notifications_base["always"] = true;
      m_show_notifications = m_show_notifications_base;
    }
  }
}

// Load style parameters from `config` into `style`. When `initialize` is true,
// unset values are replaced with defaults; otherwise existing values are kept.
// A non-empty `color_scheme` selects that scheme instead of the active one.
void LoadWeaselUIStyle(RimeConfig* config,
                       UIStyle& style,
                       bool initialize,
                       const std::string& color_scheme) {
  if (!rime_api)
    rime_api = rime_get_api();
  if (!rime_api || !config)
    return;
  const std::function<void(std::wstring&)> rmspace = [](std::wstring& str) {
    str = std::regex_replace(str, std::wregex(L"\\s*(,|:|^|$)\\s*"), L"$1");
  };
  const std::function<void(int&)> _abs = [](int& value) { value = abs(value); };
  // get font faces
  _RimeGetIntStr(config, "style/font_face", style.font_face, 0, 0, rmspace);
  std::wstring* const pFallbackFontFace = initialize ? &style.font_face : NULL;
  _RimeGetIntStr(config, "style/label_font_face", style.label_font_face, 0,
                 pFallbackFontFace, rmspace);
  _RimeGetIntStr(config, "style/comment_font_face", style.comment_font_face, 0,
                 pFallbackFontFace, rmspace);
  // able to set label font/comment font empty, force fallback to font face.
  if (style.label_font_face.empty())
    style.label_font_face = style.font_face;
  if (style.comment_font_face.empty())
    style.comment_font_face = style.font_face;
  // get font points
  _RimeGetIntStr(config, "style/font_point", style.font_point);
  if (style.font_point <= 0)
    style.font_point = 12;
  _RimeGetIntStr(config, "style/label_font_point", style.label_font_point,
                 "style/font_point", 0, _abs);
  _RimeGetIntStr(config, "style/comment_font_point", style.comment_font_point,
                 "style/font_point", 0, _abs);
  _RimeGetIntStr(config, "style/candidate_abbreviate_length",
                 style.candidate_abbreviate_length, 0, 0, _abs);
  _RimeGetBool(config, "style/inline_preedit", initialize,
               style.inline_preedit);
  _RimeGetBool(config, "style/vertical_auto_reverse", initialize,
               style.vertical_auto_reverse);
  static constexpr Array<UIStyle::PreeditType, 3> _preeditArr = {
      {{"composition", UIStyle::COMPOSITION},
       {"preview", UIStyle::PREVIEW},
       {"preview_all", UIStyle::PREVIEW_ALL}}};
  _RimeParseStringOptWithFallback(config, "style/preedit_type",
                                  style.preedit_type, _preeditArr,
                                  style.preedit_type);
  static constexpr Array<UIStyle::AntiAliasMode, 5> _aliasModeArr = {
      {{"force_dword", UIStyle::FORCE_DWORD},
       {"cleartype", UIStyle::CLEARTYPE},
       {"grayscale", UIStyle::GRAYSCALE},
       {"aliased", UIStyle::ALIASED},
       {"default", UIStyle::DEFAULT}}};
  _RimeParseStringOptWithFallback(config, "style/antialias_mode",
                                  style.antialias_mode, _aliasModeArr,
                                  style.antialias_mode);
  static constexpr Array<UIStyle::HoverType, 3> _hoverTypeArr = {
      {{"none", UIStyle::HoverType::NONE},
       {"semi_hilite", UIStyle::HoverType::SEMI_HILITE},
       {"hilite", UIStyle::HoverType::HILITE}}};
  _RimeParseStringOptWithFallback(config, "style/hover_type", style.hover_type,
                                  _hoverTypeArr, style.hover_type);
  static constexpr Array<UIStyle::LayoutAlignType, 3> _alignType = {
      {{"top", UIStyle::ALIGN_TOP},
       {"center", UIStyle::ALIGN_CENTER},
       {"bottom", UIStyle::ALIGN_BOTTOM}}};
  _RimeParseStringOptWithFallback(config, "style/layout/align_type",
                                  style.align_type, _alignType,
                                  style.align_type);
  _RimeGetBool(config, "style/display_tray_icon", initialize,
               style.display_tray_icon);
  _RimeGetBool(config, "style/ascii_tip_follow_cursor", initialize,
               style.ascii_tip_follow_cursor);
  _RimeGetBool(config, "style/horizontal", initialize, style.layout_type,
               UIStyle::LAYOUT_HORIZONTAL, UIStyle::LAYOUT_VERTICAL);
  _RimeGetBool(config, "style/paging_on_scroll", initialize,
               style.paging_on_scroll);
  _RimeGetBool(config, "style/click_to_capture", initialize,
               style.click_to_capture, true, false);
  bool fullscreen = false;
  _RimeGetBool(config, "style/fullscreen", false, fullscreen);
  bool vertical_text = false;
  _RimeGetBool(config, "style/vertical_text", false, vertical_text);
  if (vertical_text) {
    if (fullscreen) {
      style.layout_type = UIStyle::LAYOUT_VERTICAL_TEXT_FULLSCREEN;
    } else {
      style.layout_type = UIStyle::LAYOUT_VERTICAL_TEXT;
    }
  } else {
    if (fullscreen) {
      style.layout_type = (style.layout_type == UIStyle::LAYOUT_HORIZONTAL)
                              ? UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN
                              : UIStyle::LAYOUT_VERTICAL_FULLSCREEN;
    }
  }
  _RimeGetBool(config, "style/vertical_text_left_to_right", false,
               style.vertical_text_left_to_right);
  _RimeGetBool(config, "style/vertical_text_with_wrap", false,
               style.vertical_text_with_wrap);
  _RimeGetBool(config, "style/vertical_right_to_left", initialize,
               style.vertical_right_to_left);
  static constexpr Array<bool, 2> _text_orientation = {
      {{"horizontal", false}, {"vertical", true}}};
  bool _text_orientation_bool = false;
  _RimeParseStringOptWithFallback(config, "style/text_orientation",
                                  _text_orientation_bool, _text_orientation,
                                  _text_orientation_bool);
  if (_text_orientation_bool) {
    if (fullscreen) {
      style.layout_type = UIStyle::LAYOUT_VERTICAL_TEXT_FULLSCREEN;
    } else {
      style.layout_type = UIStyle::LAYOUT_VERTICAL_TEXT;
    }
  }
  _RimeGetIntStr(config, "style/label_format", style.label_text_format);
  _RimeGetIntStr(config, "style/mark_text", style.mark_text);
  _RimeGetIntStr(config, "style/layout/baseline", style.baseline, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/linespacing", style.linespacing, 0, 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/min_width", style.min_width, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/max_width", style.max_width, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/min_height", style.min_height, 0, 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/max_height", style.max_height, 0, 0,
                 _abs);
  // layout (alternative to style/horizontal)
  static constexpr Array<UIStyle::LayoutType, 6> _layoutArr = {
      {{"vertical", UIStyle::LAYOUT_VERTICAL},
       {"horizontal", UIStyle::LAYOUT_HORIZONTAL},
       {"vertical_text", UIStyle::LAYOUT_VERTICAL_TEXT},
       {"vertical+fullscreen", UIStyle::LAYOUT_VERTICAL_FULLSCREEN},
       {"horizontal+fullscreen", UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN},
       {"vertical_text+fullscreen", UIStyle::LAYOUT_VERTICAL_TEXT_FULLSCREEN}}};
  _RimeParseStringOptWithFallback(config, "style/layout/type",
                                  style.layout_type, _layoutArr,
                                  style.layout_type);
  // Keep fullscreen vertical-text mode stable even when layout/type is also
  // set. Otherwise layout/type can override the earlier
  // fullscreen+vertical_text combination and cause unintended column wrapping
  // behavior.
  if (fullscreen && (vertical_text || _text_orientation_bool)) {
    style.layout_type = UIStyle::LAYOUT_VERTICAL_TEXT_FULLSCREEN;
  }
  // disable max_width when full screen
  if (style.layout_type == UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN ||
      style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN) {
    style.max_width = 0;
    style.inline_preedit = false;
  }
  _RimeGetIntStr(config, "style/layout/border", style.border,
                 "style/layout/border_width", 0, _abs);
  _RimeGetIntStr(config, "style/layout/margin_x", style.margin_x);
  _RimeGetIntStr(config, "style/layout/margin_y", style.margin_y);
  _RimeGetIntStr(config, "style/layout/spacing", style.spacing, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/candidate_spacing",
                 style.candidate_spacing, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/hilite_spacing", style.hilite_spacing, 0,
                 0, _abs);
  _RimeGetIntStr(config, "style/layout/hilite_padding_x",
                 style.hilite_padding_x, "style/layout/hilite_padding", 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/hilite_padding_y",
                 style.hilite_padding_y, "style/layout/hilite_padding", 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/shadow_radius", style.shadow_radius, 0,
                 0, _abs);
  _RimeGetIntStr(config, "style/layout/shadow_offset_x", style.shadow_offset_x);
  _RimeGetIntStr(config, "style/layout/shadow_offset_y", style.shadow_offset_y);
  // round_corner as alias of hilited_corner_radius
  _RimeGetIntStr(config, "style/layout/hilited_corner_radius",
                 style.round_corner, "style/layout/round_corner", 0, _abs);
  // corner_radius not set, fallback to round_corner
  _RimeGetIntStr(config, "style/layout/corner_radius", style.round_corner_ex,
                 "style/layout/round_corner", 0, _abs);
  // fix padding and spacing settings
  if (style.layout_type != UIStyle::LAYOUT_VERTICAL_TEXT) {
    // hilite_padding vs spacing
    // if hilite_padding over spacing, increase spacing
    style.spacing = max(style.spacing, style.hilite_padding_y * 2);
    // hilite_padding vs candidate_spacing
    if (style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN ||
        style.layout_type == UIStyle::LAYOUT_VERTICAL) {
      // vertical, if hilite_padding_y over candidate spacing,
      // increase candidate spacing
      style.candidate_spacing =
          max(style.candidate_spacing, style.hilite_padding_y * 2);
    } else {
      // horizontal, if hilite_padding_x over candidate
      // spacing, increase candidate spacing
      style.candidate_spacing =
          max(style.candidate_spacing, style.hilite_padding_x * 2);
    }
    // hilite_padding_x vs hilite_spacing
    if (!style.inline_preedit)
      style.hilite_spacing = max(style.hilite_spacing, style.hilite_padding_x);
  } else  // LAYOUT_VERTICAL_TEXT
  {
    // hilite_padding_x vs spacing
    // if hilite_padding over spacing, increase spacing
    style.spacing = max(style.spacing, style.hilite_padding_x * 2);
    // hilite_padding vs candidate_spacing
    // if hilite_padding_x over candidate
    // spacing, increase candidate spacing
    style.candidate_spacing =
        max(style.candidate_spacing, style.hilite_padding_x * 2);
    // vertical_text_with_wrap and hilite_padding_y over candidate_spacing
    if (style.vertical_text_with_wrap)
      style.candidate_spacing =
          max(style.candidate_spacing, style.hilite_padding_y * 2);
    // hilite_padding_y vs hilite_spacing
    if (!style.inline_preedit)
      style.hilite_spacing = max(style.hilite_spacing, style.hilite_padding_y);
  }
  if (style.layout_type == UIStyle::LAYOUT_VERTICAL_TEXT_FULLSCREEN) {
    style.max_height = 0;
  }
  // fix padding and margin settings
  int scale = style.margin_x < 0 ? -1 : 1;
  style.margin_x = scale * max(style.hilite_padding_x, abs(style.margin_x));
  scale = style.margin_y < 0 ? -1 : 1;
  style.margin_y = scale * max(style.hilite_padding_y, abs(style.margin_y));
  // get enhanced_position
  _RimeGetBool(config, "style/enhanced_position", initialize,
               style.enhanced_position, true, false);
  // get color scheme
  const int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  if ((initialize && rime_api->config_get_string(config, "style/color_scheme",
                                                 buffer, BUF_SIZE)) ||
      !color_scheme.empty())
    _UpdateUIStyleColor(config, style, color_scheme);
}

static void _UpdateUIStyle(RimeConfig* config, UI* ui, bool initialize) {
  LoadWeaselUIStyle(config, ui->style(), initialize);
}
// load color configs to style, by "style/color_scheme" or specific scheme name
// "color" which is default empty
static bool _UpdateUIStyleColor(RimeConfig* config,
                                UIStyle& style,
                                const std::string& color) {
  if (!rime_api)
    rime_api = rime_get_api();
  if (!rime_api || !config)
    return false;
  const int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  std::string color_mark = "style/color_scheme";
  // color scheme
  if (rime_api->config_get_string(config, color_mark.c_str(), buffer,
                                  BUF_SIZE) ||
      !color.empty()) {
    std::string prefix("preset_color_schemes/");
    prefix += (color.empty()) ? buffer : color;
    // define color format, default abgr if not set
    ColorFormat fmt = COLOR_ABGR;
    static constexpr Array<ColorFormat, 3> _colorFmt = {
        {{"argb", COLOR_ARGB}, {"rgba", COLOR_RGBA}, {"abgr", COLOR_ABGR}}};
    _RimeParseStringOptWithFallback(config, (prefix + "/color_format").c_str(),
                                    fmt, _colorFmt, COLOR_ABGR);
#define COLOR(key, value, fallback) \
  _RimeGetColor(config, (prefix + "/" + key), value, fmt, fallback)
    COLOR("back_color", style.back_color, 0xffffffff);
    COLOR("shadow_color", style.shadow_color, 0);
    COLOR("prevpage_color", style.prevpage_color, 0);
    COLOR("nextpage_color", style.nextpage_color, 0);
    COLOR("text_color", style.text_color, 0xff000000);
    COLOR("candidate_text_color", style.candidate_text_color, style.text_color);
    COLOR("candidate_back_color", style.candidate_back_color, 0);
    COLOR("border_color", style.border_color, style.text_color);
    COLOR("hilited_text_color", style.hilited_text_color, style.text_color);
    COLOR("hilited_back_color", style.hilited_back_color, style.back_color);
    COLOR("hilited_candidate_text_color", style.hilited_candidate_text_color,
          style.hilited_text_color);
    COLOR("hilited_candidate_back_color", style.hilited_candidate_back_color,
          style.hilited_back_color);
    COLOR("hilited_candidate_shadow_color",
          style.hilited_candidate_shadow_color, 0);
    COLOR("hilited_shadow_color", style.hilited_shadow_color, 0);
    COLOR("candidate_shadow_color", style.candidate_shadow_color, 0);
    COLOR("candidate_border_color", style.candidate_border_color, 0);
    COLOR("hilited_candidate_border_color",
          style.hilited_candidate_border_color, 0);
    COLOR("label_color", style.label_text_color,
          blend_colors(style.candidate_text_color, style.candidate_back_color));
    COLOR("hilited_label_color", style.hilited_label_text_color,
          blend_colors(style.hilited_candidate_text_color,
                       style.hilited_candidate_back_color));
    COLOR("comment_text_color", style.comment_text_color,
          style.label_text_color);
    COLOR("hilited_comment_text_color", style.hilited_comment_text_color,
          style.hilited_label_text_color);
    COLOR("hilited_mark_color", style.hilited_mark_color, 0);
#undef COLOR
    return true;
  }
  return false;
}
static void _LoadAppOptions(RimeConfig* config,
                            AppOptionsByAppName& app_options) {
  app_options.clear();
  ForEachRimeMap(
      config, "app_options", [&](const char* app_key, const char* app_path) {
        AppOptions& options(app_options[app_key]);
        ForEachRimeMap(
            config, app_path, [&](const char* opt_key, const char* opt_path) {
              Bool value = False;
              if (rime_api->config_get_bool(config, opt_path, &value)) {
                options[opt_key] = !!value;
              }
            });
      });
}

void RimeWithWeaselHandler::_GetStatus(Status& stat,
                                       WeaselSessionId ipc_id,
                                       Context& ctx) {
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  RIME_STRUCT(RimeStatus, status);
  if (rime_api->get_status(session_id, &status)) {
    std::string schema_id = "";
    if (status.schema_id)
      schema_id = status.schema_id;
    stat.schema_name = u8tow(status.schema_name);
    stat.schema_id = u8tow(status.schema_id);
    stat.ascii_mode = !!status.is_ascii_mode;
    stat.composing = !!status.is_composing;
    stat.disabled = !!status.is_disabled;
    stat.full_shape = !!status.is_full_shape;
    if (schema_id != m_last_schema_id) {
      session_status.__synced = false;
      m_last_schema_id = schema_id;
      if (schema_id != ".default") {  // don't load for schema select menu
        bool inline_preedit = session_status.style.inline_preedit;
        _LoadSchemaSpecificSettings(ipc_id, schema_id);
        _LoadAppInlinePreeditSet(ipc_id, true);
        if (session_status.style.inline_preedit != inline_preedit)
          // in case of inline_preedit set in schema
          _UpdateInlinePreeditStatus(ipc_id);
        // refresh icon after schema changed
        _RefreshTrayIcon(session_id, _UpdateUICallback);
        m_ui->style() = session_status.style;
        if (m_show_notifications.find("schema") != m_show_notifications.end() &&
            m_show_notifications_time > 0) {
          ctx.aux.str = stat.schema_name;
          m_ui->Update(ctx, stat);
          m_ui->ShowWithTimeout(m_show_notifications_time);
        }
      }
    }
    rime_api->free_status(&status);
  }
}

void RimeWithWeaselHandler::_GetContext(Context& weasel_context,
                                        RimeSessionId session_id) {
  RIME_STRUCT(RimeContext, ctx);
  if (rime_api->get_context(session_id, &ctx)) {
    if (ctx.composition.length > 0) {
      weasel_context.preedit.str = u8tow(ctx.composition.preedit);
      if (ctx.composition.sel_start < ctx.composition.sel_end) {
        TextAttribute attr;
        attr.type = HIGHLIGHTED;
        attr.range.start =
            utf8towcslen(ctx.composition.preedit, ctx.composition.sel_start);
        attr.range.end =
            utf8towcslen(ctx.composition.preedit, ctx.composition.sel_end);

        weasel_context.preedit.attributes.push_back(attr);
      }
    }
    if (ctx.menu.num_candidates) {
      CandidateInfo& cinfo(weasel_context.cinfo);
      _GetCandidateInfo(cinfo, ctx);
    }
    rime_api->free_context(&ctx);
  }
}

void RimeWithWeaselHandler::_UpdateInlinePreeditStatus(WeaselSessionId ipc_id) {
  if (!m_ui)
    return;
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  // set inline_preedit option
  bool inline_preedit = session_status.style.inline_preedit;
  rime_api->set_option(session_id, "inline_preedit", Bool(inline_preedit));
  // show soft cursor on weasel panel but not inline
  rime_api->set_option(session_id, "soft_cursor", Bool(!inline_preedit));
}
