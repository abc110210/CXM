#pragma once
// 硬盘信息采集：压力文件所在物理盘的型号/总线/类型(SSD/HDD)/容量，
// 以及健康细节（寿命/温度/通电时长/累计写入）：
//   NVMe（含 M.2 NVMe）：NVMe Health Log —— 寿命/温度/通电/写入全量
//   SATA（含 M.2 SATA 形态的 SSD 与机械盘）：ATA SMART 属性表（双路径透传）
//     SSD：寿命(0xE7/0xA9)/温度(0xC2)/通电(0x09)/写入(0xF1)
//     机械盘：温度/通电/写入（无寿命属性，诚实 N/A）
// USB 盘多数拿不到 SMART，诚实显示 N/A。
#include "shared.h"
#include <string>

struct DiskInfo {
    bool        valid;            // 采集成功
    uint32_t    physicalDrive;    // PhysicalDriveN
    std::string model;            // ANSI（可直接进协议行）
    std::string bus;              // "NVMe" / "SATA" / "USB" / ...
    bool        ssdKnown;         // 类型是否判定成功
    bool        isSsd;
    double      capacityGB;
    int         pctUsed;          // NVMe PercentageUsed，-1 = N/A
    int         tempC;            // 摄氏度，-1 = N/A
    uint64_t    powerOnHours;     // UINT64_MAX = N/A
    double      writtenGB;        // NVMe Data Units Written，-1 = N/A
    double      freeGB;           // 压力盘剩余空间

    DiskInfo() : valid(false), physicalDrive(0), ssdKnown(false), isSsd(false),
                 capacityGB(0), pctUsed(-1), tempC(-1),
                 powerOnHours(UINT64_MAX), writtenGB(-1), freeGB(0) {}
};

// stressFilePath：压力文件完整路径（用于定位被压的盘）
bool QueryStressDiskInfo(const std::wstring& stressFilePath, DiskInfo& out);
