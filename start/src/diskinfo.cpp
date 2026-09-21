// 硬盘信息采集实现（LocalSystem/管理员权限下运行）
// 判定策略：NVMe 总线 -> SSD(NVMe)；SeekPenalty 特性 -> SSD/HDD；TRIM 辅助。
// 寿命/温度/通电/总写入：NVMe Health Log (Log Page 0x02)，通过
// IOCTL_STORAGE_QUERY_PROPERTY + StorageDeviceProtocolSpecificProperty 获取。
#define _CRT_SECURE_NO_WARNINGS
#include "diskinfo.h"
#include <winioctl.h>
#include <ntddstor.h>
#include <cstdio>
#include <cstring>

// NVMe Health Information Log (512B) 关键偏移（NVMe 1.3 spec）
static const size_t NV_OFF_CRITICAL   = 0;
static const size_t NV_OFF_TEMP_K     = 2;    // Composite Temperature, Kelvin
static const size_t NV_OFF_PCT_USED   = 5;    // PercentageUsed
static const size_t NV_OFF_UNIT_WRITE = 32;   // Data Units Written, 128-bit LE
static const size_t NV_OFF_PWR_HOURS  = 80;   // Power On Hours, 128-bit LE
static const double NV_UNIT_BYTES     = 512000.0;  // 1 unit = 1000 * 512 B

static uint64_t Le128ToU64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {          // 低 8 字节足够
        v = (v << 8) | p[i];
    }
    return v;
}

static bool QueryProperty(HANDLE h, STORAGE_PROPERTY_ID id, void* out, DWORD outLen) {
    STORAGE_PROPERTY_QUERY q;
    ZeroMemory(&q, sizeof(q));
    q.PropertyId = id;
    q.QueryType  = PropertyStandardQuery;
    DWORD ret = 0;
    return DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                           out, outLen, &ret, NULL) == TRUE;
}

// NVMe Health Log：缓冲 = STORAGE_PROTOCOL_SPECIFIC_DATA + 512B
static bool QueryNvmeHealth(HANDLE h, unsigned char* health512) {
    // 固定栈缓冲（不依赖 _alloca / malloc.h）
    const DWORD headerSize = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    unsigned char buf[sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + 512];
    const DWORD bufLen = sizeof(buf);
    ZeroMemory(buf, bufLen);

    STORAGE_PROPERTY_QUERY* q   = (STORAGE_PROPERTY_QUERY*)buf;
    q->PropertyId               = StorageDeviceProtocolSpecificProperty;
    q->QueryType                = PropertyStandardQuery;
    STORAGE_PROTOCOL_SPECIFIC_DATA* psd = (STORAGE_PROTOCOL_SPECIFIC_DATA*)q->AdditionalParameters;
    psd->ProtocolType           = ProtocolTypeNvme;
    psd->DataType               = NVMeDataTypeLogPage;
    psd->ProtocolDataRequestValue = 0x02;             // Health Information Log Page
    psd->ProtocolDataOffset     = headerSize;
    psd->ProtocolDataLength     = 512;

    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf, bufLen,
                         buf, bufLen, &ret, NULL)) return false;
    if (ret < headerSize + 64) return false;
    memcpy(health512, buf + headerSize, 512);
    return true;
}

// 从 StorageDeviceDescriptor 提取 ANSI 型号（Vendor + Product）
static std::string ExtractModel(const unsigned char* desc, DWORD total) {
    STORAGE_DEVICE_DESCRIPTOR* d = (STORAGE_DEVICE_DESCRIPTOR*)desc;
    std::string s;
    auto grab = [&](DWORD off) {
        if (off == 0 || off >= total) return;
        const char* p = (const char*)desc + off;
        for (DWORD i = off; i < total && *p; i++, p++) {
            char c = *p;
            if (c == '|') c = '/';
            if (c == '\r' || c == '\n') break;
            s += c;
        }
    };
    grab(d->ProductIdOffset);      // 型号优先
    if (!s.empty() && d->ProductRevisionOffset) s += " rev ";
    grab(d->ProductRevisionOffset);
    if (s.empty()) grab(d->VendorIdOffset);
    // 收敛长度与杂字符
    std::string clean;
    for (size_t i = 0; i < s.size() && clean.size() < 48; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c < 0x7F) clean += (char)c;
    }
    while (!clean.empty() && clean[clean.size()-1] == ' ') clean.erase(clean.size()-1);
    return clean;
}

