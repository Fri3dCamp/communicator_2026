#pragma once

#include <stdint.h>
#include "wch_usbhid_usb.h"
#include "wch_usbhid_config.h"
#include "wch_usbfs_compat.h"
#include <ch32x035.h>

#define EP0_SIZE 64
#define EP1_SIZE 64 

// Buffer externs
extern __attribute__((aligned(4))) unsigned char wch_usbhid_EP0_buffer[];
extern __attribute__((aligned(4))) unsigned char wch_usbhid_EP1_buffer[];

// Descriptor externs
extern const USB_DEV_DESCR wch_usbhid_DevDescr;
extern const uint8_t wch_usbhid_CfgDescr[];
extern const uint16_t wch_usbhid_CfgDescrLen;
extern const uint8_t wch_usbhid_ReportDescr[];
extern const uint16_t wch_usbhid_ReportDescrLen;

extern const USB_STR_DESCR wch_usbhid_LangDescr;
extern USB_STR_DESCR wch_usbhid_ManufDescr;
extern USB_STR_DESCR wch_usbhid_ProdDescr;
extern USB_STR_DESCR wch_usbhid_SerDescr;
extern USB_STR_DESCR wch_usbhid_InterfDescr;

// Setup buffer access
typedef struct __attribute__((packed)) {
    uint8_t  bRequestType;
    uint8_t  bRequest;
    uint8_t  wValueL;
    uint8_t  wValueH;
    uint8_t  wIndexL;
    uint8_t  wIndexH;
    uint8_t  wLengthL;
    uint8_t  wLengthH;
} USB_SETUP_REQ, *PUSB_SETUP_REQ;

#define USB_SetupBuf ((PUSB_SETUP_REQ)wch_usbhid_EP0_buffer)

#ifdef __cplusplus
extern "C" {
#endif

void generate_all_string_descriptors(void);
void USB_init(void);

// Helper for C++
uint32_t USB_write(const uint8_t* buf, uint32_t len);
uint8_t USB_ready(void);

#ifdef __cplusplus
}
#endif