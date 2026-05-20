#include "libobsensor/ObSensor.hpp"
#include "usbscsicmd.h"
#include "utils.hpp"
#include <algorithm>
#include <iostream>
#include <thread>
#include <map>
#include <string>
#include <mutex>
#include <set>
#include <vector>
#include <chrono>
#include <cstring>
#include <fstream>

#ifdef WIN32
#include <conio.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define ESC 27

typedef struct PipelineHolder_t
{
    std::shared_ptr<ob::Pipeline> pipeline;
    std::shared_ptr<ob::DeviceInfo> deviceInfo;
} PipelineHolder;

struct DeviceUpgradeContext
{
    std::string uid;
    std::string serialNumber;
    std::string name;
    std::string firmwareVersion;
    bool finalSuccess = false;
    bool finalFailure = false;
    std::string errorMsg;
};

std::recursive_mutex pipelineHolderMutex;
std::map<std::string, std::shared_ptr<PipelineHolder>> pipelineHolderMap;

void handleDeviceConnected(std::shared_ptr<ob::DeviceList> connectList);
void handleDeviceDisconnected(std::shared_ptr<ob::DeviceList> disconnectList);
void printDevicesInfo();
void printSummary(std::vector<DeviceUpgradeContext> &totalDevices);

#ifdef WIN32
bool upgradeSingleDeviceWindows(const std::string &filePath, DeviceUpgradeContext &ctx, std::set<int> &usedDiskSet);
void upgradeRecoveryDevicesWindows(const std::string &filePath, std::vector<DeviceUpgradeContext> &totalDevices, std::set<int> &usedDiskSet);
int runCommandWithTimeout(const std::string &cmd, std::string &output, DWORD timeoutMs);
#else
bool upgradeSingleDeviceLinux(const std::string &filePath, DeviceUpgradeContext &ctx, std::set<std::string> &usedDevicePaths);
void upgradeRecoveryDevicesLinux(const std::string &filePath, std::vector<DeviceUpgradeContext> &totalDevices, std::set<std::string> &usedDevicePaths);
#endif

