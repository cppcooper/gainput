/*
 * Copyright (c) 2017-2022 The Forge Interactive Inc.
 *
 * This file is part of The-Forge
 * (see https://github.com/ConfettiFX/The-Forge).
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
*/

#include "GainputHID.h"

#include "GainputHIDTypes.h"
#include "GainputHIDWhitelist.h"
#include <hidapi.h>

#include "timer/GainputTimer.h"

#include "hidparsers/HIDParserPS4Controller.h"
#include "hidparsers/HIDParserPS5Controller.h"

#if defined(_WINDOWS)
#undef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#undef STRICT
#define STRICT
#undef UNICODE
#define UNICODE 1
#undef _WIN32_WINNT
#define _WIN32_WINNT  0x501  // for raw input
#include <windows.h>
#include <Dbt.h>
#include <initguid.h>
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE, 0xA5DCBF10L, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);

#elif defined(__APPLE__)
#include <IOKit/hid/IOHIDDevice.h>

#elif defined(__linux__)
#include <libudev.h>
#include <poll.h>
#include <math.h>
#endif

// --- Types ------------------------------------------------------------------


// --- Data -------------------------------------------------------------------

HiresTimer gHIDTimer;
static bool gHIDInitialized = false;


// --- Notifications

static bool gNotifierInitialized = false;

static bool gDevicesChanged = true;

typedef void(*DeviceChangeCB)(const char*, bool, int controllerID);
static DeviceChangeCB gDeviceChangeCallback = NULL;
#if defined(_WINDOWS)
HWND gHWND;
WNDCLASSEXW gWND;
HDEVNOTIFY gNotifier;
#elif defined(__APPLE__)
IONotificationPortRef gNotificationPort;
mach_port_t gNotificationMach;
struct OutMessage
{
    mach_msg_header_t hdr;
    char data[4096];
};
#elif defined(__linux__)
struct udev* pUDev;
struct udev_monitor* pUDevMonitor;
int gUDevFD;
#endif

// backup if system has no notification system or fails to initialize
HiresTimer gForceRecheckTimer;
static uint64_t gLastCheck = 0;


// --- Device Tracking

// This is used for initializing the device tracker
// The tracker uses a linked list for ease of addition and removal, but the
//   data is stored in an array for faster memory loads and adjacency
//
// NOTE: One peripheral can have multiple HID devices. This is set to 20 since
//   most programs won't go over 16 local players, if even get close, with some
//   leeway if multiple devices are connected while swtiching controllers.
//   If the target controllers being used have multiple logical devices,
//   though, this may need to be increased.
#define MAX_DEVICES_TRACKED 20
GAINPUT_COMPILE_ASSERT(MAX_DEVICES_TRACKED <= 0x1F); // HIDDeviceInfo::id field size

HIDDeviceInfo gDeviceBuffer[MAX_DEVICES_TRACKED];
HIDDeviceInfo* pFreeList = nullptr;
uint32_t gActiveDevices = 0;
// This is equal to the max number of devices connnected over the life of the program
uint32_t gActiveSlots = 0;


// --- Controllers

HIDController gControllerBuffer[MAX_DEVICES_TRACKED];


// --- Forward Declarations ---------------------------------------------------

static void HIDInitDeviceTracking();
static void HIDExitDeviceTracking();
static void HIDRemoveDevice(HIDDeviceInfo *dev);
static void HIDInitControllerTracking();
void HIDLoadController(uint8_t devID, uint8_t playerNum);
void HIDUnloadController(uint8_t devID);


// --- System Communication ---------------------------------------------------

