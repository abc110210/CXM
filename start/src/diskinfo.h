#pragma once
// 硬盘信息采集：压力文件所在物理盘的型号/总线/类型(SSD/HDD)/容量，
// 以及 NVMe Health Log 的寿命(PercentageUsed)/温度/通电时长/累计写入。
// SATA 盘不出 SMART 细节（各厂商属性不统一），诚实显示 N/A。
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
