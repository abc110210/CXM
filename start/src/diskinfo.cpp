// 硬盘信息采集实现（LocalSystem/管理员权限下运行）
// 判定策略：NVMe 总线 -> SSD(NVMe)；SeekPenalty 特性 -> SSD/HDD；TRIM 辅助。
// 寿命/温度/通电/总写入：
//   NVMe（含 M.2 NVMe 形态）：NVMe Health Log (Log Page 0x02)，
//     通过 IOCTL_STORAGE_QUERY_PROPERTY + StorageDeviceProtocolSpecificProperty 获取。
//   SATA（含 M.2 SATA 形态的 SSD 与机械盘）：ATA SMART 属性表，双路径最大兼容：
//     路径A（Win10 1803+，官方推荐）：IOCTL_STORAGE_QUERY_PROPERTY + ProtocolTypeAta
//     路径B（更老系统回退）：SCSI PASS-THROUGH (SAT-2) ATA PASS-THROUGH(12)
//     SSD 寿命取属性 E7/A9（剩余百分比，换算为已用）；机械盘出温度/通电/写入，寿命诚实 N/A。
#define _CRT_SECURE_NO_WARNINGS
#include "diskinfo.h"
#include <winioctl.h>
#include <ntddstor.h>
#include <cstdio>
#include <cstring>

#ifndef ATAProtocolData
#define ATAProtocolData 0x00000001
#endif

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

// ---- SATA/ATA SMART（SATA SSD 含 M.2 SATA 形态；机械盘出温度/通电）----

// 路径A：ATA 寄存器级透传（IOCTL_STORAGE_QUERY_PROPERTY + ProtocolTypeAta，Win10 1803+）
static bool QuerySmartViaAtaProperty(HANDLE h, unsigned char smart512[512]) {
    const DWORD headerSize = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    unsigned char buf[sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + 512];
    const DWORD bufLen = sizeof(buf);
    ZeroMemory(buf, bufLen);

    STORAGE_PROPERTY_QUERY* q = (STORAGE_PROPERTY_QUERY*)buf;
    q->PropertyId = StorageDeviceProtocolSpecificProperty;
    q->QueryType  = PropertyStandardQuery;
    STORAGE_PROTOCOL_SPECIFIC_DATA* psd = (STORAGE_PROTOCOL_SPECIFIC_DATA*)q->AdditionalParameters;
    psd->ProtocolType                = ProtocolTypeAta;
    psd->DataType                    = ATAProtocolData;
    // SMART READ DATA：Command=0xB0, Features=0xD0, LBA Mid/High=0x4F/0xC2（SMART 魔数）
    psd->ProtocolDataRequestValue    = 0xB0;         // Command 寄存器
    psd->ProtocolDataRequestSubValue = 0xD0;         // Features 寄存器
    psd->ProtocolDataRequestSubValue2 = 0x00;        // Sector Count
    psd->ProtocolDataRequestSubValue3 = 0x00C24F00;  // LBA Low=00 | Mid=4F<<8 | High=C2<<16
    psd->ProtocolDataOffset          = headerSize;
    psd->ProtocolDataLength          = 512;

    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf, bufLen,
                         buf, bufLen, &ret, NULL)) return false;
    if (ret < headerSize + 512) return false;
    memcpy(smart512, buf + headerSize, 512);
    return true;
}

// 路径B：SCSI PASS-THROUGH (SAT-2) ATA PASS-THROUGH(12)，老系统/老驱动回退。
// 兼容性说明：SCSI_PASS_THROUGH_DIRECT / IOCTL_SCSI_PASS_THROUGH_DIRECT 在
// winioctl.h 中受 _WIN32_WINNT 条件编译保护（部分 SDK 配置下被排除），
// 因此这里手写稳定的公开 ABI（布局与 winioctl.h 完全一致，x64 含对齐填充），
// 使本文件对 SDK 基线宏免疫。
#ifndef IOCTL_SCSI_PASS_THROUGH_DIRECT
#define IOCTL_SCSI_PASS_THROUGH_DIRECT 0x0004d014
#endif
#ifndef SCSI_IOCTL_DATA_IN
#define SCSI_IOCTL_DATA_IN 1
#endif

#pragma pack(push, 8)
typedef struct _AG_SPTD {
    USHORT Length;              // 0
    UCHAR  ScsiStatus;          // 2
    UCHAR  PathId;              // 3
    UCHAR  TargetId;            // 4
    UCHAR  Lun;                 // 5
    UCHAR  CdbLength;           // 6
    UCHAR  SenseInfoLength;     // 7
    UCHAR  DataIn;              // 8
    UCHAR  Reserved1;           // 9
    ULONG  DataTransferLength;  // 12
    ULONG  TimeOutValue;        // 16
    PVOID  DataBuffer;          // 24
    ULONG  SenseInfoOffset;     // 32
    UCHAR  Cdb[16];             // 36
} AG_SPTD;                      // sizeof = 56（与 winioctl.h 的 SCSI_PASS_THROUGH_DIRECT 一致）
#pragma pack(pop)

