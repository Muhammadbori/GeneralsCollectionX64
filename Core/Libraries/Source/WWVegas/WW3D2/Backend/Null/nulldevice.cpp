/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// TheSuperHackers @feature O158 The null renderer (RTS_RENDERER=null), in place of dx12device.cpp: the renderer
// interface of DX12Device with no GPU behind it, for the headless builds of macOS and Linux. -headless never makes a
// device (W3DDisplay leaves WW3D::Init out), so these calls are reached only if a windowed start is tried: Init then
// fails with a reason and the game reports that no renderer is available.

#include "WW3D2/Backend/DX12/dx12device.h"
#include "WW3D2/Backend/DX12/dx12resource.h"

#include <chrono>
#include <map>
#include <string>
#include <thread>

DX12FrameCounters DX12Device_Counters;
bool DX12Device_DebugLayer = false;
bool DX12Device_Stats = false;
bool DX12Device_NoShaderCache = false;

bool DX12Device::Requested = false;
bool DX12Device::UnlockedPresent = false;
ID3D12Device * DX12Device::Device = nullptr;
unsigned DX12Device::Width = 0;
unsigned DX12Device::Height = 0;
unsigned long long DX12Device::FrameNumber = 0;

namespace
{
	const std::thread::id MainThread = std::this_thread::get_id();
	const std::chrono::steady_clock::time_point StartTime = std::chrono::steady_clock::now();
	std::map<std::string, bool> GameShaders;	// name + stage registered (the shader source is not kept)
	DX12TextureClass * RenderTarget = nullptr;

	std::string Shader_Key(const char * name, bool vertex_shader)
	{
		return std::string(vertex_shader ? "vs:" : "ps:") + (name ? name : "");
	}
}

unsigned long long DX12Device_Query_Ticks()
{
	return (unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - StartTime).count();
}

bool DX12Device_Is_Main_Thread()
{
	return std::this_thread::get_id() == MainThread;
}

void DX12Device_Set_User_Data_Directory(const char *)
{
}

void DX12Device_Set_Debug_Log_Directory(const char *)
{
}

bool DX12Device::Init(void *)
{
	return false;
}

void DX12Device::Shutdown()
{
	RenderTarget = nullptr;
}

const char * DX12Device::Get_Failure_Reason()
{
	return "this build has no renderer (headless build; run with -headless)";
}

const char * DX12Device::Get_Adapter_Name() { return "none"; }
unsigned DX12Device::Get_Vendor_Id() { return 0; }
unsigned DX12Device::Get_Device_Id() { return 0; }
unsigned long long DX12Device::Get_Dedicated_Video_Memory() { return 0; }
int DX12Device::Get_Display_Mode_Count() { return 0; }

DX12Device::DisplayMode DX12Device::Get_Display_Mode(int)
{
	DisplayMode mode = { 0, 0 };
	return mode;
}

bool DX12Device::Set_Resolution(unsigned width, unsigned height)
{
	Width = width;
	Height = height;
	return false;
}

void DX12Device::Begin_Frame() {}
void DX12Device::End_Frame(bool) { ++FrameNumber; }
void DX12Device::Submit_Frame(bool) {}
void DX12Device::Clear(bool, bool, unsigned, float, unsigned) {}
void DX12Device::Set_Viewport(unsigned, unsigned, unsigned, unsigned, float, float) {}
void DX12Device::Draw(const DX12DrawDesc &) {}

void DX12Device::Set_Render_Target(DX12TextureClass * color, DX12TextureClass *, bool)
{
	RenderTarget = color;
}

DX12TextureClass * DX12Device::Peek_Render_Target() { return RenderTarget; }
unsigned DX12Device::Get_Target_Width() { return RenderTarget ? RenderTarget->Get_Width() : Width; }
unsigned DX12Device::Get_Target_Height() { return RenderTarget ? RenderTarget->Get_Height() : Height; }
bool DX12Device::Copy_Target_To_Texture(DX12TextureClass *, const tagRECT *, int, int) { return false; }
DX12TextureClass * DX12Device::Read_Back_Buffer() { return nullptr; }
void DX12Device::Set_Gamma_Ramp(const unsigned short *, const unsigned short *, const unsigned short *) {}

unsigned DX12Device::Allocate_SRV_Slot() { return 1; }
void DX12Device::Free_SRV_Slot(unsigned) {}
unsigned DX12Device::Allocate_RTV_Slot() { return 1; }
void DX12Device::Free_RTV_Slot(unsigned) {}
unsigned DX12Device::Allocate_DSV_Slot() { return 1; }
void DX12Device::Free_DSV_Slot(unsigned) {}
void DX12Device::Release_When_Idle(ID3D12Resource *) {}
void DX12Device::Queue_Dirty_Texture(DX12TextureClass *) {}
void DX12Device::Queue_Texture_Upload(DX12TextureClass *, unsigned, ID3D12Resource *) {}

bool DX12Device::Allocate_Frame_Memory(unsigned, void ** cpu, unsigned long long * gpu)
{
	if (cpu) *cpu = nullptr;
	if (gpu) *gpu = 0;
	return false;
}

void DX12Device::Register_Game_Shader(const char * name, bool vertex_shader, const char *, const char *)
{
	GameShaders[Shader_Key(name, vertex_shader)] = true;
}

bool DX12Device::Is_Game_Shader_Registered(const char * name, bool vertex_shader)
{
	return GameShaders.count(Shader_Key(name, vertex_shader)) != 0;
}

unsigned DX12Device::Create_Game_Shader(const char *, bool, const unsigned *) { return 0; }
void DX12Device::Delete_Game_Shader(unsigned) {}
bool DX12Device::Is_Game_Shader(unsigned, bool) { return false; }
