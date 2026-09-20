# APNX Firmware

AVR-based firmware implementation for the **APNX USB device**.

The firmware is based on the **LUFA Bulk Vendor demo** and extends its USB Vendor-specific Bulk transfer framework with the APNX custom protocol, packet validation, logical memory mapping, READ/WRITE command processing, CRC16 calculation, error handling, and bootloader control.

> **Note:** LUFA provides the underlying USB device framework and Bulk Vendor example. The APNX-specific protocol and device-side processing logic were implemented and extended for this project.

---

## Overview

APNX Firmware communicates with a host system through USB Bulk IN/OUT endpoints.

The firmware receives APNX request packets from the host, validates and parses the packet, executes the requested command against a logical memory map, and generates an ACK or NAK response.

```text
                    USB Bulk
┌──────────────┐   OUT / IN   ┌──────────────────────┐
│     Host     │ ◄──────────► │    APNX Firmware     │
└──────────────┘              │                      │
                              │ Packet Reception     │
                              │        ↓             │
                              │ Packet Validation    │
                              │        ↓             │
                              │ Packet Parsing       │
                              │        ↓             │
                              │ Command Execution    │
                              │        ↓             │
                              │ Response Generation  │
                              └──────────┬───────────┘
                                         │
                                         ▼
                                Logical Memory Map
```

---

## Features

* USB Vendor-specific Bulk IN/OUT communication using LUFA
* Variable-length APNX packet reception
* Length-based packet framing
* READ / WRITE command processing
* 8 / 16 / 32 / 64-bit data type support
* Logical address mapping
* Big-endian protocol data representation
* CRC16-Modbus calculation
* Packet validation before command execution
* ACK / NAK response generation
* Error code handling
* Bootloader entry through a dedicated command
* Maximum packet length validation
* USB endpoint configuration and device event handling

---

## Architecture

The firmware is structured around the following processing flow:

```text
USB OUT
   │
   ▼
Receive USB data
   │
   ▼
Accumulate packet
   │
   ▼
Read LEN field
   │
   ▼
Packet validation
   │
   ├──────────── Invalid
   │                 │
   │                 ▼
   │                NAK
   │
   ▼ Valid
Parse packet
   │
   ▼
Execute command
   │
   ├── READ
   │     │
   │     ▼
   │   Read logical memory
   │     │
   │     ▼
   │   Build ACK
   │
   └── WRITE
         │
         ▼
       Write logical memory
         │
         ▼
       Build ACK
              │
              ▼
          USB IN
```

The main application loop handles USB tasks and incoming packets, while protocol-specific functions handle validation, parsing, command execution, and response generation.

---

## USB Interface

The firmware uses LUFA's Vendor-specific Bulk transfer functionality.

Two Bulk endpoints are configured:

| Endpoint            | Direction     | Type |
| ------------------- | ------------- | ---- |
| `VENDOR_OUT_EPADDR` | Host → Device | Bulk |
| `VENDOR_IN_EPADDR`  | Device → Host | Bulk |

The endpoints are configured when the USB device configuration changes.

```c
Endpoint_ConfigureEndpoint(
    VENDOR_IN_EPADDR,
    EP_TYPE_BULK,
    VENDOR_IO_EPSIZE,
    1
);

Endpoint_ConfigureEndpoint(
    VENDOR_OUT_EPADDR,
    EP_TYPE_BULK,
    VENDOR_IO_EPSIZE,
    1
);
```

The firmware reads the actual number of bytes received from the OUT endpoint and accumulates them into an internal receive buffer.

---

## Packet Reception

APNX packets are variable-length.

The firmware uses the `LEN` field in the packet header to determine the expected packet length.

```text
First USB transfer
       │
       ▼
Read STX / LEN
       │
       ▼
Determine expected length
       │
       ▼
Accumulate received data
       │
       ├── Packet incomplete
       │       │
       │       └── Receive more USB data
       │
       └── Packet complete
               │
               ▼
          Validate packet
```

The firmware prevents the receive buffer from exceeding the configured maximum packet length.

```c
#define MAX_PACKET_LEN 255
```

