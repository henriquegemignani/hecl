#include "hecl/hecl.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#ifdef WIN32
#include <windows.h>
#ifndef _WIN32_IE
#define _WIN32_IE 0x0400
#endif
#include <shlobj.h>
#endif

#ifdef __APPLE__
#include <Carbon/Carbon.h>
#endif

#ifdef __linux__
#include <mntent.h>
#include <sys/wait.h>
#endif

#include <logvisor/logvisor.hpp>

using namespace std::literals;

namespace hecl {
unsigned VerbosityLevel = 0;
bool GuiMode = false;
logvisor::Module LogModule("hecl");
constexpr std::string_view Illegals = "<>?\""sv;

void SanitizePath(std::string& path) {
  if (path.empty())
    return;
  path.erase(std::remove(path.begin(), path.end(), '\n'), path.end());
  path.erase(std::remove(path.begin(), path.end(), '\r'), path.end());
  std::string::iterator p1 = path.begin();
  bool ic = false;
  std::transform(path.begin(), path.end(), path.begin(), [&](const char a) -> char {
    ++p1;
    if (Illegals.find_first_of(a) != std::string::npos) {
      ic = false;
      return '_';
    }

    if (ic) {
      ic = false;
      return a;
    }
    if (a == '\\' && (p1 == path.end() || *p1 != '\\')) {
      ic = true;
      return '/';
    }
    return a;
  });
  while (path.back() == '/')
    path.pop_back();
}

constexpr std::wstring_view WIllegals = L"<>?\""sv;

void SanitizePath(std::wstring& path) {
  if (path.empty())
    return;
  path.erase(std::remove(path.begin(), path.end(), L'\n'), path.end());
  path.erase(std::remove(path.begin(), path.end(), L'\r'), path.end());
  std::wstring::iterator p1 = path.begin();
  bool ic = false;
  std::transform(path.begin(), path.end(), path.begin(), [&](const wchar_t a) -> wchar_t {
    ++p1;
    if (WIllegals.find_first_of(a) != std::wstring::npos) {
      ic = false;
      return L'_';
    }

    if (ic) {
      ic = false;
      return a;
    }
    if (a == L'\\' && (p1 == path.end() || *p1 != L'\\')) {
      ic = true;
      return L'/';
    }
    return a;
  });
  while (path.back() == L'/')
    path.pop_back();
}

SystemString GetcwdStr() {
  /* http://stackoverflow.com/a/2869667 */
  // const size_t ChunkSize=255;
  // const int MaxChunks=10240; // 2550 KiBs of current path are more than enough

  SystemChar stackBuffer[255]; // Stack buffer for the "normal" case
  if (Getcwd(stackBuffer, int(std::size(stackBuffer))) != nullptr) {
    return SystemString(stackBuffer);
  }
  if (errno != ERANGE) {
    // It's not ERANGE, so we don't know how to handle it
    LogModule.report(logvisor::Fatal, FMT_STRING("Cannot determine the current path."));
    // Of course you may choose a different error reporting method
  }
  // Ok, the stack buffer isn't long enough; fallback to heap allocation
  for (int chunks = 2; chunks < 10240; chunks++) {
    // With boost use scoped_ptr; in C++0x, use unique_ptr
    // If you want to be less C++ but more efficient you may want to use realloc
    const int bufSize = 255 * chunks;
    std::unique_ptr<SystemChar[]> cwd(new SystemChar[bufSize]);
    if (Getcwd(cwd.get(), bufSize) != nullptr) {
      return SystemString(cwd.get());
    }
    if (errno != ERANGE) {
      // It's not ERANGE, so we don't know how to handle it
      LogModule.report(logvisor::Fatal, FMT_STRING("Cannot determine the current path."));
      // Of course you may choose a different error reporting method
    }
  }
  LogModule.report(logvisor::Fatal, FMT_STRING("Cannot determine the current path; the path is apparently unreasonably long"));
  return SystemString();
}

static std::mutex PathsMutex;
static std::unordered_map<std::thread::id, ProjectPath> PathsInProgress;

bool ResourceLock::InProgress(const ProjectPath& path) {
  std::unique_lock lk{PathsMutex};
  return std::any_of(PathsInProgress.cbegin(), PathsInProgress.cend(),
                     [&path](const auto& entry) { return entry.second == path; });
}

bool ResourceLock::SetThreadRes(const ProjectPath& path) {
  std::unique_lock lk{PathsMutex};
  if (PathsInProgress.find(std::this_thread::get_id()) != PathsInProgress.cend()) {
    LogModule.report(logvisor::Fatal, FMT_STRING("multiple resource locks on thread"));
  }

  const bool isInProgress = std::any_of(PathsInProgress.cbegin(), PathsInProgress.cend(),
                                        [&path](const auto& entry) { return entry.second == path; });
  if (isInProgress) {
    return false;
  }

  PathsInProgress.insert_or_assign(std::this_thread::get_id(), path);
  return true;
}

void ResourceLock::ClearThreadRes() {
  std::unique_lock lk{PathsMutex};
  PathsInProgress.erase(std::this_thread::get_id());
}

bool IsPathPNG(const hecl::ProjectPath& path) {
  const auto fp = hecl::FopenUnique(path.getAbsolutePath().data(), _SYS_STR("rb"));
  if (fp == nullptr) {
    return false;
  }

  uint32_t buf = 0;
  if (std::fread(&buf, 1, sizeof(buf), fp.get()) != sizeof(buf)) {
    return false;
  }

  buf = hecl::SBig(buf);
  return buf == 0x89504e47;
}

bool IsPathBlend(const hecl::ProjectPath& path) {
  const auto lastCompExt = path.getLastComponentExt();
  if (lastCompExt.empty() || lastCompExt != _SYS_STR("blend"))
    return false;

  const auto fp = hecl::FopenUnique(path.getAbsolutePath().data(), _SYS_STR("rb"));
  if (fp == nullptr) {
    return false;
  }

  uint32_t buf = 0;
  if (std::fread(&buf, 1, sizeof(buf), fp.get()) != sizeof(buf)) {
    return false;
  }

  buf = hecl::SLittle(buf);
  return buf == 0x4e454c42 || buf == 0x88b1f;
}

bool IsPathYAML(const hecl::ProjectPath& path) {
  auto lastComp = path.getLastComponent();
  if (lastComp == _SYS_STR("!catalog.yaml") ||
      lastComp == _SYS_STR("!memoryid.yaml") ||
      lastComp == _SYS_STR("!memoryrelays.yaml"))
    return false; /* !catalog.yaml, !memoryid.yaml, !memoryrelays.yaml are exempt from general use */
  auto lastCompExt = path.getLastComponentExt();
  if (lastCompExt.empty())
    return false;
  return lastCompExt == _SYS_STR("yaml") || lastCompExt == _SYS_STR("yml");
}

hecl::DirectoryEnumerator::DirectoryEnumerator(SystemStringView path, Mode mode, bool sizeSort, bool reverse,
                                               bool noHidden) {
  hecl::Sstat theStat;
  if (hecl::Stat(path.data(), &theStat) || !S_ISDIR(theStat.st_mode))
    return;

#if _WIN32
  hecl::SystemString wc(path);
  wc += _SYS_STR("/*");
  WIN32_FIND_DATAW d;
  HANDLE dir = FindFirstFileW(wc.c_str(), &d);
  if (dir == INVALID_HANDLE_VALUE)
    return;
  switch (mode) {
  case Mode::Native:
    do {
      if (!wcscmp(d.cFileName, _SYS_STR(".")) || !wcscmp(d.cFileName, _SYS_STR("..")))
        continue;
      if (noHidden && (d.cFileName[0] == L'.' || (d.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0))
        continue;
      hecl::SystemString fp(path);
      fp += _SYS_STR('/');
      fp += d.cFileName;
      hecl::Sstat st;
      if (hecl::Stat(fp.c_str(), &st))
        continue;

      size_t sz = 0;
      bool isDir = false;
      if (S_ISDIR(st.st_mode))
        isDir = true;
      else if (S_ISREG(st.st_mode))
        sz = st.st_size;
      else
        continue;

      m_entries.emplace_back(fp, d.cFileName, sz, isDir);
    } while (FindNextFileW(dir, &d));
    break;
  case Mode::DirsThenFilesSorted:
  case Mode::DirsSorted: {
    std::map<hecl::SystemString, Entry, CaseInsensitiveCompare> sort;
    do {
      if (!wcscmp(d.cFileName, _SYS_STR(".")) || !wcscmp(d.cFileName, _SYS_STR("..")))
        continue;
      if (noHidden && (d.cFileName[0] == L'.' || (d.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0))
        continue;
      hecl::SystemString fp(path);
      fp += _SYS_STR('/');
      fp += d.cFileName;
      hecl::Sstat st;
      if (hecl::Stat(fp.c_str(), &st) || !S_ISDIR(st.st_mode))
        continue;
      sort.emplace(std::make_pair(d.cFileName, Entry(std::move(fp), d.cFileName, 0, true)));
    } while (FindNextFileW(dir, &d));

    if (reverse)
      for (auto it = sort.crbegin(); it != sort.crend(); ++it)
        m_entries.push_back(std::move(it->second));
    else
      for (auto& e : sort)
        m_entries.push_back(std::move(e.second));

    if (mode == Mode::DirsSorted)
      break;
    FindClose(dir);
    dir = FindFirstFileW(wc.c_str(), &d);
  }
  case Mode::FilesSorted: {
    if (mode == Mode::FilesSorted)
      m_entries.clear();

    if (sizeSort) {
      std::multimap<size_t, Entry> sort;
      do {
        if (!wcscmp(d.cFileName, _SYS_STR(".")) || !wcscmp(d.cFileName, _SYS_STR("..")))
          continue;
        if (noHidden && (d.cFileName[0] == L'.' || (d.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0))
          continue;
        hecl::SystemString fp(path);
        fp += _SYS_STR('/');
        fp += d.cFileName;
        hecl::Sstat st;
        if (hecl::Stat(fp.c_str(), &st) || !S_ISREG(st.st_mode))
          continue;
        sort.emplace(std::make_pair(st.st_size, Entry(std::move(fp), d.cFileName, st.st_size, false)));
      } while (FindNextFileW(dir, &d));

      if (reverse)
        for (auto it = sort.crbegin(); it != sort.crend(); ++it)
          m_entries.push_back(std::move(it->second));
      else
        for (auto& e : sort)
          m_entries.push_back(std::move(e.second));
    } else {
      std::map<hecl::SystemString, Entry, CaseInsensitiveCompare> sort;
      do {
        if (!wcscmp(d.cFileName, _SYS_STR(".")) || !wcscmp(d.cFileName, _SYS_STR("..")))
          continue;
        if (noHidden && (d.cFileName[0] == L'.' || (d.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0))
          continue;
        hecl::SystemString fp(path);
        fp += _SYS_STR('/');
        fp += d.cFileName;
        hecl::Sstat st;
        if (hecl::Stat(fp.c_str(), &st) || !S_ISREG(st.st_mode))
          continue;
        sort.emplace(std::make_pair(d.cFileName, Entry(std::move(fp), d.cFileName, st.st_size, false)));
      } while (FindNextFileW(dir, &d));

      if (reverse)
        for (auto it = sort.crbegin(); it != sort.crend(); ++it)
          m_entries.push_back(std::move(it->second));
      else
        for (auto& e : sort)
          m_entries.push_back(std::move(e.second));
    }

    break;
  }
  }
  FindClose(dir);

#else

  DIR* dir = opendir(path.data());
  if (!dir)
    return;
  const dirent* d;
  switch (mode) {
  case Mode::Native:
    while ((d = readdir(dir))) {
      if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
        continue;
      if (noHidden && d->d_name[0] == '.')
        continue;
      hecl::SystemString fp(path);
      fp += '/';
      fp += d->d_name;
      hecl::Sstat st;
      if (hecl::Stat(fp.c_str(), &st))
        continue;

      size_t sz = 0;
      bool isDir = false;
      if (S_ISDIR(st.st_mode))
        isDir = true;
      else if (S_ISREG(st.st_mode))
        sz = st.st_size;
      else
        continue;

      m_entries.push_back(Entry(std::move(fp), d->d_name, sz, isDir));
    }
    break;
  case Mode::DirsThenFilesSorted:
  case Mode::DirsSorted: {
    std::map<hecl::SystemString, Entry, CaseInsensitiveCompare> sort;
    while ((d = readdir(dir))) {
      if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
        continue;
      if (noHidden && d->d_name[0] == '.')
        continue;
      hecl::SystemString fp(path);
      fp += '/';
      fp += d->d_name;
      hecl::Sstat st;
      if (hecl::Stat(fp.c_str(), &st) || !S_ISDIR(st.st_mode))
        continue;
      sort.emplace(std::make_pair(d->d_name, Entry(std::move(fp), d->d_name, 0, true)));
    }

    if (reverse)
      for (auto it = sort.crbegin(); it != sort.crend(); ++it)
        m_entries.push_back(std::move(it->second));
    else
      for (auto& e : sort)
        m_entries.push_back(std::move(e.second));

    if (mode == Mode::DirsSorted)
      break;
    rewinddir(dir);
    [[fallthrough]];
  }
  case Mode::FilesSorted: {
    if (mode == Mode::FilesSorted)
      m_entries.clear();

    if (sizeSort) {
      std::multimap<size_t, Entry> sort;
      while ((d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
          continue;
        if (noHidden && d->d_name[0] == '.')
          continue;
        hecl::SystemString fp(path);
        fp += '/';
        fp += d->d_name;
        hecl::Sstat st;
        if (hecl::Stat(fp.c_str(), &st) || !S_ISREG(st.st_mode))
          continue;
        sort.emplace(std::make_pair(st.st_size, Entry(std::move(fp), d->d_name, st.st_size, false)));
      }

      if (reverse)
        for (auto it = sort.crbegin(); it != sort.crend(); ++it)
          m_entries.push_back(std::move(it->second));
      else
        for (auto& e : sort)
          m_entries.push_back(std::move(e.second));
    } else {
      std::map<hecl::SystemString, Entry, CaseInsensitiveCompare> sort;
      while ((d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
          continue;
        if (noHidden && d->d_name[0] == '.')
          continue;
        hecl::SystemString fp(path);
        fp += '/';
        fp += d->d_name;
        hecl::Sstat st;
        if (hecl::Stat(fp.c_str(), &st) || !S_ISREG(st.st_mode))
          continue;
        sort.emplace(std::make_pair(d->d_name, Entry(std::move(fp), d->d_name, st.st_size, false)));
      }

      if (reverse)
        for (auto it = sort.crbegin(); it != sort.crend(); ++it)
          m_entries.push_back(std::move(it->second));
      else
        for (auto& e : sort)
          m_entries.push_back(std::move(e.second));
    }

    break;
  }
  }
  closedir(dir);

#endif
}

static std::pair<hecl::SystemString, std::string> NameFromPath(hecl::SystemStringView path) {
  hecl::SystemUTF8Conv utf8(path);
  if (utf8.str().size() == 1 && utf8.str()[0] == '/')
    return {hecl::SystemString(path), "/"};
  size_t lastSlash = utf8.str().rfind('/');
  if (lastSlash != std::string::npos)
    return {hecl::SystemString(path), std::string(utf8.str().cbegin() + lastSlash + 1, utf8.str().cend())};
  else
    return {hecl::SystemString(path), std::string(utf8.str())};
}

std::vector<std::pair<hecl::SystemString, std::string>> GetSystemLocations() {
  std::vector<std::pair<hecl::SystemString, std::string>> ret;
#ifdef WIN32
#if !WINDOWS_STORE
  /* Add the drive names to the listing (as queried by blender) */
  {
    constexpr uint32_t FILE_MAXDIR = 768;
    wchar_t wline[FILE_MAXDIR];
    const uint32_t tmp = GetLogicalDrives();

    for (uint32_t i = 0; i < 26; i++) {
      if ((tmp >> i) & 1) {
        wline[0] = L'A' + i;
        wline[1] = L':';
        wline[2] = L'/';
        wline[3] = L'\0';
        wchar_t* name = nullptr;

        /* Flee from horrible win querying hover floppy drives! */
        if (i > 1) {
          /* Try to get volume label as well... */
          if (GetVolumeInformationW(wline, wline + 4, FILE_MAXDIR - 4, nullptr, nullptr, nullptr, nullptr, 0)) {
            const size_t labelLen = std::wcslen(wline + 4);
            _snwprintf(wline + 4 + labelLen, FILE_MAXDIR - 4 - labelLen, L" (%.2s)", wline);
            name = wline + 4;
          }
        }

        wline[2] = L'\0';
        if (name == nullptr) {
          ret.push_back(NameFromPath(wline));
        } else {
          ret.emplace_back(wline, hecl::WideToUTF8(name));
        }
      }
    }

    /* Adding Desktop and My Documents */
    SystemString wpath;
    SHGetSpecialFolderPathW(nullptr, wline, CSIDL_PERSONAL, 0);
    wpath.assign(wline);
    SanitizePath(wpath);
    ret.push_back(NameFromPath(wpath));
    SHGetSpecialFolderPathW(nullptr, wline, CSIDL_DESKTOPDIRECTORY, 0);
    wpath.assign(wline);
    SanitizePath(wpath);
    ret.push_back(NameFromPath(wpath));
  }
#endif
#else
#ifdef __APPLE__
  {
    hecl::Sstat theStat;
    const char* home = getenv("HOME");

    if (home) {
      ret.push_back(NameFromPath(home));
      std::string desktop(home);
      desktop += "/Desktop";
      if (!hecl::Stat(desktop.c_str(), &theStat))
        ret.push_back(NameFromPath(desktop));
    }

    /* Get mounted volumes better method OSX 10.6 and higher, see: */
    /*https://developer.apple.com/library/mac/#documentation/CoreFOundation/Reference/CFURLRef/Reference/reference.html*/
    /* we get all volumes sorted including network and do not relay on user-defined finder visibility, less confusing */

    CFURLRef cfURL = nullptr;
    CFURLEnumeratorResult result = kCFURLEnumeratorSuccess;
    CFURLEnumeratorRef volEnum =
        CFURLEnumeratorCreateForMountedVolumes(nullptr, kCFURLEnumeratorSkipInvisibles, nullptr);

    while (result != kCFURLEnumeratorEnd) {
      char defPath[1024];

      result = CFURLEnumeratorGetNextURL(volEnum, &cfURL, nullptr);
      if (result != kCFURLEnumeratorSuccess) {
        continue;
      }

      CFURLGetFileSystemRepresentation(cfURL, false, reinterpret_cast<UInt8*>(defPath), std::size(defPath));
      ret.push_back(NameFromPath(defPath));
    }

    CFRelease(volEnum);
  }
#else
  /* unix */
  {
    hecl::Sstat theStat;
    const char* home = getenv("HOME");

    if (home) {
      ret.push_back(NameFromPath(home));
      std::string desktop(home);
      desktop += "/Desktop";
      if (!hecl::Stat(desktop.c_str(), &theStat))
        ret.push_back(NameFromPath(desktop));
    }

    {
      bool found = false;
#ifdef __linux__
      /* Loop over mount points */
      struct mntent* mnt;

      FILE* fp = setmntent(MOUNTED, "r");
      if (fp) {
        while ((mnt = getmntent(fp))) {
          if (strlen(mnt->mnt_fsname) < 4 || strncmp(mnt->mnt_fsname, "/dev", 4))
            continue;

          std::string mntStr(mnt->mnt_dir);
          if (mntStr.size() > 1 && mntStr.back() == '/')
            mntStr.pop_back();
          ret.push_back(NameFromPath(mntStr));

          found = true;
        }
        endmntent(fp);
      }
#endif
      /* Fallback */
      if (!found)
        ret.push_back(NameFromPath("/"));
    }
  }
#endif
#endif
  return ret;
}

std::wstring Char16ToWide(std::u16string_view src) { return std::wstring(src.begin(), src.end()); }

/* recursive mkdir */
#if _WIN32
int RecursiveMakeDir(const SystemChar* dir) {
  SystemChar tmp[1024];

  /* copy path */
  std::wcsncpy(tmp, dir, std::size(tmp));
  const size_t len = std::wcslen(tmp);
  if (len >= std::size(tmp)) {
    return -1;
  }

  /* remove trailing slash */
  if (tmp[len - 1] == '/' || tmp[len - 1] == '\\') {
    tmp[len - 1] = 0;
  }

  /* recursive mkdir */
  SystemChar* p = nullptr;
  Sstat sb;
  for (p = tmp + 1; *p; p++) {
    if (*p == '/' || *p == '\\') {
      *p = 0;
      /* test path */
      if (Stat(tmp, &sb) != 0) {
        /* path does not exist - create directory */
        if (!CreateDirectoryW(tmp, nullptr)) {
          return -1;
        }
      } else if (!S_ISDIR(sb.st_mode)) {
        /* not a directory */
        return -1;
      }
      *p = '/';
    }
  }
  /* test path */
  if (Stat(tmp, &sb) != 0) {
    /* path does not exist - create directory */
    if (!CreateDirectoryW(tmp, nullptr)) {
      return -1;
    }
  } else if (!S_ISDIR(sb.st_mode)) {
    /* not a directory */
    return -1;
  }
  return 0;
}
#else
int RecursiveMakeDir(const SystemChar* dir) {
  SystemChar tmp[1024];

  /* copy path */
  std::strncpy(tmp, dir, std::size(tmp));
  const size_t len = std::strlen(tmp);
  if (len >= std::size(tmp)) {
    return -1;
  }

  /* remove trailing slash */
  if (tmp[len - 1] == '/') {
    tmp[len - 1] = 0;
  }

  /* recursive mkdir */
  SystemChar* p = nullptr;
  Sstat sb;
  for (p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      /* test path */
      if (Stat(tmp, &sb) != 0) {
        /* path does not exist - create directory */
        if (mkdir(tmp, 0755) < 0) {
          return -1;
        }
      } else if (!S_ISDIR(sb.st_mode)) {
        /* not a directory */
        return -1;
      }
      *p = '/';
    }
  }
  /* test path */
  if (Stat(tmp, &sb) != 0) {
    /* path does not exist - create directory */
    if (mkdir(tmp, 0755) < 0) {
      return -1;
    }
  } else if (!S_ISDIR(sb.st_mode)) {
    /* not a directory */
    return -1;
  }
  return 0;
}
#endif

const SystemChar* GetTmpDir() {
#ifdef _WIN32
#if WINDOWS_STORE
  const wchar_t* TMPDIR = nullptr;
#else
  const wchar_t* TMPDIR = _wgetenv(L"TEMP");
  if (!TMPDIR)
    TMPDIR = L"\\Temp";
#endif
#else
  const char* TMPDIR = getenv("TMPDIR");
  if (!TMPDIR)
    TMPDIR = "/tmp";
#endif
  return TMPDIR;
}

#if !WINDOWS_STORE
int RunProcess(const SystemChar* path, const SystemChar* const args[]) {
#ifdef _WIN32
  SECURITY_ATTRIBUTES sattrs = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE consoleOutReadTmp = INVALID_HANDLE_VALUE;
  HANDLE consoleOutWrite = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&consoleOutReadTmp, &consoleOutWrite, &sattrs, 0)) {
    LogModule.report(logvisor::Fatal, FMT_STRING("Error with CreatePipe"));
    return -1;
  }

  HANDLE consoleErrWrite = INVALID_HANDLE_VALUE;
  if (!DuplicateHandle(GetCurrentProcess(), consoleOutWrite, GetCurrentProcess(), &consoleErrWrite, 0, TRUE,
                       DUPLICATE_SAME_ACCESS)) {
    LogModule.report(logvisor::Fatal, FMT_STRING("Error with DuplicateHandle"));
    CloseHandle(consoleOutReadTmp);
    CloseHandle(consoleOutWrite);
    return -1;
  }

  HANDLE consoleOutRead = INVALID_HANDLE_VALUE;
  if (!DuplicateHandle(GetCurrentProcess(), consoleOutReadTmp, GetCurrentProcess(),
                       &consoleOutRead, // Address of new handle.
                       0, FALSE,        // Make it uninheritable.
                       DUPLICATE_SAME_ACCESS)) {
    LogModule.report(logvisor::Fatal, FMT_STRING("Error with DuplicateHandle"));
    CloseHandle(consoleOutReadTmp);
    CloseHandle(consoleOutWrite);
    CloseHandle(consoleErrWrite);
    return -1;
  }

  CloseHandle(consoleOutReadTmp);

  hecl::SystemString cmdLine;
  const SystemChar* const* arg = &args[1];
  while (*arg) {
    cmdLine += _SYS_STR(" \"");
    cmdLine += *arg++;
    cmdLine += _SYS_STR('"');
  }

  STARTUPINFO sinfo = {sizeof(STARTUPINFO)};
  HANDLE nulHandle = CreateFileW(L"nul", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sattrs, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
  sinfo.dwFlags = STARTF_USESTDHANDLES;
  sinfo.hStdInput = nulHandle;
  sinfo.hStdError = consoleErrWrite;
  sinfo.hStdOutput = consoleOutWrite;

  PROCESS_INFORMATION pinfo = {};
  if (!CreateProcessW(path, cmdLine.data(), nullptr, nullptr, TRUE, NORMAL_PRIORITY_CLASS, nullptr, nullptr, &sinfo,
                      &pinfo)) {
    LPWSTR messageBuffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&messageBuffer, 0, nullptr);
    LogModule.report(logvisor::Error, FMT_STRING(L"unable to launch process from {}: {}"), path, messageBuffer);
    LocalFree(messageBuffer);

    CloseHandle(nulHandle);
    CloseHandle(consoleErrWrite);
    CloseHandle(consoleOutWrite);
    CloseHandle(consoleOutRead);
    return -1;
  }

  CloseHandle(nulHandle);
  CloseHandle(consoleErrWrite);
  CloseHandle(consoleOutWrite);

  bool consoleThreadRunning = true;
  auto consoleThread = std::thread([=, &consoleThreadRunning]() {
    CHAR lpBuffer[256];
    DWORD nBytesRead;
    DWORD nCharsWritten;

    while (consoleThreadRunning) {
      if (!ReadFile(consoleOutRead, lpBuffer, sizeof(lpBuffer), &nBytesRead, nullptr) || !nBytesRead) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE)
          break; // pipe done - normal exit path.
        else
          LogModule.report(logvisor::Error, FMT_STRING("Error with ReadFile: {:08X}"), err); // Something bad happened.
      }

      // Display the character read on the screen.
      auto lk = logvisor::LockLog();
      if (!WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), lpBuffer, nBytesRead, &nCharsWritten, nullptr)) {
        // LogModule.report(logvisor::Error, FMT_STRING("Error with WriteConsole: {:08X}"), GetLastError());
      }
    }

    CloseHandle(consoleOutRead);
  });

  WaitForSingleObject(pinfo.hProcess, INFINITE);
  DWORD ret;
  if (!GetExitCodeProcess(pinfo.hProcess, &ret))
    ret = -1;
  consoleThreadRunning = false;
  if (consoleThread.joinable())
    consoleThread.join();

  CloseHandle(pinfo.hProcess);
  CloseHandle(pinfo.hThread);

  return ret;
#else
  pid_t pid = fork();
  if (!pid) {
    closefrom(3);
    execvp(path, (char* const*)args);
    exit(1);
  }
  int ret;
  if (waitpid(pid, &ret, 0) < 0)
    return -1;
  if (WIFEXITED(ret))
    return WEXITSTATUS(ret);
  return -1;
#endif
}
#endif

} // namespace hecl