// Notifications are needed so that we try to connect to a device only when
// the drivers are properly installed and not just when connected
static void InitSystemNotificationReceiver(void* window)
{
    if (gNotifierInitialized)
        return;

#if defined(_WINDOWS)
    gHWND = (HWND)window;

    memset(&gWND, 0, sizeof(gWND));
    gWND.hInstance = (HINSTANCE)GetWindowLongPtr(gHWND, GWLP_HINSTANCE);
    gWND.lpszClassName = L"The Forge";

    WNDCLASSW registeredClassData;
    GetClassInfoW(gWND.hInstance, gWND.lpszClassName, &registeredClassData);
    gWND.lpfnWndProc = registeredClassData.lpfnWndProc;
    gWND.cbSize = sizeof(registeredClassData);

    DEV_BROADCAST_DEVICEINTERFACE_A devBroadcast;
    memset(&devBroadcast, 0, sizeof(devBroadcast));
    devBroadcast.dbcc_size = sizeof(devBroadcast);
    devBroadcast.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    devBroadcast.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;

    // DEVICE_NOTIFY_ALL_INTERFACE_CLASSES allows for notification when devices are installed
    gNotifier = RegisterDeviceNotification(gHWND, &devBroadcast, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
    gNotifierInitialized = gNotifier != NULL;
#elif defined(__APPLE__)
    gNotificationPort = IONotificationPortCreate(kIOMasterPortDefault);

    if (gNotificationPort)
    {
        // Drain first match notifications, or we won't receive device added notifications
        io_iterator_t portIterator = 0;
        IOReturn result = IOServiceAddMatchingNotification(gNotificationPort, kIOFirstMatchNotification,
            IOServiceMatching(kIOHIDDeviceKey), CallbackIOServiceFunc, &gDevicesChanged, &portIterator);

        if (result == 0)
        {
            io_object_t entry;
            while ((entry = IOIteratorNext(portIterator)) != 0)
                IOObjectRelease(entry);
        }
        else
        {
            IONotificationPortDestroy(gNotificationPort);
            gNotificationPort = NULL;
        }

        // Drain termination notifications, or we won't receive device removed notifications
        portIterator = 0;
        result = IOServiceAddMatchingNotification(gNotificationPort, kIOTerminatedNotification,
            IOServiceMatching(kIOHIDDeviceKey), CallbackIOServiceFunc, &gDevicesChanged, &portIterator);

        if (result == 0)
        {
            io_object_t entry;
            while ((entry = IOIteratorNext(portIterator)) != 0)
                IOObjectRelease(entry);
        }
        else
        {
            IONotificationPortDestroy(gNotificationPort);
            gNotificationPort = NULL;
        }
    }

    gNotificationMach = MACH_PORT_NULL;
    if (gNotificationPort)
    {
        gNotificationMach = IONotificationPortGetMachPort(gNotificationPort);
    }

    gNotifierInitialized = gNotificationMach != MACH_PORT_NULL;
#elif defined(__linux__)

    pUDev = udev_new();

    pUDevMonitor = NULL;
    gUDevFD = -1;

    if (pUDev)
    {
        pUDevMonitor = udev_monitor_new_from_netlink(pUDev, "udev");

        if (pUDevMonitor)
        {
            if (udev_monitor_enable_receiving(pUDevMonitor) == 0)
            {
                gUDevFD = udev_monitor_get_fd(pUDevMonitor);
                gNotifierInitialized = gUDevFD >= 0;
            }
        }
    }
    if (!gNotifierInitialized)
    {
        if (pUDevMonitor)
            udev_monitor_unref(pUDevMonitor);
        if (pUDev)
            udev_unref(pUDev);
        pUDevMonitor = NULL;
        pUDev = NULL;
        gUDevFD = -1;
    }
#endif

    // Recheck even with notifications: failed opens may become readable without
    // another arrival event, and notification delivery is not guaranteed.
    initHiresTimer(&gForceRecheckTimer);
    gLastCheck = 0;
}

static void ExitSystemNotificationReceiver()
{
#if !defined(__linux__)
    if (!gNotifierInitialized)
        return;
#endif

#if defined(_WINDOWS)
    GAINPUT_ASSERT(gNotifier && "gNotifierInitialized should have guaranteed this was not null.");
    UnregisterDeviceNotification(gNotifier);
    gNotifier = NULL;
#elif defined(__APPLE__)
    GAINPUT_ASSERT(gNotificationPort && "gNotifierInitialized should have guaranteed this was not null.");
    IONotificationPortDestroy(gNotificationPort);
    gNotificationPort = NULL;
#elif defined(__linux__)
    if (pUDevMonitor)
        udev_monitor_unref(pUDevMonitor);
    if (pUDev)
        udev_unref(pUDev);
    pUDevMonitor = NULL;
    pUDev = NULL;
    gUDevFD = -1;
#endif

    gNotifierInitialized = false;
}

static bool CheckForDeviceInstallation(void const* message)
{
    uint64_t currentTime = getHiresTimerUSec(&gForceRecheckTimer, false);
    if (currentTime < gLastCheck)
        gLastCheck = currentTime;
    else if (currentTime - gLastCheck >= 4000000ULL)
    {
        gDevicesChanged = true;
        gLastCheck = currentTime;
    }

    if (!gNotifierInitialized)
        return false;

#if defined(_WINDOWS)
    MSG const * msg = (MSG const*)message;
    if (message && msg->message == WM_DEVICECHANGE)
    {
        WPARAM wParam = msg->wParam;
        LPARAM lParam = msg->lParam;

        if ((wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) &&
            ((DEV_BROADCAST_HDR*)lParam)->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
        {
            gDevicesChanged = true;
            return true;
        }
    }
#elif defined(__APPLE__)
    OutMessage outMSG;
    while (mach_msg(&outMSG.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(OutMessage),
                    gNotificationMach, 0, MACH_PORT_NULL) == KERN_SUCCESS)
        IODispatchCalloutFromMessage(NULL, &outMSG.hdr, gNotificationPort);
#elif defined(__linux__)
    if (gUDevFD >= 0)
    {
        struct pollfd pollFD = {};
        pollFD.fd = gUDevFD;
        pollFD.events = POLLIN;

        if (poll(&pollFD, 1, 0) == 1 && (pollFD.revents & POLLIN))
        {
            gDevicesChanged = true;
            struct udev_device* udevDev;
            while ((udevDev = udev_monitor_receive_device(pUDevMonitor)) != NULL)
                udev_device_unref(udevDev);
        }
    }
#endif
    return false;
}


// --- HID Management ---------------------------------------------------------

int HIDInit(void* window)
{
    if (gHIDInitialized)
        return 0;

    if (hid_init() != 0)
        return -1;

    HIDInitDeviceTracking();
    HIDInitControllerTracking();
    InitSystemNotificationReceiver(window);

    initHiresTimer(&gHIDTimer);

    gDevicesChanged = true;
    gHIDInitialized = true;

    return 0;
}

int HIDExit()
{
    if (!gHIDInitialized)
        return 0;

    gHIDInitialized = false;

    HIDExitDeviceTracking();
    ExitSystemNotificationReceiver();
    gDeviceChangeCallback = NULL;

    if (hid_exit() != 0)
        return -1;

    return 0;
}

// --- HID Device Tracking

static void HIDInitDeviceTracking()
{
    memset(gDeviceBuffer, 0, sizeof(gDeviceBuffer));
    gActiveDevices = 0;
    gActiveSlots = 0;

    HIDDeviceInfo* it = gDeviceBuffer;
    HIDDeviceInfo* end = it + MAX_DEVICES_TRACKED - 1;

    uint8_t idGen = 1;
    // link the array into a list
    for (; it < end; ++it, ++idGen)
    {
        it->id = idGen;
        it->next = it + 1;
    }
    it->id = idGen;
    it->next = NULL;

    // set the HEAD
    pFreeList = gDeviceBuffer;
}

static void HIDExitDeviceTracking()
{
    HIDDeviceInfo * devIt = gDeviceBuffer;
    HIDDeviceInfo * devEnd = devIt + gActiveSlots;

    for (; devIt < devEnd; ++devIt)
        if (devIt->wasActive)
            HIDRemoveDevice(devIt);
    HIDInitDeviceTracking();
}

void HIDSetDeviceChangeCallback(void (*deviceChange)(const char*, bool, int))
{
	if(!deviceChange)
	{
		gDeviceChangeCallback = NULL;
		return;
	}

	// no need to retrigger all the callbacks
	if(deviceChange == gDeviceChangeCallback)
		return;
	gDeviceChangeCallback = deviceChange;

	if(gActiveSlots > 0)
	{
		for(uint32_t i =0 ; i < gActiveSlots; i++)
		{
			if(gControllerBuffer[i].hidDev && gDeviceBuffer[i].active)
			{
				(*gDeviceChangeCallback)(ControllerName(gDeviceBuffer[i].type), true, gDeviceBuffer[i].id - 1);
			}
		}
	}
}

static void HIDAddDevice(hid_device_info *dev, uint8_t controllertype)
{
    if (!pFreeList)
    {
        // If this is hit the MAX_DEVICES_TRACKED cap needs to be increased
        // LOGF(LogLevel::eWARNING, "Failed to add controller due to lack of room: \"%ls - %ls\" at path: %s",
        //     dev->manufacturer_string,
        //     dev->product_string,
        //     dev->path);
        return;
    }
    // Never truncate an identity or overflow the tracker's fixed path storage.
    if (!dev->path || strlen(dev->path) >= MAX_PATH_LENGTH)
        return;

    // LOGF(LogLevel::eDEBUG, "Adding controller \"%ls - %ls\" at path: %s",
    //     dev->manufacturer_string,
    //     dev->product_string,
    //     dev->path);

    // pull a new registry slot off the pFreeList
    HIDDeviceInfo *newDev = pFreeList;
    pFreeList = pFreeList->next;

    newDev->vendorID = dev->vendor_id;
    newDev->productID = dev->product_id;
    newDev->active = 1;
    newDev->wasActive = 1;
    newDev->isOpen = 0;
    newDev->type = controllertype;
    strcpy(newDev->logicalSystemPath, dev->path);

    ++gActiveDevices;
    uint32_t usedSlots = uint32_t(newDev - gDeviceBuffer) + 1;
    if (usedSlots > gActiveSlots)
        gActiveSlots = usedSlots;

    HIDLoadController(newDev->id, 0);
}

static void HIDRemoveDevice(HIDDeviceInfo *dev)
{    
    bool wasOpen = dev->isOpen;
    if (wasOpen)
        HIDUnloadController(dev->id);
    if (gDeviceChangeCallback && wasOpen)
        (*gDeviceChangeCallback)(ControllerName(dev->type), false, dev->id - 1);

    dev->next = pFreeList;
    pFreeList = dev;

    dev->vendorID = 0;
    dev->productID = 0;
    dev->active = 0;
    dev->wasActive = 0;
    dev->isOpen = 0;
    dev->type = 0;

    --gActiveDevices;
}

static HIDDeviceInfo* HIDFindDevice(const hid_device_info* dev)
{
    if (!dev->path)
        return NULL;
    for (uint32_t i = 0; i < gActiveSlots; ++i)
    {
        HIDDeviceInfo* tracked = gDeviceBuffer + i;
        if (tracked->wasActive && tracked->vendorID == dev->vendor_id &&
            tracked->productID == dev->product_id &&
            strcmp(tracked->logicalSystemPath, dev->path) == 0)
            return tracked;
    }
    return NULL;
}

static void HIDLoadDevices()
{
    for (uint32_t i = 0; i < gActiveSlots; ++i)
        gDeviceBuffer[i].active = 0;

    hid_device_info* deviceList = hid_enumerate(0, 0);
    for (hid_device_info* dev = deviceList; dev; dev = dev->next)
    {
        HIDDeviceInfo* tracked = HIDFindDevice(dev);
        if (tracked)
            tracked->active = 1;
    }

    // Empty enumeration also retires stale connections. Free removed slots before
    // adding replacements so reconnect works when the tracker was at capacity.
    for (uint32_t i = 0; i < gActiveSlots; ++i)
        if (gDeviceBuffer[i].wasActive && !gDeviceBuffer[i].active)
            HIDRemoveDevice(gDeviceBuffer + i);

    // Retry unreadable controllers without announcing a connection before open.
    for (uint32_t i = 0; i < gActiveSlots; ++i)
        if (gDeviceBuffer[i].active && !gDeviceBuffer[i].isOpen)
            HIDLoadController(gDeviceBuffer[i].id, 0);

    for (hid_device_info* dev = deviceList; dev; dev = dev->next)
    {
        if (HIDFindDevice(dev))
            continue;
        if (!HIDIsController(dev))
        {
#if defined(HID_VERBOSE_LOGGING)
            LOGF(LogLevel::eDEBUG, "Ignoring non-controller \"%ls - %ls\" at path: %s",
                dev->manufacturer_string,
                dev->product_string,
                dev->path);
#endif
            continue;
        }
        uint8_t type;
        if (!HIDIsSupported(dev, &type))
        {
#if defined(HID_VERBOSE_LOGGING)
            LOGF(LogLevel::eDEBUG, "Ignoring unsupported controller \"%ls - %ls\" at path: %s",
                dev->manufacturer_string,
                dev->product_string,
                dev->path);
#endif
            continue;
        }
        HIDAddDevice(dev, type);
    }
    hid_free_enumeration(deviceList);
}

// Currently only detects controllers
static void HIDDetectDevices()
{
#if !defined(_WINDOWS)
    CheckForDeviceInstallation(NULL);
#endif

    if (gDevicesChanged)
    {
        HIDLoadDevices();

        gDevicesChanged = false;
    }
}

bool HIDHandleSystemMessage(void const* message)
{
    if (!gHIDInitialized)
        return false;

    bool result = false;
#if defined(_WINDOWS)
    result = CheckForDeviceInstallation(message);
#endif
    return result;
}


// --- HID Controller Setup

char const * HIDControllerName(uint8_t index)
{
    if (index >= gActiveSlots ||
        !gDeviceBuffer[index].active)
        return NULL;

    return ControllerName(gDeviceBuffer[index].type);
}

bool HIDControllerConnected(uint8_t index)
{
    if (index >= gActiveSlots)
        return false;

    return gDeviceBuffer[index].active && gDeviceBuffer[index].isOpen;
}


uint8_t HIDGetNextNewControllerID(uint8_t* outPlatform, uint16_t* outVendorID, uint16_t* outproductID)
{
    if (!gActiveSlots)
        return INVALID_DEV_ID;

    static uint8_t i = 0;
    if (i >= gActiveSlots)
        i = 0;

    for (uint8_t initialSlot = i;;)
    {
        if (gDeviceBuffer[i].active && !gDeviceBuffer[i].isOpen)
            break;

        i = (i + 1) % gActiveSlots;
        if (i == initialSlot)
            return INVALID_DEV_ID;
    }

    if (outproductID)
        *outproductID = gDeviceBuffer[i].productID;
    if (outVendorID)
        *outVendorID = gDeviceBuffer[i].vendorID;
    if (outPlatform)
        *outPlatform = gDeviceBuffer[i].type;

    // Device id is 1 higher than its index, plus this prepares for the next call
    return ++i;
}

static void HIDInitControllerTracking()
{
    for (uint32_t i = 0; i < MAX_DEVICES_TRACKED; ++i)
        gControllerBuffer[i] = HIDController{};
}

static inline void UnsupportedControllerDebug(HIDDeviceInfo* dev, char const* brand)
{
    // LOGF(LogLevel::eDEBUG, "Unsupported %s controller:   0x%.4X - 0x%.4X   Path: %s",
    //     brand,
    //     dev->vendorID,
    //     dev->productID,
    //     dev->logicalSystemPath);
}

static inline void FailedToOpenDebug(HIDDeviceInfo* dev, char const* brand)
{
    // LOGF(LogLevel::eDEBUG, "Failed to open %s controller:   0x%.4X - 0x%.4X   Path: %s",
    //     brand,
    //     dev->vendorID,
    //     dev->productID,
    //     dev->logicalSystemPath);
}

static inline void SuccessfulOpenDebug(HIDDeviceInfo* dev, char const* brand)
{
    // LOGF(LogLevel::eDEBUG, "Successfully opened %s controller:   0x%.4X - 0x%.4X   Path: %s",
    //     brand,
    //     dev->vendorID,
    //     dev->productID,
    //     dev->logicalSystemPath);
}

void HIDLoadController(uint8_t devID, uint8_t playerNum)
{
    GAINPUT_ASSERT(devID > 0 && devID <= MAX_DEVICES_TRACKED);
    uint8_t index = devID - 1;

    if (!gDeviceBuffer[index].active || gDeviceBuffer[index].isOpen)
    {
        // LOGF(LogLevel::eWARNING, "Tried to load non-existant device at index %i", index);
        return;
    }

    HIDDeviceInfo* dev = gDeviceBuffer + index;
    HIDController* con = gControllerBuffer + index;
    char const* name = ControllerName(dev->type);

    switch (dev->type)
    {
    case ctPS4:
        if (!HIDIsSupportedPS4Controller(dev))
        {
            UnsupportedControllerDebug(dev, name);
            return;
        }
        if (HIDOpenPS4Controller(dev, con, playerNum) == -1)
        {
            FailedToOpenDebug(dev, name);
            return;
        }
        SuccessfulOpenDebug(dev, name);
        break;
    case ctPS5:
        if (!HIDIsSupportedPS5Controller(dev))
        {
            UnsupportedControllerDebug(dev, name);
            return;
        }
        if (HIDOpenPS5Controller(dev, con, playerNum) == -1)
        {
            FailedToOpenDebug(dev, name);
            return;
        }
        SuccessfulOpenDebug(dev, name);
        break;
    default:
        return;
    }

    GAINPUT_ASSERT(con->Close && con->Update && "All controllers must set update & close methods.");
    dev->isOpen = 1;
    if (gDeviceChangeCallback)
        (*gDeviceChangeCallback)(name, true, index);
}

void HIDUnloadController(uint8_t devID)
{
    GAINPUT_ASSERT(devID > 0 && devID <= MAX_DEVICES_TRACKED);
    uint8_t index = devID - 1;

    HIDController* con = gControllerBuffer + index;
    gDeviceBuffer[index].isOpen = 0;
    if (!con->hidDev)
    {
        // LOGF(LogLevel::eWARNING, "Tried to unload non-existant controller at index %i", index);
        return;
    }

    GAINPUT_ASSERT(con->Close && "All controllers must set a close method.");

    con->Close(con);
    con->hidDev = NULL;

    // wipe data
    *con = HIDController{};
}


static bool GetController(uint8_t devID, HIDController** outCon)
{
    GAINPUT_ASSERT(devID > 0 && devID <= MAX_DEVICES_TRACKED);
    uint8_t index = devID - 1;

    HIDController* con = gControllerBuffer + index;
    if (!con->hidDev)
    {
        // LOGF(LogLevel::eWARNING, "Tried to unload non-existant controller at index %i", index);
        return false;
    }

    *outCon = con;
    return true;
}

void HIDSetPlayer(uint8_t devID, uint8_t playerNum)
{
    HIDController* con = nullptr;
    if (!GetController(devID, &con))
        return;

    if (con->SetPlayer)
        con->SetPlayer(con, playerNum);
}

void HIDSetLights(uint8_t devID, uint8_t r, uint8_t g, uint8_t b)
{
    HIDController* con = nullptr;
    if (!GetController(devID, &con))
        return;

    if (con->SetLights)
        con->SetLights(con, r, g, b);
}

void HIDDoRumble(uint8_t devID, float leftFlt, float rightFlt, uint32_t durationMS)
{
    HIDController* con = nullptr;
    if (!GetController(devID, &con))
        return;

    if (durationMS)
        con->rumbleEndTime = getHiresTimerUSec(&gHIDTimer, false) / 1000000UL + durationMS;

    if (leftFlt < 0)
        leftFlt = 0;
    if (leftFlt > 1)
        leftFlt = 1;
    if (rightFlt < 0)
        rightFlt = 0;
    if (rightFlt > 1)
        rightFlt = 1;

    //if both motors are set to 0 but we have a duration set
    // just set duration to 0 and cancel out the rumble itself
    if(durationMS && (rightFlt + leftFlt) <= 0.0001f)
        con->rumbleEndTime = 0;
    
    uint16_t left = (uint16_t)(0xFFFFu * leftFlt);
    uint16_t right = (uint16_t)(0xFFFFu * rightFlt);

    if (con->DoRumble)
        con->DoRumble(con, left, right);
}


// --- HID Input Fetching

static void HIDUpdateDevices(gainput::InputDeltaState * state)
{
    for (uint32_t i = 0; i < gActiveSlots; ++i)
    {
        HIDController* con = gControllerBuffer + i;
        if (!con->hidDev)
            continue;
        if (con->Update(con, state) < 0)
        {
            HIDRemoveDevice(gDeviceBuffer + i);
            gDevicesChanged = true;
            continue;
        }
        if (con->rumbleEndTime &&
            con->rumbleEndTime <= getHiresTimerUSec(&gHIDTimer, false) / 1000000UL)
        {
            con->DoRumble(con, 0, 0);
            con->rumbleEndTime = 0;
        }
    }
}

void HIDPromptForDeviceStateReports(gainput::InputDeltaState * state)
{
    if (!gHIDInitialized)
        return;

    // Connection changes precede this poll's reports. A read failure retires its
    // handle now and can only reopen during a later poll.
    HIDDetectDevices();
    HIDUpdateDevices(state);
}
