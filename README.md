# FemtoBoltUpgrade

This repository contains a sample project for upgrading the firmware of Femto Bolt devices.The sample project supports Windows, Linux (x64), and Linux (ARM64) platforms, offering a foundational framework for integrating firmware upgrades into developer applications.

## build

### Prerequisites

For windows, you need:

- Windows 10 or later
- Visual Studio 2017 or later with MSVC v141 toolset
- CMake (v3.5 or later)

For Linux (x64), you need:

- Ubuntu 18.04 is recommended
- GCC 7.5
- CMake (v3.5 or later)

For Linux (ARM64), you need:

- Ubuntu 18.04 or later for ARM64 is recommended
- GCC 7.5 or later (aarch64 toolchain)
- CMake (v3.5 or later)

Firmware download link for femto bolt：[Femto Bolt Firmware](https://github.com/orbbec/OrbbecFirmware)


### Build

windows

``` powershell
 mkdir build && cd build && cmake .. && cmake --build . --config Release
```

Linux (x64)

```bash
 mkdir build && cd build && cmake .. && cmake --build . --config Release
```

Linux (ARM64)

```bash
 mkdir build && cd build && cmake .. && cmake --build . --config Release
```

## Usage

Connect your femto bolt camera to your PC, proceed the following steps:

> **⚠️ Important:** Please make sure to connect your Femto Bolt device via a **USB 3.0** port. Using a USB 2.0 port may result in unexpected errors during the firmware upgrade process.

windows

- 1.Copy FemtoBoltUpgrader.exe to the Release/Windows directory
- 2.Open the PowerShell in the Windows directory
- 3.Enter the following command:

``` powershell
.\FemtoBoltUpgrader.exe  path(Directory to upgrade firmware)
```

- 4.Wait a moment, then type U or u to start the upgrade, or Esc to exit the upgrade

Linux (x64)

- 1.Copy FemtoBoltUpgrader to the Release/linux directory
- 2.Open the terminal in the linux directory
- 3.in Release/linux directory, Enter
 ```bash
     chmod +x usbdownload
```
- 4.Enter the following command:

```bash
 sudo ./FemtoBoltUpgrader  path(Directory to upgrade firmware)
```

- 5.Wait a moment, then type U or u to start the upgrade, or Esc to exit the upgrade

Linux (ARM64)

- 1.Copy FemtoBoltUpgrader to the Release/arm64 directory
- 2.Open the terminal in the arm64 directory
- 3.in Release/arm64 directory, Enter
 ```bash
     chmod +x usbdownload
```
- 4.Enter the following command:

```bash
 sudo ./FemtoBoltUpgrader  path(Directory to upgrade firmware)
```

- 5.Wait a moment, then type U or u to start the upgrade, or Esc to exit the upgrade

## Command-line Arguments

```
Usage: FemtoBoltUpgrader <firmware_path> [--auto]
```

| Argument | Description |
|----------|-------------|
| `firmware_path` | Path to the firmware directory or `.zip` file |
| `--auto` | (Optional) Non-interactive auto-run mode — skips the key-press confirmation and proceeds with upgrade automatically |

The firmware path is automatically normalized: trailing backslashes from PowerShell escaping are converted, and directory paths are ensured to end with `/`.

---

## Code Description

The firmware upgrade process consists of the following key steps:

### 1. Data Structures

``` c++
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
```

- **PipelineHolder** — stores device information for each connected device, keyed by UID.
- **DeviceUpgradeContext** — tracks the upgrade state (pending/success/failure) and metadata for each device.
- **pipelineHolderMap** — the global registry of currently connected devices, protected by a recursive mutex for thread safety.

### 2. Enumerating Devices

``` c++
// create context
ob::Context ctx;

// register device callback
ctx.setDeviceChangedCallback([](std::shared_ptr<ob::DeviceList> removedList, std::shared_ptr<ob::DeviceList> addedList)
                             {
    handleDeviceDisconnected(removedList);
    handleDeviceConnected(addedList); });

// handle current connected devices.
handleDeviceConnected(ctx.queryDeviceList());
```

The `ob::Context` object serves as the central point for managing device connections. It allows you to:
- Register callbacks for device connection and disconnection events.
- Query the current list of connected devices.
- Handle the initial set of connected devices when the application starts.

The `handleDeviceConnected()` callback iterates over the connected device list, retrieves each device's info via `getDeviceInfo()`, and inserts a new `PipelineHolder` into `pipelineHolderMap` keyed by UID. The `handleDeviceDisconnected()` callback removes devices from the map when they go offline.

### 3. Single Device Upgrade Flow

The per-device upgrade follows this sequence (common to both Windows and Linux):

**Step 1 — Find the target device and set recovery mode:**

``` c++
// Re-enumerate the target device by UID
std::shared_ptr<ob::Device> targetDevice;
auto devList = sdkCtx.queryDeviceList();
for (uint32_t i = 0; i < devList->deviceCount(); ++i)
{
    if (std::string(devList->uid(i)) == ctx.uid)
    {
        targetDevice = devList->getDevice(i);
        break;
    }
}

// Create a temporary Pipeline and set boot-into-recovery property
std::shared_ptr<ob::Pipeline> pipeline = std::make_shared<ob::Pipeline>(targetDevice);
auto device = pipeline->getDevice();
if (device->isPropertySupported(OB_PROP_BOOT_INTO_RECOVERY_MODE_BOOL, OB_PERMISSION_WRITE))
{
    device->setBoolProperty(OB_PROP_BOOT_INTO_RECOVERY_MODE_BOOL, true);
}
```

> **Note:** After setting the recovery mode property, the device reboots immediately. A `try/catch` block wraps this call because the control transfer may fail as the device drops off the USB bus — this is expected behavior and is safely ignored.

**Step 2 — Wait for the device to appear as a recovery (SCSI) device, then run the upgrade tool:**

*On Windows:*

``` c++
// Poll for the SCSI recovery device (up to 30 seconds)
HANDLE handle = NULL;
int devState = DEV_STATE_UNKNOWN;
int diskNumber = 0;
for (int retry = 0; retry < 60; ++retry)
{
    handle = USB_ScsiFindDevice(&devState, &diskNumber, scanExclude);
    if (handle && devState != DEV_STATE_UNKNOWN) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// Execute USBDownloadTool.exe with 5-minute timeout
std::string cmd = "USBDownloadTool.exe \"" + filePath + "\" " + std::to_string(diskNumber);
std::string cmdOutput;
int status = runCommandWithTimeout(cmd, cmdOutput, 300000);
```

*On Linux:*

``` c++
// Poll for recovery device by scanning /dev/sg* and /dev/sd* (up to 60 seconds)
std::string targetPath;
for (int retry = 0; retry < 120; ++retry)
{
    auto devices = findRecoveryDevices(scanExclude);
    if (!devices.empty()) { targetPath = devices.front(); break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// Execute usbdownload with 5-minute timeout
std::string cmd = "./usbdownload \"" + targetPath + "\" \"" + filePath + "\"";
std::string cmdOutput;
int status = runCommandWithTimeoutLinux(cmd, cmdOutput, 300000);
```

**Step 3 — Validate the upgrade output** (both platforms check for known failure keywords):

``` c++
// Check output for known failure indicators
if (cmdOutput.find("open script fail") != std::string::npos ||
    cmdOutput.find("load fail") != std::string::npos)
{
    // Upgrade failed
}
// On Linux, also check for "rdonly" (permission denied) and generic "error"
```

### 4. Recovery Device Scanning (Linux)

On Linux, recovery devices are identified not just by their `/dev/sg*` or `/dev/sd*` name, but by verifying the vendor string through sysfs:

``` c++
static bool isFemtoBoltRecoveryDevice(const std::string &devicePath)
{
    // Extract device name and determine the sysfs vendor path
    std::string devName = devicePath.substr(devicePath.rfind('/') + 1);
    std::string vendorPath;
    if (devName.compare(0, 2, "sg") == 0) {
        vendorPath = "/sys/class/scsi_generic/" + devName + "/device/vendor";
    } else if (devName.compare(0, 2, "sd") == 0) {
        vendorPath = "/sys/block/" + devName + "/device/vendor";
    } else {
        return false;
    }

    // Read vendor file; Femto Bolt devices report vendor starting with "GC"
    std::ifstream fs(vendorPath);
    std::string vendor;
    fs >> vendor;
    return (vendor.size() >= 2 && vendor[0] == 'G' && vendor[1] == 'C');
}
```

This prevents accidentally flashing non-Femto-Bolt SCSI devices that happen to share the same `/dev/sg*` namespace.

### 5. Post-Upgrade Finalization

After a successful flash, the device must be verified as healthy and online:

``` c++
static void finalizeDeviceAfterUpgrade(ob::Context &sdkCtx, DeviceUpgradeContext &ctx,
                                        const std::set<std::string> &preFlashUIDs)
{
    // Step 1: Wait for the device to auto-reboot back to normal mode (up to 60s)
    std::string sn, fw;
    bool found = waitForDeviceByCriteria(ctx.serialNumber, 60, sn, fw);
    if (!found) return;  // timeout — device did not come back

    ctx.serialNumber = sn;
    ctx.firmwareVersion = fw;

    // Step 2: Perform an active reboot to stabilize the serial number
    rebootDeviceBySN(sdkCtx, ctx.serialNumber);

    // Step 3: Wait for the device to come back after the explicit reboot
    std::set<std::string> uidsBefore = getOnlineUIDs();
    if (waitForNewDevice(uidsBefore, 60, sn, fw)) {
        ctx.serialNumber = sn;
        ctx.firmwareVersion = fw;   // final post-upgrade firmware version
    }
}
```

This three-stage process ensures the device is fully operational and reports a stable serial number and firmware version.

### 6. Recovery-Only Device Upgrade

After all normal-mode devices have been processed, the tool also scans for any devices already in recovery mode (e.g., devices that were bricked or interrupted during a previous upgrade attempt):

``` c++
void upgradeRecoveryDevices(ob::Context &sdkCtx, const std::string &filePath,
                            std::vector<DeviceUpgradeContext> &totalDevices, ...)
{
    while (true)
    {
        // Find next unprocessed recovery device
        handle = USB_ScsiFindDevice(&devState, &diskNumber, usedDiskSet);
        if (!handle) break;

        DeviceUpgradeContext ctx;
        ctx.name = "Femto Bolt (Recovery)";
        ctx.serialNumber = "Unknown";

        // Run the same upgrade flow...
        // On success, finalizeDeviceAfterUpgrade() is called
        totalDevices.push_back(ctx);
    }
}
```

Recovery devices have unknown serial numbers before upgrade, so `finalizeDeviceAfterUpgrade()` uses `waitForNewDevice()` (detecting any new UID) instead of `waitForDeviceByCriteria()` (matching by serial number).

### 7. Upgrade Summary

After all devices have been processed, a summary is printed showing successes and failures:

```
Upgrade Summary:
==================================================
Success (2):
  - Name: Femto Bolt | SN: CL9BC2600000 | Firmware version: 1.2.3
  - Name: Femto Bolt | SN: CL9BC2600001 | Firmware version: 1.2.3

Failure (0):

Upgrade process completed.
```



