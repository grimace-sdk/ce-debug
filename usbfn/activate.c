#include "activate.h"

#define IOCTL_UFN_GET_CURRENT_CLIENT 0x0022000c
#define IOCTL_UFN_CHANGE_CURRENT_CLIENT 0x00220010
#define UFN_CLIENT_NAME_BYTES 0x100
#define UFN_CLIENT_INFO_BYTES 0x2f4
#define CLIENT_KEY L"Drivers\\USB\\FunctionDrivers\\" USB_SERIAL_CLIENT
#define RNDIS_CLIENT_KEY L"Drivers\\USB\\FunctionDrivers\\" USB_RNDIS_CLIENT
#define RNDIS_DLL L"rndisfn.dll"
#define RNDIS_VENDOR 0x045e
#define RNDIS_PRODUCT 0x0302
#define RNDIS_SHARING_PRODUCT 0x0303

typedef struct {
    DWORD data1;
    WORD data2;
    WORD data3;
    BYTE data4[8];
} interface_guid;

static const interface_guid usb_function_bus = { 0xe2bdc372, 0x598f, 0x4619, { 0xbc, 0x50, 0x54, 0xb3, 0xf7, 0x84, 0x8d, 0x35 } };

static void copy_text(WCHAR *destination, const WCHAR *source, DWORD characters)
{
    DWORD index;

    for (index = 0; index + 1 < characters && source[index]; index++)
        destination[index] = source[index];
    if (characters)
        destination[index] = 0;
}

static LONG set_string(HKEY key, const WCHAR *name, const WCHAR *value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (lstrlenW(value) + 1) * sizeof(WCHAR));
}

static LONG set_number(HKEY key, const WCHAR *name, DWORD value)
{
    return RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
}

LONG usb_register_serial_client(const WCHAR *driver_path)
{
    HKEY key;
    DWORD disposition;
    LONG result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, CLIENT_KEY, 0, NULL, 0, 0, NULL, &key, &disposition);

    if (result != ERROR_SUCCESS)
        return result;
    result = set_string(key, L"Dll", driver_path);
    if (result == ERROR_SUCCESS)
        result = set_string(key, L"Prefix", L"USC");
    if (result == ERROR_SUCCESS)
        result = set_number(key, L"Index", 1);
    if (result == ERROR_SUCCESS)
        result = set_string(key, L"FriendlyName", L"Brain USB Serial");
    RegCloseKey(key);
    return result;
}

static LONG create_key(const WCHAR *path, HKEY *key)
{
    DWORD disposition;

    return RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, 0, NULL, key, &disposition);
}

static LONG register_ndis_driver(const WCHAR *path, BOOL no_device)
{
    HKEY key;
    LONG result = create_key(path, &key);

    if (result != ERROR_SUCCESS)
        return result;
    result = set_string(key, L"ImagePath", RNDIS_DLL);
    if (result == ERROR_SUCCESS)
        result = set_string(key, L"Group", L"NDIS");
    if (result == ERROR_SUCCESS && no_device)
        result = set_number(key, L"NoDeviceCreate", 1);
    RegCloseKey(key);
    return result;
}

static LONG set_multi_string(HKEY key, const WCHAR *name, const WCHAR *value)
{
    WCHAR data[32];
    int length = lstrlenW(value);

    memset(data, 0, sizeof(data));
    memcpy(data, value, length * sizeof(WCHAR));
    return RegSetValueExW(key, name, 0, REG_MULTI_SZ, (const BYTE *)data, (length + 2) * sizeof(WCHAR));
}

LONG usb_register_rndis_client(void)
{
    HKEY key;
    LONG result = create_key(RNDIS_CLIENT_KEY, &key);

    if (result != ERROR_SUCCESS)
        return result;
    result = set_string(key, L"Dll", RNDIS_DLL);
    if (result == ERROR_SUCCESS)
        result = set_string(key, L"FriendlyName", L"Rndis");
    if (result == ERROR_SUCCESS)
        result = set_number(key, L"idVendor", RNDIS_VENDOR);
    if (result == ERROR_SUCCESS)
        result = set_number(key, L"idProduct", RNDIS_PRODUCT);
    if (result == ERROR_SUCCESS)
        result = set_number(key, L"idProductInternetSharing", RNDIS_SHARING_PRODUCT);
    if (result == ERROR_SUCCESS)
        result = set_number(key, L"bcdDevice", 0);
    if (result == ERROR_SUCCESS)
        result = set_string(key, L"Manufacturer", L"pwsh2");
    if (result == ERROR_SUCCESS)
        result = set_string(key, L"Product", L"Brain RNDIS");
    RegCloseKey(key);
    if (result == ERROR_SUCCESS)
        result = register_ndis_driver(L"Comm\\RndisFn", TRUE);
    if (result == ERROR_SUCCESS)
        result = create_key(L"Comm\\RndisFn\\Linkage", &key);
    if (result == ERROR_SUCCESS) {
        result = set_multi_string(key, L"Route", L"RndisFn1");
        RegCloseKey(key);
    }
    if (result == ERROR_SUCCESS)
        result = register_ndis_driver(L"Comm\\RndisFn1", FALSE);
    if (result == ERROR_SUCCESS)
        result = create_key(L"Comm\\RndisFn1\\Parms", &key);
    if (result == ERROR_SUCCESS) {
        result = set_number(key, L"BusType", 0);
        if (result == ERROR_SUCCESS)
            result = set_number(key, L"BusNumber", 0);
        if (result == ERROR_SUCCESS)
            result = set_number(key, L"RebindOnResume", 0);
        RegCloseKey(key);
    }
    if (result == ERROR_SUCCESS)
        result = create_key(L"Comm\\RndisFn1\\Parms\\TcpIp", &key);
    if (result == ERROR_SUCCESS) {
        result = set_number(key, L"EnableDHCP", 1);
        RegCloseKey(key);
    }
    return result;
}