static bool QuerySmartViaSat(HANDLE h, unsigned char smart512[512]) {
    union {
        AG_SPTD sptd;
        unsigned char buf[sizeof(AG_SPTD) + 512];
    } u;
    ZeroMemory(&u, sizeof(u));
    u.sptd.Length             = sizeof(AG_SPTD);
    u.sptd.CdbLength          = 12;
    u.sptd.DataIn             = SCSI_IOCTL_DATA_IN;
    u.sptd.DataTransferLength = 512;
    u.sptd.TimeOutValue       = 10;
    u.sptd.DataBuffer         = u.buf + sizeof(AG_SPTD);
    u.sptd.Cdb[0] = 0xA1;   // ATA PASS-THROUGH (12)
    u.sptd.Cdb[1] = 0x08;   // Protocol = PIO Data-In（SAT-2: protocol<<1）
    u.sptd.Cdb[2] = 0x2E;   // T_LENGTH=3 | T_DIR=1(data-in) | BYT_BLOC=1
    u.sptd.Cdb[3] = 0xD0;   // Features  = SMART READ DATA
    u.sptd.Cdb[4] = 0x00;   // Sector Count
    u.sptd.Cdb[5] = 0x00;   // LBA Low
    u.sptd.Cdb[6] = 0x4F;   // LBA Mid  (SMART 魔数)
    u.sptd.Cdb[7] = 0xC2;   // LBA High (SMART 魔数)
    u.sptd.Cdb[8] = 0xA0;   // Device
    u.sptd.Cdb[9] = 0xB0;   // Command  = SMART

    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &u, sizeof(u),
                         &u, sizeof(u), &ret, NULL)) return false;
    if (u.sptd.ScsiStatus != 0) return false;
    memcpy(smart512, u.buf + sizeof(AG_SPTD), 512);
    return true;
}

// SMART READ DATA 512B -> 属性表解析（offset 2 起，30 个 12 字节属性）
static void ParseSmartToInfo(const unsigned char smart[512], DiskInfo& out) {
    for (int i = 0; i < 30; i++) {
        const unsigned char* a = smart + 2 + i * 12;
        unsigned char id = a[0];
        if (id == 0x00) continue;
        unsigned char value = a[3];
        unsigned long long raw = 0;
        for (int k = 5; k >= 0; k--) raw = (raw << 8) | a[4 + k];   // raw 低 6 字节
        switch (id) {
            case 0x09: {  // Power-On Hours（个别盘单位为分钟/秒，温和启发式纠正）
                unsigned long long h = raw;
                if (h > 200000ULL) h /= 60;
                if (h > 200000ULL) h /= 60;
                out.powerOnHours = h;
                break;
            }
            case 0xC2: {  // Temperature（raw 低 2 字节）
                int t = (int)(raw & 0xFFFF);
                out.tempC = (t >= -50 && t <= 150) ? t : -1;
                break;
            }
            case 0xA9:  // Remaining Life Percentage（较新 SATA SSD）
            case 0xE7:  // SSD Life Left（经典 SATA SSD 剩余寿命百分比）
                if (value <= 100 && out.pctUsed < 0) out.pctUsed = 100 - value;
                break;
            case 0xF1: {  // Total LBAs Written（主机累计写入）
                if (out.writtenGB < 0 && raw > 0) {
                    out.writtenGB = (double)(raw & 0xFFFFFFFFFFFFULL) * 512.0
                                    / (1024.0 * 1024.0 * 1024.0);
                }
                break;
            }
        }
    }
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

    // ---- 6) SMART / Health：寿命 / 温度 / 通电 / 总写入 ----
    if (out.bus == "NVMe") {
        // M.2 NVMe 与 PCIe NVMe：NVMe Health Log
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
    } else if (out.bus == "SATA" || out.bus == "ATA") {
        // SATA SSD（含 M.2 SATA 形态）与机械盘：ATA SMART 双路径
        unsigned char sm[512];
        ZeroMemory(sm, sizeof(sm));
        if (QuerySmartViaAtaProperty(hd, sm) || QuerySmartViaSat(hd, sm)) {
            ParseSmartToInfo(sm, out);
        }
        // 机械盘没有寿命属性：pctUsed 保持 -1，服务端诚实显示 N/A
    }

    CloseHandle(hd);
    out.valid = true;
    return true;
}