---

## APNX Protocol

The APNX protocol uses a fixed header followed by a variable-length payload.

### Request Header

```text
┌─────┬─────┬────┬─────┬──────────┬───────────┬───────┐
│ STX │ LEN │ ID │ CMD │ CMD_TYPE │ DATA_TYPE │ COUNT │
└─────┴─────┴────┴─────┴──────────┴───────────┴───────┘
```

The firmware parses the received packet into the following fields:

| Field       | Description             |
| ----------- | ----------------------- |
| `STX`       | Start of packet         |
| `LEN`       | Total packet length     |
| `ID`        | Device ID               |
| `CMD`       | Command                 |
| `CMD_TYPE`  | Command/addressing type |
| `DATA_TYPE` | Data width              |
| `COUNT`     | Number of data elements |
| `PAYLOAD`   | Address and data fields |

The current implementation processes the payload as variable-length raw data after parsing the fixed header.

---

## Supported Commands

### Host → Device

| Command |  Value | Description                      |
| ------- | -----: | -------------------------------- |
| `READ`  | `0x00` | Read data from logical addresses |
| `WRITE` | `0x01` | Write data to logical addresses  |

### Device → Host

| Command |  Value | Description                      |
| ------- | -----: | -------------------------------- |
| `ACK`   | `0x05` | Successful command response      |
| `NAK`   | `0x0F` | Invalid request / error response |

---

## Command Type

The firmware currently accepts the single-address command type.

```c
static int is_valid_cmd_type(uint8_t cmd_type)
{
    if (cmd_type != 1)
        return 0;

    return 1;
}
```

The sequence/burst command type is reserved for future implementation.

---

## Data Types

APNX supports four data widths:

| Data Type      |  Value |    Size |
| -------------- | -----: | ------: |
| `DATA_TYPE_8`  | `0x00` |  1 byte |
| `DATA_TYPE_16` | `0x01` | 2 bytes |
| `DATA_TYPE_32` | `0x02` | 4 bytes |
| `DATA_TYPE_64` | `0x03` | 8 bytes |

The firmware determines the number of bytes associated with each data type before processing the payload.

```text
8-bit   → 1 byte
16-bit  → 2 bytes
32-bit  → 4 bytes
64-bit  → 8 bytes
```

---

## Logical Memory Map

The firmware exposes a software-defined logical address space.

```text
Address Range

0x100 ───────────── Input
                    16 bytes

0x200 ───────────── Output
                    16 bytes

0x300 ───────────── Data
                    64 bytes

0x400 ───────────── Flag
                    32 bytes
```

The corresponding memory representation is:

```c
struct Address_list {
    uint8_t input[16];
    uint8_t output[16];
    uint8_t data[64];
    uint8_t flag[32];
} memory_map;
```

### Address Regions

| Region | Base Address |     Size |
| ------ | -----------: | -------: |
| Input  |      `0x100` | 16 bytes |
| Output |      `0x200` | 16 bytes |
| Data   |      `0x300` | 64 bytes |
| Flag   |      `0x400` | 32 bytes |

The upper address bits identify the logical region, while the lower bits are interpreted as an offset within that region.

For example:

```text
0x301

0x300 → DATA region
0x001 → offset
```

---

## Packet Validation

Before executing a command, the firmware validates the request.

The validation sequence is:

```text
Command
   ↓
Command Type
   ↓
Count
   ↓
Data Type
   ↓
Address
```

### Command Validation

Only `READ` and `WRITE` are accepted as host commands.

A dedicated command value is also reserved for entering the bootloader.

### Count Validation

The current implementation supports:

```text
1 ≤ COUNT ≤ 16
```

### Data Type Validation

The firmware verifies that the data type is one of the supported 8/16/32/64-bit types.

### Address Validation

The logical address is separated into:

```text
Address
 ├── Region
 └── Offset
```

The offset is then checked against the size of the corresponding logical memory region.

This validation is performed before memory access.

---

## READ Processing

For a READ request, the payload contains logical addresses.

```text
READ Request

Header
  │
  ▼
Address 1
Address 2
...
Address N
```

