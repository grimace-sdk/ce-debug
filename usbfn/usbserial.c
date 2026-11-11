#include <windows.h>

#include "events.h"

#define UFN_CLIENT_INTERFACE_VERSION 0x00060000
#define IOCTL_UFN_GET_CLIENT_INTERFACE 0x0022002c
#define USB_IN_TRANSFER 0x80
#define USB_OUT_TRANSFER 0x00
#define UFN_MSG_BUS_EVENTS 1
#define UFN_MSG_CONFIGURED 2
#define UFN_MSG_BUS_SPEED 3
#define UFN_MSG_SETUP_PACKET 4
#define UFN_DETACH 0
#define UFN_RESET 2
#define EVENT_RESET_CODE 2
#define EVENT_SET_CODE 3
#define IOCTL_USBSERIAL_STATE 0x00222000

#define BULK_IN_ENDPOINT 0x81
#define BULK_OUT_ENDPOINT 0x02
#define NOTIFY_ENDPOINT 0x83
#define RX_TRANSFER_SIZE 512
#define DMA_PAGE_SIZE 4096
#define RX_RING_SIZE 32768
#define TX_TRANSFER_SIZE 2048
#define FULL_SPEED_PACKET 64
#define WRITE_TIMEOUT_MS 5000
#define READ_POLL_MS 1000

typedef void *ufn_handle;
typedef void *ufn_pipe;
typedef void *ufn_transfer;

typedef BOOL (*device_notify_routine)(void *parameter, DWORD message, DWORD message_parameter);
typedef DWORD (*transfer_complete_routine)(void *parameter);

typedef struct {
    DWORD (*register_device)(ufn_handle device, const BYTE *high_speed_device, const void *high_speed_configuration,
        const BYTE *full_speed_device, const void *full_speed_configuration, const void *string_sets, DWORD string_set_count);
    DWORD (*start)(ufn_handle device, device_notify_routine notify, void *parameter, ufn_pipe *default_pipe);
    DWORD (*stop)(ufn_handle device);
    DWORD (*deregister_device)(ufn_handle device);
    DWORD (*open_pipe)(ufn_handle device, DWORD endpoint_address, ufn_pipe *pipe);
    DWORD (*close_pipe)(ufn_handle device, ufn_pipe pipe);
    DWORD (*issue_transfer)(ufn_handle device, ufn_pipe pipe, transfer_complete_routine complete, void *parameter, DWORD flags,
        DWORD length, void *buffer, DWORD physical_address, void *pdd_information, ufn_transfer *transfer);
    DWORD (*get_transfer_status)(ufn_handle device, ufn_transfer transfer, DWORD *transferred, DWORD *usb_error);
    DWORD (*abort_transfer)(ufn_handle device, ufn_transfer transfer);
    DWORD (*close_transfer)(ufn_handle device, ufn_transfer transfer);
    DWORD (*stall_pipe)(ufn_handle device, ufn_pipe pipe);
    DWORD (*clear_pipe_stall)(ufn_handle device, ufn_pipe pipe);
    DWORD (*initiate_remote_wakeup)(ufn_handle device);
    DWORD (*send_control_status_handshake)(ufn_handle device);
} ufn_functions;

typedef struct {
    DWORD version;
    ufn_handle device;
    ufn_functions functions;
    void *interface_context;
} ufn_client_interface;

typedef struct {
    DWORD count;
    BYTE descriptor[7];
    BYTE padding;
    void *extended;
    DWORD extended_size;
} ufn_endpoint;

typedef struct {
    DWORD count;
    BYTE descriptor[9];
    BYTE padding[3];
    void *extended;
    DWORD extended_size;
    ufn_endpoint *endpoints;
} ufn_interface;

typedef struct {
    DWORD count;
    BYTE descriptor[9];
    BYTE padding[3];
    void *extended;
    DWORD extended_size;
    ufn_interface *interfaces;
} ufn_configuration;

typedef struct {
    WORD language;
    const WCHAR **strings;
    DWORD count;
} ufn_string_set;

typedef struct {
    BYTE request_type;
    BYTE request;
    WORD value;
    WORD index;
    WORD length;
} setup_packet;

