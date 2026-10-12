#pragma once
#include <string>
#include <vector>

struct ID3D12GraphicsCommandList;

namespace DlssNrNative {

void* WrapNvapi(unsigned id, void* original);
void SetEnabled(bool enabled);
bool IsActive();
std::string Status();
void BeginEvaluate(const void* feature, bool reset, unsigned every, unsigned everyPlain, long long slot, ID3D12GraphicsCommandList* cmd, bool profile);
bool EndEvaluate(ID3D12GraphicsCommandList* cmd);
std::vector<std::string> TakeProfileReports();

}
