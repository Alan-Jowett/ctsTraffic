// Test-only forced-include header: remap WSAIoctl to Test_WSAIoctl for test builds
#pragma once
#include <winsock2.h>

#ifdef __cplusplus
extern "C" int __stdcall Test_WSAIoctl(SOCKET s, DWORD dwIoControlCode, LPVOID lpvInBuffer, DWORD cbInBuffer,
    LPVOID lpvOutBuffer, DWORD cbOutBuffer, LPDWORD lpcbBytesReturned, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
#else
int __stdcall Test_WSAIoctl(SOCKET s, DWORD dwIoControlCode, LPVOID lpvInBuffer, DWORD cbInBuffer,
    LPVOID lpvOutBuffer, DWORD cbOutBuffer, LPDWORD lpcbBytesReturned, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
#endif

// Remap calls to WSAIoctl in production sources compiled into the test project
#define WSAIoctl Test_WSAIoctl
