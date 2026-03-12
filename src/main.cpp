#include "libobsensor/ObSensor.hpp"
#include "usbscsicmd.h"
#include "utils.hpp"
#include <iostream>
#include <thread>
#include <map>
#include <string>
#include <iostream>
#include <mutex>
#include <future>
#include <set>
#include <vector>
#include <atomic>
#include <queue>

#ifdef WIN32
#include <conio.h>
#else
#include <dirent.h>
#include <poll.h>
#endif

#define ESC 27

typedef struct PipelineHolder_t
{
    std::shared_ptr<ob::Pipeline> pipeline;
    std::shared_ptr<ob::DeviceInfo> deviceInfo;
    bool isUpgraded;
} PipelineHolder;

std::recursive_mutex pipelineHolderMutex;
std::map<std::string, std::shared_ptr<PipelineHolder>> pipelineHolderMap;
std::set<int> upgradedDeviceSet;
std::mutex upgradeFuturesMutex;
std::vector<std::future<void>> upgradeFutures;
std::atomic<bool> upgradeInProgress{false};

void handleDeviceConnected(std::shared_ptr<ob::DeviceList> connectList);
void handleDeviceDisconnected(std::shared_ptr<ob::DeviceList> disconnectList);
void upgradeDevices(std::string filePath);
void printDevicesInfo();

bool isAutoMode = false;
std::atomic<bool> shouldExitAuto{false};
std::queue<char> autoInputQueue;
std::mutex autoInputQueueMutex;
std::thread autoInputThread;

void waitForAutoInput() {
#ifdef WIN32
    char input;
    while (!shouldExitAuto && std::cin.get(input)) {
        std::lock_guard<std::mutex> lock(autoInputQueueMutex);
        autoInputQueue.push(input);
    }
#else
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;

    while (!shouldExitAuto) {
        int ret = poll(&pfd, 1, 500); // 500ms timeout to check shouldExitAuto
        if (ret > 0 && (pfd.revents & POLLIN)) {
            char input;
            ssize_t n = read(STDIN_FILENO, &input, 1);
            if (n <= 0) {
                break; // EOF or error
            }
            std::lock_guard<std::mutex> lock(autoInputQueueMutex);
            autoInputQueue.push(input);
        } else if (ret < 0) {
            break; // poll error
        }
        // ret == 0 means timeout, loop back and check shouldExitAuto
    }
#endif
}

bool hasAutoInput() {
    std::lock_guard<std::mutex> lock(autoInputQueueMutex);
    return !autoInputQueue.empty();
}

char getAutoInput() {
    std::lock_guard<std::mutex> lock(autoInputQueueMutex);
    if (autoInputQueue.empty()) {
        return 0;
    } 
    char c = autoInputQueue.front();
    autoInputQueue.pop();
    return c;
}

void drainAutoInput() {
    std::lock_guard<std::mutex> lock(autoInputQueueMutex);
    while (!autoInputQueue.empty()) {
        autoInputQueue.pop();
    }
}

void waitForUpgradesToComplete() {
    std::lock_guard<std::mutex> lock(upgradeFuturesMutex);
    for (auto &f : upgradeFutures) {
        if (f.valid()) {
            std::cout << "Waiting for firmware upgrade to complete..." << std::endl;
            f.get();
        }
    }
    upgradeFutures.clear();
    upgradeInProgress = false;
    drainAutoInput();
}

void endWaitForInput() {
    shouldExitAuto = true;
    if(autoInputThread.joinable()) {
        autoInputThread.join();
    }
}