The firmware:

1. Extracts each logical address.
2. Converts the protocol representation into the local address value.
3. Resolves the logical memory region.
4. Reads data according to `DATA_TYPE`.
5. Converts multi-byte data to the protocol byte order.
6. Builds an ACK response.

```text
execute_read()
      │
      ▼
Parse address
      │
      ▼
read_memory()
      │
      ▼
read_data_by_type()
      │
      ▼
build_read_ack_packet()
```

---

## WRITE Processing

For a WRITE request, each element contains an address followed by its corresponding data.

```text
WRITE Request

Header
  │
  ├── Address
  ├── Data
  │
  ├── Address
  ├── Data
  │
  └── ...
```

The firmware:

1. Extracts the logical address.
2. Determines the data size from `DATA_TYPE`.
3. Resolves the logical memory region.
4. Converts the received byte order.
5. Writes the value into the corresponding memory region.
6. Generates a WRITE ACK.

```text
execute_write()
      │
      ▼
Parse address
      │
      ▼
write_memory()
      │
      ▼
write_data_by_type()
      │
      ▼
build_write_ack_packet()
```

---

## Endianness

The APNX protocol represents multi-byte values in big-endian byte order.

The firmware provides conversion functions for:

* 16-bit
* 32-bit
* 64-bit

values.

```text
Protocol                  AVR Memory

Big Endian                Little Endian

MSB → LSB                 LSB → MSB
```

During READ processing, data is converted from the local memory representation into protocol byte order.

During WRITE processing, received protocol data is converted into the local representation before being written to the logical memory.

---

## CRC16

APNX uses a CRC16 calculation compatible with the Modbus CRC algorithm.

```text
Initial value : 0xFFFF
Polynomial    : 0xA001
Bit order     : LSB-first
```

The firmware calculates CRC over the packet contents excluding the CRC field.

```c
uint16_t calculate_crc(const uint8_t *buf, size_t len);
```

The generated CRC is appended to the response packet.

---

## Response Format

### ACK

A successful WRITE response contains the response header followed by CRC.

A READ response additionally contains the data type and returned payload.

```text
WRITE ACK

┌───────────────┬─────┐
│ Response Data │ CRC │
└───────────────┴─────┘
```

```text
READ ACK

┌───────────────┬───────────┬─────────┬─────┐
│ Response Hdr  │ DATA_TYPE │ PAYLOAD │ CRC │
└───────────────┴───────────┴─────────┴─────┘
```

### NAK

Invalid requests generate a NAK response containing an error code.

```text
┌───────────────┬────────────┬─────┐
│ Response Hdr  │ Error Code │ CRC │
└───────────────┴────────────┴─────┘
```

---

## Error Handling

The firmware defines protocol-level error codes.

| Error                   |     Code | Description              |
| ----------------------- | -------: | ------------------------ |
| `ERR_INVALID_CMD`       | `0x0001` | Invalid command          |
| `ERR_INVALID_CMD_TYPE`  | `0x0002` | Invalid command type     |
| `ERR_INVALID_DATA_TYPE` | `0x0101` | Invalid data type        |
| `ERR_INVALID_COUNT`     | `0x0102` | Invalid count            |
| `ERR_INVALID_ADDRESS`   | `0x0201` | Invalid logical address  |
| `ERR_OUT_OF_RANGE`      | `0x0202` | Data exceeds valid range |
| `ERR_INTERNAL`          | `0xFF01` | Internal error           |
| `ERR_UNKNOWN`           | `0xFFFF` | Unknown error            |

Invalid requests are rejected before command execution and can result in a NAK response containing the corresponding error code.

---

## Bootloader

The firmware provides a mechanism to intentionally reset into the bootloader.

The bootloader entry mechanism uses a predefined key written to a dedicated memory location.

```c
#define BOOT_KEY       0x7777
#define BOOT_KEY_ADDR  ((uint16_t*)0x0800)
#define GO_TO_BOOT     0x77
```

When the bootloader command is received, the firmware:

```text
Disable interrupts
       ↓
Write boot key
       ↓
Blink LED 3 times
       ↓
Disable USB
       ↓
Enable watchdog
       ↓
Wait for reset
```