_Static_assert(sizeof(ufn_functions) == 0x38, "UFN function table size");
_Static_assert(sizeof(ufn_client_interface) == 0x44, "UFN client interface size");
_Static_assert(sizeof(ufn_endpoint) == 20, "UFN endpoint size");
_Static_assert(sizeof(ufn_interface) == 28, "UFN interface size");
_Static_assert(sizeof(ufn_configuration) == 28, "UFN configuration size");
_Static_assert(sizeof(ufn_string_set) == 12, "UFN string set size");

HANDLE CreateBusAccessHandle(LPCWSTR active_key);
void CloseBusAccessHandle(HANDLE bus);
BOOL BusChildIoControl(HANDLE bus, DWORD code, void *buffer, DWORD size);
BOOL EventModify(HANDLE event, DWORD function);

typedef struct {
    HANDLE bus;
    ufn_handle device;
    ufn_functions ufn;
    ufn_pipe control_pipe;
    ufn_pipe bulk_in;
    ufn_pipe bulk_out;
    ufn_pipe notify_in;
    volatile BOOL configured;
    volatile BOOL closing;
    DWORD speed;
    DWORD line_state;
    CRITICAL_SECTION lock;
    CRITICAL_SECTION write_lock;
    BYTE line_coding[7];
    BYTE *control_buffer;
    BOOL control_is_set_line_coding;
    ufn_transfer control_transfer;
    BYTE rx_ring[RX_RING_SIZE];
    DWORD rx_start;
    DWORD rx_count;
    BYTE *rx_packet;
    ufn_transfer rx_transfer;
    BOOL rx_pending;
    HANDLE rx_event;
    BYTE *tx_buffer;
    BYTE *dma_pages;
    ufn_transfer tx_transfer;
    DWORD tx_transferred;
    DWORD tx_error;
    HANDLE tx_event;
    HANDLE rx_done_event;
    HANDLE control_done_event;
    HANDLE worker_stop_event;
    HANDLE worker;
    LONG open_count;
} serial_context;

static const BYTE device_descriptor[18] = {
    18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 0xdd, 0x04, 0x44, 0x92, 0x00, 0x01, 1, 2, 3, 1,
};

static const BYTE cdc_functional_descriptors[19] = {
    5, 0x24, 0x00, 0x10, 0x01,
    5, 0x24, 0x01, 0x00, 0x01,
    4, 0x24, 0x02, 0x02,
    5, 0x24, 0x06, 0x00, 0x01,
};

static const WCHAR manufacturer[] = L"SHARP";
static const WCHAR product[] = L"Brain USB Serial";
static const WCHAR serial_number[] = L"PW-SH2";
static const WCHAR *strings[3] = { manufacturer, product, serial_number };
static const ufn_string_set string_set = { 0x0409, strings, 3 };

static ufn_endpoint high_speed_comm_endpoints[1];
static ufn_endpoint high_speed_data_endpoints[2];
static ufn_endpoint full_speed_comm_endpoints[1];
static ufn_endpoint full_speed_data_endpoints[2];
static ufn_interface high_speed_interfaces[2];
static ufn_interface full_speed_interfaces[2];
static ufn_configuration high_speed_configuration;
static ufn_configuration full_speed_configuration;

static void set_endpoint(ufn_endpoint *endpoint, BYTE address, BYTE attributes, WORD max_packet, BYTE interval)
{
    endpoint->count = sizeof(ufn_endpoint);
    endpoint->descriptor[0] = 7;
    endpoint->descriptor[1] = 5;
    endpoint->descriptor[2] = address;
    endpoint->descriptor[3] = attributes;
    endpoint->descriptor[4] = (BYTE)max_packet;
    endpoint->descriptor[5] = (BYTE)(max_packet >> 8);
    endpoint->descriptor[6] = interval;
}

static void set_interface(ufn_interface *interface_entry, BYTE number, BYTE endpoint_count, BYTE class_code, BYTE subclass,
    BYTE protocol, ufn_endpoint *endpoints, const BYTE *extended, DWORD extended_size)
{
    interface_entry->count = sizeof(ufn_interface);
    interface_entry->descriptor[0] = 9;
    interface_entry->descriptor[1] = 4;
    interface_entry->descriptor[2] = number;
    interface_entry->descriptor[3] = 0;
    interface_entry->descriptor[4] = endpoint_count;
    interface_entry->descriptor[5] = class_code;
    interface_entry->descriptor[6] = subclass;
    interface_entry->descriptor[7] = protocol;
    interface_entry->descriptor[8] = 0;
    interface_entry->extended = (void *)extended;
    interface_entry->extended_size = extended_size;
    interface_entry->endpoints = endpoints;
}

