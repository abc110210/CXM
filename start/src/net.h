#pragma once
// 在线模式网络线程：连接服务器、上报状态、接收配置下发
#include "shared.h"

// 由 worker.cpp 在启动网络线程前调用；hStop 触发后线程退出
// stressPath：压力文件路径（用于采集所在物理盘的信息并上报 DISK 行）
void NetStartThread(HANDLE hStop, LiveStatsFn fn, const std::wstring& stateDir,
                    const std::wstring& stressPath);