static HANDLE open_function_bus(void)
{
    DEVMGR_DEVICE_INFORMATION *information = (DEVMGR_DEVICE_INFORMATION *)LocalAlloc(LPTR, sizeof(DEVMGR_DEVICE_INFORMATION));
    HANDLE search, bus = INVALID_HANDLE_VALUE;

    if (!information)
        return INVALID_HANDLE_VALUE;
    information->dwSize = sizeof(DEVMGR_DEVICE_INFORMATION);
    search = FindFirstDevice(DeviceSearchByGuid, &usb_function_bus, information);
    if (search != INVALID_HANDLE_VALUE) {
        bus = CreateFileW(information->szBusName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        FindClose(search);
    }
    LocalFree(information);
    return bus;
}

BOOL usb_current_client(WCHAR *name, DWORD name_characters)
{
    HANDLE bus = open_function_bus();
    BYTE *information;
    DWORD returned = 0;
    BOOL ok;

    if (bus == INVALID_HANDLE_VALUE)
        return FALSE;
    information = (BYTE *)LocalAlloc(LPTR, UFN_CLIENT_INFO_BYTES);
    ok = information && DeviceIoControl(bus, IOCTL_UFN_GET_CURRENT_CLIENT, NULL, 0, information, UFN_CLIENT_INFO_BYTES,
        &returned, NULL);
    if (ok)
        copy_text(name, (const WCHAR *)information, name_characters);
    if (information)
        LocalFree(information);
    CloseHandle(bus);
    return ok;
}

BOOL usb_select_client(const WCHAR *name)
{
    HANDLE bus = open_function_bus();
    WCHAR client[UFN_CLIENT_NAME_BYTES / sizeof(WCHAR)];
    BOOL ok;

    if (bus == INVALID_HANDLE_VALUE)
        return FALSE;
    memset(client, 0, sizeof(client));
    copy_text(client, name, UFN_CLIENT_NAME_BYTES / sizeof(WCHAR));
    ok = DeviceIoControl(bus, IOCTL_UFN_CHANGE_CURRENT_CLIENT, client, sizeof(client), NULL, 0, NULL, NULL);
    CloseHandle(bus);
    return ok;
}

BOOL usb_find_active_device(const WCHAR *client, WCHAR *name, DWORD name_characters)
{
    HKEY active;
    WCHAR subkey[64];
    DWORD index;
    BOOL found = FALSE;
    int client_length = lstrlenW(client);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Drivers\\Active", 0, 0, &active) != ERROR_SUCCESS)
        return FALSE;
    for (index = 0; !found; index++) {
        DWORD subkey_length = sizeof(subkey) / sizeof(subkey[0]);
        HKEY device;
        WCHAR key_path[MAX_PATH];
        DWORD size = sizeof(key_path), type = 0;
        int key_length;

        if (RegEnumKeyExW(active, index, subkey, &subkey_length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExW(active, subkey, 0, 0, &device) != ERROR_SUCCESS)
            continue;
        if (RegQueryValueExW(device, L"Key", NULL, &type, (BYTE *)key_path, &size) == ERROR_SUCCESS && type == REG_SZ) {
            key_length = lstrlenW(key_path);
            if (key_length >= client_length && !lstrcmpiW(key_path + key_length - client_length, client)) {
                size = name_characters * sizeof(WCHAR);
                found = RegQueryValueExW(device, L"Name", NULL, &type, (BYTE *)name, &size) == ERROR_SUCCESS && type == REG_SZ;
            }
        }
        RegCloseKey(device);
    }
    RegCloseKey(active);
    return found;
}