static void set_configuration(ufn_configuration *configuration, ufn_interface *interfaces)
{
    WORD total = 9 + 9 + sizeof(cdc_functional_descriptors) + 7 + 9 + 7 + 7;

    configuration->count = sizeof(ufn_configuration);
    configuration->descriptor[0] = 9;
    configuration->descriptor[1] = 2;
    configuration->descriptor[2] = (BYTE)total;
    configuration->descriptor[3] = (BYTE)(total >> 8);
    configuration->descriptor[4] = 2;
    configuration->descriptor[5] = 1;
    configuration->descriptor[6] = 0;
    configuration->descriptor[7] = 0xc0;
    configuration->descriptor[8] = 50;
    configuration->interfaces = interfaces;
}

static void build_descriptors(void)
{
    set_endpoint(&high_speed_comm_endpoints[0], NOTIFY_ENDPOINT, 3, 16, 9);
    set_endpoint(&high_speed_data_endpoints[0], BULK_IN_ENDPOINT, 2, 512, 0);
    set_endpoint(&high_speed_data_endpoints[1], BULK_OUT_ENDPOINT, 2, 512, 0);
    set_endpoint(&full_speed_comm_endpoints[0], NOTIFY_ENDPOINT, 3, 16, 32);
    set_endpoint(&full_speed_data_endpoints[0], BULK_IN_ENDPOINT, 2, 64, 0);
    set_endpoint(&full_speed_data_endpoints[1], BULK_OUT_ENDPOINT, 2, 64, 0);
    set_interface(&high_speed_interfaces[0], 0, 1, 0x02, 0x02, 0x01, high_speed_comm_endpoints,
        cdc_functional_descriptors, sizeof(cdc_functional_descriptors));
    set_interface(&high_speed_interfaces[1], 1, 2, 0x0a, 0x00, 0x00, high_speed_data_endpoints, NULL, 0);
    set_interface(&full_speed_interfaces[0], 0, 1, 0x02, 0x02, 0x01, full_speed_comm_endpoints,
        cdc_functional_descriptors, sizeof(cdc_functional_descriptors));
    set_interface(&full_speed_interfaces[1], 1, 2, 0x0a, 0x00, 0x00, full_speed_data_endpoints, NULL, 0);
    set_configuration(&high_speed_configuration, high_speed_interfaces);
    set_configuration(&full_speed_configuration, full_speed_interfaces);
}

static usb_serial_event event_ring[USB_SERIAL_EVENT_LIMIT];
static LONG event_sequence;

static void record_event(DWORD code, DWORD first, DWORD second)
{
    DWORD sequence = (DWORD)InterlockedIncrement(&event_sequence);
    usb_serial_event *event = &event_ring[(sequence - 1) % USB_SERIAL_EVENT_LIMIT];

    event->tick = GetTickCount();
    event->code = code;
    event->first = first;
    event->second = second;
    event->sequence = sequence;
}

static void start_receive(serial_context *context);

static DWORD rx_complete(void *parameter)
{
    serial_context *context = (serial_context *)parameter;

    SetEvent(context->rx_done_event);
    return 0;
}

