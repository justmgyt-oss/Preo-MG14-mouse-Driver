/*
 * hidinfo.c - Query the mouse's HID collection: identity, capabilities and
 * strings. Read-only; we never write a report to the device here.
 */
#include "app.h"

static void copy_hid_string(const wchar_t *src, wchar_t *dst, size_t cch)
{
    if (!dst || cch == 0) return;
    if (!src) { dst[0] = L'\0'; return; }
    wstr_copy(dst, cch, src, wcslen(src));
}

/* HidD_Get*String returns a WCHAR array whose length is the buffer size in
 * wchar_t minus one, not a NUL-terminated string. Copy defensively. */
static void read_hid_string(HANDLE h, wchar_t *out, size_t cch)
{
    wchar_t raw[256];
    DWORD   bytes = (DWORD)sizeof(raw);
    if (!out || cch == 0) return;
    out[0] = L'\0';
    ZeroMemory(raw, sizeof(raw));
    if (HidD_GetManufacturerString(h, raw, bytes)) copy_hid_string(raw, out, cch);
    else out[0] = L'\0';
}

static void read_product_string(HANDLE h, wchar_t *out, size_t cch)
{
    wchar_t raw[256];
    DWORD   bytes = (DWORD)sizeof(raw);
    if (!out || cch == 0) return;
    out[0] = L'\0';
    ZeroMemory(raw, sizeof(raw));
    if (HidD_GetProductString(h, raw, bytes)) copy_hid_string(raw, out, cch);
    else out[0] = L'\0';
}

static void read_serial_string(HANDLE h, wchar_t *out, size_t cch)
{
    wchar_t raw[256];
    DWORD   bytes = (DWORD)sizeof(raw);
    if (!out || cch == 0) return;
    out[0] = L'\0';
    ZeroMemory(raw, sizeof(raw));
    if (HidD_GetSerialNumberString(h, raw, bytes)) copy_hid_string(raw, out, cch);
    else out[0] = L'\0';
}

BOOL hid_query(DeviceInfo *dev)
{
    HANDLE  h;
    HIDD_ATTRIBUTES attr;
    PHIDP_PREPARSED_DATA pp = NULL;
    HIDP_CAPS caps;
    NTSTATUS st;

    if (!dev || !dev->found || !dev->interfacePath[0]) return FALSE;

    /* Read-only open with sharing so we never lock out another application. */
    h = CreateFileW(dev->interfacePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    ZeroMemory(&attr, sizeof(attr));
    attr.Size = sizeof(attr);
    if (HidD_GetAttributes(h, &attr)) {
        dev->vendorId      = attr.VendorID;
        dev->productId     = attr.ProductID;
        dev->versionNumber = attr.VersionNumber;
    }
    read_hid_string(h,    dev->manufacturer, 128);
    read_product_string(h, dev->productName,  128);
    read_serial_string(h,  dev->serial,       128);

    if (HidD_GetPreparsedData(h, &pp) && pp) {
        ZeroMemory(&caps, sizeof(caps));
        st = HidP_GetCaps(pp, &caps);
        if (st == HIDP_STATUS_SUCCESS) {
            dev->numberValueCaps      = caps.NumberInputValueCaps;
            dev->numberInputReportCaps = caps.NumberInputButtonCaps;
            if (!dev->usagePage) dev->usagePage = caps.UsagePage;
            if (!dev->usage)     dev->usage     = caps.Usage;

            /* HIDP_CAPS has no total-button field, so ask for the input button
             * caps directly and count them. The MG14 is a 6-button mouse, so
             * this is what decides how many remappable buttons we offer. */
            {
                USHORT need = 0;
                HidP_GetButtonCaps(HidP_Input, NULL, &need, pp);
                if (need > 0 && need < 64) {
                    PHIDP_BUTTON_CAPS bc =
                        (PHIDP_BUTTON_CAPS)HeapAlloc(GetProcessHeap(), 0,
                                                    need * sizeof(HIDP_BUTTON_CAPS));
                    if (bc) {
                        USHORT got = need, i, n = 0;
                        if (HidP_GetButtonCaps(HidP_Input, bc, &got, pp)
                            == HIDP_STATUS_SUCCESS) {
                            for (i = 0; i < got; i++) {
                                /* HIDP_BUTTON_CAPS is an anonymous union: a
                                 * single button lives in NotRange, a run of
                                 * buttons in Range. */
                                if (bc[i].IsRange) {
                                    if (bc[i].Range.UsageMax >= bc[i].Range.UsageMin)
                                        n += (USHORT)(bc[i].Range.UsageMax - bc[i].Range.UsageMin + 1);
                                } else if (bc[i].NotRange.Usage != 0) {
                                    n++;
                                }
                            }
                        }
                        HeapFree(GetProcessHeap(), 0, bc);
                        if (n) dev->numberButtons = n;
                    }
                }
            }
        }
        HidD_FreePreparsedData(pp);
    }

    CloseHandle(h);
    return TRUE;
}