The watchdog is configured for a 120 ms reset.

This provides a controlled software path from the application firmware to the bootloader.

---

## Project Structure

```text
APNX-firmware/
├── Config/
│   └── LUFAConfig.h
├── APNX_firmware.c
├── APNX_firmware.hex
├── BulkVendor.h
├── Descriptors.c
├── Descriptors.h
├── makefile
├── .gitignore
└── README.md
```

- APNX_firmware.c — APNX firmware application logic
- BulkVendor.h — LUFA USB Bulk Vendor interface configuration
- Descriptors.c/h — USB device descriptors
- Config/LUFAConfig.h — LUFA configuration
- makefile — AVR firmware build configuration
- APNX_firmware.hex — pre-built firmware image for flashing

The exact directory layout may vary depending on the LUFA project setup.

---

## Build

The firmware can be built using the AVR toolchain and the provided makefile.

```bash
make
```

The resulting firmware image can be generated in HEX format for programming the AVR device.

> Build requirements depend on the target AVR device and the LUFA project configuration.

---

## Flash

The generated HEX image can be programmed to the target device using an AVR programmer or the supported USB bootloader.

Example:

```bash
avrdude ...
```

The exact `avrdude` parameters depend on the target MCU, bootloader, programmer, and board configuration.

---

## Design Notes

### LUFA as the USB Layer

The firmware uses LUFA's Bulk Vendor example as the USB communication foundation.

The project does not reimplement the USB device stack. Instead, APNX-specific functionality is implemented above the USB transport layer.

```text
┌─────────────────────────────┐
│      APNX Application       │
│                             │
│ Protocol / Commands / CRC   │
│ Memory Map / Validation     │
├─────────────────────────────┤
│            LUFA             │
│ USB Device / Bulk Endpoint  │
├─────────────────────────────┤
│          AVR MCU            │
└─────────────────────────────┘
```

### Separation of Validation and Execution

The firmware separates request validation from command execution.

```text
Receive
  ↓
Validate
  ↓
Parse
  ↓
Execute
  ↓
Generate Response
```

This allows malformed commands, unsupported data types, invalid counts, and invalid logical addresses to be rejected before accessing the logical memory map.

### Variable-Length Payload

The APNX protocol uses a variable-length payload because the number of requested elements and data width can vary.

The firmware therefore treats the fixed header separately from the variable payload and calculates payload offsets according to `COUNT` and `DATA_TYPE`.

---

## Related Components

APNX is implemented as several independent components.

```text
                 APNX Monitor
                    Qt / C++
                       │
                       ▼
                APNX USB Driver
                  Linux Kernel
                       │
                       │ USB Bulk
                       ▼
                APNX Firmware
                  AVR + LUFA
```

| Component    | Repository        | Role                                                |
| ------------ | ----------------- | --------------------------------------------------- |
| Firmware     | `APNX-firmware`   | USB device firmware and APNX protocol processing    |
| Linux Driver | `APNX-usb-driver` | Linux kernel USB driver                             |
| Monitor      | `APNX-monitor`    | Qt/C++ host-side monitoring and control application |

Each component is maintained as an independent repository.

---

## Current Limitations

The current implementation has the following limitations:

* Only the currently implemented single-address command type is supported.
* Sequence/burst command type is reserved for future implementation.
* The logical memory map is currently implemented as firmware-managed memory regions.
* Maximum packet length is limited to 255 bytes.
* The current firmware is based on the LUFA Bulk Vendor example and therefore retains the structure of the underlying LUFA project.

---

## LUFA Attribution

This project is based on the **LUFA (Lightweight USB Framework for AVRs)** library and its Bulk Vendor demo.

Original LUFA copyright:

```text
Copyright (C) Dean Camera, 2021.
```

The original LUFA license and copyright notices are retained in the source code.

APNX-specific functionality was added and extended on top of the LUFA Bulk Vendor framework.

See the LUFA source files for the complete original license text.

---

## Author

**JaemoPark**

APNX Firmware — AVR / USB / Custom Protocol