int main(int argc, char **argv)
try
{

    if (argc < 2 || argc > 3)
    {
        std::cerr << "Usage:[app] [firmware path] [--auto]" << std::endl;
        std::cerr << "Note: --auto must be the final argument when used" << std::endl;
        return -1;
    }

    std::string filePath = std::string(argv[1]);
    // create context
    ob::Context ctx;

    if(argc == 3 && std::string(argv[2]) == "--auto") {
        isAutoMode = true;
        std::cout << "Running in automation mode - reading commands from stdin" << std::endl;
        autoInputThread = std::thread(waitForAutoInput);
    }

    // register device callback
    ctx.setDeviceChangedCallback([](std::shared_ptr<ob::DeviceList> removedList, std::shared_ptr<ob::DeviceList> addedList)
                                 {
        handleDeviceDisconnected(removedList);
        handleDeviceConnected(addedList); });

    // handle current connected devices.
    handleDeviceConnected(ctx.queryDeviceList());

    while (true)
    {
        bool keyPressed = false;
        int key = 0;
        if (isAutoMode && hasAutoInput()) {
            key = getAutoInput();
            keyPressed = true;
        } else if (!isAutoMode && kbhit()) {
            key = getch();
            keyPressed = true;
        }

        if (keyPressed)
        {
            if (upgradeInProgress) {
                // Ignore all input while an upgrade is running
                continue;
            }
            // Press the esc key to exit
            if (key == ESC || key == 'q' || key == 'Q')
            {
                break;
            }
            if (key == 'u' || key == 'U')
            {
                upgradeDevices(filePath);
            }
        }
        else     
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    waitForUpgradesToComplete();
    endWaitForInput();

    return 0;
}
catch (ob::Error &e)
{
    std::cerr << "function:" << e.getName() << "\nargs:" << e.getArgs() << "\nmessage:" << e.getMessage() << "\ntype:" << e.getExceptionType() << std::endl;

    waitForUpgradesToComplete();
    endWaitForInput();

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
            std::shared_ptr<ob::Pipeline> pipeline(new ob::Pipeline(device));
            std::shared_ptr<PipelineHolder> holder(new PipelineHolder{pipeline, deviceInfo, false});
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
/**
 * Upgrades the firmware of all connected devices using the specified file path.
 *
 * @param filePath The path to the firmware file to be used for the upgrade.
 */
void upgradeDevices(std::string filePath)
{
    if (upgradeInProgress.exchange(true)) {
        std::cout << "Upgrade already in progress, ignoring request." << std::endl;
        return;
    }
    std::cout << "Starting firmware upgrade..." << std::endl;

    // Ensure filePath ends with '/'
    if (filePath.back() != '/') {
        filePath += '/';
    }
    
    // set device boot into recovery mode
    for (auto iter : pipelineHolderMap)
    {
        auto holder = iter.second;
        auto pipeline = holder->pipeline;
        if (!pipeline)
        {
            continue;
        }
        try
        {
            auto device = pipeline->getDevice();
            // Check if the device supports setting the property to boot into recovery mode and has write permission
            if (device->isPropertySupported(OB_PROP_BOOT_INTO_RECOVERY_MODE_BOOL, OB_PERMISSION_WRITE))
            {
                // If supported and permission is granted, set the device to boot into recovery mode on next reboot
                device->setBoolProperty(OB_PROP_BOOT_INTO_RECOVERY_MODE_BOOL, true);
            }
        }
        catch (...)
        {
        }
    }

#ifdef WIN32
     // Upgrade devices on Windows
    uint8_t retryTimes = 10;
    while (true)
    {
        HANDLE handle = NULL;
        int devState = DEV_STATE_UNKNOWN;
        int diskNumber = 0;

        // Call a function to find the device and retrieve its handle, state, and disk number
        handle = USB_ScsiFindDevice(&devState, &diskNumber, upgradedDeviceSet);

        // Check if the device is found and not in the upgraded set
        auto iter = upgradedDeviceSet.find(diskNumber);
        if (handle && iter == upgradedDeviceSet.end())
        {
            // Reset the retry counter
            retryTimes = 10;
            upgradedDeviceSet.insert(diskNumber);

            // Construct the command line for firmware upgrade
            //USBDownloadTool.exe "<firmware_path>" <disk_number>
            std::string cmd = "USBDownloadTool.exe \"" + filePath + "\"" + " " + std::to_string(diskNumber) + " <NUL";

            auto cmdFuture = std::async(std::launch::async, [cmd, diskNumber]()
                                   {
                uint64_t startTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

                 // Execute an external command and read its output
                FILE *pipe               = NULL;
                pipe = _popen(cmd.c_str(), "r");
                if(NULL == pipe) {
                    std::cout << "popen failed!" << std::endl;
                    return;
                }
                while(!feof(pipe)) {
                    char buf[1024];
                    fgets(buf, 1024, pipe);
                    std::string msg = std::string(buf);
                    if(msg != "\r\n") {
                        std::cout << "Firmware Upgrade Msg:" << msg << std::endl;
                    }
                }

                // Close the pipe and calculate the end time
                _pclose(pipe);

                uint64_t endTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                float    costTime       = (endTimestamp - startTimestamp) / 1000.0;
                std::cout << "Firmware Upgrade cost time(s):" << costTime << std::endl;

                upgradedDeviceSet.erase(diskNumber); });
            {
                std::lock_guard<std::mutex> lock(upgradeFuturesMutex);
                upgradeFutures.push_back(std::move(cmdFuture));
            }
        }

        // If the device is not found and retries are exhausted, exit the loop
        if (!handle && retryTimes-- == 0)
        {
            break;
        }

        // If no device is found, sleep for 500 milliseconds before trying again
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
#else
    // wait all device boot into recovery mode,adjust according to the actual situation
    uint8_t retryTimes = 20;
    while (retryTimes-- > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Define the system device directory and the prefix for SCSI generic devices.
    const std::string devDir = "/dev/";
    const std::string sgPrefix = "sg";

    DIR *dir = opendir(devDir.c_str());
    if (dir == nullptr)
    {
        std::cerr << "Failed to open directory: " << devDir << std::endl;
        return;
    }

    // Iterate over the directory entries.
    struct dirent *entry;
    std::vector<std::string> devicePaths;

    // Collect paths of devices starting with the specified prefix.
    while ((entry = readdir(dir)) != nullptr)
    {
        std::string filename(entry->d_name);
        if (filename.compare(0, sgPrefix.size(), sgPrefix) == 0)
        {
            // Construct the full path to the device.
            std::string devicePath = devDir + filename;
            devicePaths.push_back(devicePath);
        }
    }

    closedir(dir);

    // For each collected device path, perform the firmware upgrade.
    for (const auto &path : devicePaths)
    {
        // upgrade devices
        // Construct the command line for the firmware upgrade tool.
        //usbdownloade  <scsi_device>   <firmware_path>
        std::string cmd = "./usbdownload \"" + path + "\"" + " " + filePath + " </dev/null";
        auto cmdFuture = std::async(std::launch::async, [cmd]()
                               {
            uint64_t startTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            // Execute the command and open a pipe to read its output.
            FILE *pipe               = NULL;
            pipe = popen(cmd.c_str(), "r");
            if(NULL == pipe) {
                std::cout << "popen failed!" << std::endl;
                return;
            }
            while(!feof(pipe)) {
                // Read lines from the pipe and print any non-empty output.
                char buf[1024];
                fgets(buf, 1024, pipe);
                std::string msg = std::string(buf);
                if(msg != "\r\n") {
                    std::cout << "Firmware Upgrade Msg:" << msg << std::endl;
                }
            }

            // Close the pipe and calculate the end time.
            pclose(pipe);
            uint64_t endTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            float    costTime       = (endTimestamp - startTimestamp) / 1000.0;
            std::cout << "Firmware Upgrade cost time(s):" << costTime << std::endl;
        });
        {
            std::lock_guard<std::mutex> lock(upgradeFuturesMutex);
            upgradeFutures.push_back(std::move(cmdFuture));
        }
    }

#endif

    // Wait for all flash tasks to finish before returning
    waitForUpgradesToComplete();
    std::cout << "Firmware upgrade finished." << std::endl;
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