static void finish_receive(serial_context *context)
{
    DWORD transferred = 0, usb_error = 0, index;
    ufn_transfer transfer;

    EnterCriticalSection(&context->lock);
    transfer = context->rx_transfer;
    context->rx_transfer = NULL;
    context->rx_pending = FALSE;
    LeaveCriticalSection(&context->lock);
    if (!transfer) {
        record_event(USB_SERIAL_EVENT_RX_COMPLETE, 0xffffffff, 0);
        return;
    }
    context->ufn.get_transfer_status(context->device, transfer, &transferred, &usb_error);
    context->ufn.close_transfer(context->device, transfer);
    record_event(USB_SERIAL_EVENT_RX_COMPLETE, transferred, usb_error);
    EnterCriticalSection(&context->lock);
    if (usb_error == 0 && context->configured) {
        for (index = 0; index < transferred && context->rx_count < RX_RING_SIZE; index++) {
            context->rx_ring[(context->rx_start + context->rx_count) % RX_RING_SIZE] = context->rx_packet[index];
            context->rx_count++;
        }
        if (transferred)
            EventModify(context->rx_event, EVENT_SET_CODE);
    }
    LeaveCriticalSection(&context->lock);
    start_receive(context);
}
static void start_receive(serial_context *context)
{
    DWORD result;

    EnterCriticalSection(&context->lock);
    if (!context->configured || context->rx_pending || RX_RING_SIZE - context->rx_count < RX_TRANSFER_SIZE) {
        record_event(USB_SERIAL_EVENT_RX_SKIPPED, context->configured | context->rx_pending << 1, context->rx_count);
        LeaveCriticalSection(&context->lock);
        return;
    }
    context->rx_pending = TRUE;
    LeaveCriticalSection(&context->lock);
    result = context->ufn.issue_transfer(context->device, context->bulk_out, rx_complete, context, USB_OUT_TRANSFER,
        RX_TRANSFER_SIZE, context->rx_packet, 0, NULL, &context->rx_transfer);
    record_event(USB_SERIAL_EVENT_RX_ISSUE, result, (DWORD)context->rx_transfer);
    if (result != ERROR_SUCCESS) {
        EnterCriticalSection(&context->lock);
        context->rx_transfer = NULL;
        context->rx_pending = FALSE;
        LeaveCriticalSection(&context->lock);
    }
}

static DWORD tx_complete(void *parameter)
{
    serial_context *context = (serial_context *)parameter;

    EventModify(context->tx_event, EVENT_SET_CODE);
    return 0;
}

static void finish_transmit(serial_context *context)
{
    context->tx_transferred = 0;
    context->tx_error = 0;
    if (context->tx_transfer) {
        context->ufn.get_transfer_status(context->device, context->tx_transfer, &context->tx_transferred, &context->tx_error);
        context->ufn.close_transfer(context->device, context->tx_transfer);
        context->tx_transfer = NULL;
    }
    record_event(USB_SERIAL_EVENT_TX_COMPLETE, context->tx_transferred, context->tx_error);
}
static DWORD control_complete(void *parameter)
{
    serial_context *context = (serial_context *)parameter;

    SetEvent(context->control_done_event);
    return 0;
}

static void finish_control(serial_context *context)
{
    DWORD transferred = 0, usb_error = 0;
    ufn_transfer transfer = context->control_transfer;

    context->control_transfer = NULL;
    if (transfer) {
        context->ufn.get_transfer_status(context->device, transfer, &transferred, &usb_error);
        context->ufn.close_transfer(context->device, transfer);
    }
    record_event(USB_SERIAL_EVENT_CONTROL_COMPLETE, transferred, usb_error);
    if (context->control_is_set_line_coding && usb_error == 0 && transferred >= sizeof(context->line_coding))
        memcpy(context->line_coding, context->control_buffer, sizeof(context->line_coding));
    context->ufn.send_control_status_handshake(context->device);
}

static DWORD WINAPI completion_worker(LPVOID parameter)
{
    serial_context *context = (serial_context *)parameter;
    HANDLE events[3];

    events[0] = context->worker_stop_event;
    events[1] = context->control_done_event;
    events[2] = context->rx_done_event;
    for (;;) {
        DWORD signalled = WaitForMultipleObjects(3, events, FALSE, INFINITE);

        if (signalled == WAIT_OBJECT_0 + 1)
            finish_control(context);
        else if (signalled == WAIT_OBJECT_0 + 2)
            finish_receive(context);
        else
            return 0;
    }
}
static void control_data_stage(serial_context *context, DWORD flags, DWORD length, BOOL set_line_coding)
{
    DWORD result;

    context->control_is_set_line_coding = set_line_coding;
    result = context->ufn.issue_transfer(context->device, context->control_pipe, control_complete, context, flags, length,
        context->control_buffer, 0, NULL, &context->control_transfer);
    record_event(USB_SERIAL_EVENT_CONTROL_ISSUE, result, flags | length << 8);
    if (result != ERROR_SUCCESS) {
        context->control_transfer = NULL;
        context->ufn.stall_pipe(context->device, context->control_pipe);
        context->ufn.send_control_status_handshake(context->device);
    }
}

