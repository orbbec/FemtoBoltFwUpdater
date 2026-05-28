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
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>

#ifdef WIN32
#include <conio.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define ESC 27

struct PipelineHolder
{
    std::shared_ptr<ob::DeviceInfo> deviceInfo;
};

enum class UpgradeResult
{
    Pending,
    Success,
    Failure
};

struct DeviceUpgradeContext
{
    std::string uid;
    std::string serialNumber;
    std::string name;
    std::string firmwareVersion;
    UpgradeResult result = UpgradeResult::Pending;
    std::string errorMsg;
};

std::recursive_mutex pipelineHolderMutex;
std::map<std::string, std::shared_ptr<PipelineHolder>> pipelineHolderMap;

void handleDeviceConnected(std::shared_ptr<ob::DeviceList> connectList);
void handleDeviceDisconnected(std::shared_ptr<ob::DeviceList> disconnectList);
void printDevicesInfo();
void printSummary(std::vector<DeviceUpgradeContext> &totalDevices);


static std::string normalizeFirmwarePath(const std::string &rawPath)
{
    std::string path = rawPath;
    // PowerShell trailing-backslash escaping (e.g. "path\") leaves a trailing '"' in argv[1].
    while (!path.empty() && path.back() == '"') {
        path.pop_back();
    }
    // Normalize trailing backslash to forward slash.
    if (!path.empty() && path.back() == '\\') {
        path.back() = '/';
    }
    // Ensure directory paths end with '/' but do not append to .zip files.
    if (!path.empty() && path.back() != '/') {
        if (path.size() < 4) {
            path += '/';
        } else {
            std::string ext = path.substr(path.size() - 4);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".zip") {
                path += '/';
            }
        }
    }
    return path;
}

