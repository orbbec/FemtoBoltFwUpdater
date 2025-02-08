# FemtoBoltUpgrade

This repository contains a sample project for upgrading the firmware of Femto Bolt devices.The sample project supports both Windows and Linux platforms, offering a foundational framework for integrating firmware upgrades into developer applications.

## build

### Prerequisites

For windows, you need:

- Windows 10 or later
- Visual Studio 2017 or later with MSVC v141 toolset
- CMake (v3.5 or later)

For Linux,  you need:

- Ubuntu 18.04 is recommended
- GCC 7.4
- CMake (v3.5 or later)

Firmware download link for femto bolt：[Femto Bolt Firmware](https://github.com/orbbec/OrbbecFirmware)


### Build

windows

``` powershell
 mkdir build && cd build && cmake .. && cmake --build . --config Release
```

Linux

```bash
 mkdir build && cd build && cmake .. && cmake --build . --config Release
```

## Usage

Connect your femto bolt camera to your PC, proceed the following steps:

windows

- 1.Copy FemtoBoltUpgrader.exe to the Release/Windows directory
- 2.Open the PowerShell in the Windows directory
- 3.Enter the following command:

``` powershell
.\FemtoBoltUpgrader.exe  path(Directory to upgrade firmware)
```

- 4.Wait a moment, then type U or u to start the upgrade, or Esc to exit the upgrade

Linux

- 1.Copy FemtoBoltUpgrader.exe to the Release/linux directory
- 2.Open the terminal in the linux directory
- 3.Enter the following command:

```bash
 .\FemtoBoltUpgrader  path(Directory to upgrade firmware)
```

- 4.Wait a moment, then type U or u to start the upgrade, or Esc to exit the upgrade

## Code Description

The firmware upgrade process involves three main steps:

### 1. Enumerating Femto Bolt devices and maintains their information in a global variable named pipelineHolderMap

``` c++
// create context
ob::Context ctx;

// register device callback
ctx.setDeviceChangedCallback([](std::shared_ptr<ob::DeviceList> removedList, std::shared_ptr<ob::DeviceList> addedList){
    handleDeviceDisconnected(removedList);
    handleDeviceConnected(addedList); 
    });

// handle current connected devices.
handleDeviceConnected(ctx.queryDeviceList());
```
The ctx object serves as the central point for managing device connections and changes. It allows you to:Register callbacks for device connection and disconnection events.Query the current list of connected devices.Handle the initial set of connected devices when the application starts.The device callback function is used to monitor the connection status of the device. When a device is connected or disconnected, the callback function will be triggered. Use ctx.queryDeviceList() to get the list of currently connected devices. The function handleDeviceConnected() is used to add the device to the pipelineHolderMap.

### 2、Switching devices to recovery mode

``` c++
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
```

This code segment traverses the pipelineHolderMap, checking each device to see if it supports a specific property (booting into covery mode) and has the corresponding write permission. If conditions are met, the device is set to boot into recovery mode.

### 3、Upgrading firmware

Upgrade devices on Windows

``` c++
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
            //USBDownloadTool.exe "<file_path>" <disk_number>
            std::string cmd = "USBDownloadTool.exe \"" + filePath + "\"" + " " + std::to_string(diskNumber);
            
            // Create a future object for an asynchronous task
            std::future<void> cmdFuture;
            if (!cmdFuture.valid())
            {
                // If the future object is invalid, create a new asynchronous task
                cmdFuture = std::async(std::launch::async, [cmd, diskNumber]()
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
```

This code segment uses a loop to continuously check for a device and upgrade its firmware. It first finds the device and retrieves its handle, state, and disk number. Then, it checks if the device is found and not in the upgraded set. If conditions are met, the device is added to the upgraded set and a command line(/USBDownloadTool.exe <file_path> <disk_number>) for firmware upgrade is constructed. A future object is created for an asynchronous task that executes the command line and reads its output. If the future object is invalid, a new asynchronous task is created. The task reads the output of the command line.

Upgrade devices on Linux

``` c++
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
    //./usbdownloade  <scsi_device>  <firmware_path>
    std::string cmd = "./usbdownload \"" + path + "\"" + " " + filePath;
    std::future<void> cmdFuture;
    if (!cmdFuture.valid())
    {
        cmdFuture = std::async(std::launch::async, [cmd]()
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
    }
}
```

This code first waits for all devices to boot into recovery mode by using a loop and sleeping for a specific period. It then opens the /dev/ directory to find all devices that start with the prefix sg (typically SCSI generic devices), collects their paths into a vector, and performs a firmware upgrade for each device path. The upgrade is executed in an asynchronous task, which records the start and end times to calculate and print the total time taken for the firmware upgrade.