bool QueryStressDiskInfo(const std::wstring& stressFilePath, DiskInfo& out) {
    // ---- 1) 压力盘剩余空间 ----
    if (stressFilePath.size() >= 2 && stressFilePath[1] == L':') {
        ULARGE_INTEGER freeB, totalB, dummy;
        std::wstring root = stressFilePath.substr(0, 3);
        if (GetDiskFreeSpaceExW(root.c_str(), &freeB, &totalB, &dummy)) {
            out.freeGB = (double)freeB.QuadPart / (1024.0 * 1024.0 * 1024.0);
        }
    }

    // ---- 2) 卷 -> 物理盘号 ----
    if (stressFilePath.size() < 2 || stressFilePath[1] != L':') return false;
    std::wstring vol = L"\\\\.\\" + stressFilePath.substr(0, 2);
    HANDLE hv = CreateFileW(vol.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
    if (hv == INVALID_HANDLE_VALUE) return false;
    STORAGE_DEVICE_NUMBER sdn;
    ZeroMemory(&sdn, sizeof(sdn));
    DWORD ret = 0;
    bool hasNum = DeviceIoControl(hv, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                                  &sdn, sizeof(sdn), &ret, NULL) == TRUE;
    CloseHandle(hv);
    if (!hasNum) return false;
    out.physicalDrive = sdn.DeviceNumber;

    // ---- 3) 物理盘句柄 ----
    wchar_t pd[64];
    swprintf(pd, 64, L"\\\\.\\PhysicalDrive%u", out.physicalDrive);
    HANDLE hd = CreateFileW(pd, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
    if (hd == INVALID_HANDLE_VALUE)
        hd = CreateFileW(pd, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    if (hd == INVALID_HANDLE_VALUE) return false;

    // ---- 4) 描述符：型号 / 总线 / 容量 ----
    unsigned char descBuf[4096];
    ZeroMemory(descBuf, sizeof(descBuf));
    if (QueryProperty(hd, StorageDeviceProperty, descBuf, sizeof(descBuf))) {
        STORAGE_DEVICE_DESCRIPTOR* d = (STORAGE_DEVICE_DESCRIPTOR*)descBuf;
        out.model = ExtractModel(descBuf, sizeof(descBuf));
        switch (d->BusType) {
            case BusTypeSata:  out.bus = "SATA";  break;
            case BusTypeNvme:  out.bus = "NVMe";  break;
            case BusTypeUsb:   out.bus = "USB";   break;
            case BusTypeSas:   out.bus = "SAS";   break;
            case BusTypeAta:   out.bus = "ATA";   break;
            case BusTypeVirtual: out.bus = "Virtual"; break;
            default:           out.bus = "Other"; break;
        }
        if (d->BusType == BusTypeNvme) { out.ssdKnown = true; out.isSsd = true; }
    }

    {
        DISK_GEOMETRY_EX geo;
        ZeroMemory(&geo, sizeof(geo));
        DWORD gret = 0;
        if (DeviceIoControl(hd, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                            &geo, sizeof(geo), &gret, NULL)) {
            out.capacityGB = (double)geo.DiskSize.QuadPart / (1024.0 * 1024.0 * 1024.0);
        }
    }

    // ---- 5) SSD/HDD 判定：寻道惩罚 + TRIM ----
    {
        DEVICE_SEEK_PENALTY_DESCRIPTOR sp;
        ZeroMemory(&sp, sizeof(sp));
        if (QueryProperty(hd, StorageDeviceSeekPenaltyProperty, &sp, sizeof(sp)) && sp.Version >= sizeof(sp)) {
            out.ssdKnown = true;
            out.isSsd = (sp.IncursSeekPenalty == FALSE);
        }
    }
    if (!out.ssdKnown) {
        DEVICE_TRIM_DESCRIPTOR tr;
        ZeroMemory(&tr, sizeof(tr));
        if (QueryProperty(hd, StorageDeviceTrimProperty, &tr, sizeof(tr)) && tr.Version >= sizeof(tr) && tr.TrimEnabled) {
            out.ssdKnown = true;
            out.isSsd = true;
        }
    }

    // ---- 6) NVMe Health Log：寿命 / 温度 / 通电 / 总写入 ----
    if (out.bus == "NVMe") {
        unsigned char hl[512];
        ZeroMemory(hl, sizeof(hl));
        if (QueryNvmeHealth(hd, hl)) {
            out.pctUsed = (int)hl[NV_OFF_PCT_USED];
            out.tempC   = (int)hl[NV_OFF_TEMP_K] - 273;
            if (out.tempC < -50 || out.tempC > 150) out.tempC = -1;
            uint64_t units = Le128ToU64(hl + NV_OFF_UNIT_WRITE);
            out.writtenGB     = (double)units * NV_UNIT_BYTES / (1024.0 * 1024.0 * 1024.0);
            out.powerOnHours  = Le128ToU64(hl + NV_OFF_PWR_HOURS);
        }
    }

    CloseHandle(hd);
    out.valid = true;
    return true;
}