#ifdef WIN32
static int waitForKeyPress()
{
    while (true) {
        if (_kbhit()) {
            return _getch();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
#else
static int waitForKeyPress()
{
    while (true) {
        if (kbhit()) {
            return getch();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
#endif

#ifdef WIN32
bool upgradeSingleDeviceWindows(ob::Context &sdkCtx, const std::string &filePath, DeviceUpgradeContext &ctx, std::set<int> &usedDiskSet);
void upgradeRecoveryDevicesWindows(ob::Context &sdkCtx, const std::string &filePath, std::vector<DeviceUpgradeContext> &totalDevices, std::set<int> &usedDiskSet);
int runCommandWithTimeout(const std::string &cmd, std::string &output, DWORD timeoutMs);
#else
bool upgradeSingleDeviceLinux(ob::Context &sdkCtx, const std::string &filePath, DeviceUpgradeContext &ctx, std::set<std::string> &usedDevicePaths);
void upgradeRecoveryDevicesLinux(ob::Context &sdkCtx, const std::string &filePath, std::vector<DeviceUpgradeContext> &totalDevices, std::set<std::string> &usedDevicePaths);
#endif

int main(int argc, char **argv)
try
{
    if (argc < 2 || argc > 3)
    {
        std::cerr << "Usage:[app] [firmware path] [--auto]" << std::endl;
        return -1;
    }

    std::string filePath = normalizeFirmwarePath(argv[1]);

    // Non-interactive mode: auto-confirm prompts, no key press required.
    bool autoMode = (argc >= 3 && std::string(argv[2]) == "--auto");

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
    if (!autoMode)
    {
        while (true)
        {
            int key = waitForKeyPress();
            if (key == ESC)
            {
                return 0;
            }
            if (key == 'u' || key == 'U')
            {
                break;
            }
        }
    }
    else
    {
        std::cout << "[Auto mode] Starting upgrade automatically..." << std::endl;
    }

    std::cout << "\nStarting firmware upgrade..." << std::endl;

#ifdef WIN32
    std::set<int> usedDiskSet;
    for (size_t i = 0; i < totalDevices.size(); ++i)
    {
        std::cout << "\nUpgrading device: " << (i + 1) << "/" << totalDevices.size()
                  << " - " << totalDevices[i].name << " | SN: " << totalDevices[i].serialNumber << std::endl;
        if (!upgradeSingleDeviceWindows(ctx, filePath, totalDevices[i], usedDiskSet))
        {
            std::cerr << "Failed to upgrade device: " << totalDevices[i].serialNumber
                      << " - " << totalDevices[i].errorMsg << std::endl;
        }
    }
    // After normal devices, scan for any remaining recovery mode devices
    upgradeRecoveryDevicesWindows(ctx, filePath, totalDevices, usedDiskSet);
#else
    std::set<std::string> usedDevicePaths;
    for (size_t i = 0; i < totalDevices.size(); ++i)
    {
        std::cout << "\nUpgrading device: " << (i + 1) << "/" << totalDevices.size()
                  << " - " << totalDevices[i].name << " | SN: " << totalDevices[i].serialNumber << std::endl;
        if (!upgradeSingleDeviceLinux(ctx, filePath, totalDevices[i], usedDevicePaths))
        {
            std::cerr << "Failed to upgrade device: " << totalDevices[i].serialNumber
                      << " - " << totalDevices[i].errorMsg << std::endl;
        }
    }
    // After normal devices, scan for any remaining recovery mode devices
    upgradeRecoveryDevicesLinux(ctx, filePath, totalDevices, usedDevicePaths);
#endif

    // Suppress further device connect/disconnect logs before printing Summary
    // so that background SDK events do not interleave with the final output.
    ctx.setDeviceChangedCallback([](std::shared_ptr<ob::DeviceList>, std::shared_ptr<ob::DeviceList>) {});

    printSummary(totalDevices);

    if (!autoMode)
    {
        std::cout << "\nPress any key to exit..." << std::endl;
        waitForKeyPress();
    }

    return 0;
}
catch (ob::Error &e)
{
    std::cerr << "function:" << e.getName() << "\nargs:" << e.getArgs() << "\nmessage:" << e.getMessage() << "\ntype:" << e.getExceptionType() << std::endl;
    exit(EXIT_FAILURE);
}
catch (const std::exception &e)
{
    std::cerr << "Unhandled exception: " << e.what() << std::endl;
    exit(EXIT_FAILURE);
}
catch (...)
{
    std::cerr << "Unknown fatal error occurred." << std::endl;
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
            auto holder = std::make_shared<PipelineHolder>();
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

static std::set<std::string> getOnlineUIDs()
{
    std::lock_guard<std::recursive_mutex> lk(pipelineHolderMutex);
    std::set<std::string> uids;
    for (const auto &iter : pipelineHolderMap) {
        uids.insert(iter.first);
    }
    return uids;
}

static bool waitForDeviceByCriteria(const std::string &expectedSN, int timeoutSeconds, std::string &outSN, std::string &outFW)
{
    int retries = timeoutSeconds * 2;
    for (int i = 0; i < retries; ++i) {
        {
            std::lock_guard<std::recursive_mutex> lk(pipelineHolderMutex);
            for (const auto &iter : pipelineHolderMap) {
                auto di = iter.second->deviceInfo;
                if (!di) continue;

                if (expectedSN == "Unknown" || di->serialNumber() == expectedSN) {
                    outSN = di->serialNumber();
                    outFW = di->firmwareVersion();
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

static bool waitForNewDevice(const std::set<std::string> &excludeUIDs, int timeoutSeconds, std::string &outSN, std::string &outFW)
{
    int retries = timeoutSeconds * 2;
    for (int i = 0; i < retries; ++i) {
        {
            std::lock_guard<std::recursive_mutex> lk(pipelineHolderMutex);
            for (const auto &iter : pipelineHolderMap) {
                if (excludeUIDs.find(iter.first) != excludeUIDs.end()) continue;
                auto di = iter.second->deviceInfo;
                if (di) {
                    outSN = di->serialNumber();
                    outFW = di->firmwareVersion();
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

static void rebootDeviceBySN(ob::Context &sdkCtx, const std::string &sn)
{
    try {
        auto devList = sdkCtx.queryDeviceList();
        for (uint32_t i = 0; i < devList->deviceCount(); ++i) {
            auto device = devList->getDevice(i);
            auto di = device->getDeviceInfo();
            if (di && di->serialNumber() == sn) {
                std::cout << "Rebooting device: " << sn << "..." << std::endl;
                device->reboot();
                break;
            }
        }
    }
    catch (const ob::Error &e) {
        std::cout << "Device " << sn
                  << " may have rebooted (" << e.getMessage() << ")" << std::endl;
    }
    catch (const std::exception &e) {
        std::cerr << "Failed to reboot device " << sn
                  << ": " << e.what() << std::endl;
    }
}

static void finalizeDeviceAfterUpgrade(ob::Context &sdkCtx, DeviceUpgradeContext &ctx, const std::set<std::string> &preFlashUIDs)
{
    if (ctx.result != UpgradeResult::Success) return;

    std::cout << "\nWaiting for device to reconnect after upgrade..." << std::endl;

    std::string sn, fw;
    // Step 1: Wait for device to auto-reboot back to normal mode after flash.
    bool found = false;
    if (ctx.serialNumber != "Unknown") {
        found = waitForDeviceByCriteria(ctx.serialNumber, 60, sn, fw);
    } else {
        found = waitForNewDevice(preFlashUIDs, 60, sn, fw);
    }
    if (!found) {
        std::cout << "Warning: Device did not reconnect within timeout." << std::endl;
        return;
    }

    ctx.serialNumber = sn;
    ctx.firmwareVersion = fw;

    // Step 2: Always perform an active reboot to ensure the serial number stabilizes.
    std::cout << "Device reconnected (SN: " << ctx.serialNumber
              << "). Performing final reboot..." << std::endl;
    rebootDeviceBySN(sdkCtx, ctx.serialNumber);

    // Step 3: Wait for device to come back after the explicit reboot.
    // Snapshot current UIDs so we can detect the newly reconnected device.
    std::set<std::string> uidsBefore = getOnlineUIDs();

    std::cout << "Waiting for device to reconnect after final reboot..." << std::endl;
    if (waitForNewDevice(uidsBefore, 60, sn, fw)) {
        ctx.serialNumber = sn;
        ctx.firmwareVersion = fw;
        std::cout << "Device back online (SN: " << ctx.serialNumber
                  << ", FW: " << ctx.firmwareVersion << ")." << std::endl;
    } else {
        std::cout << "Warning: Device did not reconnect after final reboot." << std::endl;
    }
}

void printSummary(std::vector<DeviceUpgradeContext> &totalDevices)
{
    std::vector<DeviceUpgradeContext*> successDevices;
    std::vector<DeviceUpgradeContext*> failedDevices;

    for (auto &ctx : totalDevices)
    {
        if (ctx.result == UpgradeResult::Success)
        {
            successDevices.push_back(&ctx);
        }
        else if (ctx.result == UpgradeResult::Failure)
        {
            failedDevices.push_back(&ctx);
        }
    }

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
bool upgradeSingleDeviceWindows(ob::Context &sdkCtx, const std::string &filePath, DeviceUpgradeContext &ctx, std::set<int> &usedDiskSet)
{
    // 1. Re-enumerate the target device and create a temporary Pipeline to set recovery mode.
    std::shared_ptr<ob::Device> targetDevice;
    {
        auto devList = sdkCtx.queryDeviceList();
        for (uint32_t i = 0; i < devList->deviceCount(); ++i)
        {
            if (std::string(devList->uid(i)) == ctx.uid)
            {
                targetDevice = devList->getDevice(i);
                break;
            }
        }
    }

    if (!targetDevice)
    {
        ctx.errorMsg = "Device disconnected before upgrade";
        ctx.result = UpgradeResult::Failure;
        return false;
    }

    std::shared_ptr<ob::Pipeline> pipeline;
    try
    {
        pipeline = std::make_shared<ob::Pipeline>(targetDevice);
    }
    catch (const std::exception &e)
    {
        ctx.errorMsg = std::string("Failed to initialize device pipeline: ") + e.what();
        ctx.result = UpgradeResult::Failure;
        return false;
    }
    catch (...)
    {
        ctx.errorMsg = "Failed to initialize device pipeline: unknown error";
        ctx.result = UpgradeResult::Failure;
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
            ctx.result = UpgradeResult::Failure;
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
        ctx.result = UpgradeResult::Failure;
        return false;
    }

    // The device will reboot back to normal mode after the flash completes,
    // causing its SCSI disk to disappear. We must remove the disk number from
    // usedDiskSet on exit so that another device can reuse the same disk letter.
    struct DiskGuard
    {
        std::set<int> *set;
        int disk;
        DiskGuard(std::set<int> *s, int d) : set(s), disk(d) {}
        ~DiskGuard() { if (set) set->erase(disk); }
    } diskGuard(&usedDiskSet, diskNumber);

    // Snapshot online UIDs before flash so finalizeDeviceAfterUpgrade can detect the reconnected device.
    std::set<std::string> preFlashUIDs = getOnlineUIDs();

    // 3. Execute the firmware upgrade tool synchronously with timeout (5 minutes)
    std::string cmd = "USBDownloadTool.exe \"" + filePath + "\" " + std::to_string(diskNumber);
    std::cout << "Executing: " << cmd << std::endl;

    std::string cmdOutput;
    int status = runCommandWithTimeout(cmd, cmdOutput, 300000);  // 5 minutes timeout
    if (status == -1 && cmdOutput.empty())
    {
        ctx.errorMsg = "Failed to execute USBDownloadTool.exe or process creation failed";
        ctx.result = UpgradeResult::Failure;
        return false;
    }

    if (status != 0)
    {
        ctx.errorMsg = "Firmware upgrade tool returned error code: " + std::to_string(status);
        ctx.result = UpgradeResult::Failure;
        return false;
    }

    // USBDownloadTool.exe may return 0 even when upgrade actually failed (e.g. open script fail).
    // Check output for known failure keywords.
    if (cmdOutput.find("open script fail") != std::string::npos)
    {
        ctx.errorMsg = "Firmware upgrade failed: script not found (check firmware path)";
        ctx.result = UpgradeResult::Failure;
        return false;
    }
    if (cmdOutput.find("load fail") != std::string::npos)
    {
        ctx.errorMsg = "Firmware upgrade failed: one or more firmware files failed to load";
        ctx.result = UpgradeResult::Failure;
        return false;
    }

    ctx.result = UpgradeResult::Success;
    finalizeDeviceAfterUpgrade(sdkCtx, ctx, preFlashUIDs);
    return true;
}

void upgradeRecoveryDevicesWindows(ob::Context &sdkCtx, const std::string &filePath, std::vector<DeviceUpgradeContext> &totalDevices, std::set<int> &usedDiskSet)
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

            // Snapshot online UIDs before flash so finalizeDeviceAfterUpgrade can detect the reconnected device.
            std::set<std::string> preFlashUIDs = getOnlineUIDs();

            std::string cmd = "USBDownloadTool.exe \"" + filePath + "\" " + std::to_string(diskNumber);
            std::cout << "Executing: " << cmd << std::endl;

            std::string cmdOutput;
            int status = runCommandWithTimeout(cmd, cmdOutput, 300000);  // 5 minutes timeout
            if (status == -1 && cmdOutput.empty())
            {
                ctx.errorMsg = "Failed to execute USBDownloadTool.exe or process creation failed";
                ctx.result = UpgradeResult::Failure;
                totalDevices.push_back(ctx);
                continue;
            }

            if (status != 0)
            {
                ctx.errorMsg = "Firmware upgrade tool returned error code: " + std::to_string(status);
                ctx.result = UpgradeResult::Failure;
            }
            else if (cmdOutput.find("open script fail") != std::string::npos)
            {
                ctx.errorMsg = "Firmware upgrade failed: script not found (check firmware path)";
                ctx.result = UpgradeResult::Failure;
            }
            else if (cmdOutput.find("load fail") != std::string::npos)
            {
                ctx.errorMsg = "Firmware upgrade failed: one or more firmware files failed to load";
                ctx.result = UpgradeResult::Failure;
            }
            else
            {
                ctx.result = UpgradeResult::Success;
            }
            totalDevices.push_back(ctx);
            if (ctx.result == UpgradeResult::Success) {
                finalizeDeviceAfterUpgrade(sdkCtx, totalDevices.back(), preFlashUIDs);
            }
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
        // ReadFile returns FALSE when the pipe is closed (ERROR_BROKEN_PIPE) or on error.
        // A broken pipe is expected when the child process exits; any other error is benign here.
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
    // Use popen + blocking fgets to match the main branch's proven approach.
    // The polling loop + sleep pattern previously used caused pipe buffer
    // backpressure that could stall usbdownload and corrupt the flash on ARM64.
    auto future = std::async(std::launch::async, [&]() {
        FILE *pipe = popen(cmd.c_str(), "r");
        if (!pipe) return -1;
        char buf[1024];
        while (fgets(buf, sizeof(buf), pipe) != NULL) {
            output += buf;
            std::cout << buf << std::flush;
        }
        int status = pclose(pipe);
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return -1;
    });

    auto waitStatus = future.wait_for(std::chrono::milliseconds(timeoutMs));
    if (waitStatus == std::future_status::timeout)
    {
        std::cerr << "Command timed out after " << timeoutMs << " ms." << std::endl;
        return -1;
    }
    return future.get();
}

static bool isFemtoBoltRecoveryDevice(const std::string &devicePath)
{
    size_t pos = devicePath.rfind('/');
    if (pos == std::string::npos) return false;
    std::string devName = devicePath.substr(pos + 1);

    std::string vendorPath;
    if (devName.compare(0, 2, "sg") == 0) {
        vendorPath = "/sys/class/scsi_generic/" + devName + "/device/vendor";
    } else if (devName.compare(0, 2, "sd") == 0) {
        vendorPath = "/sys/block/" + devName + "/device/vendor";
    } else {
        return false;
    }

    std::ifstream fs(vendorPath);
    if (!fs) return false;

    std::string vendor;
    fs >> vendor;
    return (vendor.size() >= 2 && vendor[0] == 'G' && vendor[1] == 'C');
}

static std::vector<std::string> findRecoveryDevices(const std::set<std::string> &exclude)
{
    std::vector<std::string> result;
    const std::string devDir = "/dev/";
    DIR *dir = opendir(devDir.c_str());
    if (!dir) {
        std::cerr << "  Cannot open " << devDir << std::endl;
        return result;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename(entry->d_name);
        bool isSg = (filename.compare(0, 2, "sg") == 0);
        bool isSd = (filename.compare(0, 2, "sd") == 0);
        if (!isSg && !isSd) continue;

        std::string devicePath = devDir + filename;
        if (exclude.find(devicePath) != exclude.end()) continue;
        if (isFemtoBoltRecoveryDevice(devicePath)) {
            result.push_back(devicePath);
        }
    }
    closedir(dir);
    return result;
}

bool upgradeSingleDeviceLinux(ob::Context &sdkCtx, const std::string &filePath, DeviceUpgradeContext &ctx, std::set<std::string> &usedDevicePaths)
{
    // 1. Re-enumerate the target device and create a temporary Pipeline to set recovery mode.
    std::shared_ptr<ob::Device> targetDevice;
    {
        auto devList = sdkCtx.queryDeviceList();
        for (uint32_t i = 0; i < devList->deviceCount(); ++i)
        {
            if (std::string(devList->uid(i)) == ctx.uid)
            {
                targetDevice = devList->getDevice(i);
                break;
            }
        }
    }

    if (!targetDevice)
    {
        ctx.errorMsg = "Device disconnected before upgrade";
        ctx.result = UpgradeResult::Failure;
        return false;
    }

    std::shared_ptr<ob::Pipeline> pipeline;
    try
    {
        pipeline = std::make_shared<ob::Pipeline>(targetDevice);
    }
    catch (const std::exception &e)
    {
        ctx.errorMsg = std::string("Failed to initialize device pipeline: ") + e.what();
        ctx.result = UpgradeResult::Failure;
        return false;
    }
    catch (...)
    {
        ctx.errorMsg = "Failed to initialize device pipeline: unknown error";
        ctx.result = UpgradeResult::Failure;
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
            ctx.result = UpgradeResult::Failure;
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
        auto devices = findRecoveryDevices(usedDevicePaths);
        for (const auto &path : devices) {
            otherRecoveryDevices.insert(path);
        }
    }

    // 2. Poll for the device to appear in recovery mode (up to 60 seconds)
    std::set<std::string> scanExclude = usedDevicePaths;
    scanExclude.insert(otherRecoveryDevices.begin(), otherRecoveryDevices.end());

    if (!otherRecoveryDevices.empty()) {
        std::cout << "  Excluding " << otherRecoveryDevices.size()
                  << " pre-existing recovery device(s) from scan" << std::endl;
    }

    std::string targetPath;
    for (int retry = 0; retry < 120; ++retry)
    {
        auto devices = findRecoveryDevices(scanExclude);
        if (!devices.empty()) {
            targetPath = devices.front();
            break;
        }
        // Print progress every 10 seconds
        if (retry > 0 && retry % 20 == 0) {
            std::cout << "  Still waiting for recovery device... (" << retry / 2 << "s)" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (targetPath.empty())
    {
        ctx.errorMsg = "No recovery device found";
        ctx.result = UpgradeResult::Failure;
        return false;
    }

    usedDevicePaths.insert(targetPath);

    // The device will reboot back to normal mode after the flash completes,
    // causing its /dev node to disappear. We must remove the path from
    // usedDevicePaths on exit so that another device can reuse the same node.
    struct PathGuard
    {
        std::set<std::string> *set;
        std::string path;
        PathGuard(std::set<std::string> *s, const std::string &p) : set(s), path(p) {}
        ~PathGuard() { if (set) set->erase(path); }
    } pathGuard(&usedDevicePaths, targetPath);

    std::cout << "Recovery device found at: " << targetPath << std::endl;

    // Snapshot online UIDs before flash so finalizeDeviceAfterUpgrade can detect the reconnected device.
    std::set<std::string> preFlashUIDs = getOnlineUIDs();

    // 4. Execute the firmware upgrade tool synchronously with timeout (5 minutes)
    std::string cmd = "./usbdownload \"" + targetPath + "\" \"" + filePath + "\"";
    std::cout << "Executing: " << cmd << std::endl;

    std::string cmdOutput;
    int status = runCommandWithTimeoutLinux(cmd, cmdOutput, 300000); // 5 minutes
    if (status == -1 && cmdOutput.empty())
    {
        ctx.errorMsg = "Failed to execute usbdownload or process creation failed";
        ctx.result = UpgradeResult::Failure;
        return false;
    }

    if (status != 0)
    {
        ctx.errorMsg = "Firmware upgrade tool returned error code: " + std::to_string(status);
        ctx.result = UpgradeResult::Failure;
        return false;
    }

    // usbdownload may return 0 even when errors occurred (e.g. permission denied, RDONLY).
    // Check output for known failure keywords.
    std::string lowerOutput = cmdOutput;
    std::transform(lowerOutput.begin(), lowerOutput.end(), lowerOutput.begin(), ::tolower);
    bool hasFailure = false;
    if (lowerOutput.find("rdonly") != std::string::npos) hasFailure = true;
    if (lowerOutput.find("load fail") != std::string::npos) hasFailure = true;
    if (lowerOutput.find("open script fail") != std::string::npos) hasFailure = true;
    // Only treat generic "error" as a failure if the output does not explicitly say "no error".
    if (lowerOutput.find("error") != std::string::npos && lowerOutput.find("no error") == std::string::npos)
        hasFailure = true;

    if (hasFailure)
    {
        ctx.errorMsg = "Firmware upgrade failed: device I/O error or permission denied (try sudo)";
        ctx.result = UpgradeResult::Failure;
        return false;
    }

    ctx.result = UpgradeResult::Success;
    finalizeDeviceAfterUpgrade(sdkCtx, ctx, preFlashUIDs);
    return true;
}

void upgradeRecoveryDevicesLinux(ob::Context &sdkCtx, const std::string &filePath, std::vector<DeviceUpgradeContext> &totalDevices, std::set<std::string> &usedDevicePaths)
{
    std::cout << "Scanning for recovery mode devices..." << std::endl;

    // Quick scan for remaining recovery devices (up to 5 seconds).
    std::vector<std::string> devicePaths;
    for (int retry = 0; retry < 10; ++retry)
    {
        devicePaths = findRecoveryDevices(usedDevicePaths);
        if (!devicePaths.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (devicePaths.empty())
        return;

    int deviceIndex = 0;
    for (const auto &path : devicePaths)
    {
        // Re-verify the device is still a valid recovery device before flashing.
        // After a prior device was flashed and rebooted, its sg/sd node may
        // linger momentarily. Calling usbdownload on a stale node produces
        // "open /dev/sg*(RDWR) error" which would be misreported as a failure.
        if (!isFemtoBoltRecoveryDevice(path)) {
            std::cout << "  Recovery device " << path << " no longer available, skipping." << std::endl;
            continue;
        }

        usedDevicePaths.insert(path);
        deviceIndex++;

        DeviceUpgradeContext ctx;
        ctx.name = "Femto Bolt (Recovery)";
        ctx.serialNumber = "Unknown";
        ctx.firmwareVersion = "Unknown";

        std::cout << "\nUpgrading recovery device: " << deviceIndex << " - " << path << std::endl;

        // Snapshot online UIDs before flash so finalizeDeviceAfterUpgrade can detect the reconnected device.
        std::set<std::string> preFlashUIDs = getOnlineUIDs();

        std::string cmd = "./usbdownload \"" + path + "\" \"" + filePath + "\"";
        std::cout << "Executing: " << cmd << std::endl;

        std::string cmdOutput;
        int status = runCommandWithTimeoutLinux(cmd, cmdOutput, 300000); // 5 minutes
        if (status == -1 && cmdOutput.empty())
        {
            ctx.errorMsg = "Failed to execute usbdownload or process creation failed";
            ctx.result = UpgradeResult::Failure;
            totalDevices.push_back(ctx);
            continue;
        }

        if (status != 0)
        {
            ctx.errorMsg = "Firmware upgrade tool returned error code: " + std::to_string(status);
            ctx.result = UpgradeResult::Failure;
        }
        else
        {
            std::string lowerOutput = cmdOutput;
            std::transform(lowerOutput.begin(), lowerOutput.end(), lowerOutput.begin(), ::tolower);
            bool hasFailure = false;
            if (lowerOutput.find("rdonly") != std::string::npos) hasFailure = true;
            if (lowerOutput.find("load fail") != std::string::npos) hasFailure = true;
            if (lowerOutput.find("open script fail") != std::string::npos) hasFailure = true;
            if (lowerOutput.find("error") != std::string::npos && lowerOutput.find("no error") == std::string::npos)
                hasFailure = true;

            if (hasFailure)
            {
                ctx.errorMsg = "Firmware upgrade failed: device I/O error or permission denied (try sudo)";
                ctx.result = UpgradeResult::Failure;
            }
            else
            {
                ctx.result = UpgradeResult::Success;
            }
        }
        totalDevices.push_back(ctx);
        if (ctx.result == UpgradeResult::Success) {
            finalizeDeviceAfterUpgrade(sdkCtx, totalDevices.back(), preFlashUIDs);
        }
    }
}
#endif
