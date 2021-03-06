#include "furi_hal_version.h"
#include "furi_hal_usb_i.h"
#include "furi_hal_usb.h"
#include "furi_hal_vcp.h"
#include <furi_hal_power.h>
#include <furi.h>

#include "usb.h"

#define TAG "FuriHalUsb"

#define USB_RECONNECT_DELAY 500

typedef struct {
    FuriThread* thread;
    osTimerId_t tmr;
    bool enabled;
    bool connected;
    FuriHalUsbInterface* if_cur;
    FuriHalUsbInterface* if_next;
    FuriHalUsbStateCallback callback;
    void* cb_ctx;
} UsbSrv;

typedef enum {
    EventModeChange = (1 << 0),
    EventEnable = (1 << 1),
    EventDisable = (1 << 2),
    EventReinit = (1 << 3),

    EventReset = (1 << 4),
    EventRequest = (1 << 5),

    EventModeChangeStart = (1 << 6),
} UsbEvent;

#define USB_SRV_ALL_EVENTS                                                                    \
    (EventModeChange | EventEnable | EventDisable | EventReinit | EventReset | EventRequest | \
     EventModeChangeStart)

static UsbSrv usb;

static const struct usb_string_descriptor dev_lang_desc = USB_ARRAY_DESC(USB_LANGID_ENG_US);

static uint32_t ubuf[0x20];
usbd_device udev;

static int32_t furi_hal_usb_thread(void* context);
static usbd_respond usb_descriptor_get(usbd_ctlreq* req, void** address, uint16_t* length);
static void reset_evt(usbd_device* dev, uint8_t event, uint8_t ep);
static void susp_evt(usbd_device* dev, uint8_t event, uint8_t ep);
static void wkup_evt(usbd_device* dev, uint8_t event, uint8_t ep);

static void furi_hal_usb_tmr_cb(void* context);

/* Low-level init */
void furi_hal_usb_init(void) {
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};
    LL_PWR_EnableVddUSB();

    GPIO_InitStruct.Pin = LL_GPIO_PIN_11 | LL_GPIO_PIN_12;
    GPIO_InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
    GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
    GPIO_InitStruct.Alternate = LL_GPIO_AF_10;
    LL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    usbd_init(&udev, &usbd_hw, USB_EP0_SIZE, ubuf, sizeof(ubuf));
    usbd_enable(&udev, true);

    usbd_reg_descr(&udev, usb_descriptor_get);
    usbd_reg_event(&udev, usbd_evt_susp, susp_evt);
    usbd_reg_event(&udev, usbd_evt_wkup, wkup_evt);
    // Reset callback will be enabled after first mode change to avoid getting false reset events

    usb.enabled = false;
    usb.if_cur = NULL;
    HAL_NVIC_SetPriority(USB_LP_IRQn, 5, 0);
    NVIC_EnableIRQ(USB_LP_IRQn);

    usb.thread = furi_thread_alloc();
    furi_thread_set_name(usb.thread, "UsbDriver");
    furi_thread_set_stack_size(usb.thread, 1024);
    furi_thread_set_callback(usb.thread, furi_hal_usb_thread);
    furi_thread_start(usb.thread);

    FURI_LOG_I(TAG, "Init OK");
}

void furi_hal_usb_set_config(FuriHalUsbInterface* new_if) {
    usb.if_next = new_if;
    if(usb.thread == NULL) {
        // Service thread hasn't started yet, so just save interface mode
        return;
    }
    furi_assert(usb.thread);
    osThreadFlagsSet(furi_thread_get_thread_id(usb.thread), EventModeChange);
}

FuriHalUsbInterface* furi_hal_usb_get_config() {
    return usb.if_cur;
}

void furi_hal_usb_disable() {
    furi_assert(usb.thread);
    osThreadFlagsSet(furi_thread_get_thread_id(usb.thread), EventDisable);
}

void furi_hal_usb_enable() {
    osThreadFlagsSet(furi_thread_get_thread_id(usb.thread), EventEnable);
}

void furi_hal_usb_reinit() {
    furi_assert(usb.thread);
    osThreadFlagsSet(furi_thread_get_thread_id(usb.thread), EventReinit);
}

static void furi_hal_usb_tmr_cb(void* context) {
    furi_assert(usb.thread);
    osThreadFlagsSet(furi_thread_get_thread_id(usb.thread), EventModeChangeStart);
}