static void handle_setup(serial_context *context, const setup_packet *setup)
{
    record_event(USB_SERIAL_EVENT_SETUP, setup->request_type | setup->request << 8 | (DWORD)setup->value << 16,
        setup->index | (DWORD)setup->length << 16);
    if (setup->request_type == 0x21 && setup->request == 0x20 && setup->length == sizeof(context->line_coding)) {
        control_data_stage(context, USB_OUT_TRANSFER, sizeof(context->line_coding), TRUE);
    } else if (setup->request_type == 0xa1 && setup->request == 0x21) {
        memcpy(context->control_buffer, context->line_coding, sizeof(context->line_coding));
        control_data_stage(context, USB_IN_TRANSFER, setup->length < sizeof(context->line_coding) ? setup->length :
            sizeof(context->line_coding), FALSE);
    } else if (setup->request_type == 0x21 && (setup->request == 0x22 || setup->request == 0x23)) {
        if (setup->request == 0x22)
            context->line_state = setup->value;
        context->ufn.send_control_status_handshake(context->device);
    } else {
        context->ufn.stall_pipe(context->device, context->control_pipe);
        context->ufn.send_control_status_handshake(context->device);
    }
}

static void close_pipe(serial_context *context, ufn_pipe *pipe)
{
    if (*pipe) {
        context->ufn.close_pipe(context->device, *pipe);
        *pipe = NULL;
    }
}

static void unconfigure(serial_context *context)
{
    ufn_transfer transfer;

    EnterCriticalSection(&context->lock);
    context->configured = FALSE;
    transfer = context->rx_transfer;
    LeaveCriticalSection(&context->lock);
    if (transfer)
        context->ufn.abort_transfer(context->device, transfer);
    EnterCriticalSection(&context->lock);
    context->rx_start = 0;
    context->rx_count = 0;
    context->line_state = 0;
    LeaveCriticalSection(&context->lock);
    close_pipe(context, &context->bulk_in);
    close_pipe(context, &context->bulk_out);
    close_pipe(context, &context->notify_in);
    EventModify(context->rx_event, EVENT_SET_CODE);
}

static void configure(serial_context *context)
{
    DWORD in_result, out_result = 0xffffffff, notify_result = 0xffffffff;

    if (context->configured)
        unconfigure(context);
    in_result = context->ufn.open_pipe(context->device, BULK_IN_ENDPOINT, &context->bulk_in);
    if (in_result == ERROR_SUCCESS)
        out_result = context->ufn.open_pipe(context->device, BULK_OUT_ENDPOINT, &context->bulk_out);
    if (out_result == ERROR_SUCCESS)
        notify_result = context->ufn.open_pipe(context->device, NOTIFY_ENDPOINT, &context->notify_in);
    record_event(USB_SERIAL_EVENT_PIPE, in_result | out_result << 16, notify_result);
    if (in_result != ERROR_SUCCESS || out_result != ERROR_SUCCESS || notify_result != ERROR_SUCCESS) {
        close_pipe(context, &context->bulk_in);
        close_pipe(context, &context->bulk_out);
        close_pipe(context, &context->notify_in);
        return;
    }
    in_result = context->ufn.clear_pipe_stall(context->device, context->bulk_in);
    out_result = context->ufn.clear_pipe_stall(context->device, context->bulk_out);
    record_event(USB_SERIAL_EVENT_CLEAR_STALL, in_result, out_result);
    context->configured = TRUE;
    start_receive(context);
}

static BOOL device_notify(void *parameter, DWORD message, DWORD message_parameter)
{
    serial_context *context = (serial_context *)parameter;

    if (message != UFN_MSG_SETUP_PACKET)
        record_event(USB_SERIAL_EVENT_NOTIFY, message, message_parameter);
    switch (message) {
    case UFN_MSG_BUS_EVENTS:
        if (message_parameter == UFN_DETACH || message_parameter == UFN_RESET)
            unconfigure(context);
        break;
    case UFN_MSG_CONFIGURED:
        if (message_parameter)
            configure(context);
        else
            unconfigure(context);
        break;
    case UFN_MSG_BUS_SPEED:
        context->speed = message_parameter;
        break;
    case UFN_MSG_SETUP_PACKET:
        handle_setup(context, (const setup_packet *)message_parameter);
        break;
    }
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}

