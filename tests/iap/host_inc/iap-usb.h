#pragma once
#include <stdbool.h>

/* host build: no iAP session trace (see firmware/export/iap-usb.h) */
#define iap_trace_logf(...)     do { } while(0)
#define iap_trace_hex(...)      do { } while(0)
#define iap_trace_flush()       do { } while(0)
#define iap_trace_needs_flush() false