int main(int argc, char **argv)
try
{
    if (argc != 2)
    {
        std::cerr << "Usage:[app] [firmware path]" << std::endl;
        return -1;
    }

    std::string filePath = std::string(argv[1]);
    // PowerShell trailing-backslash escaping (e.g. "path\") leaves a trailing '"' in argv[1].
    // Strip any trailing quotes first.
    while (!filePath.empty() && filePath.back() == '"') {
        filePath.pop_back();
    }
    // Normalize trailing backslash to forward slash so we only have one separator to check.
    if (!filePath.empty() && filePath.back() == '\\') {
        filePath.back() = '/';
    }
    // Ensure filePath ends with '/' for directory-based firmware path (original behavior).
    // Do not append '/' if the path points to a .zip file, since USBDownloadTool also accepts zip directly.
    if (!filePath.empty() && filePath.back() != '/') {
        if (filePath.size() < 4) {
            filePath += '/';
        } else {
            std::string ext = filePath.substr(filePath.size() - 4);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".zip") {
                filePath += '/';
            }
        }
    }
    // create context
    ob::Context ctx;

    // register device callback
    ctx.setDeviceChangedCallback([](std::shared_ptr<ob::DeviceList> removedList, std::shared_ptr<ob::DeviceList> addedList)
                                 {
        handleDeviceDisconnected(removedList);
        handleDeviceConnected(addedList); });

    // handle current connected devices.
    handleDeviceConnected(ctx.queryDeviceList());

    // Collect device information before upgrade
    std::vector<DeviceUpgradeContext> totalDevices;
    {
        std::lock_guard<std::recursive_mutex> lk(pipelineHolderMutex);
        for (const auto &iter : pipelineHolderMap)
        {
            DeviceUpgradeContext devCtx;
            devCtx.uid = iter.first;
            devCtx.serialNumber = iter.second->deviceInfo->serialNumber();
            devCtx.name = iter.second->deviceInfo->name();
            devCtx.firmwareVersion = iter.second->deviceInfo->firmwareVersion();
            totalDevices.push_back(devCtx);
        }
    }

    if (!totalDevices.empty())
    {
        std::cout << "Devices found:" << std::endl;
        std::cout << "--------------------------------------------------------------------------------" << std::endl;
        for (size_t i = 0; i < totalDevices.size(); ++i)
        {
            std::cout << "[" << i << "] Device: " << totalDevices[i].name
                      << " | SN: " << totalDevices[i].serialNumber
                      << " | Firmware version: " << totalDevices[i].firmwareVersion << std::endl;
        }
        std::cout << "--------------------------------------------------------------------------------" << std::endl;
        std::cout << "\nPress 'U' or 'u' to start upgrading all devices, or Esc to exit." << std::endl;
    }
    else
    {
        std::cout << "\nNo normal mode device found. Will attempt to find recovery mode devices." << std::endl;
        std::cout << "Press 'U' or 'u' to start scanning recovery devices, or Esc to exit." << std::endl;
    }
    while (true)
    {
#ifdef WIN32
        if (_kbhit())
        {
            int key = _getch();
#else
        if (kbhit())
        {
            int key = getch();
#endif
            // Press the esc key to exit
            if (key == ESC)
            {
                return 0;
            }

            if (key == 'u' || key == 'U')
            {
                break;
            }
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::cout << "\nStarting firmware upgrade..." << std::endl;

#ifdef WIN32
    std::set<int> usedDiskSet;
    for (size_t i = 0; i < totalDevices.size(); ++i)
    {
        std::cout << "\nUpgrading device: " << (i + 1) << "/" << totalDevices.size()
                  << " - " << totalDevices[i].name << " | SN: " << totalDevices[i].serialNumber << std::endl;
        if (!upgradeSingleDeviceWindows(filePath, totalDevices[i], usedDiskSet))
        {
            std::cerr << "Failed to upgrade device: " << totalDevices[i].serialNumber
                      << " - " << totalDevices[i].errorMsg << std::endl;
        }
    }
    // After normal devices, scan for any remaining recovery mode devices
    upgradeRecoveryDevicesWindows(filePath, totalDevices, usedDiskSet);
#else
    std::set<std::string> usedDevicePaths;
    for (size_t i = 0; i < totalDevices.size(); ++i)
    {
        std::cout << "\nUpgrading device: " << (i + 1) << "/" << totalDevices.size()
                  << " - " << totalDevices[i].name << " | SN: " << totalDevices[i].serialNumber << std::endl;
        if (!upgradeSingleDeviceLinux(filePath, totalDevices[i], usedDevicePaths))
        {
            std::cerr << "Failed to upgrade device: " << totalDevices[i].serialNumber
                      << " - " << totalDevices[i].errorMsg << std::endl;
        }
    }
    // After normal devices, scan for any remaining recovery mode devices
    upgradeRecoveryDevicesLinux(filePath, totalDevices, usedDevicePaths);
#endif

    printSummary(totalDevices);

    std::cout << "\nPress any key to exit..." << std::endl;
    while (true)
    {
#ifdef WIN32
        if (_kbhit())
        {
            _getch();
            break;
        }
#else
        if (kbhit())
        {
            getch();
            break;
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}
catch (ob::Error &e)
{
    std::cerr << "function:" << e.getName() << "\nargs:" << e.getArgs() << "\nmessage:" << e.getMessage() << "\ntype:" << e.getExceptionType() << std::endl;
    exit(EXIT_FAILURE);
}

/**
 * Handle current connected devices.
 *
 * Iterates over a list of disconnected devices, safely adding them to the pipelineHolderMap.
 *
 * @param connectList A shared pointer to a DeviceList object containing information about all connected devices.
 */
void handleDeviceConnected(std::shared_ptr<ob::DeviceList> connectList)
{
    // Ensure thread safety
    std::lock_guard<std::recursive_mutex> lk(pipelineHolderMutex);

    // Gets the number of connected devices.
    const auto deviceCount = connectList->deviceCount();
    std::cout << "Device connect, deviceCount: " << deviceCount << std::endl;

    // Iterates through all connected devices.
    for (uint32_t i = 0; i < deviceCount; i++)
    {
        auto uid = std::string(connectList->uid(i));
        auto itr = pipelineHolderMap.find(uid);

        // If the device already exists in the pipelineHolderMap.
        if (itr != pipelineHolderMap.end())
        {
            std::cout << "Device connected. device already exist. " << itr->second->deviceInfo << std::endl;
        }
        else
        {
            auto device = connectList->getDevice(i);
            auto deviceInfo = device->getDeviceInfo();
            auto pipeline = std::make_shared<ob::Pipeline>(device);
            auto holder = std::make_shared<PipelineHolder>();
            holder->pipeline = pipeline;
            holder->deviceInfo = deviceInfo;
            // Adds the new PipelineHolder to the pipelineHolderMap.
            pipelineHolderMap.insert({uid, holder});
            std::cout << "Device connected. " << deviceInfo << std::endl;
        }
    }

    // Prints detailed information about all devices.
    printDevicesInfo();
}

/**
 * @brief Handles  devices being disconnected.
 *
 * Iterates over a list of disconnected devices, safely removing them from the pipelineHolderMap.
 *
 * @param disconnectList A shared pointer containing all the information about disconnected devices.
 */
void handleDeviceDisconnected(std::shared_ptr<ob::DeviceList> disconnectList)
{
    // Ensure thread safety
    std::lock_guard<std::recursive_mutex> lk(pipelineHolderMutex);

    // Gets the number of connected devices.
    const auto deviceCount = disconnectList->deviceCount();
    std::cout << "Device disconnect, deviceCount: " << deviceCount << std::endl;

    // Iterates through all disconnected devices.
    for (uint32_t i = 0; i < deviceCount; i++)
    {
        auto uid = std::string(disconnectList->uid(i));
        auto itr = pipelineHolderMap.find(uid);
        if (itr != pipelineHolderMap.end())
        {
            auto deviceInfo = itr->second->deviceInfo;

            // Remove the device from the map
            pipelineHolderMap.erase(uid);
            std::cout << "Device disconnected. " << deviceInfo << std::endl;
        }
        else
        {
            std::cout << "Device disconnect, unresolve deviceUid: " << uid << std::endl;
        }
    }
    printDevicesInfo();
}

void printDevicesInfo()
{
    std::lock_guard<std::recursive_mutex> lk(pipelineHolderMutex);
    for (auto iter : pipelineHolderMap)
    {
        auto holder = iter.second;
        auto deviceInfo = holder->deviceInfo;
        std::cout << "Device name: " << deviceInfo->name() << ", serial number:" << deviceInfo->serialNumber() << ", firmware version:" << deviceInfo->firmwareVersion() << std::endl;
    }
}

static std::string getLatestFirmwareVersionBySN(const std::string &serialNumber)
{
    std::lock_guard<std::recursive_mutex> lk(pipelineHolderMutex);
    for (const auto &iter : pipelineHolderMap)
    {
        if (iter.second->deviceInfo && iter.second->deviceInfo->serialNumber() == serialNumber)
        {
            return iter.second->deviceInfo->firmwareVersion();
        }
    }
    return "";
}

static void waitForDevicesReconnection(const std::vector<DeviceUpgradeContext*> &successDevices)
{
    if (successDevices.empty()) return;
    std::cout << "\nWaiting for upgraded devices to reconnect..." << std::endl;
    // Wait up to 30 seconds (60 * 500ms) for devices to reboot and re-enumerate
    for (int retry = 0; retry < 60; ++retry)
    {
        bool allOnline = true;
        int unknownCount = 0;
        int onlineCount = 0;
        for (const auto *ctx : successDevices)
        {
            if (ctx->serialNumber == "Unknown")
            {
                unknownCount++;
            }
            else if (getLatestFirmwareVersionBySN(ctx->serialNumber).empty())
            {
                allOnline = false;
                break;
            }
        }
        if (unknownCount > 0)
        {
            std::lock_guard<std::recursive_mutex> lk(pipelineHolderMutex);
            onlineCount = static_cast<int>(pipelineHolderMap.size());
            if (onlineCount < unknownCount)
            {
                allOnline = false;
            }
        }
        if (allOnline)
        {
            std::cout << "All upgraded devices are back online." << std::endl;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::cout << "Some devices have not reconnected yet, using pre-upgrade version for those." << std::endl;
}

static void updateDeviceInfoFromOnlineMap(std::vector<DeviceUpgradeContext> &totalDevices)
{
    std::lock_guard<std::recursive_mutex> lk(pipelineHolderMutex);
    std::set<std::string> assignedUIDs;
    for (auto &ctx : totalDevices)
    {
        if (!ctx.finalSuccess) continue;

        // Try to match by serial number first
        if (ctx.serialNumber != "Unknown")
        {
            for (const auto &iter : pipelineHolderMap)
            {
                if (iter.second->deviceInfo &&
                    iter.second->deviceInfo->serialNumber() == ctx.serialNumber)
                {
                    ctx.firmwareVersion = iter.second->deviceInfo->firmwareVersion();
                    assignedUIDs.insert(iter.first);
                    break;
                }
            }
        }
        else
        {
            // Recovery mode device: match any unassigned online device
            for (const auto &iter : pipelineHolderMap)
            {
                if (assignedUIDs.find(iter.first) != assignedUIDs.end())
                    continue;
                if (iter.second->deviceInfo)
                {
                    ctx.serialNumber = iter.second->deviceInfo->serialNumber();
                    ctx.firmwareVersion = iter.second->deviceInfo->firmwareVersion();
                    assignedUIDs.insert(iter.first);
                    break;
                }
            }
        }
    }
}

void printSummary(std::vector<DeviceUpgradeContext> &totalDevices)
{
    std::vector<DeviceUpgradeContext*> successDevices;
    std::vector<DeviceUpgradeContext*> failedDevices;

    for (auto &ctx : totalDevices)
    {
        if (ctx.finalSuccess)
        {
            successDevices.push_back(&ctx);
        }
        else
        {
            failedDevices.push_back(&ctx);
        }
    }

    waitForDevicesReconnection(successDevices);
    updateDeviceInfoFromOnlineMap(totalDevices);

    std::cout << "\nUpgrade Summary:" << std::endl;
    std::cout << "==================================================" << std::endl;

    std::cout << "Success (" << successDevices.size() << "):" << std::endl;
    for (const auto *ctx : successDevices)
    {
        std::string versionStr = ctx->firmwareVersion;
        if (versionStr.empty())
        {
            versionStr = "Unknown (device offline, pre-upgrade version)";
        }
        std::cout << "  - Name: " << ctx->name
                  << " | SN: " << ctx->serialNumber
                  << " | Firmware version: " << versionStr << std::endl;
    }

    std::cout << "\nFailure (" << failedDevices.size() << "):" << std::endl;
    for (const auto *ctx : failedDevices)
    {
        std::cout << "  - Name: " << ctx->name
                  << " | SN: " << ctx->serialNumber
                  << " | Firmware version: " << ctx->firmwareVersion;
        if (!ctx->errorMsg.empty())
        {
            std::cout << " | Error: " << ctx->errorMsg;
        }
        std::cout << std::endl;
    }

    std::cout << "\nUpgrade process completed." << std::endl;
}

#ifdef WIN32
bool upgradeSingleDeviceWindows(const std::string &filePath, DeviceUpgradeContext &ctx, std::set<int> &usedDiskSet)
{
    // 1. Find the device in pipelineHolderMap and set it to recovery mode
    std::shared_ptr<ob::Pipeline> pipeline;
    {
        std::lock_guard<std::recursive_mutex> lk(pipelineHolderMutex);
        auto itr = pipelineHolderMap.find(ctx.uid);
        if (itr == pipelineHolderMap.end())
        {
            ctx.errorMsg = "Device disconnected before upgrade";
            ctx.finalFailure = true;
            return false;
        }
        pipeline = itr->second->pipeline;
    }

    if (!pipeline)
    {
        ctx.errorMsg = "Invalid pipeline";
        ctx.finalFailure = true;
        return false;
    }

    try
    {
        auto device = pipeline->getDevice();
        if (device->isPropertySupported(OB_PROP_BOOT_INTO_RECOVERY_MODE_BOOL, OB_PERMISSION_WRITE))
        {
            device->setBoolProperty(OB_PROP_BOOT_INTO_RECOVERY_MODE_BOOL, true);
            std::cout << "Device set to recovery mode. Waiting for device to reboot..." << std::endl;
        }
        else
        {
            ctx.errorMsg = "Device does not support recovery mode";
            ctx.finalFailure = true;
            return false;
        }
    }
    catch (const std::exception &e)
    {
        // Device may reboot immediately after setting recovery mode, causing control transfer to fail.
        // This is expected behavior, so we ignore the exception and continue waiting for the SCSI device.
        std::cout << "Warning: Device may have rebooted into recovery mode (" << e.what() << "), continuing..." << std::endl;
    }
    catch (...)
    {
        // Same as above - expected behavior when device reboots into recovery mode.
        std::cout << "Warning: Device may have rebooted into recovery mode, continuing..." << std::endl;
    }

    // 2. Wait for the device to appear as a SCSI recovery device.
    // Before scanning, find all OTHER recovery devices currently online
    // so we don't accidentally pick them up instead of the device we just rebooted.
    std::set<int> otherRecoveryDisks;
    {
        std::set<int> tempExclude = usedDiskSet;
        while (true) {
            int ds = DEV_STATE_UNKNOWN, dn = 0;
            HANDLE h = USB_ScsiFindDevice(&ds, &dn, tempExclude);
            if (!h || ds == DEV_STATE_UNKNOWN) break;
            tempExclude.insert(dn);
            otherRecoveryDisks.insert(dn);
        }
    }
    std::set<int> scanExclude = usedDiskSet;
    scanExclude.insert(otherRecoveryDisks.begin(), otherRecoveryDisks.end());

    HANDLE handle = NULL;
    int devState = DEV_STATE_UNKNOWN;
    int diskNumber = 0;
    bool found = false;

    // Wait up to 30 seconds (60 * 500ms)
    for (int retry = 0; retry < 60; ++retry)
    {
        handle = USB_ScsiFindDevice(&devState, &diskNumber, scanExclude);
        if (handle && devState != DEV_STATE_UNKNOWN)
        {
            found = true;
            usedDiskSet.insert(diskNumber);
            std::cout << "Recovery device found at disk " << (char)diskNumber << std::endl;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (!found)
    {
        ctx.errorMsg = "Device did not enter recovery mode within timeout";
        ctx.finalFailure = true;
        return false;
    }

    // 3. Execute the firmware upgrade tool synchronously with timeout (5 minutes)
    std::string cmd = "USBDownloadTool.exe \"" + filePath + "\" " + std::to_string(diskNumber);
    std::cout << "Executing: " << cmd << std::endl;

    std::string cmdOutput;
    int status = runCommandWithTimeout(cmd, cmdOutput, 300000);  // 5 minutes timeout
    if (status == -1 && cmdOutput.empty())
    {
        ctx.errorMsg = "Failed to execute USBDownloadTool.exe or process creation failed";
        ctx.finalFailure = true;
        return false;
    }

    if (status != 0)
    {
        ctx.errorMsg = "Firmware upgrade tool returned error code: " + std::to_string(status);
        ctx.finalFailure = true;
        return false;
    }

    // USBDownloadTool.exe may return 0 even when upgrade actually failed (e.g. open script fail).
    // Check output for known failure keywords.
    if (cmdOutput.find("open script fail") != std::string::npos)
    {
        ctx.errorMsg = "Firmware upgrade failed: script not found (check firmware path)";
        ctx.finalFailure = true;
        return false;
    }
    if (cmdOutput.find("load fail") != std::string::npos)
    {
        ctx.errorMsg = "Firmware upgrade failed: one or more firmware files failed to load";
        ctx.finalFailure = true;
        return false;
    }

    ctx.finalSuccess = true;
    return true;
}

void upgradeRecoveryDevicesWindows(const std::string &filePath, std::vector<DeviceUpgradeContext> &totalDevices, std::set<int> &usedDiskSet)
{
    std::cout << "Scanning for recovery mode devices..." << std::endl;

    int retryTimes = 10;
    int deviceIndex = 0;
    while (true)
    {
        HANDLE handle = NULL;
        int devState = DEV_STATE_UNKNOWN;
        int diskNumber = 0;

        handle = USB_ScsiFindDevice(&devState, &diskNumber, usedDiskSet);

        if (handle)
        {
            retryTimes = 10;
            usedDiskSet.insert(diskNumber);
            deviceIndex++;

            DeviceUpgradeContext ctx;
            ctx.name = "Femto Bolt (Recovery)";
            ctx.serialNumber = "Unknown";
            ctx.firmwareVersion = "Unknown";

            std::cout << "\nUpgrading recovery device: " << deviceIndex << " - disk " << (char)diskNumber << std::endl;

            std::string cmd = "USBDownloadTool.exe \"" + filePath + "\" " + std::to_string(diskNumber);
            std::cout << "Executing: " << cmd << std::endl;

            std::string cmdOutput;
            int status = runCommandWithTimeout(cmd, cmdOutput, 300000);  // 5 minutes timeout
            if (status == -1 && cmdOutput.empty())
            {
                ctx.errorMsg = "Failed to execute USBDownloadTool.exe or process creation failed";
                ctx.finalFailure = true;
                totalDevices.push_back(ctx);
                continue;
            }

            if (status != 0)
            {
                ctx.errorMsg = "Firmware upgrade tool returned error code: " + std::to_string(status);
                ctx.finalFailure = true;
            }
            else if (cmdOutput.find("open script fail") != std::string::npos)
            {
                ctx.errorMsg = "Firmware upgrade failed: script not found (check firmware path)";
                ctx.finalFailure = true;
            }
            else if (cmdOutput.find("load fail") != std::string::npos)
            {
                ctx.errorMsg = "Firmware upgrade failed: one or more firmware files failed to load";
                ctx.finalFailure = true;
            }
            else
            {
                ctx.finalSuccess = true;
            }
            totalDevices.push_back(ctx);
        }

        if (!handle && --retryTimes == 0)
        {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (deviceIndex == 0)
    {
        std::cerr << "No recovery mode device found." << std::endl;
    }
}

int runCommandWithTimeout(const std::string &cmd, std::string &output, DWORD timeoutMs)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hRead = NULL, hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
    {
        return -1;
    }

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hWrite;
    si.hStdOutput = hWrite;
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return -1;
    }

    CloseHandle(hWrite);

    std::thread reader([&]() {
        char buf[1024];
        DWORD bytesRead = 0;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0)
        {
            buf[bytesRead] = '\0';
            output += buf;
            // Print output in real time so the user knows the upgrade is progressing
            std::cout << buf << std::flush;
        }
    });

    DWORD waitResult = WaitForSingleObject(pi.hProcess, timeoutMs);
    int exitCode = -1;
    if (waitResult == WAIT_TIMEOUT)
    {
        std::cerr << "Command timed out after " << timeoutMs << " ms, terminating process..." << std::endl;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        exitCode = -1;
    }
    else if (waitResult == WAIT_OBJECT_0)
    {
        DWORD ec = 0;
        if (GetExitCodeProcess(pi.hProcess, &ec))
        {
            exitCode = static_cast<int>(ec);
        }
    }

    // Close read handle so ReadFile returns and reader thread exits
    CloseHandle(hRead);
    if (reader.joinable())
    {
        reader.join();
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode;
}
#endif

#ifndef WIN32
int runCommandWithTimeoutLinux(const std::string &cmd, std::string &output, int timeoutMs)
{
    int stdoutPipe[2];
    if (pipe(stdoutPipe) == -1) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        return -1;
    }
    if (pid == 0) {
        close(stdoutPipe[0]);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stdoutPipe[1], STDERR_FILENO);
        close(stdoutPipe[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
        _exit(127);
    }
    close(stdoutPipe[1]);

    int flags = fcntl(stdoutPipe[0], F_GETFL, 0);
    fcntl(stdoutPipe[0], F_SETFL, flags | O_NONBLOCK);

    auto start = std::chrono::steady_clock::now();
    char buf[1024];
    int exitCode = -1;
    bool processExited = false;
    int status = 0;

    while (true) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == -1) break;

        if (result == pid) {
            processExited = true;
            if (WIFEXITED(status)) {
                exitCode = WEXITSTATUS(status);
            } else {
                exitCode = -1;
            }
        }

        ssize_t n = read(stdoutPipe[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            output += buf;
            std::cout << buf << std::flush;
        }

        if (processExited) {
            while (true) {
                n = read(stdoutPipe[0], buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    output += buf;
                    std::cout << buf << std::flush;
                } else {
                    break;
                }
            }
            close(stdoutPipe[0]);
            return exitCode;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeoutMs) {
            std::cerr << "Command timed out after " << timeoutMs << " ms, terminating process..." << std::endl;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            close(stdoutPipe[0]);
            return -1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    close(stdoutPipe[0]);
    return -1;
}

static bool isFemtoBoltRecoveryDevice(const std::string &devicePath)
{
    // devicePath is like "/dev/sg1" or "/dev/sdb"
    size_t pos = devicePath.rfind('/');
    if (pos == std::string::npos) return false;
    std::string devName = devicePath.substr(pos + 1);

    // Try scsi_generic sysfs first (for /dev/sg*)
    std::string vendorPath = "/sys/class/scsi_generic/" + devName + "/device/vendor";
    std::ifstream fs(vendorPath);
    if (!fs) {
        // Fallback: try scsi_disk sysfs (for /dev/sd*)
        vendorPath = "/sys/class/scsi_disk/" + devName + "/device/vendor";
        fs.open(vendorPath);
        if (!fs) return false;
    }

    std::string vendor;
    fs >> vendor;
    return (vendor.size() >= 2 && vendor[0] == 'G' && vendor[1] == 'C');
}

bool upgradeSingleDeviceLinux(const std::string &filePath, DeviceUpgradeContext &ctx, std::set<std::string> &usedDevicePaths)
{

    // 1. Find the device in pipelineHolderMap and set it to recovery mode
    std::shared_ptr<ob::Pipeline> pipeline;
    {
        std::lock_guard<std::recursive_mutex> lk(pipelineHolderMutex);
        auto itr = pipelineHolderMap.find(ctx.uid);
        if (itr == pipelineHolderMap.end())
        {
            ctx.errorMsg = "Device disconnected before upgrade";
            ctx.finalFailure = true;
            return false;
        }
        pipeline = itr->second->pipeline;
    }

    if (!pipeline)
    {
        ctx.errorMsg = "Invalid pipeline";
        ctx.finalFailure = true;
        return false;
    }

    try
    {
        auto device = pipeline->getDevice();
        if (device->isPropertySupported(OB_PROP_BOOT_INTO_RECOVERY_MODE_BOOL, OB_PERMISSION_WRITE))
        {
            device->setBoolProperty(OB_PROP_BOOT_INTO_RECOVERY_MODE_BOOL, true);
            std::cout << "Device set to recovery mode. Waiting for device to reboot..." << std::endl;
        }
        else
        {
            ctx.errorMsg = "Device does not support recovery mode";
            ctx.finalFailure = true;
            return false;
        }
    }
    catch (const std::exception &e)
    {
        // Device may reboot immediately after setting recovery mode, causing control transfer to fail.
        // This is expected behavior, so we ignore the exception and continue waiting for the SCSI device.
        std::cout << "Warning: Device may have rebooted into recovery mode (" << e.what() << "), continuing..." << std::endl;
    }
    catch (...)
    {
        // Same as above - expected behavior when device reboots into recovery mode.
        std::cout << "Warning: Device may have rebooted into recovery mode, continuing..." << std::endl;
    }

    // Before waiting, find all OTHER recovery devices currently online
    // so we don't accidentally pick them up instead of the device we just rebooted.
    std::set<std::string> otherRecoveryDevices;
    {
        const std::string devDir = "/dev/";
        const std::vector<std::string> devPrefixes = {"sg", "sd"};
        DIR *dir = opendir(devDir.c_str());
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string filename(entry->d_name);
                for (const auto &prefix : devPrefixes) {
                    if (filename.compare(0, prefix.size(), prefix) == 0) {
                        std::string devicePath = devDir + filename;
                        if (usedDevicePaths.find(devicePath) == usedDevicePaths.end() &&
                            isFemtoBoltRecoveryDevice(devicePath)) {
                            otherRecoveryDevices.insert(devicePath);
                        }
                        break;
                    }
                }
            }
            closedir(dir);
        }
    }

    // 2. Wait for the device to enter recovery mode
    // Wait 10 seconds for the device to reboot
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // 3. Scan for SCSI generic devices, skipping already upgraded ones and other pre-existing recovery devices
    const std::string devDir = "/dev/";
    const std::vector<std::string> devPrefixes = {"sg", "sd"};

    DIR *dir = opendir(devDir.c_str());
    if (dir == nullptr)
    {
        ctx.errorMsg = "Failed to open /dev directory";
        ctx.finalFailure = true;
        return false;
    }

    std::string targetPath;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        std::string filename(entry->d_name);
        for (const auto &prefix : devPrefixes)
        {
            if (filename.compare(0, prefix.size(), prefix) == 0)
            {
                std::string devicePath = devDir + filename;
                if (usedDevicePaths.find(devicePath) == usedDevicePaths.end() &&
                    otherRecoveryDevices.find(devicePath) == otherRecoveryDevices.end() &&
                    isFemtoBoltRecoveryDevice(devicePath))
                {
                    targetPath = devicePath;
                    break;
                }
            }
        }
        if (!targetPath.empty())
            break;
    }
    closedir(dir);

    if (targetPath.empty())
    {
        ctx.errorMsg = "No recovery device found";
        ctx.finalFailure = true;
        return false;
    }

    usedDevicePaths.insert(targetPath);
    std::cout << "Recovery device found at: " << targetPath << std::endl;

    // 4. Execute the firmware upgrade tool synchronously with timeout (5 minutes)
    std::string cmd = "./usbdownload \"" + targetPath + "\" \"" + filePath + "\"";
    std::cout << "Executing: " << cmd << std::endl;

    std::string cmdOutput;
    int status = runCommandWithTimeoutLinux(cmd, cmdOutput, 300000); // 5 minutes
    if (status == -1 && cmdOutput.empty())
    {
        ctx.errorMsg = "Failed to execute usbdownload or process creation failed";
        ctx.finalFailure = true;
        return false;
    }

    if (status != 0)
    {
        ctx.errorMsg = "Firmware upgrade tool returned error code: " + std::to_string(status);
        ctx.finalFailure = true;
        return false;
    }

    // usbdownload may return 0 even when errors occurred (e.g. permission denied, RDONLY).
    // Check output for known failure keywords.
    if (cmdOutput.find("RDONLY") != std::string::npos ||
        cmdOutput.find("error") != std::string::npos ||
        cmdOutput.find("load fail") != std::string::npos ||
        cmdOutput.find("open script fail") != std::string::npos)
    {
        ctx.errorMsg = "Firmware upgrade failed: device I/O error or permission denied (try sudo)";
        ctx.finalFailure = true;
        return false;
    }

    ctx.finalSuccess = true;
    return true;
}

void upgradeRecoveryDevicesLinux(const std::string &filePath, std::vector<DeviceUpgradeContext> &totalDevices, std::set<std::string> &usedDevicePaths)
{
    std::cout << "Scanning for recovery mode devices..." << std::endl;

    // Wait for devices to appear in recovery mode
    std::this_thread::sleep_for(std::chrono::seconds(10));

    const std::string devDir = "/dev/";
    const std::vector<std::string> devPrefixes = {"sg", "sd"};

    DIR *dir = opendir(devDir.c_str());
    if (dir == nullptr)
    {
        std::cerr << "Failed to open /dev directory" << std::endl;
        return;
    }

    std::vector<std::string> devicePaths;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        std::string filename(entry->d_name);
        for (const auto &prefix : devPrefixes)
        {
            if (filename.compare(0, prefix.size(), prefix) == 0)
            {
                std::string devicePath = devDir + filename;
                if (usedDevicePaths.find(devicePath) == usedDevicePaths.end() &&
                    isFemtoBoltRecoveryDevice(devicePath))
                {
                    devicePaths.push_back(devicePath);
                }
                break;
            }
        }
    }
    closedir(dir);

    if (devicePaths.empty())
    {
        std::cerr << "No recovery mode device found." << std::endl;
        return;
    }

    int deviceIndex = 0;
    for (const auto &path : devicePaths)
    {
        usedDevicePaths.insert(path);
        deviceIndex++;

        DeviceUpgradeContext ctx;
        ctx.name = "Femto Bolt (Recovery)";
        ctx.serialNumber = "Unknown";
        ctx.firmwareVersion = "Unknown";

        std::cout << "\nUpgrading recovery device: " << deviceIndex << " - " << path << std::endl;

        std::string cmd = "./usbdownload \"" + path + "\" \"" + filePath + "\"";
        std::cout << "Executing: " << cmd << std::endl;

        std::string cmdOutput;
        int status = runCommandWithTimeoutLinux(cmd, cmdOutput, 300000); // 5 minutes
        if (status == -1 && cmdOutput.empty())
        {
            ctx.errorMsg = "Failed to execute usbdownload or process creation failed";
            ctx.finalFailure = true;
            totalDevices.push_back(ctx);
            continue;
        }

        if (status != 0)
        {
            ctx.errorMsg = "Firmware upgrade tool returned error code: " + std::to_string(status);
            ctx.finalFailure = true;
        }
        else if (cmdOutput.find("RDONLY") != std::string::npos ||
                 cmdOutput.find("error") != std::string::npos ||
                 cmdOutput.find("load fail") != std::string::npos ||
                 cmdOutput.find("open script fail") != std::string::npos)
        {
            ctx.errorMsg = "Firmware upgrade failed: device I/O error or permission denied (try sudo)";
            ctx.finalFailure = true;
        }
        else
        {
            ctx.finalSuccess = true;
        }
        totalDevices.push_back(ctx);
    }
}
#endif