DWORD USC_Init(LPCWSTR active_key, const void *bus_context)
{
    serial_context *context = (serial_context *)LocalAlloc(LPTR, sizeof(serial_context));
    ufn_client_interface client;
    static const BYTE default_line_coding[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 };

    if (!context)
        return 0;
    context->dma_pages = (BYTE *)VirtualAlloc(NULL, 3 * DMA_PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (!context->dma_pages) {
        LocalFree(context);
        return 0;
    }
    context->rx_packet = context->dma_pages;
    context->tx_buffer = context->dma_pages + DMA_PAGE_SIZE;
    context->control_buffer = context->dma_pages + 2 * DMA_PAGE_SIZE;
    InitializeCriticalSection(&context->lock);
    InitializeCriticalSection(&context->write_lock);
    memcpy(context->line_coding, default_line_coding, sizeof(default_line_coding));
    context->rx_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    context->tx_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    context->rx_done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    context->control_done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    context->worker_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    context->bus = CreateBusAccessHandle(active_key);
    memset(&client, 0, sizeof(client));
    client.version = UFN_CLIENT_INTERFACE_VERSION;
    if (!context->rx_event || !context->tx_event || !context->rx_done_event || !context->control_done_event ||
        !context->worker_stop_event || !context->bus ||
        !BusChildIoControl(context->bus, IOCTL_UFN_GET_CLIENT_INTERFACE, &client, sizeof(client)))
        goto failed;
    context->device = client.device;
    context->ufn = client.functions;
    build_descriptors();
    context->worker = CreateThread(NULL, 0, completion_worker, context, 0, NULL);
    if (!context->worker)
        goto failed;
    if (context->ufn.register_device(context->device, device_descriptor, &high_speed_configuration, device_descriptor,
            &full_speed_configuration, &string_set, 1) != ERROR_SUCCESS)
        goto failed;
    if (context->ufn.start(context->device, device_notify, context, &context->control_pipe) != ERROR_SUCCESS) {
        context->ufn.deregister_device(context->device);
        goto failed;
    }
    return (DWORD)context;

failed:
    if (context->worker) {
        SetEvent(context->worker_stop_event);
        WaitForSingleObject(context->worker, INFINITE);
        CloseHandle(context->worker);
    }
    if (context->bus)
        CloseBusAccessHandle(context->bus);
    if (context->rx_event)
        CloseHandle(context->rx_event);
    if (context->tx_event)
        CloseHandle(context->tx_event);
    if (context->rx_done_event)
        CloseHandle(context->rx_done_event);
    if (context->control_done_event)
        CloseHandle(context->control_done_event);
    if (context->worker_stop_event)
        CloseHandle(context->worker_stop_event);
    DeleteCriticalSection(&context->lock);
    DeleteCriticalSection(&context->write_lock);
    VirtualFree(context->dma_pages, 0, MEM_RELEASE);
    LocalFree(context);
    return 0;
}

BOOL USC_Deinit(DWORD handle)
{
    serial_context *context = (serial_context *)handle;

    if (!context)
        return FALSE;
    context->closing = TRUE;
    unconfigure(context);
    context->ufn.stop(context->device);
    context->ufn.deregister_device(context->device);
    SetEvent(context->worker_stop_event);
    WaitForSingleObject(context->worker, INFINITE);
    CloseHandle(context->worker);
    CloseBusAccessHandle(context->bus);
    CloseHandle(context->rx_event);
    CloseHandle(context->tx_event);
    CloseHandle(context->rx_done_event);
    CloseHandle(context->control_done_event);
    CloseHandle(context->worker_stop_event);
    DeleteCriticalSection(&context->lock);
    DeleteCriticalSection(&context->write_lock);
    VirtualFree(context->dma_pages, 0, MEM_RELEASE);
    LocalFree(context);
    return TRUE;
}

DWORD USC_Open(DWORD handle, DWORD access, DWORD share)
{
    serial_context *context = (serial_context *)handle;

    InterlockedIncrement(&context->open_count);
    context->closing = FALSE;
    return handle;
}

BOOL USC_Close(DWORD handle)
{
    serial_context *context = (serial_context *)handle;

    if (InterlockedDecrement(&context->open_count) == 0) {
        context->closing = TRUE;
        EventModify(context->rx_event, EVENT_SET_CODE);
    }
    return TRUE;
}

DWORD USC_Read(DWORD handle, void *buffer, DWORD length)
{
    serial_context *context = (serial_context *)handle;
    BYTE *output = (BYTE *)buffer;
    DWORD copied = 0;

    if (!length)
        return 0;
    for (;;) {
        EnterCriticalSection(&context->lock);
        while (copied < length && context->rx_count) {
            output[copied++] = context->rx_ring[context->rx_start];
            context->rx_start = (context->rx_start + 1) % RX_RING_SIZE;
            context->rx_count--;
        }
        LeaveCriticalSection(&context->lock);
        if (copied) {
            record_event(USB_SERIAL_EVENT_READ, copied, context->rx_count);
            start_receive(context);
            return copied;
        }
        if (context->closing)
            return 0;
        WaitForSingleObject(context->rx_event, READ_POLL_MS);
    }
}

static BOOL send_chunk(serial_context *context, DWORD length)
{
    DWORD result;

    EventModify(context->tx_event, EVENT_RESET_CODE);
    result = context->ufn.issue_transfer(context->device, context->bulk_in, tx_complete, context, USB_IN_TRANSFER, length,
        context->tx_buffer, 0, NULL, &context->tx_transfer);
    record_event(USB_SERIAL_EVENT_TX_ISSUE, result, length);
    if (result != ERROR_SUCCESS) {
        context->tx_transfer = NULL;
        return FALSE;
    }
    if (WaitForSingleObject(context->tx_event, WRITE_TIMEOUT_MS) != WAIT_OBJECT_0) {
        record_event(USB_SERIAL_EVENT_TX_TIMEOUT, length, (DWORD)context->tx_transfer);
        if (context->tx_transfer) {
            context->ufn.abort_transfer(context->device, context->tx_transfer);
            WaitForSingleObject(context->tx_event, WRITE_TIMEOUT_MS);
            finish_transmit(context);
        }
        return FALSE;
    }
    finish_transmit(context);
    return context->tx_error == 0;
}

DWORD USC_Write(DWORD handle, const void *buffer, DWORD length)
{
    serial_context *context = (serial_context *)handle;
    const BYTE *input = (const BYTE *)buffer;
    DWORD sent = 0;

    EnterCriticalSection(&context->write_lock);
    while (sent < length) {
        DWORD chunk = length - sent < TX_TRANSFER_SIZE ? length - sent : TX_TRANSFER_SIZE;

        if (!context->configured)
            break;
        memcpy(context->tx_buffer, input + sent, chunk);
        if (!send_chunk(context, chunk))
            break;
        sent += chunk;
        if (chunk % FULL_SPEED_PACKET == 0 && sent == length && !send_chunk(context, 0))
            break;
    }
    LeaveCriticalSection(&context->write_lock);
    if (sent == 0 && length) {
        SetLastError(ERROR_NOT_READY);
        return (DWORD)-1;
    }
    return sent;
}

DWORD USC_Seek(DWORD handle, long distance, DWORD method)
{
    return (DWORD)-1;
}

BOOL USC_IOControl(DWORD handle, DWORD code, BYTE *input, DWORD input_length, BYTE *output, DWORD output_length,
    DWORD *returned)
{
    serial_context *context = (serial_context *)handle;
    DWORD state;

    if (code == IOCTL_USBSERIAL_EVENTS && output && output_length >= sizeof(usb_serial_event_log)) {
        usb_serial_event_log *log = (usb_serial_event_log *)output;

        log->next_sequence = (DWORD)event_sequence + 1;
        memcpy(log->events, event_ring, sizeof(event_ring));
        if (returned)
            *returned = sizeof(usb_serial_event_log);
        return TRUE;
    }
    if (code == IOCTL_USBSERIAL_STATE && output && output_length >= sizeof(DWORD)) {
        state = (context->configured ? 1 : 0) | (context->line_state & 3) << 1;
        memcpy(output, &state, sizeof(state));
        if (returned)
            *returned = sizeof(state);
        return TRUE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

void USC_PowerUp(DWORD handle)
{
}

void USC_PowerDown(DWORD handle)
{
}