/* Get device / configuration descriptors */
static usbd_respond usb_descriptor_get(usbd_ctlreq* req, void** address, uint16_t* length) {
    const uint8_t dtype = req->wValue >> 8;
    const uint8_t dnumber = req->wValue & 0xFF;
    const void* desc;
    uint16_t len = 0;
    if(usb.if_cur == NULL) return usbd_fail;

    switch(dtype) {
    case USB_DTYPE_DEVICE:
        osThreadFlagsSet(furi_thread_get_thread_id(usb.thread), EventRequest);
        if(usb.callback != NULL) {
            usb.callback(FuriHalUsbStateEventDescriptorRequest, usb.cb_ctx);
        }
        desc = usb.if_cur->dev_descr;
        break;
    case USB_DTYPE_CONFIGURATION:
        desc = usb.if_cur->cfg_descr;
        len = ((struct usb_string_descriptor*)(usb.if_cur->cfg_descr))->wString[0];
        break;
    case USB_DTYPE_STRING:
        if(dnumber == UsbDevLang) {
            desc = &dev_lang_desc;
        } else if((dnumber == UsbDevManuf) && (usb.if_cur->str_manuf_descr != NULL)) {
            desc = usb.if_cur->str_manuf_descr;
        } else if((dnumber == UsbDevProduct) && (usb.if_cur->str_prod_descr != NULL)) {
            desc = usb.if_cur->str_prod_descr;
        } else if((dnumber == UsbDevSerial) && (usb.if_cur->str_serial_descr != NULL)) {
            desc = usb.if_cur->str_serial_descr;
        } else
            return usbd_fail;
        break;
    default:
        return usbd_fail;
    }
    if(desc == NULL) return usbd_fail;

    if(len == 0) {
        len = ((struct usb_header_descriptor*)desc)->bLength;
    }
    *address = (void*)desc;
    *length = len;
    return usbd_ack;
}

void furi_hal_usb_set_state_callback(FuriHalUsbStateCallback cb, void* ctx) {
    usb.callback = cb;
    usb.cb_ctx = ctx;
}

static void reset_evt(usbd_device* dev, uint8_t event, uint8_t ep) {
    osThreadFlagsSet(furi_thread_get_thread_id(usb.thread), EventReset);
    if(usb.callback != NULL) {
        usb.callback(FuriHalUsbStateEventReset, usb.cb_ctx);
    }
}

static void susp_evt(usbd_device* dev, uint8_t event, uint8_t ep) {
    if((usb.if_cur != NULL) && (usb.connected == true)) {
        usb.connected = false;
        usb.if_cur->suspend(&udev);

        furi_hal_power_insomnia_exit();
    }
    if(usb.callback != NULL) {
        usb.callback(FuriHalUsbStateEventSuspend, usb.cb_ctx);
    }
}

static void wkup_evt(usbd_device* dev, uint8_t event, uint8_t ep) {
    if((usb.if_cur != NULL) && (usb.connected == false)) {
        usb.connected = true;
        usb.if_cur->wakeup(&udev);

        furi_hal_power_insomnia_enter();
    }
    if(usb.callback != NULL) {
        usb.callback(FuriHalUsbStateEventWakeup, usb.cb_ctx);
    }
}

static int32_t furi_hal_usb_thread(void* context) {
    usb.tmr = osTimerNew(furi_hal_usb_tmr_cb, osTimerOnce, NULL, NULL);

    bool usb_request_pending = false;
    uint8_t usb_wait_time = 0;

    if(usb.if_next != NULL) {
        osThreadFlagsSet(furi_thread_get_thread_id(usb.thread), EventModeChange);
    }

    while(true) {
        uint32_t flags = osThreadFlagsWait(USB_SRV_ALL_EVENTS, osFlagsWaitAny, 500);
        if((flags & osFlagsError) == 0) {
            if(flags & EventModeChange) {
                if(usb.if_next != usb.if_cur) {
                    if(usb.enabled) { // Disable current interface
                        susp_evt(&udev, 0, 0);
                        usbd_connect(&udev, false);
                        usb.enabled = false;
                        osTimerStart(usb.tmr, USB_RECONNECT_DELAY);
                    } else {
                        flags |= EventModeChangeStart;
                    }
                }
            }
            if(flags & EventReinit) {
                // Temporary disable callback to avoid getting false reset events
                usbd_reg_event(&udev, usbd_evt_reset, NULL);
                FURI_LOG_I(TAG, "USB Reinit");
                susp_evt(&udev, 0, 0);
                usbd_connect(&udev, false);
                usb.enabled = false;

                usbd_enable(&udev, false);
                usbd_enable(&udev, true);

                usb.if_next = usb.if_cur;
                osTimerStart(usb.tmr, USB_RECONNECT_DELAY);
            }
            if(flags & EventModeChangeStart) { // Second stage of mode change process
                if(usb.if_cur != NULL) {
                    usb.if_cur->deinit(&udev);
                }
                if(usb.if_next != NULL) {
                    usb.if_next->init(&udev, usb.if_next);
                    usbd_reg_event(&udev, usbd_evt_reset, reset_evt);
                    FURI_LOG_I(TAG, "USB Mode change done");
                    usb.enabled = true;
                    usb.if_cur = usb.if_next;
                }
            }
            if(flags & EventEnable) {
                if((!usb.enabled) && (usb.if_cur != NULL)) {
                    usbd_connect(&udev, true);
                    usb.enabled = true;
                    FURI_LOG_I(TAG, "USB Enable");
                }
            }
            if(flags & EventDisable) {
                if(usb.enabled) {
                    susp_evt(&udev, 0, 0);
                    usbd_connect(&udev, false);
                    usb.enabled = false;
                    usb_request_pending = false;
                    FURI_LOG_I(TAG, "USB Disable");
                }
            }
            if(flags & EventReset) {
                usb_request_pending = true;
                usb_wait_time = 0;
            }
            if(flags & EventRequest) {
                usb_request_pending = false;
            }
        } else if(usb_request_pending) {
            usb_wait_time++;
            if(usb_wait_time > 4) {
                furi_hal_usb_reinit();
                usb_request_pending = false;
            }
        }
    }
    return 0;
}
