// astra_updater.exe
// =================
// Lightweight Windows helper that:
//   1. Waits for the ASTra process to exit (by PID).
//   2. Copies updated files from the extracted update directory to the
//      application install directory.
//   3. Re-launches ASTra.
//   4. Exits.
//
// Usage (launched by ASTra):
//   astra_updater.exe <parent-pid> <update-source-dir> <install-dir>
//
// The process is intentionally minimal – no Qt dependency – so it can be
// compiled as a small standalone Win32 application.

#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>

#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;

// ── logging ──────────────────────────────────────────────────────────────────

static void log(const std::string& msg) {
    // Write to both stdout and a side-car log file.
    std::cout << "[astra_updater] " << msg << "\n";
    std::cout.flush();

    // Optional: append to astra_updater.log in %TEMP%.
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    std::string logPath = std::string(tmp) + "astra_update\\astra_updater.log";
    if (FILE* f = fopen(logPath.c_str(), "a")) {
        fprintf(f, "[astra_updater] %s\n", msg.c_str());
        fclose(f);
    }
}

// ── helpers ───────────────────────────────────────────────────────────────────

// Wait for a process (by PID) to exit, up to timeoutMs milliseconds.
// Returns true if the process exited within the timeout.
static bool waitForProcess(DWORD pid, DWORD timeoutMs = 30'000) {
    HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!hProc) {
        // Process may have already exited.
        log("OpenProcess failed (process may have already exited) – continuing");
        return true;
    }
    DWORD result = WaitForSingleObject(hProc, timeoutMs);
    CloseHandle(hProc);
    return result == WAIT_OBJECT_0;
}

// Recursively copy all files from src to dst, overwriting existing files.
static bool copyFilesRecursive(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(src, ec)) {
        if (ec) {
            log("Directory iteration error: " + ec.message());
            return false;
        }
        if (!entry.is_regular_file()) continue;

        const fs::path rel    = fs::relative(entry.path(), src, ec);
        const fs::path target = dst / rel;

        // Ensure parent directory exists.
        fs::create_directories(target.parent_path(), ec);
        if (ec && ec != std::errc::file_exists) {
            log("mkdir failed: " + ec.message());
            return false;
        }

        fs::copy_file(entry.path(), target,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            log("copy_file failed " + entry.path().string()
                + " → " + target.string() + " : " + ec.message());
            return false;
        }
        log("Copied: " + rel.string());
    }
    return true;
}

// Launch ASTra and detach.
static void launchAstra(const fs::path& installDir) {
    fs::path exe = installDir / "ASTra.exe";
    // Fallback to ASTra.exe naming convention.
    if (!fs::exists(exe))
        exe = installDir / "gf_toolsuite_gui.exe";

    if (!fs::exists(exe)) {
        log("Could not find ASTra executable to relaunch: " + exe.string());
        return;
    }

    const std::wstring wexe = exe.wstring();
    SHELLEXECUTEINFOW sei   = {};
    sei.cbSize              = sizeof(sei);
    sei.fMask               = SEE_MASK_NOASYNC;
    sei.lpFile              = wexe.c_str();
    sei.nShow               = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        log("ShellExecuteExW failed, error: " + std::to_string(GetLastError()));
    } else {
        log("ASTra relaunched: " + exe.string());
    }
}

// ── entry point ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: astra_updater.exe <parent-pid> <update-source-dir> <install-dir>\n";
        return 1;
    }

    const DWORD  parentPid  = static_cast<DWORD>(std::stoul(argv[1]));
    const fs::path updateSrc = fs::path(argv[2]);
    const fs::path installDir = fs::path(argv[3]);

    log("astra_updater started");
    log("PID to wait for : " + std::to_string(parentPid));
    log("Update source   : " + updateSrc.string());
    log("Install dir     : " + installDir.string());

    // ── Step 1: wait for ASTra to exit ─────────────────────────────────────
    log("Waiting for ASTra (PID " + std::to_string(parentPid) + ") to exit…");

    if (!waitForProcess(parentPid, 60'000)) {
        log("Timeout waiting for ASTra – aborting update for safety");
        return 2;
    }
    log("ASTra has exited");

    // Brief extra wait to let file handles fully release on Windows.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ── Step 2: verify update source ───────────────────────────────────────
    std::error_code ec;
    if (!fs::exists(updateSrc, ec) || !fs::is_directory(updateSrc, ec)) {
        log("Update source directory not found: " + updateSrc.string());
        return 3;
    }

    // ── Step 3: copy files ─────────────────────────────────────────────────
    log("Copying update files…");
    if (!copyFilesRecursive(updateSrc, installDir)) {
        log("Update copy failed – ASTra files may be in a broken state");
        return 4;
    }
    log("All files copied successfully");

    // ── Step 4: relaunch ASTra ─────────────────────────────────────────────
    launchAstra(installDir);

    log("astra_updater finished");
    return 0;
}
