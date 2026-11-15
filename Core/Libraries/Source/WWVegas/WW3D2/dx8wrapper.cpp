/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WW3D                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/dx8wrapper.cpp                         $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                      $Author:: Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 08/05/02 1:27p                                              $*
 *                                                                                             *
 *                    $Revision:: 170                                                         $*
 *                                                                                             *
 * 06/26/02 KM Matrix name change to avoid MAX conflicts                                       *
 * 06/27/02 KM Render to shadow buffer texture support														*
 * 06/27/02 KM Shader system updates																				*
 * 08/05/02 KM Texture class redesign
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

//#define CREATE_DX8_MULTI_THREADED
//#define CREATE_DX8_FPU_PRESERVE
#define WW3D_DEVTYPE D3DDEVTYPE_HAL

#if !defined(WINVER) || WINVER < 0x0500
#undef WINVER
#define WINVER 0x0500 // Required to access GetMonitorInfo in VC6.
#endif

#include "dx8wrapper.h"
#include "dx8webbrowser.h"
#include "dx8fvf.h"
#include "dx8vertexbuffer.h"
#include "dx8indexbuffer.h"
#include "dx8renderer.h"
#include "ww3d.h"
#include "camera.h"
#include "WWLib/wwstring.h"
#include "WWMath/matrix4.h"
#include "vertmaterial.h"
#include "rddesc.h"
#include "lightenvironment.h"
#include "statistics.h"
#include "WWLib/registry.h"
#include "boxrobj.h"
#include "pointgr.h"
#include "render2d.h"
#include "sortingrenderer.h"
#include "shattersystem.h"
#include "light.h"
#include "assetmgr.h"
#include "textureloader.h"
#include "missingtexture.h"
#include "WWLib/thread.h"
#include "WWMath/pot.h"
#include "WWDebug/wwprofile.h"
#include "WWLib/ffactory.h"
#include "dx8caps.h"
#include "formconv.h"
#include "WWLib/bound.h"
#include "DbgHelpGuard.h"
#include "WW3D2/Backend/DX12/dx12device.h"
#include "WW3D2/Backend/DX12/dx12resource.h"
#include <Utility/bit_adapter.h>

#include "shdlib.h"

const int DEFAULT_RESOLUTION_WIDTH = 640;
const int DEFAULT_RESOLUTION_HEIGHT = 480;
const int DEFAULT_BIT_DEPTH = 32;
const int DEFAULT_TEXTURE_BIT_DEPTH = 16;
const D3DMULTISAMPLE_TYPE DEFAULT_MSAA = D3DMULTISAMPLE_NONE;

DX8FrameStatistics DX8Wrapper::FrameStatistics;
static DX8FrameStatistics LastFrameStatistics;

bool DX8Wrapper_IsWindowed = true;

/***********************************************************************************
**
** DX8Wrapper Static Variables
**
***********************************************************************************/

static HWND						_Hwnd															= nullptr;
bool								DX8Wrapper::IsInitted									= false;
bool								DX8Wrapper::_EnableTriangleDraw						= true;

int								DX8Wrapper::CurRenderDevice							= -1;
int								DX8Wrapper::ResolutionWidth							= DEFAULT_RESOLUTION_WIDTH;
int								DX8Wrapper::ResolutionHeight							= DEFAULT_RESOLUTION_HEIGHT;
int								DX8Wrapper::BitDepth										= DEFAULT_BIT_DEPTH;
int								DX8Wrapper::TextureBitDepth							= DEFAULT_TEXTURE_BIT_DEPTH;
bool								DX8Wrapper::IsWindowed									= false;
int									DX8Wrapper::WindowPlacement							= 0;
D3DFORMAT					DX8Wrapper::DisplayFormat	= D3DFMT_UNKNOWN;
D3DMULTISAMPLE_TYPE DX8Wrapper::MultiSampleAntiAliasing	= DEFAULT_MSAA;

// shader system additions KJM v
DWORD								DX8Wrapper::Vertex_Shader								= 0;
DWORD								DX8Wrapper::Pixel_Shader								= 0;

Vector4							DX8Wrapper::Vertex_Shader_Constants[MAX_VERTEX_SHADER_CONSTANTS];
Vector4							DX8Wrapper::Pixel_Shader_Constants[MAX_PIXEL_SHADER_CONSTANTS];

LightEnvironmentClass*		DX8Wrapper::Light_Environment							= nullptr;

DWORD								DX8Wrapper::Vertex_Processing_Behavior				= 0;
ZTextureClass*					DX8Wrapper::Shadow_Map[MAX_SHADOW_MAPS];

Vector3							DX8Wrapper::Ambient_Color;
// shader system additions KJM ^

bool								DX8Wrapper::world_identity;
unsigned							DX8Wrapper::RenderStates[256];
unsigned							DX8Wrapper::TextureStageStates[MAX_TEXTURE_STAGES][32];
DX12TextureClass *			DX8Wrapper::DX12Textures[MAX_TEXTURE_STAGES];
DX8Wrapper::DX12DeviceStateStruct DX8Wrapper::DX12Device_State;
D3DMATERIAL8				DX8Wrapper::DX8Material;
D3DLIGHT8						DX8Wrapper::DX8Lights[4];
D3DVIEWPORT8					DX8Wrapper::DX12Viewport;
RenderStateStruct				DX8Wrapper::render_state;
unsigned							DX8Wrapper::render_state_changed;

bool								DX8Wrapper::FogEnable									= false;
D3DCOLOR							DX8Wrapper::FogColor										= 0;

bool								DX8Wrapper::IsRenderToTexture							= false;

unsigned							DX8Wrapper::_MainThreadID								= 0;
bool								DX8Wrapper::CurrentDX8LightEnables[4];
bool								DX8Wrapper::IsDeviceLost;
int								DX8Wrapper::ZBias;
float								DX8Wrapper::ZNear;
float								DX8Wrapper::ZFar;
D3DMATRIX						DX8Wrapper::ProjectionMatrix;
D3DMATRIX						DX8Wrapper::DX8Transforms[D3DTS_WORLD+1];

DX8Caps*							DX8Wrapper::CurrentCaps = nullptr;

// Hack test... this disables rendering of batches of too few polygons.
unsigned							DX8Wrapper::DrawPolygonLowBoundLimit=0;

D3DADAPTER_IDENTIFIER8		DX8Wrapper::CurrentAdapterIdentifier;

unsigned long DX8Wrapper::FrameCount = 0;

bool								_DX8SingleThreaded										= false;

static D3DPRESENT_PARAMETERS								_PresentParameters;
static DynamicVectorClass<StringClass>					_RenderDeviceNameTable;
static DynamicVectorClass<StringClass>					_RenderDeviceShortNameTable;
static DynamicVectorClass<RenderDeviceDescClass>	_RenderDeviceDescriptionTable;


DX8_CleanupHook	 *DX8Wrapper::m_pCleanupHook=nullptr;

namespace wrapper
{
void D3DMatrixIdentity(D3DMATRIX* dxm);
} // namespace wrapper
#ifdef EXTENDED_STATS
DX8_Stats	 DX8Wrapper::stats;
#endif
/***********************************************************************************
**
** DX8Wrapper Implementation
**
***********************************************************************************/

// TheSuperHackers @refactor The error code is logged as a number; the D3DX error string lookup is gone.
void Log_DX8_ErrorCode(unsigned res)
{
	WWDEBUG_SAY(("Direct3D error 0x%08X", res));

	WWASSERT(0);
}

void Non_Fatal_Log_DX8_ErrorCode(unsigned res,const char * file,int line)
{
	WWDEBUG_SAY(("DX8 Error: 0x%08X, File: %s, Line: %d",res,file,line));
}

// TheSuperHackers @info helmutbuhler 14/04/2025
// Helper function that moves x and y such that the inner rect fits into the outer rect.
// If the inner rect already is in the outer rect, then this does nothing.
// If the inner rect is larger than the outer rect, then the inner rect will be aligned to the top left of the outer rect.
void MoveRectIntoOtherRect(const RECT& inner, const RECT& outer, int* x, int* y)
{
	int dx = 0;
	if (inner.right > outer.right)
		dx = outer.right-inner.right;
	if (inner.left < outer.left)
		dx = outer.left-inner.left;

	int dy = 0;
	if (inner.bottom > outer.bottom)
		dy = outer.bottom-inner.bottom;
	if (inner.top < outer.top)
		dy = outer.top-inner.top;

	*x += dx;
	*y += dy;
}


bool DX8Wrapper::Init(void * hwnd, bool lite)
{
	WWASSERT(!IsInitted);

	// zero memory
	memset(DX12Textures,0,sizeof(DX12TextureClass*)*MAX_TEXTURE_STAGES);
	memset(DX8Lights,0,sizeof(DX8Lights));
	memset(&DX12Viewport,0,sizeof(DX12Viewport));
	memset(&DX8Material,0,sizeof(DX8Material));
	DX8Material.Diffuse.r=DX8Material.Diffuse.g=DX8Material.Diffuse.b=DX8Material.Diffuse.a=1.0f;
	DX8Material.Ambient=DX8Material.Diffuse;
	memset(RenderStates,0,sizeof(unsigned)*256);
	memset(TextureStageStates,0,sizeof(unsigned)*32*MAX_TEXTURE_STAGES);
	memset(Vertex_Shader_Constants,0,sizeof(Vector4)*MAX_VERTEX_SHADER_CONSTANTS);
	memset(Pixel_Shader_Constants,0,sizeof(Vector4)*MAX_PIXEL_SHADER_CONSTANTS);
	memset(&render_state,0,sizeof(RenderStateStruct));
	memset(Shadow_Map,0,sizeof(ZTextureClass*)*MAX_SHADOW_MAPS);

	/*
	** Initialize all variables!
	*/
	_Hwnd = (HWND)hwnd;
	_MainThreadID=ThreadClass::_Get_Current_Thread_ID();
	WWDEBUG_SAY(("DX8Wrapper main thread: 0x%x",_MainThreadID));
	CurRenderDevice = -1;
	ResolutionWidth = DEFAULT_RESOLUTION_WIDTH;
	ResolutionHeight = DEFAULT_RESOLUTION_HEIGHT;
	// Initialize Render2DClass Screen Resolution
	Render2DClass::Set_Screen_Resolution( RectClass( 0, 0, ResolutionWidth, ResolutionHeight ) );
	BitDepth = DEFAULT_BIT_DEPTH;
	IsWindowed = false;
	DX8Wrapper_IsWindowed = false;

	for (int light=0;light<4;++light) CurrentDX8LightEnables[light]=false;

	//old_vertex_shader; TODO
	//old_sr_shader;
	//current_shader;

	//world_identity;
	//CurrentFogColor;

	WWDEBUG_SAY(("Reset DX8Wrapper statistics"));
	Reset_Statistics();

	Invalidate_Cached_Render_States();

	// Every caller of WW3D::Init renders with Direct3D 12.
	DX12Device::Set_Requested(true);

	if (!lite) {
		WWDEBUG_SAY(("Create Direct3D12 device"));
		{
			DbgHelpGuard dbgHelpGuard;
			if (!DX12Device::Init(hwnd)) {
				WWDEBUG_SAY(("Direct3D12 initialization failed"));
				Shutdown();
				return false;
			}
		}
		IsInitted = true;

		WWDEBUG_SAY(("Enumerate devices"));
		Enumerate_DX12_Device();
		WWDEBUG_SAY(("DX8Wrapper Init completed (Direct3D12)"));
	}

	return(true);
}

static void Restore_Desktop_Display_Mode();

void DX8Wrapper::Shutdown()
{
	if (DX12Device::Is_Active()) {
		Release_Device();
	}

	Release_DX12_Textures();
	Release_DX12_Device_State();
	if (DX12Device::Is_Active()) {
		DX12Device::Shutdown();
	}

	Restore_Desktop_Display_Mode();

	_RenderDeviceNameTable.Clear();		 // note - Delete_All() resizes the vector, causing a reallocation.  Clear is better. jba.
	_RenderDeviceShortNameTable.Clear();
	_RenderDeviceDescriptionTable.Clear();

	DX8Caps::Shutdown();
	IsInitted = false;		// 010803 srj
}

void DX8Wrapper::Do_Onetime_Device_Dependent_Inits()
{
	/*
	** Set Global render states (some of which depend on caps)
	*/
	Compute_Caps(D3DFormat_To_WW3DFormat(DisplayFormat));

   /*
	** Initialize any other subsystems inside of WW3D
	*/
	MissingTexture::_Init();
	TextureFilterClass::_Init_Filters(
		(TextureFilterClass::TextureFilterMode)WW3D::Get_Texture_Filter(),
		(TextureFilterClass::AnisotropicFilterMode)WW3D::Get_Anisotropy_Level()
	);
	TheDX8MeshRenderer.Init();
	SHD_INIT;
	BoxRenderObjClass::Init();
	VertexMaterialClass::Init();
	PointGroupClass::_Init(); // This needs the VertexMaterialClass to be initted
	ShatterSystem::Init();
	TextureLoader::Init();

	Set_Default_Global_Render_States();
}

void DX8Wrapper::Set_Default_Global_Render_States()
{
	DX8_THREAD_ASSERT();
	const D3DCAPS8 &caps = Get_Current_Caps()->Get_DX8_Caps();

	Set_DX8_Render_State(D3DRS_RANGEFOGENABLE, (caps.RasterCaps & D3DPRASTERCAPS_FOGRANGE) ? TRUE : FALSE);
	Set_DX8_Render_State(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
	Set_DX8_Render_State(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
	Set_DX8_Render_State(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
	Set_DX8_Render_State(D3DRS_COLORVERTEX, TRUE);
	Set_DX8_Render_State(D3DRS_ZBIAS,0);
	Set_DX8_Texture_Stage_State(1, D3DTSS_BUMPENVLSCALE, F2DW(1.0f));
	Set_DX8_Texture_Stage_State(1, D3DTSS_BUMPENVLOFFSET, F2DW(0.0f));
	Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT00,F2DW(1.0f));
	Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT01,F2DW(0.0f));
	Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT10,F2DW(0.0f));
	Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT11,F2DW(1.0f));

//	Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_CW);
	// Set dither mode here?
}

void DX8Wrapper::Invalidate_Cached_Render_States()
{
	render_state_changed=0;

	int a;
	for (a=0;a<sizeof(RenderStates)/sizeof(unsigned);++a) {
		RenderStates[a]=0x12345678;
	}
	for (a=0;a<MAX_TEXTURE_STAGES;++a)
	{
		for (int b=0; b<32;b++)
		{
			TextureStageStates[a][b]=0x12345678;
		}
		//Need to explicitly set texture to null, otherwise app will not be able to
		//set it to null because of redundant state checker. MW
		if (DX12Device::Is_Active())
			Device_Set_DX12_Texture(a,nullptr);
	}
	Release_DX12_Textures();

	ShaderClass::Invalidate();

	//Need to explicitly set render_state texture pointers to null. MW
	Release_Render_State();

	// (gth) clear the matrix shadows too
	// In Direct3D 12 mode DX8Transforms holds the device transforms, which stay as they are.
	if (!DX12Device::Is_Active()) {
		memset(&DX8Transforms, 0, sizeof(DX8Transforms));
	}
}

void DX8Wrapper::Release_DX12_Textures()
{
	for (unsigned i=0;i<MAX_TEXTURE_STAGES;++i) {
		REF_PTR_RELEASE(DX12Textures[i]);
	}
}

void DX8Wrapper::Set_DX12_Texture(unsigned stage, DX12TextureClass* texture)
{
	WWASSERT(stage<MAX_TEXTURE_STAGES);
	if (stage>=MAX_TEXTURE_STAGES || DX12Textures[stage]==texture) return;

	SNAPSHOT_SAY(("DX12 - SetTexture(%x) ",texture));
	REF_PTR_SET(DX12Textures[stage],texture);
	Device_Set_DX12_Texture(stage,texture);
	DX8_RECORD_TEXTURE_CHANGE();
}

void DX8Wrapper::Device_Set_DX12_Texture(unsigned stage, DX12TextureClass* texture)
{
	if (stage>=MAX_TEXTURE_STAGES) return;
	REF_PTR_SET(DX12Device_State.Textures[stage],texture);
}

void DX8Wrapper::Device_Set_Stream(DX12BufferClass* buffer, unsigned stride)
{
	REF_PTR_SET(DX12Device_State.StreamBuffer,buffer);
	DX12Device_State.StreamStride=stride;
}

void DX8Wrapper::Device_Set_Indices(DX12BufferClass* buffer, unsigned base_vertex_index)
{
	REF_PTR_SET(DX12Device_State.IndexBuffer,buffer);
	DX12Device_State.IndexBaseVertex=base_vertex_index;
}

// Releases the textures and buffers the Direct3D 12 device state mirror holds, as the Direct3D 8
// code does by binding null before releasing the device.
void DX8Wrapper::Release_DX12_Device_State()
{
	for (unsigned i=0;i<MAX_TEXTURE_STAGES;++i) {
		REF_PTR_RELEASE(DX12Device_State.Textures[i]);
	}
	REF_PTR_RELEASE(DX12Device_State.StreamBuffer);
	DX12Device_State.StreamStride=0;
	REF_PTR_RELEASE(DX12Device_State.IndexBuffer);
	DX12Device_State.IndexBaseVertex=0;
}

// Puts the Direct3D 12 device state mirror into the state of a newly created Direct3D 8 device.
// The wrapper's caches are not touched; like in Direct3D 8 they stay invalidated until set.
void DX8Wrapper::Set_DX12_Default_Device_States()
{
	Release_DX12_Device_State();

	DX12DeviceStateStruct& dev=DX12Device_State;
	unsigned a;
	for (a=0;a<sizeof(dev.RenderStates)/sizeof(unsigned);++a) {
		dev.RenderStates[a]=0;
	}
	dev.RenderStates[D3DRS_ZENABLE]=D3DZB_TRUE;
	dev.RenderStates[D3DRS_FILLMODE]=D3DFILL_SOLID;
	dev.RenderStates[D3DRS_SHADEMODE]=D3DSHADE_GOURAUD;
	dev.RenderStates[D3DRS_ZWRITEENABLE]=TRUE;
	dev.RenderStates[D3DRS_LASTPIXEL]=TRUE;
	dev.RenderStates[D3DRS_SRCBLEND]=D3DBLEND_ONE;
	dev.RenderStates[D3DRS_DESTBLEND]=D3DBLEND_ZERO;
	dev.RenderStates[D3DRS_CULLMODE]=D3DCULL_CCW;
	dev.RenderStates[D3DRS_ZFUNC]=D3DCMP_LESSEQUAL;
	dev.RenderStates[D3DRS_ALPHAFUNC]=D3DCMP_ALWAYS;
	dev.RenderStates[D3DRS_FOGEND]=F2DW(1.0f);
	dev.RenderStates[D3DRS_FOGDENSITY]=F2DW(1.0f);
	dev.RenderStates[D3DRS_STENCILFAIL]=D3DSTENCILOP_KEEP;
	dev.RenderStates[D3DRS_STENCILZFAIL]=D3DSTENCILOP_KEEP;
	dev.RenderStates[D3DRS_STENCILPASS]=D3DSTENCILOP_KEEP;
	dev.RenderStates[D3DRS_STENCILFUNC]=D3DCMP_ALWAYS;
	dev.RenderStates[D3DRS_STENCILMASK]=0xffffffff;
	dev.RenderStates[D3DRS_STENCILWRITEMASK]=0xffffffff;
	dev.RenderStates[D3DRS_TEXTUREFACTOR]=0xffffffff;
	dev.RenderStates[D3DRS_CLIPPING]=TRUE;
	dev.RenderStates[D3DRS_LIGHTING]=TRUE;
	dev.RenderStates[D3DRS_COLORVERTEX]=TRUE;
	dev.RenderStates[D3DRS_LOCALVIEWER]=TRUE;
	dev.RenderStates[D3DRS_DIFFUSEMATERIALSOURCE]=D3DMCS_COLOR1;
	dev.RenderStates[D3DRS_SPECULARMATERIALSOURCE]=D3DMCS_COLOR2;
	dev.RenderStates[D3DRS_POINTSIZE]=F2DW(1.0f);
	dev.RenderStates[D3DRS_POINTSCALE_A]=F2DW(1.0f);
	dev.RenderStates[D3DRS_POINTSIZE_MAX]=F2DW(64.0f);
	dev.RenderStates[D3DRS_MULTISAMPLEANTIALIAS]=TRUE;
	dev.RenderStates[D3DRS_MULTISAMPLEMASK]=0xffffffff;
	dev.RenderStates[D3DRS_PATCHSEGMENTS]=F2DW(1.0f);
	dev.RenderStates[D3DRS_COLORWRITEENABLE]=0x0000000f;
	dev.RenderStates[D3DRS_BLENDOP]=D3DBLENDOP_ADD;

	for (a=0;a<MAX_TEXTURE_STAGES;++a) {
		unsigned* tss=dev.TextureStageStates[a];
		for (int b=0;b<32;++b) {
			tss[b]=0;
		}
		tss[D3DTSS_COLOROP]=(a==0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
		tss[D3DTSS_COLORARG1]=D3DTA_TEXTURE;
		tss[D3DTSS_COLORARG2]=D3DTA_CURRENT;
		tss[D3DTSS_ALPHAOP]=(a==0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
		tss[D3DTSS_ALPHAARG1]=D3DTA_TEXTURE;
		tss[D3DTSS_ALPHAARG2]=D3DTA_CURRENT;
		tss[D3DTSS_COLORARG0]=D3DTA_CURRENT;
		tss[D3DTSS_ALPHAARG0]=D3DTA_CURRENT;
		tss[D3DTSS_RESULTARG]=D3DTA_CURRENT;
		tss[D3DTSS_TEXCOORDINDEX]=a;
		tss[D3DTSS_ADDRESSU]=D3DTADDRESS_WRAP;
		tss[D3DTSS_ADDRESSV]=D3DTADDRESS_WRAP;
		tss[D3DTSS_ADDRESSW]=D3DTADDRESS_WRAP;
		tss[D3DTSS_MAGFILTER]=D3DTEXF_POINT;
		tss[D3DTSS_MINFILTER]=D3DTEXF_POINT;
		tss[D3DTSS_MIPFILTER]=D3DTEXF_NONE;
		tss[D3DTSS_MAXANISOTROPY]=1;
		tss[D3DTSS_TEXTURETRANSFORMFLAGS]=D3DTTFF_DISABLE;
	}

	dev.FVF=0;
	dev.VertexShader=0;
	dev.PixelShader=0;
	memset(dev.VertexShaderConstants,0,sizeof(dev.VertexShaderConstants));
	memset(dev.PixelShaderConstants,0,sizeof(dev.PixelShaderConstants));

	// A new device has every light disabled. CurrentDX8LightEnables doubles as the device's enables.
	memset(DX8Lights,0,sizeof(DX8Lights));
	for (a=0;a<4;++a) {
		CurrentDX8LightEnables[a]=false;
	}
	memset(&DX8Material,0,sizeof(DX8Material));
	DX8Material.Diffuse.r=DX8Material.Diffuse.g=DX8Material.Diffuse.b=DX8Material.Diffuse.a=1.0f;
	DX8Material.Ambient=DX8Material.Diffuse;

	for (a=0;a<=D3DTS_WORLD;++a) {
		wrapper::D3DMatrixIdentity(&DX8Transforms[a]);
	}
}

void DX8Wrapper::Do_Onetime_Device_Dependent_Shutdowns()
{
	/*
	** Shutdown ww3d systems
	*/
	int i;
	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		if (render_state.vertex_buffers[i]) render_state.vertex_buffers[i]->Release_Engine_Ref();
		REF_PTR_RELEASE(render_state.vertex_buffers[i]);
	}
	if (render_state.index_buffer) render_state.index_buffer->Release_Engine_Ref();
	REF_PTR_RELEASE(render_state.index_buffer);
	REF_PTR_RELEASE(render_state.material);
	for (i=0;i<CurrentCaps->Get_Max_Textures_Per_Pass();++i) REF_PTR_RELEASE(render_state.Textures[i]);


	TextureLoader::Deinit();
	SortingRendererClass::Deinit();
	DynamicVBAccessClass::_Deinit();
	DynamicIBAccessClass::_Deinit();
	ShatterSystem::Shutdown();
	PointGroupClass::_Shutdown();
	VertexMaterialClass::Shutdown();
	BoxRenderObjClass::Shutdown();
	SHD_SHUTDOWN;
	TheDX8MeshRenderer.Shutdown();
	MissingTexture::_Deinit();

	delete CurrentCaps;
	CurrentCaps=nullptr;

}


bool DX8Wrapper::Reset_Device(bool reload_assets)
{
	WWDEBUG_SAY(("Resetting device."));
	DX8_THREAD_ASSERT();
	if (IsInitted && DX12Device::Is_Active()) {
		// A Direct3D 12 device is never lost and keeps every resource, so a reset only resizes the
		// swap chain. The wrapper's state caches stay valid.
		if (!DX12Device::Set_Resolution(ResolutionWidth, ResolutionHeight)) {
			WWDEBUG_SAY(("Device reset failed"));
			return false;
		}
		// A Direct3D 8 reset also leaves the back buffer as the render target.
		Set_DX12_Render_Target(nullptr, nullptr);
		D3DVIEWPORT8 vp = { 0, 0, (DWORD)ResolutionWidth, (DWORD)ResolutionHeight, 0.0f, 1.0f };
		Set_Viewport(&vp);
		WWDEBUG_SAY(("Device reset completed"));
		return true;
	}
	WWDEBUG_SAY(("Device reset failed"));
	return false;
}

void DX8Wrapper::Release_Device()
{
	if (DX12Device::Is_Active()) {
		// The Direct3D 12 device itself is destroyed by Shutdown.
		Release_DX12_Device_State();
		if (CurrentCaps) {
			for (unsigned i=0;i<MAX_VERTEX_STREAMS;++i)
			{
				if (render_state.vertex_buffers[i]) render_state.vertex_buffers[i]->Release_Engine_Ref();
				REF_PTR_RELEASE(render_state.vertex_buffers[i]);
			}
			if (render_state.index_buffer) render_state.index_buffer->Release_Engine_Ref();
			REF_PTR_RELEASE(render_state.index_buffer);

			Do_Onetime_Device_Dependent_Shutdowns();
		}
	}
}

// Direct3D 12 mode has a single render device: the adapter DX12Device picked, with its display
// modes, all reported as 32 bit.
void DX8Wrapper::Enumerate_DX12_Device()
{
	WWASSERT(DX12Device::Is_Active());

	::ZeroMemory(&CurrentAdapterIdentifier, sizeof(D3DADAPTER_IDENTIFIER8));
	const char * adapter_name = DX12Device::Get_Adapter_Name();
	strlcpy(CurrentAdapterIdentifier.Description, adapter_name ? adapter_name : "Direct3D 12", sizeof(CurrentAdapterIdentifier.Description));
	strlcpy(CurrentAdapterIdentifier.Driver, "d3d12.dll", sizeof(CurrentAdapterIdentifier.Driver));
	CurrentAdapterIdentifier.VendorId = DX12Device::Get_Vendor_Id();
	CurrentAdapterIdentifier.DeviceId = DX12Device::Get_Device_Id();

	RenderDeviceDescClass desc;
	desc.set_device_name(CurrentAdapterIdentifier.Description);
	desc.set_driver_name(CurrentAdapterIdentifier.Driver);
	desc.set_driver_version("12.0.0.0");

	DX8Caps dx12caps(CurrentAdapterIdentifier);
	desc.Caps = dx12caps.Get_DX8_Caps();
	desc.AdapterIdentifier = CurrentAdapterIdentifier;

	desc.reset_resolution_list();
	const int mode_count = DX12Device::Get_Display_Mode_Count();
	for (int mode_index=0; mode_index<mode_count; mode_index++) {
		DX12Device::DisplayMode mode = DX12Device::Get_Display_Mode(mode_index);
		if (mode.Width > 0 && mode.Height > 0) {
			desc.add_resolution(mode.Width, mode.Height, 32);
		}
	}

	// The adapter may not report modes (for example a render-only adapter), but the game window can
	// still be presented to, so offer the desktop resolution.
	if (desc.Enumerate_Resolutions().Count() == 0) {
		desc.add_resolution(::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN), 32);
	}

	StringClass device_name(CurrentAdapterIdentifier.Description,true);
	_RenderDeviceNameTable.Add(device_name);
	_RenderDeviceShortNameTable.Add(device_name);
	_RenderDeviceDescriptionTable.Add(desc);
}

bool DX8Wrapper::Set_Any_Render_Device()
{
	// Try fullscreen first
	int dev_number = 0;
	for (; dev_number < _RenderDeviceNameTable.Count(); dev_number++) {
		if (Set_Render_Device(dev_number,-1,-1,-1,0,false)) {
			return true;
		}
	}

	// Then windowed
	for (dev_number = 0; dev_number < _RenderDeviceNameTable.Count(); dev_number++) {
		if (Set_Render_Device(dev_number,-1,-1,-1,1,false)) {
			return true;
		}
	}

	return false;
}

bool DX8Wrapper::Set_Render_Device
(
	const char * dev_name,
	int width,
	int height,
	int bits,
	int windowed,
	bool resize_window
)
{
	for ( int dev_number = 0; dev_number < _RenderDeviceNameTable.Count(); dev_number++) {
		if ( strcmp( dev_name, _RenderDeviceNameTable[dev_number]) == 0) {
			return Set_Render_Device( dev_number, width, height, bits, windowed, resize_window );
		}

		if ( strcmp( dev_name, _RenderDeviceShortNameTable[dev_number]) == 0) {
			return Set_Render_Device( dev_number, width, height, bits, windowed, resize_window );
		}
	}
	return false;
}

void DX8Wrapper::Get_Format_Name(unsigned int format, StringClass *tex_format)
{
		*tex_format="Unknown";
		switch (format) {
		case D3DFMT_A8R8G8B8: *tex_format="D3DFMT_A8R8G8B8"; break;
		case D3DFMT_R8G8B8: *tex_format="D3DFMT_R8G8B8"; break;
		case D3DFMT_A4R4G4B4: *tex_format="D3DFMT_A4R4G4B4"; break;
		case D3DFMT_A1R5G5B5: *tex_format="D3DFMT_A1R5G5B5"; break;
		case D3DFMT_R5G6B5: *tex_format="D3DFMT_R5G6B5"; break;
		case D3DFMT_L8: *tex_format="D3DFMT_L8"; break;
		case D3DFMT_A8: *tex_format="D3DFMT_A8"; break;
		case D3DFMT_P8: *tex_format="D3DFMT_P8"; break;
		case D3DFMT_X8R8G8B8: *tex_format="D3DFMT_X8R8G8B8"; break;
		case D3DFMT_X1R5G5B5: *tex_format="D3DFMT_X1R5G5B5"; break;
		case D3DFMT_R3G3B2: *tex_format="D3DFMT_R3G3B2"; break;
		case D3DFMT_A8R3G3B2: *tex_format="D3DFMT_A8R3G3B2"; break;
		case D3DFMT_X4R4G4B4: *tex_format="D3DFMT_X4R4G4B4"; break;
		case D3DFMT_A8P8: *tex_format="D3DFMT_A8P8"; break;
		case D3DFMT_A8L8: *tex_format="D3DFMT_A8L8"; break;
		case D3DFMT_A4L4: *tex_format="D3DFMT_A4L4"; break;
		case D3DFMT_V8U8: *tex_format="D3DFMT_V8U8"; break;
		case D3DFMT_L6V5U5: *tex_format="D3DFMT_L6V5U5"; break;
		case D3DFMT_X8L8V8U8: *tex_format="D3DFMT_X8L8V8U8"; break;
		case D3DFMT_Q8W8V8U8: *tex_format="D3DFMT_Q8W8V8U8"; break;
		case D3DFMT_V16U16: *tex_format="D3DFMT_V16U16"; break;
		case D3DFMT_W11V11U10: *tex_format="D3DFMT_W11V11U10"; break;
		case D3DFMT_UYVY: *tex_format="D3DFMT_UYVY"; break;
		case D3DFMT_YUY2: *tex_format="D3DFMT_YUY2"; break;
		case D3DFMT_DXT1: *tex_format="D3DFMT_DXT1"; break;
		case D3DFMT_DXT2: *tex_format="D3DFMT_DXT2"; break;
		case D3DFMT_DXT3: *tex_format="D3DFMT_DXT3"; break;
		case D3DFMT_DXT4: *tex_format="D3DFMT_DXT4"; break;
		case D3DFMT_DXT5: *tex_format="D3DFMT_DXT5"; break;
		case D3DFMT_D16_LOCKABLE: *tex_format="D3DFMT_D16_LOCKABLE"; break;
		case D3DFMT_D32: *tex_format="D3DFMT_D32"; break;
		case D3DFMT_D15S1: *tex_format="D3DFMT_D15S1"; break;
		case D3DFMT_D24S8: *tex_format="D3DFMT_D24S8"; break;
		case D3DFMT_D16: *tex_format="D3DFMT_D16"; break;
		case D3DFMT_D24X8: *tex_format="D3DFMT_D24X8"; break;
		case D3DFMT_D24X4S4: *tex_format="D3DFMT_D24X4S4"; break;
		default:	break;
		}
}

// Fullscreen is presented as a borderless window covering the monitor. Exclusive fullscreen
// Direct3D 8 never presents on current drivers (the window stays on the splash screen and
// all frames are black), and the Direct3D 12 flip model presents the same borderless way.
// A resolution other than the monitor's own is applied as a temporary display mode change.
static bool _DisplayModeChanged = false;

static void Restore_Desktop_Display_Mode()
{
	if (_DisplayModeChanged) {
		::ChangeDisplaySettingsEx(nullptr, nullptr, nullptr, 0, nullptr);
		_DisplayModeChanged = false;
	}
}

static void Set_Fullscreen_Display_Mode(HWND hwnd, int width, int height)
{
	Restore_Desktop_Display_Mode();

	MONITORINFOEX mi;
	mi.cbSize = sizeof(mi);
	::GetMonitorInfo(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
	if ((mi.rcMonitor.right - mi.rcMonitor.left) == width && (mi.rcMonitor.bottom - mi.rcMonitor.top) == height) {
		return;
	}

	DEVMODE dm = { 0 };
	dm.dmSize = sizeof(dm);
	dm.dmPelsWidth = width;
	dm.dmPelsHeight = height;
	dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
	if (::ChangeDisplaySettingsEx(mi.szDevice, &dm, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL) {
		_DisplayModeChanged = true;
	} else {
		WWDEBUG_SAY(("Display mode %dx%d is not supported, keeping the desktop mode", width, height));
	}
}

void DX8Wrapper::Resize_And_Position_Window()
{
	if (IsWindowed) {
		Restore_Desktop_Display_Mode();
	} else {
		Set_Fullscreen_Display_Mode(_Hwnd, ResolutionWidth, ResolutionHeight);
	}

	// Get the current dimensions of the 'render area' of the window
	RECT rect = { 0 };
	::GetClientRect (_Hwnd, &rect);

	// Is the window the correct size for this resolution?
	if ((rect.right-rect.left) != ResolutionWidth ||
			(rect.bottom-rect.top) != ResolutionHeight) {

		// Calculate what the main window's bounding rectangle should be to
		// accommodate this resolution
		rect.left = 0;
		rect.top = 0;
		rect.right = ResolutionWidth;
		rect.bottom = ResolutionHeight;
		DWORD dwstyle = ::GetWindowLong (_Hwnd, GWL_STYLE);
		AdjustWindowRect (&rect, dwstyle, FALSE);
		int width = rect.right-rect.left;
		int height = rect.bottom-rect.top;

		// Resize the window to fit this resolution
		if (!IsWindowed)
		{
			MONITORINFO mi = {sizeof(MONITORINFO)};
			GetMonitorInfo(MonitorFromWindow(_Hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
			::SetWindowPos(_Hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, width, height, 0);

			DEBUG_LOG(("Window resized to w:%d h:%d", width, height));
		}
		else
		{
			// TheSuperHackers @feature helmutbuhler 14/04/2025
			// Center the window in the workarea of the monitor it is on.
			MONITORINFO mi = {sizeof(MONITORINFO)};
			GetMonitorInfo(MonitorFromWindow(_Hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
			int left = (mi.rcWork.left + mi.rcWork.right - width) / 2;
			int top  = (mi.rcWork.top + mi.rcWork.bottom - height) / 2;

			// TheSuperHackers @feature A corner of the work area instead of its centre (WindowPosition of Options.ini).
			if (WindowPlacement == 1 || WindowPlacement == 3)
				left = mi.rcWork.left;
			if (WindowPlacement == 2 || WindowPlacement == 4)
				left = mi.rcWork.right - width;
			if (WindowPlacement == 1 || WindowPlacement == 2)
				top = mi.rcWork.top;
			if (WindowPlacement == 3 || WindowPlacement == 4)
				top = mi.rcWork.bottom - height;

			// TheSuperHackers @feature helmutbuhler 14/04/2025
			// Move the window to try fit it into the monitor area, if one of its dimensions is larger than the work area.
			// Otherwise align the window to the top left edges, if it is even larger than the monitor area.
			RECT rectClient;
			rectClient.left = left - rect.left;
			rectClient.top = top - rect.top;
			rectClient.right = rectClient.left + ResolutionWidth;
			rectClient.bottom = rectClient.top + ResolutionHeight;
			MoveRectIntoOtherRect(rectClient, mi.rcMonitor, &left, &top);

			::SetWindowPos (_Hwnd, nullptr, left, top, width, height, SWP_NOZORDER);

			DEBUG_LOG(("Window positioned to x:%d y:%d, resized to w:%d h:%d", left, top, width, height));
		}
	}
}

bool DX8Wrapper::Set_Render_Device(int dev, int width, int height, int bits, int windowed,
								   bool resize_window,bool reset_device, bool restore_assets)
{
	WWASSERT(IsInitted);
	WWASSERT(dev >= -1);
	WWASSERT(dev < _RenderDeviceNameTable.Count());

	/*
	** If user has never selected a render device, start out with device 0
	*/
	if ((CurRenderDevice == -1) && (dev == -1)) {
		CurRenderDevice = 0;
	} else if (dev != -1) {
		CurRenderDevice = dev;
	}

	/*
	** If user doesn't want to change res, set the res variables to match the
	** current resolution
	*/
	if (width != -1)		ResolutionWidth = width;
	if (height != -1)		ResolutionHeight = height;

	if (bits != -1)		BitDepth = bits;
	if (windowed != -1)	IsWindowed = (windowed != 0);
	DX8Wrapper_IsWindowed = IsWindowed;

	WWDEBUG_SAY(("Attempting Set_Render_Device: name: %s (%s:%s), width: %d, height: %d, windowed: %d",
		_RenderDeviceNameTable[CurRenderDevice].str(),_RenderDeviceDescriptionTable[CurRenderDevice].Get_Driver_Name(),
		_RenderDeviceDescriptionTable[CurRenderDevice].Get_Driver_Version(),ResolutionWidth,ResolutionHeight,(IsWindowed ? 1 : 0)));

#ifdef _WIN32
	// PWG 4/13/2000 - changed so that if you say to resize the window it resizes
	// regardless of whether its windowed or not as OpenGL resizes its self around
	// the caption and edges of the window type you provide, so its important to
	// push the client area to be the size you really want.
	// if ( resize_window && windowed ) {
	if (resize_window) {
		Resize_And_Position_Window();
	}
#endif
	return Set_DX12_Render_Device(reset_device, restore_assets);
}

// Direct3D 12 part of Set_Render_Device. The swap chain always presents to the game window
// (fullscreen is the borderless window set up by Resize_And_Position_Window) in 32 bit with a
// D24S8 depth buffer. _PresentParameters is kept up to date for the code that reads it.
bool DX8Wrapper::Set_DX12_Render_Device(bool reset_device, bool restore_assets)
{
	BitDepth = 32;
	DisplayFormat = D3DFMT_X8R8G8B8;
	MultiSampleAntiAliasing = D3DMULTISAMPLE_NONE;

	const UINT presentation_interval = _PresentParameters.FullScreen_PresentationInterval;
	::ZeroMemory(&_PresentParameters, sizeof(D3DPRESENT_PARAMETERS));
	_PresentParameters.BackBufferWidth = ResolutionWidth;
	_PresentParameters.BackBufferHeight = ResolutionHeight;
	_PresentParameters.BackBufferCount = 1;
	_PresentParameters.BackBufferFormat = D3DFMT_A8R8G8B8;
	_PresentParameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
	_PresentParameters.hDeviceWindow = _Hwnd;
	_PresentParameters.Windowed = TRUE;
	_PresentParameters.EnableAutoDepthStencil = TRUE;
	_PresentParameters.AutoDepthStencilFormat = D3DFMT_D24S8;
	_PresentParameters.MultiSampleType = D3DMULTISAMPLE_NONE;
	_PresentParameters.FullScreen_PresentationInterval = presentation_interval ? presentation_interval : D3DPRESENT_INTERVAL_DEFAULT;
	_PresentParameters.FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;

	bool ret;
	if (reset_device && CurrentCaps != nullptr) {
		WWDEBUG_SAY(("DX8Wrapper::Set_Render_Device is resetting the device."));
		ret = Reset_Device(restore_assets);
	}
	else if (CurrentCaps != nullptr) {
		// The device-dependent systems already exist; only the swap chain changes.
		ret = DX12Device::Set_Resolution(ResolutionWidth, ResolutionHeight);
	}
	else {
		ret = DX12Device::Set_Resolution(ResolutionWidth, ResolutionHeight);
		if (ret) {
			_DX8SingleThreaded = true;
			Vertex_Processing_Behavior = D3DCREATE_HARDWARE_VERTEXPROCESSING;
			Set_DX12_Default_Device_States();
			Do_Onetime_Device_Dependent_Inits();
		}
	}

	WWDEBUG_SAY(("Direct3D12 Set_Render_Device done: %dx%d, windowed: %d, success: %d", ResolutionWidth, ResolutionHeight, IsWindowed ? 1 : 0, ret ? 1 : 0));

	if (ret) {
		D3DVIEWPORT8 vp = { 0, 0, (DWORD)ResolutionWidth, (DWORD)ResolutionHeight, 0.0f, 1.0f };
		Set_Viewport(&vp);
		Render2DClass::Set_Screen_Resolution( RectClass( 0, 0, ResolutionWidth, ResolutionHeight ) );
	}

	return ret;
}

bool DX8Wrapper::Set_Next_Render_Device()
{
	int new_dev = (CurRenderDevice + 1) % _RenderDeviceNameTable.Count();
	return Set_Render_Device(new_dev);
}

bool DX8Wrapper::Toggle_Windowed()
{
#ifdef WW3D_DX8
	// State OK?
	assert (IsInitted);
	if (IsInitted) {

		// Get information about the current render device's resolutions
		const RenderDeviceDescClass &render_device = Get_Render_Device_Desc ();
		const DynamicVectorClass<ResolutionDescClass> &resolutions = render_device.Enumerate_Resolutions ();

		// Loop through all the resolutions supported by the current device.
		// If we aren't currently running under one of these resolutions,
		// then we should probably		 to the closest resolution before
		// toggling the windowed state.
		int curr_res = -1;
		for (int res = 0;
		     (res < resolutions.Count ()) && (curr_res == -1);
			  res ++) {

			// Is this the resolution we are looking for?
			if ((resolutions[res].Width == ResolutionWidth) &&
				 (resolutions[res].Height == ResolutionHeight) &&
				 (resolutions[res].BitDepth == BitDepth)) {
				curr_res = res;
			}
		}

		if (curr_res == -1) {

			// We don't match any of the standard resolutions,
			// so set the first resolution and toggle the windowed state.
			return Set_Device_Resolution (resolutions[0].Width,
								 resolutions[0].Height,
								 resolutions[0].BitDepth,
								 !IsWindowed, true);
		} else {

			// Toggle the windowed state
			return Set_Device_Resolution (-1, -1, -1, !IsWindowed, true);
		}
	}
#endif //WW3D_DX8

	return false;
}

void DX8Wrapper::Set_Swap_Interval(int swap)
{
	switch (swap) {
		case 0: _PresentParameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE; break;
		case 1: _PresentParameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE ; break;
		case 2: _PresentParameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_TWO; break;
		case 3: _PresentParameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_THREE; break;
		default: _PresentParameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE ; break;
	}

	WWDEBUG_SAY(("DX8Wrapper::Set_Swap_Interval is resetting the device."));
	Reset_Device();
}

int DX8Wrapper::Get_Swap_Interval()
{
	return _PresentParameters.FullScreen_PresentationInterval;
}

bool DX8Wrapper::Has_Stencil()
{
	// The Direct3D 12 renderer always has a D24S8 depth buffer and emulates the stencil states.
	return true;
}

int DX8Wrapper::Get_Render_Device_Count()
{
	return _RenderDeviceNameTable.Count();

}
int DX8Wrapper::Get_Render_Device()
{
	assert(IsInitted);
	return CurRenderDevice;
}

const RenderDeviceDescClass & DX8Wrapper::Get_Render_Device_Desc(int deviceidx)
{
	WWASSERT(IsInitted);

	if ((deviceidx == -1) && (CurRenderDevice == -1)) {
		CurRenderDevice = 0;
	}

	// if the device index is -1 then we want the current device
	if (deviceidx == -1) {
		WWASSERT(CurRenderDevice >= 0);
		WWASSERT(CurRenderDevice < _RenderDeviceNameTable.Count());
		return _RenderDeviceDescriptionTable[CurRenderDevice];
	}

	// We can only ask for multiple device information if the devices
	// have been detected.
	WWASSERT(deviceidx >= 0);
	WWASSERT(deviceidx < _RenderDeviceNameTable.Count());
	return _RenderDeviceDescriptionTable[deviceidx];
}

const char * DX8Wrapper::Get_Render_Device_Name(int device_index)
{
	device_index = device_index % _RenderDeviceShortNameTable.Count();
	return _RenderDeviceShortNameTable[device_index];
}

bool DX8Wrapper::Set_Device_Resolution(int width,int height,int bits,int windowed, bool resize_window)
{
	if (DX12Device::Is_Active()) {

		if (width != -1) {
			_PresentParameters.BackBufferWidth = ResolutionWidth = width;
		}
		if (height != -1) {
			_PresentParameters.BackBufferHeight = ResolutionHeight = height;
		}
		// Direct3D 12 fullscreen is a borderless window, so the windowed state can change freely.
		if (windowed != -1) {
			IsWindowed = (windowed != 0);
			DX8Wrapper_IsWindowed = IsWindowed;
		}
		if (resize_window)
		{
			Resize_And_Position_Window();
		}
#pragma message("TODO: support changing windowed status and changing the bit depth")
		WWDEBUG_SAY(("DX8Wrapper::Set_Device_Resolution is resetting the device."));
		return Reset_Device();
	} else {
		return false;
	}
}

void DX8Wrapper::Get_Device_Resolution(int & set_w,int & set_h,int & set_bits,bool & set_windowed)
{
	WWASSERT(IsInitted);

	set_w = ResolutionWidth;
	set_h = ResolutionHeight;
	set_bits = BitDepth;
	set_windowed = IsWindowed;
}

void DX8Wrapper::Get_Render_Target_Resolution(int & set_w,int & set_h,int & set_bits,bool & set_windowed)
{
	WWASSERT(IsInitted);

	if (DX12Device::Is_Render_To_Texture()) {
		set_w				= (int)DX12Device::Get_Target_Width();
		set_h				= (int)DX12Device::Get_Target_Height();
		set_bits			= BitDepth;
		set_windowed	= IsWindowed;
		return;
	}

	Get_Device_Resolution (set_w, set_h, set_bits, set_windowed);
}

bool DX8Wrapper::Registry_Save_Render_Device( const char * sub_key )
{
	int	width, height, depth;
	bool	windowed;
	Get_Device_Resolution(width, height, depth, windowed);
	return Registry_Save_Render_Device(sub_key, CurRenderDevice, ResolutionWidth, ResolutionHeight, BitDepth, IsWindowed, TextureBitDepth);
}

bool DX8Wrapper::Registry_Save_Render_Device( const char *sub_key, int device, int width, int height, int depth, bool windowed, int texture_depth)
{
	RegistryClass * registry = W3DNEW RegistryClass( sub_key );
	WWASSERT( registry );

	if ( !registry->Is_Valid() ) {
		delete registry;
		WWDEBUG_SAY(( "Error getting Registry" ));
		return false;
	}

	registry->Set_String( VALUE_NAME_RENDER_DEVICE_NAME,
		_RenderDeviceShortNameTable[device] );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_WIDTH,	width );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_HEIGHT, height );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_DEPTH, depth );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_WINDOWED, windowed );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_TEXTURE_DEPTH, texture_depth );

	delete registry;
	return true;
}

bool DX8Wrapper::Registry_Load_Render_Device( const char * sub_key, bool resize_window )
{
	char	name[ 200 ];
	int	width,height,depth,windowed;

	if (	Registry_Load_Render_Device(	sub_key,
													name,
													sizeof(name),
													width,
													height,
													depth,
													windowed,
													TextureBitDepth) &&
			(*name != 0))
	{
		WWDEBUG_SAY(( "Device %s (%d X %d) %d bit windowed:%d", name,width,height,depth,windowed));

		if (TextureBitDepth==16 || TextureBitDepth==32) {
//			WWDEBUG_SAY(( "Texture depth %d", TextureBitDepth));
		} else {
			WWDEBUG_SAY(( "Invalid texture depth %d, switching to 16 bits", TextureBitDepth));
			TextureBitDepth=16;
		}

		if ( Set_Render_Device( name, width,height,depth,windowed, resize_window ) != true) {
			if (depth==16) depth=32;
			else depth=16;
			if ( Set_Render_Device( name, width,height,depth,windowed, resize_window ) == true) {
				return true;
			}
			if (depth==16) depth=32;
			else depth=16;
			// we'll test resolutions down, so if start is 640, increase to begin with...
			if (width==640) {
				width=1024;
				height=768;
			}
			for(;;) {
				if (width>2048) {
					width=2048;
					height=1536;
				}
				else if (width>1920) {
					width=1920;
					height=1440;
				}
				else if (width>1600) {
					width=1600;
					height=1200;
				}
				else if (width>1280) {
					width=1280;
					height=1024;
				}
				else if (width>1024) {
					width=1024;
					height=768;
				}
				else if (width>800) {
					width=800;
					height=600;
				}
				else if (width!=640) {
					width=640;
					height=480;
				}
				else {
					return Set_Any_Render_Device();
				}
				for (int i=0;i<2;++i) {
					if ( Set_Render_Device( name, width,height,depth,windowed, resize_window ) == true) {
						return true;
					}
					if (depth==16) depth=32;
					else depth=16;
				}
			}
		}

		return true;
	}

	WWDEBUG_SAY(( "Error getting Registry" ));

	return Set_Any_Render_Device();
}

bool DX8Wrapper::Registry_Load_Render_Device( const char * sub_key, char *device, int device_len, int &width, int &height, int &depth, int &windowed, int &texture_depth)
{
	RegistryClass registry( sub_key );

	if ( registry.Is_Valid() ) {
		registry.Get_String( VALUE_NAME_RENDER_DEVICE_NAME,
			device, device_len);

		width =		registry.Get_Int( VALUE_NAME_RENDER_DEVICE_WIDTH, -1 );
		height =		registry.Get_Int( VALUE_NAME_RENDER_DEVICE_HEIGHT, -1 );
		depth =		registry.Get_Int( VALUE_NAME_RENDER_DEVICE_DEPTH, -1 );
		windowed =	registry.Get_Int( VALUE_NAME_RENDER_DEVICE_WINDOWED, -1 );
		texture_depth = registry.Get_Int( VALUE_NAME_RENDER_DEVICE_TEXTURE_DEPTH, -1 );
		return true;
	}
	*device=0;
	width=-1;
	height=-1;
	depth=-1;
	windowed=-1;
	texture_depth=-1;
	return false;
}


void DX8Wrapper::Reset_Statistics()
{
	FrameStatistics = DX8FrameStatistics();
	LastFrameStatistics = DX8FrameStatistics();
}

void DX8Wrapper::Begin_Statistics()
{
	FrameStatistics = DX8FrameStatistics();
}

void DX8Wrapper::End_Statistics()
{
	LastFrameStatistics = FrameStatistics;
}

const DX8FrameStatistics& DX8Wrapper::Get_Last_Frame_Statistics()
{
	return LastFrameStatistics;
}

unsigned long DX8Wrapper::Get_FrameCount() {return FrameCount;}

void DX8_Assert()
{
	DX8_THREAD_ASSERT();
}

void DX8Wrapper::Begin_Scene()
{
	DX8_THREAD_ASSERT();

#if ENABLE_EMBEDDED_BROWSER
	DX8WebBrowser::Update();
#endif

	DX12Device::Begin_Frame();

	DX8WebBrowser::Update();
}

void DX8Wrapper::End_Scene(bool flip_frames)
{
	DX8_THREAD_ASSERT();

	DX8WebBrowser::Render(0);

	{
		WWPROFILE("DX12Device::End_Frame()");
		DX12Device::End_Frame(flip_frames);
	}
	if (flip_frames) {
		IsDeviceLost=false;
		FrameCount++;
	}

	// Each frame, release all of the buffers and textures.
	Set_Vertex_Buffer(nullptr);
	Set_Index_Buffer(nullptr,0);
	for (int i=0;i<CurrentCaps->Get_Max_Textures_Per_Pass();++i) Set_Texture(i,nullptr);
	Set_Material(nullptr);
}


void DX8Wrapper::Flip_To_Primary()
{
	// If we are fullscreen and the current frame is odd then we need
	// to force a page flip to ensure that the first buffer in the flipping
	// chain is the one visible.
}


//**********************************************************************************************
//! Clear current render device
/*! KM
/* 5/17/02 KM Fixed support for render to texture with depth/stencil buffers
*/
void DX8Wrapper::Clear(bool clear_color, bool clear_z_stencil, const Vector3 &color, float dest_alpha, float z, unsigned int stencil)
{
	DX8_THREAD_ASSERT();

	if (clear_color || clear_z_stencil) {
		DX12Device::Clear(clear_color, clear_z_stencil, Convert_Color(color,dest_alpha), z, stencil);
	}
}

void DX8Wrapper::Set_Viewport(CONST D3DVIEWPORT8* pViewport)
{
	DX8_THREAD_ASSERT();
	WWASSERT(pViewport);
	// Direct3D 8 rejects an empty viewport or one outside the render target (D3DERR_INVALIDCALL)
	// and keeps the current one. A shadow projector's first update relies on this: its texture
	// size is still 0 then, so its camera asks for a 0 x 0 viewport.
	if (pViewport->Width==0 || pViewport->Height==0
		|| (unsigned long long)pViewport->X+pViewport->Width > DX12Device::Get_Target_Width()
		|| (unsigned long long)pViewport->Y+pViewport->Height > DX12Device::Get_Target_Height()) {
		return;
	}
	DX12Viewport=*pViewport;
	DX12Device::Set_Viewport(pViewport->X, pViewport->Y, pViewport->Width, pViewport->Height, pViewport->MinZ, pViewport->MaxZ);
}

void DX8Wrapper::Get_Viewport(D3DVIEWPORT8* pViewport)
{
	DX8_THREAD_ASSERT();
	*pViewport=DX12Viewport;
}

// ----------------------------------------------------------------------------
//
// Pass-through calls whose result the caller inspects. They do not assert or log on failure and
// do not touch any of the wrapper's shadow state.
//
// ----------------------------------------------------------------------------

HRESULT DX8Wrapper::Get_DX8_Render_State_Raw(D3DRENDERSTATETYPE state, DWORD* value)
{
	if (value) *value = DX12Device_State.RenderStates[state];
	return D3D_OK;
}

HRESULT DX8Wrapper::Create_DX12_Shader(const char* name, bool vertex_shader, const DWORD* declaration, DWORD* handle)
{
	if (handle) *handle = 0;
	if (name == nullptr || handle == nullptr)
		return E_FAIL;
	static_assert(sizeof(DWORD) == sizeof(unsigned), "vertex declaration tokens are 32-bit");
	const unsigned result = DX12Device::Create_Game_Shader(name, vertex_shader, reinterpret_cast<const unsigned*>(declaration));
	if (result == 0)
		return E_FAIL;
	*handle = result;
	return D3D_OK;
}

HRESULT DX8Wrapper::Delete_Vertex_Shader(DWORD handle)
{
	// Only free a handle of the kind this call is for, as Direct3D 8 keeps the two handle spaces apart.
	if (DX12Device::Is_Game_Shader(handle, true)) {
		DX12Device::Delete_Game_Shader(handle);
	}
	return D3D_OK;
}

HRESULT DX8Wrapper::Delete_Pixel_Shader(DWORD handle)
{
	// Only free a handle of the kind this call is for, as Direct3D 8 keeps the two handle spaces apart.
	if (DX12Device::Is_Game_Shader(handle, false)) {
		DX12Device::Delete_Game_Shader(handle);
	}
	return D3D_OK;
}

// ----------------------------------------------------------------------------
//
// Set vertex buffer. A reference to previous vertex buffer is released and
// this one is assigned the current vertex buffer. The DX8 vertex buffer will
// actually be set in Apply() which is called by Draw_Indexed_Triangles().
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Set_Vertex_Buffer(const VertexBufferClass* vb, unsigned stream)
{
	render_state.vba_offset=0;
	render_state.vba_count=0;
	if (render_state.vertex_buffers[stream]) {
		render_state.vertex_buffers[stream]->Release_Engine_Ref();
	}
	REF_PTR_SET(render_state.vertex_buffers[stream],const_cast<VertexBufferClass*>(vb));
	if (vb) {
		vb->Add_Engine_Ref();
		render_state.vertex_buffer_types[stream]=vb->Type();
	}
	else {
		render_state.vertex_buffer_types[stream]=BUFFER_TYPE_INVALID;
	}
	render_state_changed|=VERTEX_BUFFER_CHANGED;
}

// ----------------------------------------------------------------------------
//
// Set index buffer. A reference to previous index buffer is released and
// this one is assigned the current index buffer. The DX8 index buffer will
// actually be set in Apply() which is called by Draw_Indexed_Triangles().
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Set_Index_Buffer(const IndexBufferClass* ib,unsigned short index_base_offset)
{
	render_state.iba_offset=0;
	if (render_state.index_buffer) {
		render_state.index_buffer->Release_Engine_Ref();
	}
	REF_PTR_SET(render_state.index_buffer,const_cast<IndexBufferClass*>(ib));
	render_state.index_base_offset=index_base_offset;
	if (ib) {
		ib->Add_Engine_Ref();
		render_state.index_buffer_type=ib->Type();
	}
	else {
		render_state.index_buffer_type=BUFFER_TYPE_INVALID;
	}
	render_state_changed|=INDEX_BUFFER_CHANGED;
}

// ----------------------------------------------------------------------------
//
// Set vertex buffer using dynamic access object.
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Set_Vertex_Buffer(const DynamicVBAccessClass& vba_)
{
	// Release all streams (only one stream allowed in the legacy pipeline)
	for (int i=1;i<MAX_VERTEX_STREAMS;++i) {
		DX8Wrapper::Set_Vertex_Buffer(nullptr, i);
	}

	if (render_state.vertex_buffers[0]) render_state.vertex_buffers[0]->Release_Engine_Ref();
	DynamicVBAccessClass& vba=const_cast<DynamicVBAccessClass&>(vba_);
	render_state.vertex_buffer_types[0]=vba.Get_Type();
	render_state.vba_offset=vba.VertexBufferOffset;
	render_state.vba_count=vba.Get_Vertex_Count();
	REF_PTR_SET(render_state.vertex_buffers[0],vba.VertexBuffer);
	render_state.vertex_buffers[0]->Add_Engine_Ref();
	render_state_changed|=VERTEX_BUFFER_CHANGED;
	render_state_changed|=INDEX_BUFFER_CHANGED;		// vba_offset changes so index buffer needs to be reset as well.
}

// ----------------------------------------------------------------------------
//
// Set index buffer using dynamic access object.
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Set_Index_Buffer(const DynamicIBAccessClass& iba_,unsigned short index_base_offset)
{
	if (render_state.index_buffer) render_state.index_buffer->Release_Engine_Ref();

	DynamicIBAccessClass& iba=const_cast<DynamicIBAccessClass&>(iba_);
	render_state.index_base_offset=index_base_offset;
	render_state.index_buffer_type=iba.Get_Type();
	render_state.iba_offset=iba.IndexBufferOffset;
	REF_PTR_SET(render_state.index_buffer,iba.IndexBuffer);
	render_state.index_buffer->Add_Engine_Ref();
	render_state_changed|=INDEX_BUFFER_CHANGED;
}

// ----------------------------------------------------------------------------
//
// Private function for the special case of rendering polygons from sorting
// index and vertex buffers.
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw_Sorting_IB_VB(
	unsigned primitive_type,
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	WWASSERT(render_state.vertex_buffer_types[0]==BUFFER_TYPE_SORTING || render_state.vertex_buffer_types[0]==BUFFER_TYPE_DYNAMIC_SORTING);
	WWASSERT(render_state.index_buffer_type==BUFFER_TYPE_SORTING || render_state.index_buffer_type==BUFFER_TYPE_DYNAMIC_SORTING);

	// Fill dynamic vertex buffer with sorting vertex buffer vertices
	DynamicVBAccessClass dyn_vb_access(BUFFER_TYPE_DYNAMIC_DX8,dynamic_fvf_type,vertex_count);
	{
		DynamicVBAccessClass::WriteLockClass lock(&dyn_vb_access);
		VertexFormatXYZNDUV2* src = static_cast<SortingVertexBufferClass*>(render_state.vertex_buffers[0])->VertexBuffer;
		VertexFormatXYZNDUV2* dest= lock.Get_Formatted_Vertex_Array();
		src += render_state.vba_offset + render_state.index_base_offset + min_vertex_index;
		unsigned  size = dyn_vb_access.FVF_Info().Get_FVF_Size()*vertex_count/sizeof(unsigned);
		unsigned *dest_u =(unsigned*) dest;
		unsigned *src_u = (unsigned*) src;

		for (unsigned i=0;i<size;++i) {
			*dest_u++=*src_u++;
		}
	}

	Device_Set_Stream(
		static_cast<DX8VertexBufferClass*>(dyn_vb_access.VertexBuffer)->Get_DX12_Buffer(),
		dyn_vb_access.FVF_Info().Get_FVF_Size());
	// If using FVF format VB, set the FVF as vertex shader (may not be needed here KM)
	unsigned fvf=dyn_vb_access.FVF_Info().Get_FVF();
	if (fvf!=0) {
		Device_Set_Vertex_Shader(fvf);
	}
	DX8_RECORD_VERTEX_BUFFER_CHANGE();

	unsigned index_count=0;
	switch (primitive_type) {
	case D3DPT_TRIANGLELIST: index_count=polygon_count*3; break;
	case D3DPT_TRIANGLESTRIP: index_count=polygon_count+2; break;
	case D3DPT_TRIANGLEFAN: index_count=polygon_count+2; break;
	default: WWASSERT(0); break; // Unsupported primitive type
	}

	// Fill dynamic index buffer with sorting index buffer vertices
	DynamicIBAccessClass dyn_ib_access(BUFFER_TYPE_DYNAMIC_DX8,index_count);
	{
		DynamicIBAccessClass::WriteLockClass lock(&dyn_ib_access);
		unsigned short* dest=lock.Get_Index_Array();
		unsigned short* src=nullptr;
		src=static_cast<SortingIndexBufferClass*>(render_state.index_buffer)->index_buffer;
		src+=render_state.iba_offset+start_index;

		for (unsigned short i=0;i<index_count;++i) {
			unsigned short index=*src++;
			index-=min_vertex_index;
			WWASSERT(index<vertex_count);
			*dest++=index;
		}
	}

	Device_Set_Indices(
		static_cast<DX8IndexBufferClass*>(dyn_ib_access.IndexBuffer)->Get_DX12_Buffer(),
		dyn_vb_access.VertexBufferOffset);
	DX8_RECORD_INDEX_BUFFER_CHANGE();

	DX8_RECORD_DRAW_CALLS();
	Draw_DX12(
		D3DPT_TRIANGLELIST,
		dyn_ib_access.IndexBufferOffset,
		polygon_count,
		true);

	DX8_RECORD_RENDER(polygon_count,vertex_count,render_state.shader);
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw(
	unsigned primitive_type,
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	if (DrawPolygonLowBoundLimit && DrawPolygonLowBoundLimit>=polygon_count) return;

	DX8_THREAD_ASSERT();
	SNAPSHOT_SAY(("DX8 - draw"));

	Apply_Render_State_Changes();

	// Debug feature to disable triangle drawing...
	if (!_Is_Triangle_Draw_Enabled()) return;


	if (vertex_count<3) {
		min_vertex_index=0;
		switch (render_state.vertex_buffer_types[0]) {
		case BUFFER_TYPE_DX8:
		case BUFFER_TYPE_SORTING:
			vertex_count=render_state.vertex_buffers[0]->Get_Vertex_Count()-render_state.index_base_offset-render_state.vba_offset-min_vertex_index;
			break;
		case BUFFER_TYPE_DYNAMIC_DX8:
		case BUFFER_TYPE_DYNAMIC_SORTING:
			vertex_count=render_state.vba_count;
			break;
		}
	}

	switch (render_state.vertex_buffer_types[0]) {
	case BUFFER_TYPE_DX8:
	case BUFFER_TYPE_DYNAMIC_DX8:
		switch (render_state.index_buffer_type) {
		case BUFFER_TYPE_DX8:
		case BUFFER_TYPE_DYNAMIC_DX8:
			{
/*				if ((start_index+render_state.iba_offset+polygon_count*3) > render_state.index_buffer->Get_Index_Count())
				{	WWASSERT_PRINT(0,"OVERFLOWING INDEX BUFFER");
					///@todo: MUST FIND OUT WHY THIS HAPPENS WITH LOTS OF PARTICLES ON BIG FIGHT!  -MW
					break;
				}*/
				DX8_RECORD_RENDER(polygon_count,vertex_count,render_state.shader);
				DX8_RECORD_DRAW_CALLS();
				Draw_DX12(
					primitive_type,
					start_index+render_state.iba_offset,
					polygon_count,
					true);
			}
			break;
		case BUFFER_TYPE_SORTING:
		case BUFFER_TYPE_DYNAMIC_SORTING:
			WWASSERT_PRINT(0,"VB and IB must of same type (sorting or dx8)");
			break;
		case BUFFER_TYPE_INVALID:
			WWASSERT(0);
			break;
		}
		break;
	case BUFFER_TYPE_SORTING:
	case BUFFER_TYPE_DYNAMIC_SORTING:
		switch (render_state.index_buffer_type) {
		case BUFFER_TYPE_DX8:
		case BUFFER_TYPE_DYNAMIC_DX8:
			WWASSERT_PRINT(0,"VB and IB must of same type (sorting or dx8)");
			break;
		case BUFFER_TYPE_SORTING:
		case BUFFER_TYPE_DYNAMIC_SORTING:
			Draw_Sorting_IB_VB(primitive_type,start_index,polygon_count,min_vertex_index,vertex_count);
			break;
		case BUFFER_TYPE_INVALID:
			WWASSERT(0);
			break;
		}
		break;
	case BUFFER_TYPE_INVALID:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw_Triangles(
	unsigned buffer_type,
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	if (buffer_type==BUFFER_TYPE_SORTING || buffer_type==BUFFER_TYPE_DYNAMIC_SORTING) {
		SortingRendererClass::Insert_Triangles(start_index,polygon_count,min_vertex_index,vertex_count);
	}
	else {
		Draw(D3DPT_TRIANGLELIST,start_index,polygon_count,min_vertex_index,vertex_count);
	}
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw_Triangles(
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	Draw(D3DPT_TRIANGLELIST,start_index,polygon_count,min_vertex_index,vertex_count);
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw_Strip(
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	Draw(D3DPT_TRIANGLESTRIP,start_index,polygon_count,min_vertex_index,vertex_count);
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
//
// Direct3D 12 draws. Everything comes from DX12Device_State, the mirror of what
// the Direct3D 8 device would hold, plus DX8Transforms, DX8Material, DX8Lights with
// CurrentDX8LightEnables and the viewport DX12Device already has. The wrapper
// caches are never read here.
//
// ----------------------------------------------------------------------------

static_assert(DX12_MAX_STAGES<=MAX_TEXTURE_STAGES, "the Direct3D 12 stages come from the Direct3D 8 state mirror");
static_assert(DX12_MAX_VS_CONSTANTS==MAX_VERTEX_SHADER_CONSTANTS && DX12_MAX_PS_CONSTANTS==MAX_PIXEL_SHADER_CONSTANTS,
	"the shader constant registers come from the Direct3D 8 state mirror");

// Vertices (or indices) a Direct3D 8 draw of primitive_count primitives reads. 0 for unknown types.
static unsigned DX12_Primitive_Vertex_Count(unsigned primitive_type, unsigned primitive_count)
{
	switch (primitive_type) {
	case D3DPT_POINTLIST: return primitive_count;
	case D3DPT_LINELIST: return primitive_count*2;
	case D3DPT_LINESTRIP: return primitive_count+1;
	case D3DPT_TRIANGLELIST: return primitive_count*3;
	case D3DPT_TRIANGLESTRIP: return primitive_count+2;
	case D3DPT_TRIANGLEFAN: return primitive_count+2;
	default: return 0;
	}
}

// Writes a triangle fan as a triangle list into per-frame memory. src holds the fan's indices, or
// is null for the vertices 0..primitive_count+1. Returns the list's GPU address, 0 on failure.
static unsigned long long DX12_Fan_To_List(const unsigned short* src, unsigned primitive_count)
{
	void* cpu=nullptr;
	unsigned long long gpu=0;
	if (!DX12Device::Allocate_Frame_Memory(primitive_count*3*sizeof(unsigned short),&cpu,&gpu)) return 0;

	unsigned short* dest=(unsigned short*)cpu;
	for (unsigned i=1;i<=primitive_count;++i) {
		*dest++=src ? src[0] : 0;
		*dest++=src ? src[i] : (unsigned short)i;
		*dest++=src ? src[i+1] : (unsigned short)(i+1);
	}
	return gpu;
}

bool DX8Wrapper::Fill_DX12_Draw_State(DX12DrawDesc& desc)
{
	DX12StatsScope stats_scope(DX12Device_Counters.DrawFillTicks);
	const DX12DeviceStateStruct& dev=DX12Device_State;

	// Programmable shaders are the game shaders of Create_DX12_Shader; other handles cannot be drawn.
	if (dev.PixelShader!=0 && !DX12Device::Is_Game_Shader(dev.PixelShader,false)) {
		static bool logged=false;
		if (!logged) {
			logged=true;
			WWDEBUG_SAY(("DX12: skipping draws with a pixel shader that is not a game shader"));
		}
		return false;
	}
	const bool game_vertex_shader=dev.FVF==0 && DX12Device::Is_Game_Shader(dev.VertexShader,true);
	if (dev.FVF==0 && !game_vertex_shader) {
		static bool logged=false;
		if (!logged) {
			logged=true;
			WWDEBUG_SAY(("DX12: skipping draws without an FVF vertex format or a game vertex shader"));
		}
		return false;
	}

	const unsigned* rs=dev.RenderStates;

	desc.FVF=dev.FVF;
	desc.VertexShader=game_vertex_shader ? dev.VertexShader : 0;
	desc.PixelShader=dev.PixelShader;
	desc.VertexShaderConstants=&dev.VertexShaderConstants[0][0];
	desc.PixelShaderConstants=&dev.PixelShaderConstants[0][0];

	desc.World=&DX8Transforms[D3DTS_WORLD].m[0][0];
	desc.View=&DX8Transforms[D3DTS_VIEW].m[0][0];
	desc.Projection=&DX8Transforms[D3DTS_PROJECTION].m[0][0];

	desc.AlphaBlendEnable=rs[D3DRS_ALPHABLENDENABLE]!=0;
	desc.SrcBlend=rs[D3DRS_SRCBLEND];
	desc.DestBlend=rs[D3DRS_DESTBLEND];
	desc.BlendOp=rs[D3DRS_BLENDOP];
	desc.ColorWriteMask=rs[D3DRS_COLORWRITEENABLE]&0xf;
	desc.ZEnable=rs[D3DRS_ZENABLE]!=D3DZB_FALSE;
	desc.ZWriteEnable=rs[D3DRS_ZWRITEENABLE]!=0;
	desc.ZFunc=rs[D3DRS_ZFUNC];
	desc.CullMode=rs[D3DRS_CULLMODE];
	desc.FillMode=rs[D3DRS_FILLMODE];
	desc.DepthBias=(int)rs[D3DRS_ZBIAS];
	desc.AlphaTestEnable=rs[D3DRS_ALPHATESTENABLE]!=0;
	desc.AlphaFunc=rs[D3DRS_ALPHAFUNC];
	desc.AlphaRef=rs[D3DRS_ALPHAREF]&0xff;
	desc.SpecularEnable=rs[D3DRS_SPECULARENABLE]!=0;
	desc.TextureFactor=rs[D3DRS_TEXTUREFACTOR];

	// The device derives the fixed-function pipeline from the full state mirror.
	desc.RenderStates=dev.RenderStates;
	desc.TextureStageStates=&dev.TextureStageStates[0][0];

	const D3DMATERIAL8& mat=DX8Material;
	desc.Material.Diffuse[0]=mat.Diffuse.r;
	desc.Material.Diffuse[1]=mat.Diffuse.g;
	desc.Material.Diffuse[2]=mat.Diffuse.b;
	desc.Material.Diffuse[3]=mat.Diffuse.a;
	desc.Material.Ambient[0]=mat.Ambient.r;
	desc.Material.Ambient[1]=mat.Ambient.g;
	desc.Material.Ambient[2]=mat.Ambient.b;
	desc.Material.Ambient[3]=mat.Ambient.a;
	desc.Material.Specular[0]=mat.Specular.r;
	desc.Material.Specular[1]=mat.Specular.g;
	desc.Material.Specular[2]=mat.Specular.b;
	desc.Material.Specular[3]=mat.Specular.a;
	desc.Material.Emissive[0]=mat.Emissive.r;
	desc.Material.Emissive[1]=mat.Emissive.g;
	desc.Material.Emissive[2]=mat.Emissive.b;
	desc.Material.Emissive[3]=mat.Emissive.a;
	desc.Material.Power=mat.Power;

	for (unsigned index=0;index<4;++index) {
		const D3DLIGHT8& light=DX8Lights[index];
		desc.Lights[index].Enabled=CurrentDX8LightEnables[index];
		desc.Lights[index].Type=(unsigned)light.Type;
		desc.Lights[index].Diffuse[0]=light.Diffuse.r;
		desc.Lights[index].Diffuse[1]=light.Diffuse.g;
		desc.Lights[index].Diffuse[2]=light.Diffuse.b;
		desc.Lights[index].Diffuse[3]=light.Diffuse.a;
		desc.Lights[index].Specular[0]=light.Specular.r;
		desc.Lights[index].Specular[1]=light.Specular.g;
		desc.Lights[index].Specular[2]=light.Specular.b;
		desc.Lights[index].Specular[3]=light.Specular.a;
		desc.Lights[index].Ambient[0]=light.Ambient.r;
		desc.Lights[index].Ambient[1]=light.Ambient.g;
		desc.Lights[index].Ambient[2]=light.Ambient.b;
		desc.Lights[index].Ambient[3]=light.Ambient.a;
		desc.Lights[index].Position[0]=light.Position.x;
		desc.Lights[index].Position[1]=light.Position.y;
		desc.Lights[index].Position[2]=light.Position.z;
		desc.Lights[index].Direction[0]=light.Direction.x;
		desc.Lights[index].Direction[1]=light.Direction.y;
		desc.Lights[index].Direction[2]=light.Direction.z;
		desc.Lights[index].Range=light.Range;
		desc.Lights[index].Falloff=light.Falloff;
		desc.Lights[index].Attenuation0=light.Attenuation0;
		desc.Lights[index].Attenuation1=light.Attenuation1;
		desc.Lights[index].Attenuation2=light.Attenuation2;
		desc.Lights[index].Theta=light.Theta;
		desc.Lights[index].Phi=light.Phi;
	}

	// MaterialDiffuse is the M1 approximation of lighting, kept for the existing field: the material
	// diffuse when lit, white when unlit.
	if (rs[D3DRS_LIGHTING]) {
		desc.MaterialDiffuse[0]=DX8Material.Diffuse.r;
		desc.MaterialDiffuse[1]=DX8Material.Diffuse.g;
		desc.MaterialDiffuse[2]=DX8Material.Diffuse.b;
		desc.MaterialDiffuse[3]=DX8Material.Diffuse.a;
	}
	else {
		desc.MaterialDiffuse[0]=desc.MaterialDiffuse[1]=desc.MaterialDiffuse[2]=desc.MaterialDiffuse[3]=1.0f;
	}

	for (unsigned stage=0;stage<DX12_MAX_STAGES;++stage) {
		const unsigned* tss=dev.TextureStageStates[stage];
		desc.ColorOp[stage]=tss[D3DTSS_COLOROP];
		desc.ColorArg0[stage]=tss[D3DTSS_COLORARG0];
		desc.ColorArg1[stage]=tss[D3DTSS_COLORARG1];
		desc.ColorArg2[stage]=tss[D3DTSS_COLORARG2];
		desc.AlphaOp[stage]=tss[D3DTSS_ALPHAOP];
		desc.AlphaArg0[stage]=tss[D3DTSS_ALPHAARG0];
		desc.AlphaArg1[stage]=tss[D3DTSS_ALPHAARG1];
		desc.AlphaArg2[stage]=tss[D3DTSS_ALPHAARG2];
		desc.TexCoordIndex[stage]=tss[D3DTSS_TEXCOORDINDEX];
		desc.MinFilter[stage]=tss[D3DTSS_MINFILTER];
		desc.MagFilter[stage]=tss[D3DTSS_MAGFILTER];
		desc.MipFilter[stage]=tss[D3DTSS_MIPFILTER];
		desc.AddressU[stage]=tss[D3DTSS_ADDRESSU];
		desc.AddressV[stage]=tss[D3DTSS_ADDRESSV];
		desc.MaxAnisotropy[stage]=tss[D3DTSS_MAXANISOTROPY];
		desc.Textures[stage]=dev.Textures[stage];
		desc.TextureMatrices[stage]=&DX8Transforms[D3DTS_TEXTURE0+stage].m[0][0];
	}

	return true;
}

void DX8Wrapper::Draw_DX12(unsigned primitive_type, unsigned start, unsigned primitive_count, bool indexed)
{
	const DX12DeviceStateStruct& dev=DX12Device_State;

	const unsigned count=DX12_Primitive_Vertex_Count(primitive_type,primitive_count);
	if (count==0) {
		WWASSERT(primitive_count==0);
		return;
	}
	// Like Direct3D 8, a draw without the buffers it reads does nothing.
	if (dev.StreamBuffer==nullptr || dev.StreamStride==0) return;
	if (indexed && dev.IndexBuffer==nullptr) return;

	DX12DrawDesc desc;
	memset(&desc,0,sizeof(desc));
	if (!Fill_DX12_Draw_State(desc)) return;

	desc.VertexAddress=dev.StreamBuffer->Get_GPU_Address();
	desc.VertexBufferSize=dev.StreamBuffer->Get_Size();
	desc.VertexStride=dev.StreamStride;

	if (primitive_type==D3DPT_TRIANGLEFAN) {
		// Direct3D 12 has no fans; draw the equivalent triangle list.
		unsigned long long list=0;
		if (indexed) {
			const unsigned char* indices=dev.IndexBuffer->Peek_CPU_Data();
			if (indices==nullptr || (unsigned long long)(start+count)*sizeof(unsigned short)>dev.IndexBuffer->Get_Size()) {
				static bool logged=false;
				if (!logged) {
					logged=true;
					WWDEBUG_SAY(("DX12: skipping an indexed triangle fan outside its index buffer"));
				}
				return;
			}
			list=DX12_Fan_To_List((const unsigned short*)indices+start,primitive_count);
			desc.BaseVertex=(int)dev.IndexBaseVertex;
		}
		else {
			if (count>0x10000) return;
			list=DX12_Fan_To_List(nullptr,primitive_count);
			desc.BaseVertex=(int)start;
		}
		if (list==0) return;
		desc.PrimitiveType=D3DPT_TRIANGLELIST;
		desc.IndexCount=primitive_count*3;
		desc.StartIndex=0;
		desc.IndexAddress=list;
		desc.IndexBufferSize=primitive_count*3*sizeof(unsigned short);
	}
	else if (indexed) {
		desc.PrimitiveType=primitive_type;
		desc.IndexCount=count;
		desc.StartIndex=start;
		desc.BaseVertex=(int)dev.IndexBaseVertex;
		desc.IndexAddress=dev.IndexBuffer->Get_GPU_Address();
		desc.IndexBufferSize=dev.IndexBuffer->Get_Size();
	}
	else {
		desc.PrimitiveType=primitive_type;
		desc.VertexCount=count;
		desc.StartVertex=start;
	}

	DX12Device::Draw(desc);
}

void DX8Wrapper::Draw_Primitive_UP_DX12(unsigned primitive_type, unsigned primitive_count, const void* vertex_data, unsigned vertex_stride)
{
	const unsigned count=DX12_Primitive_Vertex_Count(primitive_type,primitive_count);
	WWASSERT(count!=0 || primitive_count==0);

	DX12DrawDesc desc;
	memset(&desc,0,sizeof(desc));
	if (count!=0 && vertex_data!=nullptr && vertex_stride!=0 && Fill_DX12_Draw_State(desc)) {
		const unsigned size=count*vertex_stride;
		void* cpu=nullptr;
		unsigned long long gpu=0;
		if (DX12Device::Allocate_Frame_Memory(size,&cpu,&gpu)) {
			memcpy(cpu,vertex_data,size);
			desc.VertexAddress=gpu;
			desc.VertexBufferSize=size;
			desc.VertexStride=vertex_stride;

			bool draw=true;
			if (primitive_type==D3DPT_TRIANGLEFAN) {
				const unsigned long long list=(count<=0x10000) ? DX12_Fan_To_List(nullptr,primitive_count) : 0;
				desc.PrimitiveType=D3DPT_TRIANGLELIST;
				desc.IndexCount=primitive_count*3;
				desc.IndexAddress=list;
				desc.IndexBufferSize=primitive_count*3*sizeof(unsigned short);
				draw=list!=0;
			}
			else {
				desc.PrimitiveType=primitive_type;
				desc.VertexCount=count;
			}
			if (draw) {
				DX12Device::Draw(desc);
			}
		}
	}

	// Direct3D 8 leaves stream 0 unbound after DrawPrimitiveUP; the indices stay bound
	// (only DrawIndexedPrimitiveUP also resets them).
	Device_Set_Stream(nullptr,0);
}

void DX8Wrapper::Apply_Render_State_Changes()
{
	SNAPSHOT_SAY(("DX8Wrapper::Apply_Render_State_Changes()"));

	if (!render_state_changed) return;
	if (render_state_changed&SHADER_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply shader"));
		render_state.shader.Apply();
	}

	unsigned mask=TEXTURE0_CHANGED;
	int i=0;
	for (;i<CurrentCaps->Get_Max_Textures_Per_Pass();++i,mask<<=1)
	{
		if (render_state_changed&mask)
		{
			SNAPSHOT_SAY(("DX8 - apply texture %d (%s)",i,render_state.Textures[i] ? render_state.Textures[i]->Get_Full_Path().str() : "null"));

			if (render_state.Textures[i])
			{
				render_state.Textures[i]->Apply(i);
			}
			else
			{
				TextureBaseClass::Apply_Null(i);
			}
		}
	}

	if (render_state_changed&MATERIAL_CHANGED)
	{
		SNAPSHOT_SAY(("DX8 - apply material"));
		VertexMaterialClass* material=const_cast<VertexMaterialClass*>(render_state.material);
		if (material)
		{
			material->Apply();
		}
		else VertexMaterialClass::Apply_Null();
	}

	if (render_state_changed&LIGHTS_CHANGED)
	{
		unsigned mask=LIGHT0_CHANGED;
		for (unsigned index=0;index<4;++index,mask<<=1) {
			if (render_state_changed&mask) {
				SNAPSHOT_SAY(("DX8 - apply light %d",index));
				if (render_state.LightEnable[index]) {
#if defined(DEBUG_CRASHING) || defined(DEBUG_LOGGING)
					if ( WW3D::Is_Snapshot_Activated() ) {
						D3DLIGHT8 * light = &(render_state.Lights[index]);
						static const char * _light_types[] = { "Unknown", "Point","Spot", "Directional" };
						WWASSERT((light->Type >= 0) && (light->Type <= 3));

						SNAPSHOT_SAY((" type = %s amb = %4.2f,%4.2f,%4.2f  diff = %4.2f,%4.2f,%4.2f spec = %4.2f, %4.2f, %4.2f",
							_light_types[light->Type],
							light->Ambient.r,light->Ambient.g,light->Ambient.b,
							light->Diffuse.r,light->Diffuse.g,light->Diffuse.b,
							light->Specular.r,light->Specular.g,light->Specular.b ));
						SNAPSHOT_SAY((" pos = %f, %f, %f  dir = %f, %f, %f",
							light->Position.x, light->Position.y, light->Position.z,
							light->Direction.x, light->Direction.y, light->Direction.z ));
					}
#endif

					Set_DX8_Light(index,&render_state.Lights[index]);
				}
				else {
					Set_DX8_Light(index,nullptr);
					SNAPSHOT_SAY((" clearing light to null"));
				}
			}
		}
	}

	if (render_state_changed&WORLD_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply world matrix"));
		_Set_DX8_Transform(D3DTS_WORLD,render_state.world);
	}
	if (render_state_changed&VIEW_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply view matrix"));
		_Set_DX8_Transform(D3DTS_VIEW,render_state.view);
	}
	if (render_state_changed&VERTEX_BUFFER_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply vb change"));
		for (i=0;i<MAX_VERTEX_STREAMS;++i) {
			if (render_state.vertex_buffers[i]) {
				switch (render_state.vertex_buffer_types[i]) {
					//->Type()) {
				case BUFFER_TYPE_DX8:
				case BUFFER_TYPE_DYNAMIC_DX8:
					if (i==0) {
						Device_Set_Stream(
							static_cast<DX8VertexBufferClass*>(render_state.vertex_buffers[i])->Get_DX12_Buffer(),
							render_state.vertex_buffers[i]->FVF_Info().Get_FVF_Size());
					}
					DX8_RECORD_VERTEX_BUFFER_CHANGE();
					{
						// If the VB format is FVF, set the FVF as a vertex shader
						unsigned fvf=render_state.vertex_buffers[i]->FVF_Info().Get_FVF();
						if (fvf!=0) {
							Set_Vertex_Shader(fvf);
						}
					}
					break;
				case BUFFER_TYPE_SORTING:
				case BUFFER_TYPE_DYNAMIC_SORTING:
					break;
				default:
					WWASSERT(0);
				}
			} else {
				if (i==0) Device_Set_Stream(nullptr,0);
				DX8_RECORD_VERTEX_BUFFER_CHANGE();
			}
		}
	}
	if (render_state_changed&INDEX_BUFFER_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply ib change"));
		if (render_state.index_buffer) {
			switch (render_state.index_buffer_type) {
				//->Type()) {
			case BUFFER_TYPE_DX8:
			case BUFFER_TYPE_DYNAMIC_DX8:
				Device_Set_Indices(
					static_cast<DX8IndexBufferClass*>(render_state.index_buffer)->Get_DX12_Buffer(),
					render_state.index_base_offset+render_state.vba_offset);
				DX8_RECORD_INDEX_BUFFER_CHANGE();
				break;
			case BUFFER_TYPE_SORTING:
			case BUFFER_TYPE_DYNAMIC_SORTING:
				break;
			default:
				WWASSERT(0);
			}
		}
		else {
			Device_Set_Indices(nullptr,0);
			DX8_RECORD_INDEX_BUFFER_CHANGE();
		}
	}

	render_state_changed&=((unsigned)WORLD_IDENTITY|(unsigned)VIEW_IDENTITY);

	SNAPSHOT_SAY(("DX8Wrapper::Apply_Render_State_Changes() - finished"));
}

void DX8Wrapper::Compute_Caps(WW3DFormat display_format)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	delete CurrentCaps;
	CurrentCaps=new DX8Caps(Get_Current_Adapter_Identifier());
}


void DX8Wrapper::Set_Light(unsigned index, const D3DLIGHT8* light)
{
	if (light) {
		render_state.Lights[index]=*light;
		render_state.LightEnable[index]=true;
	}
	else {
		render_state.LightEnable[index]=false;
	}
	render_state_changed|=(LIGHT0_CHANGED<<index);
}

void DX8Wrapper::Set_Light(unsigned index,const LightClass &light)
{
	D3DLIGHT8 dlight;
	Vector3 temp;
	memset(&dlight,0,sizeof(D3DLIGHT8));

	switch (light.Get_Type())
	{
	case LightClass::POINT:
		{
			dlight.Type=D3DLIGHT_POINT;
		}
		break;
	case LightClass::DIRECTIONAL:
		{
			dlight.Type=D3DLIGHT_DIRECTIONAL;
		}
		break;
	case LightClass::SPOT:
		{
			dlight.Type=D3DLIGHT_SPOT;
		}
		break;
	}

	light.Get_Diffuse(&temp);
	temp*=light.Get_Intensity();
	dlight.Diffuse.r=temp.X;
	dlight.Diffuse.g=temp.Y;
	dlight.Diffuse.b=temp.Z;
	dlight.Diffuse.a=1.0f;

	light.Get_Specular(&temp);
	temp*=light.Get_Intensity();
	dlight.Specular.r=temp.X;
	dlight.Specular.g=temp.Y;
	dlight.Specular.b=temp.Z;
	dlight.Specular.a=1.0f;

	light.Get_Ambient(&temp);
	temp*=light.Get_Intensity();
	dlight.Ambient.r=temp.X;
	dlight.Ambient.g=temp.Y;
	dlight.Ambient.b=temp.Z;
	dlight.Ambient.a=1.0f;

	temp=light.Get_Position();
	dlight.Position=*(D3DVECTOR*) &temp;

	light.Get_Spot_Direction(temp);
	dlight.Direction=*(D3DVECTOR*) &temp;

	dlight.Range=light.Get_Attenuation_Range();
	dlight.Falloff=light.Get_Spot_Exponent();
	dlight.Theta=light.Get_Spot_Angle();
	dlight.Phi=light.Get_Spot_Angle();

	// Inverse linear light 1/(1+D)
	double a,b;
	light.Get_Far_Attenuation_Range(a,b);
	dlight.Attenuation0=1.0f;
	if (fabs(a-b)<1e-5)
		// if the attenuation range is too small assume uniform with cutoff
		dlight.Attenuation1=0.0f;
	else
		// this will cause the light to drop to half intensity at the first far attenuation
		dlight.Attenuation1=(float) 1.0/a;
	dlight.Attenuation2=0.0f;

	Set_Light(index,&dlight);
}

//**********************************************************************************************
//! Set the light environment. This is a lighting model which used up to four
//! directional lights to produce the lighting.
/*! 5/27/02 KJM Added shader light environment support
*/
void DX8Wrapper::Set_Light_Environment(LightEnvironmentClass* light_env)
{
	// Shader light environment support															*
	Light_Environment=light_env;

	if (light_env)
	{
		int light_count = light_env->Get_Light_Count();
		unsigned int color=Convert_Color(light_env->Get_Equivalent_Ambient(),0.0f);
		if (RenderStates[D3DRS_AMBIENT]!=color)
		{
			Set_DX8_Render_State(D3DRS_AMBIENT,color);
//buggy Radeon 9700 driver doesn't apply new ambient unless the material also changes.
#if 1
			render_state_changed|=MATERIAL_CHANGED;
#endif
		}

		D3DLIGHT8 light;
		int l=0;
		for (;l<light_count;++l) {

			::ZeroMemory(&light, sizeof(D3DLIGHT8));

			light.Type=D3DLIGHT_DIRECTIONAL;
			(Vector3&)light.Diffuse=light_env->Get_Light_Diffuse(l);
			Vector3 dir=-light_env->Get_Light_Direction(l);
			light.Direction=(const D3DVECTOR&)(dir);

			// (gth) TODO: put specular into LightEnvironment?  Much work to be done on lights :-)'
			if (l==0) {
				light.Specular.r = light.Specular.g = light.Specular.b = 1.0f;
			}

			if (light_env->isPointLight(l)) {
				light.Type = D3DLIGHT_POINT;
				(Vector3&)light.Diffuse=light_env->getPointDiffuse(l);
				(Vector3&)light.Ambient=light_env->getPointAmbient(l);
				light.Position = (const D3DVECTOR&)light_env->getPointCenter(l);
				light.Range = light_env->getPointOrad(l);

				// Inverse linear light 1/(1+D)
				double a,b;
				b = light_env->getPointOrad(l);
				a = light_env->getPointIrad(l);

//(gth) CNC3 Generals code for the attenuation factors is causing the lights to over-brighten
//I'm changing the Attenuation0 parameter to 1.0 to avoid this problem.
#if 0
				light.Attenuation0=0.01f;
#else
				light.Attenuation0=1.0f;
#endif
				if (fabs(a-b)<1e-5)
					// if the attenuation range is too small assume uniform with cutoff
					light.Attenuation1=0.0f;
				else
					// this will cause the light to drop to half intensity at the first far attenuation
					light.Attenuation1=(float) 0.1/a;

				light.Attenuation2=8.0f/(b*b);
			}

			Set_Light(l,&light);
		}

		for (;l<4;++l) {
			Set_Light(l,nullptr);
		}
	}
/*	else {
		for (int l=0;l<4;++l) {
			Set_Light(l,nullptr);
		}
	}
*/
}

bool DX8Wrapper::Copy_Back_Buffer_To_Texture(TextureClass* dest, const RECT* src_rect, int dest_x, int dest_y)
{
	// Direct3D 8 callers keep their CopyRects code.
	DX8_THREAD_ASSERT();

	if (dest==nullptr || dest->Peek_DX12_Texture()==nullptr) return false;
	return DX12Device::Copy_Target_To_Texture(dest->Peek_DX12_Texture(), src_rect, dest_x, dest_y);
}

SurfaceClass * DX8Wrapper::Read_Back_Buffer()
{
	DX8_THREAD_ASSERT();

	DX12TextureClass * texture=DX12Device::Read_Back_Buffer();
	if (texture==nullptr) {
		WWDEBUG_SAY(("DX8Wrapper::Read_Back_Buffer - the back buffer could not be read"));
		return nullptr;
	}
	SurfaceClass * surface=NEW_REF(SurfaceClass,(texture,0));
	texture->Release_Ref();
	return surface;
}

// Direct3D 12 version of Create_Render_Target: the same square power of two sizes as Direct3D 8 mode.
TextureClass * DX8Wrapper::Create_DX12_Render_Target(int width, int height, WW3DFormat format)
{
	DX8_THREAD_ASSERT();

	// There is no display format; Direct3D 8 mode gets X8R8G8B8 from a 32 bit desktop.
	if (format==WW3D_FORMAT_UNKNOWN) {
		format=WW3D_FORMAT_X8R8G8B8;
	}

	if (!Get_Current_Caps()->Support_Render_To_Texture_Format(format)) {
		WWDEBUG_SAY(("DX8Wrapper - Render target format is not supported"));
		return nullptr;
	}

	const D3DCAPS8& dx8caps=Get_Current_Caps()->Get_DX8_Caps();
	float poweroftwosize = width;
	if (height > 0 && height < width) {
		poweroftwosize = height;
	}
	poweroftwosize = ::Find_POT (poweroftwosize);

	if (poweroftwosize>dx8caps.MaxTextureWidth) {
		poweroftwosize=dx8caps.MaxTextureWidth;
	}
	if (poweroftwosize>dx8caps.MaxTextureHeight) {
		poweroftwosize=dx8caps.MaxTextureHeight;
	}

	width = height = poweroftwosize;

	TextureClass * tex = NEW_REF(TextureClass,(width,height,format,MIP_LEVELS_1,TextureClass::POOL_DEFAULT,true));
	if (tex->Peek_DX12_Texture() == nullptr)
	{
		WWDEBUG_SAY(("DX8Wrapper - Render target creation failed!"));
		REF_PTR_RELEASE(tex);
	}

	return tex;
}

// Direct3D 12 version of the Set_Render_Target functions. A null texture selects the back buffer and
// the default depth buffer. Without ztexture, a texture uses the default depth buffer if it fits.
void DX8Wrapper::Set_DX12_Render_Target(TextureClass * texture, ZTextureClass * ztexture)
{
	DX8_THREAD_ASSERT();

	if (texture==nullptr && !DX12Device::Is_Render_To_Texture()) {
		// Already on the back buffer; like Direct3D 8 mode, keep the viewport.
		IsRenderToTexture=false;
		return;
	}

	DX12TextureClass * color=texture ? texture->Peek_DX12_Texture() : nullptr;
	DX12TextureClass * depth=(texture && ztexture) ? ztexture->Peek_DX12_Texture() : nullptr;
	if (texture && (color==nullptr || !color->Is_Render_Target())) {
		WWDEBUG_SAY(("DX8Wrapper::Set_Render_Target - %s is not a render target", texture->Get_Texture_Name().str()));
	}

	// This also resets the device viewport to the whole target.
	DX12Device::Set_Render_Target(color, depth, ztexture==nullptr);
	IsRenderToTexture=DX12Device::Is_Render_To_Texture();

	DX12Viewport.X=0;
	DX12Viewport.Y=0;
	DX12Viewport.Width=DX12Device::Get_Target_Width();
	DX12Viewport.Height=DX12Device::Get_Target_Height();
	DX12Viewport.MinZ=0.0f;
	DX12Viewport.MaxZ=1.0f;
}

// Like Set_Render_Target_Raw in Direct3D 8 mode, this keeps IsRenderToTexture, so effects that skip
// render to texture passes (e.g. dazzles) still draw into the screen filter target.
bool DX8Wrapper::Set_DX12_Render_Target_Raw(TextureClass * texture)
{

	const bool was_render_to_texture=IsRenderToTexture;
	Set_DX12_Render_Target(texture, nullptr);
	IsRenderToTexture=was_render_to_texture;
	return DX12Device::Is_Render_To_Texture()==(texture!=nullptr);
}

TextureClass *
DX8Wrapper::Create_Render_Target (int width, int height, WW3DFormat format)
{
	return Create_DX12_Render_Target(width, height, format);
}

//**********************************************************************************************
//! Create render target with associated depth stencil buffer
/*! KJM
*/
void DX8Wrapper::Create_Render_Target
(
	int width,
	int height,
	WW3DFormat format,
	WW3DZFormat zformat,
	TextureClass** target,
	ZTextureClass** depth_buffer
)
{
	*target=nullptr;
	*depth_buffer=nullptr;

	// As in Direct3D 8 mode, there is no display format to fall back on here.
	if (format==WW3D_FORMAT_UNKNOWN || !Get_Current_Caps()->Support_Depth_Stencil_Format(zformat)) {
		WWDEBUG_SAY(("DX8Wrapper - Render target with depth format is not supported"));
		return;
	}

	*target=Create_DX12_Render_Target(width,height,format);
	if (*target!=nullptr) {
		const int size=(*target)->Get_Width();
		*depth_buffer=NEW_REF(ZTextureClass,(size,size,zformat,MIP_LEVELS_1,TextureClass::POOL_DEFAULT));
		if ((*depth_buffer)->Peek_DX12_Texture()==nullptr) {
			REF_PTR_RELEASE((*depth_buffer));
		}
	}
}

/*!
 * Set render target
 * KM Added optional custom z target
 */
void DX8Wrapper::Set_Render_Target_With_Z
(
	TextureClass* texture,
	ZTextureClass* ztexture
)
{
	Set_DX12_Render_Target(texture,ztexture);
}

void
DX8Wrapper::Set_Render_Target(TextureClass *texture)
{
	Set_DX12_Render_Target(texture,nullptr);
}

unsigned int DX8Wrapper::Get_Free_Texture_RAM()
{
	const unsigned long long memory = DX12Device::Get_Dedicated_Video_Memory();
	return (memory > 0x7fffffff) ? 0x7fffffff : (unsigned int)memory;
}

// Converts a linear gamma ramp to one that is controlled by:
// Gamma - controls the curvature of the middle of the curve
// Bright - controls the minimum value of the curve
// Contrast - controls the difference between the maximum and the minimum of the curve
void DX8Wrapper::Set_Gamma(float gamma,float bright,float contrast,bool calibrate,bool uselimit)
{
	gamma=Bound(gamma,0.6f,6.0f);
	bright=Bound(bright,-0.5f,0.5f);
	contrast=Bound(contrast,0.5f,2.0f);
	float oo_gamma=1.0f/gamma;

	DX8_Assert();
	DX8_RECORD_DX8_CALLS();

	D3DGAMMARAMP ramp;
	float			 limit;

	// IML: I'm not really sure what the intent of the 'limit' variable is. It does not produce useful results for my purposes.
	if (uselimit) {
		limit=(contrast-1)/2*contrast;
	} else {
		limit = 0.0f;
	}

	// HY - arrived at this equation after much trial and error.
	for (int i=0; i<256; i++) {
		float in,out;
		in=i/256.0f;
		float x=in-limit;
		x=Bound(x,0.0f,1.0f);
		x=powf(x,oo_gamma);
		out=contrast*x+bright;
		out=Bound(out,0.0f,1.0f);
		ramp.red[i]=(WORD) (out*65535);
		ramp.green[i]=(WORD) (out*65535);
		ramp.blue[i]=(WORD) (out*65535);
	}

	// TheSuperHackers @feature Direct3D 12 applies the same ramp as a final color curve on the presented
	// frame (DX12Device::Set_Gamma_Ramp), since its swap chain is never exclusive fullscreen.
	DX12Device::Set_Gamma_Ramp(ramp.red, ramp.green, ramp.blue);
}

namespace wrapper
{
void D3DMatrixIdentity(D3DMATRIX* dxm)
{
	memset(dxm, 0, sizeof(*dxm));
	dxm->_11 = 1.0f;
	dxm->_22 = 1.0f;
	dxm->_33 = 1.0f;
	dxm->_44 = 1.0f;
}
} // namespace wrapper

void DX8Wrapper::Set_World_Identity()
{
	if (render_state_changed&(unsigned)WORLD_IDENTITY)
		return;
	wrapper::D3DMatrixIdentity(&render_state.world);
	render_state_changed|=(unsigned)WORLD_CHANGED|(unsigned)WORLD_IDENTITY;
}

void DX8Wrapper::Set_View_Identity()
{
	if (render_state_changed&(unsigned)VIEW_IDENTITY)
		return;
	wrapper::D3DMatrixIdentity(&render_state.view);
	render_state_changed|=(unsigned)VIEW_CHANGED|(unsigned)VIEW_IDENTITY;
}

//**********************************************************************************************
//! Resets render device to default state
/*!
*/
void DX8Wrapper::Apply_Default_State()
{
	SNAPSHOT_SAY(("DX8Wrapper::Apply_Default_State()"));

	// only set states used in game
	Set_DX8_Render_State(D3DRS_ZENABLE, TRUE);
//	Set_DX8_Render_State(D3DRS_FILLMODE, D3DFILL_SOLID);
	Set_DX8_Render_State(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
	//Set_DX8_Render_State(D3DRS_LINEPATTERN, 0);
	Set_DX8_Render_State(D3DRS_ZWRITEENABLE, TRUE);
	Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, FALSE);
	//Set_DX8_Render_State(D3DRS_LASTPIXEL, FALSE);
	Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_ONE);
	Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_ZERO);
	Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_CW);
	Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	Set_DX8_Render_State(D3DRS_ALPHAREF, 0);
	Set_DX8_Render_State(D3DRS_ALPHAFUNC, D3DCMP_LESSEQUAL);
	Set_DX8_Render_State(D3DRS_DITHERENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_FOGENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_SPECULARENABLE, FALSE);
//	Set_DX8_Render_State(D3DRS_ZVISIBLE, FALSE);
//	Set_DX8_Render_State(D3DRS_FOGCOLOR, 0);
//	Set_DX8_Render_State(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
//	Set_DX8_Render_State(D3DRS_FOGSTART, 0);

//	Set_DX8_Render_State(D3DRS_FOGEND, WWMath::Float_As_Int(1.0f));
//	Set_DX8_Render_State(D3DRS_FOGDENSITY, WWMath::Float_As_Int(1.0f));

	//Set_DX8_Render_State(D3DRS_EDGEANTIALIAS, FALSE);
	Set_DX8_Render_State(D3DRS_ZBIAS, 0);
//	Set_DX8_Render_State(D3DRS_RANGEFOGENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_STENCILENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
	Set_DX8_Render_State(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
	Set_DX8_Render_State(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
	Set_DX8_Render_State(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
	Set_DX8_Render_State(D3DRS_STENCILREF, 0);
	Set_DX8_Render_State(D3DRS_STENCILMASK, 0xffffffff);
	Set_DX8_Render_State(D3DRS_STENCILWRITEMASK, 0xffffffff);
	Set_DX8_Render_State(D3DRS_TEXTUREFACTOR, 0);
/*	Set_DX8_Render_State(D3DRS_WRAP0, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP1, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP2, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP3, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP4, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP5, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP6, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP7, D3DWRAP_U| D3DWRAP_V);*/
	Set_DX8_Render_State(D3DRS_CLIPPING, TRUE);
	Set_DX8_Render_State(D3DRS_LIGHTING, FALSE);
	//Set_DX8_Render_State(D3DRS_AMBIENT, 0);
//	Set_DX8_Render_State(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
	Set_DX8_Render_State(D3DRS_COLORVERTEX, TRUE);
/*	Set_DX8_Render_State(D3DRS_LOCALVIEWER, TRUE);
	Set_DX8_Render_State(D3DRS_NORMALIZENORMALS, FALSE);
	Set_DX8_Render_State(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
	Set_DX8_Render_State(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
	Set_DX8_Render_State(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
	Set_DX8_Render_State(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
	Set_DX8_Render_State(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);*/
	//Set_DX8_Render_State(D3DRS_CLIPPLANEENABLE, 0);
	Set_DX8_Render_State(D3DRS_SOFTWAREVERTEXPROCESSING, FALSE);
	//Set_DX8_Render_State(D3DRS_POINTSIZE, 0x3f800000);
	//Set_DX8_Render_State(D3DRS_POINTSIZE_MIN, 0);
	//Set_DX8_Render_State(D3DRS_POINTSPRITEENABLE, FALSE);
	//Set_DX8_Render_State(D3DRS_POINTSCALEENABLE, FALSE);
	//Set_DX8_Render_State(D3DRS_POINTSCALE_A, 0);
	//Set_DX8_Render_State(D3DRS_POINTSCALE_B, 0);
	//Set_DX8_Render_State(D3DRS_POINTSCALE_C, 0);
	//Set_DX8_Render_State(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
	//Set_DX8_Render_State(D3DRS_MULTISAMPLEMASK, 0xffffffff);
	//Set_DX8_Render_State(D3DRS_PATCHEDGESTYLE, D3DPATCHEDGE_DISCRETE);
	//Set_DX8_Render_State(D3DRS_PATCHSEGMENTS, 0x3f800000);
	//Set_DX8_Render_State(D3DRS_DEBUGMONITORTOKEN, D3DDMT_ENABLE);
	//Set_DX8_Render_State(D3DRS_POINTSIZE_MAX, Float_At_Int(64.0f));
	//Set_DX8_Render_State(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0x0000000f);
	//Set_DX8_Render_State(D3DRS_TWEENFACTOR, 0);
	Set_DX8_Render_State(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	//Set_DX8_Render_State(D3DRS_POSITIONORDER, D3DORDER_CUBIC);
	//Set_DX8_Render_State(D3DRS_NORMALORDER, D3DORDER_LINEAR);

	// disable TSS stages
	int i;
	for (i=0; i<CurrentCaps->Get_Max_Textures_Per_Pass(); i++)
	{
		Set_DX8_Texture_Stage_State(i, D3DTSS_COLOROP, D3DTOP_DISABLE);
		Set_DX8_Texture_Stage_State(i, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		Set_DX8_Texture_Stage_State(i, D3DTSS_COLORARG2, D3DTA_DIFFUSE);

		Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

		/*Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVMAT00, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVMAT01, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVMAT10, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVMAT11, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVLSCALE, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVLOFFSET, 0);*/

		Set_DX8_Texture_Stage_State(i, D3DTSS_TEXCOORDINDEX, i);


		Set_DX8_Texture_Stage_State(i, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
		Set_DX8_Texture_Stage_State(i, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BORDERCOLOR, 0);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MIPMAPLODBIAS, 0);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MAXMIPLEVEL, 0);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MAXANISOTROPY, 1);
		//Set_DX8_Texture_Stage_State(i, D3DTSS_ADDRESSW, D3DTADDRESS_WRAP);
		//Set_DX8_Texture_Stage_State(i, D3DTSS_COLORARG0, D3DTA_CURRENT);
		//Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAARG0, D3DTA_CURRENT);
		//Set_DX8_Texture_Stage_State(i, D3DTSS_RESULTARG, D3DTA_CURRENT);

		Set_DX8_Texture_Stage_State(i, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		Set_Texture(i,nullptr);
	}

//	DX8Wrapper::Set_Material(nullptr);
	VertexMaterialClass::Apply_Null();

	for (unsigned index=0;index<4;++index) {
		SNAPSHOT_SAY(("Clearing light %d to null",index));
		Set_DX8_Light(index,nullptr);
	}

	// set up simple default TSS
	Vector4 vconst[MAX_VERTEX_SHADER_CONSTANTS];
	memset(vconst,0,sizeof(Vector4)*MAX_VERTEX_SHADER_CONSTANTS);
	Set_Vertex_Shader_Constant(0, vconst, MAX_VERTEX_SHADER_CONSTANTS);

	Vector4 pconst[MAX_PIXEL_SHADER_CONSTANTS];
	memset(pconst,0,sizeof(Vector4)*MAX_PIXEL_SHADER_CONSTANTS);
	Set_Pixel_Shader_Constant(0, pconst, MAX_PIXEL_SHADER_CONSTANTS);

	Set_Vertex_Shader(DX8_FVF_XYZNDUV2);
	Set_Pixel_Shader(0);

	ShaderClass::Invalidate();
}

const char* DX8Wrapper::Get_DX8_Render_State_Name(D3DRENDERSTATETYPE state)
{
	switch (state) {
	case D3DRS_ZENABLE                       : return "D3DRS_ZENABLE";
	case D3DRS_FILLMODE                      : return "D3DRS_FILLMODE";
	case D3DRS_SHADEMODE                     : return "D3DRS_SHADEMODE";
	case D3DRS_LINEPATTERN                   : return "D3DRS_LINEPATTERN";
	case D3DRS_ZWRITEENABLE                  : return "D3DRS_ZWRITEENABLE";
	case D3DRS_ALPHATESTENABLE               : return "D3DRS_ALPHATESTENABLE";
	case D3DRS_LASTPIXEL                     : return "D3DRS_LASTPIXEL";
	case D3DRS_SRCBLEND                      : return "D3DRS_SRCBLEND";
	case D3DRS_DESTBLEND                     : return "D3DRS_DESTBLEND";
	case D3DRS_CULLMODE                      : return "D3DRS_CULLMODE";
	case D3DRS_ZFUNC                         : return "D3DRS_ZFUNC";
	case D3DRS_ALPHAREF                      : return "D3DRS_ALPHAREF";
	case D3DRS_ALPHAFUNC                     : return "D3DRS_ALPHAFUNC";
	case D3DRS_DITHERENABLE                  : return "D3DRS_DITHERENABLE";
	case D3DRS_ALPHABLENDENABLE              : return "D3DRS_ALPHABLENDENABLE";
	case D3DRS_FOGENABLE                     : return "D3DRS_FOGENABLE";
	case D3DRS_SPECULARENABLE                : return "D3DRS_SPECULARENABLE";
	case D3DRS_ZVISIBLE                      : return "D3DRS_ZVISIBLE";
	case D3DRS_FOGCOLOR                      : return "D3DRS_FOGCOLOR";
	case D3DRS_FOGTABLEMODE                  : return "D3DRS_FOGTABLEMODE";
	case D3DRS_FOGSTART                      : return "D3DRS_FOGSTART";
	case D3DRS_FOGEND                        : return "D3DRS_FOGEND";
	case D3DRS_FOGDENSITY                    : return "D3DRS_FOGDENSITY";
	case D3DRS_EDGEANTIALIAS                 : return "D3DRS_EDGEANTIALIAS";
	case D3DRS_ZBIAS                         : return "D3DRS_ZBIAS";
	case D3DRS_RANGEFOGENABLE                : return "D3DRS_RANGEFOGENABLE";
	case D3DRS_STENCILENABLE                 : return "D3DRS_STENCILENABLE";
	case D3DRS_STENCILFAIL                   : return "D3DRS_STENCILFAIL";
	case D3DRS_STENCILZFAIL                  : return "D3DRS_STENCILZFAIL";
	case D3DRS_STENCILPASS                   : return "D3DRS_STENCILPASS";
	case D3DRS_STENCILFUNC                   : return "D3DRS_STENCILFUNC";
	case D3DRS_STENCILREF                    : return "D3DRS_STENCILREF";
	case D3DRS_STENCILMASK                   : return "D3DRS_STENCILMASK";
	case D3DRS_STENCILWRITEMASK              : return "D3DRS_STENCILWRITEMASK";
	case D3DRS_TEXTUREFACTOR                 : return "D3DRS_TEXTUREFACTOR";
	case D3DRS_WRAP0                         : return "D3DRS_WRAP0";
	case D3DRS_WRAP1                         : return "D3DRS_WRAP1";
	case D3DRS_WRAP2                         : return "D3DRS_WRAP2";
	case D3DRS_WRAP3                         : return "D3DRS_WRAP3";
	case D3DRS_WRAP4                         : return "D3DRS_WRAP4";
	case D3DRS_WRAP5                         : return "D3DRS_WRAP5";
	case D3DRS_WRAP6                         : return "D3DRS_WRAP6";
	case D3DRS_WRAP7                         : return "D3DRS_WRAP7";
	case D3DRS_CLIPPING                      : return "D3DRS_CLIPPING";
	case D3DRS_LIGHTING                      : return "D3DRS_LIGHTING";
	case D3DRS_AMBIENT                       : return "D3DRS_AMBIENT";
	case D3DRS_FOGVERTEXMODE                 : return "D3DRS_FOGVERTEXMODE";
	case D3DRS_COLORVERTEX                   : return "D3DRS_COLORVERTEX";
	case D3DRS_LOCALVIEWER                   : return "D3DRS_LOCALVIEWER";
	case D3DRS_NORMALIZENORMALS              : return "D3DRS_NORMALIZENORMALS";
	case D3DRS_DIFFUSEMATERIALSOURCE         : return "D3DRS_DIFFUSEMATERIALSOURCE";
	case D3DRS_SPECULARMATERIALSOURCE        : return "D3DRS_SPECULARMATERIALSOURCE";
	case D3DRS_AMBIENTMATERIALSOURCE         : return "D3DRS_AMBIENTMATERIALSOURCE";
	case D3DRS_EMISSIVEMATERIALSOURCE        : return "D3DRS_EMISSIVEMATERIALSOURCE";
	case D3DRS_VERTEXBLEND                   : return "D3DRS_VERTEXBLEND";
	case D3DRS_CLIPPLANEENABLE               : return "D3DRS_CLIPPLANEENABLE";
	case D3DRS_SOFTWAREVERTEXPROCESSING      : return "D3DRS_SOFTWAREVERTEXPROCESSING";
	case D3DRS_POINTSIZE                     : return "D3DRS_POINTSIZE";
	case D3DRS_POINTSIZE_MIN                 : return "D3DRS_POINTSIZE_MIN";
	case D3DRS_POINTSPRITEENABLE             : return "D3DRS_POINTSPRITEENABLE";
	case D3DRS_POINTSCALEENABLE              : return "D3DRS_POINTSCALEENABLE";
	case D3DRS_POINTSCALE_A                  : return "D3DRS_POINTSCALE_A";
	case D3DRS_POINTSCALE_B                  : return "D3DRS_POINTSCALE_B";
	case D3DRS_POINTSCALE_C                  : return "D3DRS_POINTSCALE_C";
	case D3DRS_MULTISAMPLEANTIALIAS          : return "D3DRS_MULTISAMPLEANTIALIAS";
	case D3DRS_MULTISAMPLEMASK               : return "D3DRS_MULTISAMPLEMASK";
	case D3DRS_PATCHEDGESTYLE                : return "D3DRS_PATCHEDGESTYLE";
	case D3DRS_PATCHSEGMENTS                 : return "D3DRS_PATCHSEGMENTS";
	case D3DRS_DEBUGMONITORTOKEN             : return "D3DRS_DEBUGMONITORTOKEN";
	case D3DRS_POINTSIZE_MAX                 : return "D3DRS_POINTSIZE_MAX";
	case D3DRS_INDEXEDVERTEXBLENDENABLE      : return "D3DRS_INDEXEDVERTEXBLENDENABLE";
	case D3DRS_COLORWRITEENABLE              : return "D3DRS_COLORWRITEENABLE";
	case D3DRS_TWEENFACTOR                   : return "D3DRS_TWEENFACTOR";
	case D3DRS_BLENDOP                       : return "D3DRS_BLENDOP";
//	case D3DRS_POSITIONORDER                 : return "D3DRS_POSITIONORDER";
//	case D3DRS_NORMALORDER                   : return "D3DRS_NORMALORDER";
	default											  : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Stage_State_Name(D3DTEXTURESTAGESTATETYPE state)
{
	switch (state) {
	case D3DTSS_COLOROP                   : return "D3DTSS_COLOROP";
	case D3DTSS_COLORARG1                 : return "D3DTSS_COLORARG1";
	case D3DTSS_COLORARG2                 : return "D3DTSS_COLORARG2";
	case D3DTSS_ALPHAOP                   : return "D3DTSS_ALPHAOP";
	case D3DTSS_ALPHAARG1                 : return "D3DTSS_ALPHAARG1";
	case D3DTSS_ALPHAARG2                 : return "D3DTSS_ALPHAARG2";
	case D3DTSS_BUMPENVMAT00              : return "D3DTSS_BUMPENVMAT00";
	case D3DTSS_BUMPENVMAT01              : return "D3DTSS_BUMPENVMAT01";
	case D3DTSS_BUMPENVMAT10              : return "D3DTSS_BUMPENVMAT10";
	case D3DTSS_BUMPENVMAT11              : return "D3DTSS_BUMPENVMAT11";
	case D3DTSS_TEXCOORDINDEX             : return "D3DTSS_TEXCOORDINDEX";
	case D3DTSS_ADDRESSU                  : return "D3DTSS_ADDRESSU";
	case D3DTSS_ADDRESSV                  : return "D3DTSS_ADDRESSV";
	case D3DTSS_BORDERCOLOR               : return "D3DTSS_BORDERCOLOR";
	case D3DTSS_MAGFILTER                 : return "D3DTSS_MAGFILTER";
	case D3DTSS_MINFILTER                 : return "D3DTSS_MINFILTER";
	case D3DTSS_MIPFILTER                 : return "D3DTSS_MIPFILTER";
	case D3DTSS_MIPMAPLODBIAS             : return "D3DTSS_MIPMAPLODBIAS";
	case D3DTSS_MAXMIPLEVEL               : return "D3DTSS_MAXMIPLEVEL";
	case D3DTSS_MAXANISOTROPY             : return "D3DTSS_MAXANISOTROPY";
	case D3DTSS_BUMPENVLSCALE             : return "D3DTSS_BUMPENVLSCALE";
	case D3DTSS_BUMPENVLOFFSET            : return "D3DTSS_BUMPENVLOFFSET";
	case D3DTSS_TEXTURETRANSFORMFLAGS     : return "D3DTSS_TEXTURETRANSFORMFLAGS";
	case D3DTSS_ADDRESSW                  : return "D3DTSS_ADDRESSW";
	case D3DTSS_COLORARG0                 : return "D3DTSS_COLORARG0";
	case D3DTSS_ALPHAARG0                 : return "D3DTSS_ALPHAARG0";
	case D3DTSS_RESULTARG                 : return "D3DTSS_RESULTARG";
	default										  : return "UNKNOWN";
	}
}

void DX8Wrapper::Get_DX8_Render_State_Value_Name(StringClass& name, D3DRENDERSTATETYPE state, unsigned value)
{
	switch (state) {
	case D3DRS_ZENABLE:
		name=Get_DX8_ZBuffer_Type_Name(value);
		break;

	case D3DRS_FILLMODE:
		name=Get_DX8_Fill_Mode_Name(value);
		break;

	case D3DRS_SHADEMODE:
		name=Get_DX8_Shade_Mode_Name(value);
		break;

	case D3DRS_LINEPATTERN:
	case D3DRS_FOGCOLOR:
	case D3DRS_ALPHAREF:
	case D3DRS_STENCILMASK:
	case D3DRS_STENCILWRITEMASK:
	case D3DRS_TEXTUREFACTOR:
	case D3DRS_AMBIENT:
	case D3DRS_CLIPPLANEENABLE:
	case D3DRS_MULTISAMPLEMASK:
		name.Format("0x%x",value);
		break;

	case D3DRS_ZWRITEENABLE:
	case D3DRS_ALPHATESTENABLE:
	case D3DRS_LASTPIXEL:
	case D3DRS_DITHERENABLE:
	case D3DRS_ALPHABLENDENABLE:
	case D3DRS_FOGENABLE:
	case D3DRS_SPECULARENABLE:
	case D3DRS_STENCILENABLE:
	case D3DRS_RANGEFOGENABLE:
	case D3DRS_EDGEANTIALIAS:
	case D3DRS_CLIPPING:
	case D3DRS_LIGHTING:
	case D3DRS_COLORVERTEX:
	case D3DRS_LOCALVIEWER:
	case D3DRS_NORMALIZENORMALS:
	case D3DRS_SOFTWAREVERTEXPROCESSING:
	case D3DRS_POINTSPRITEENABLE:
	case D3DRS_POINTSCALEENABLE:
	case D3DRS_MULTISAMPLEANTIALIAS:
	case D3DRS_INDEXEDVERTEXBLENDENABLE:
		name=value ? "TRUE" : "FALSE";
		break;

	case D3DRS_SRCBLEND:
	case D3DRS_DESTBLEND:
		name=Get_DX8_Blend_Name(value);
		break;

	case D3DRS_CULLMODE:
		name=Get_DX8_Cull_Mode_Name(value);
		break;

	case D3DRS_ZFUNC:
	case D3DRS_ALPHAFUNC:
	case D3DRS_STENCILFUNC:
		name=Get_DX8_Cmp_Func_Name(value);
		break;

	case D3DRS_ZVISIBLE:
		name="NOTSUPPORTED";
		break;

	case D3DRS_FOGTABLEMODE:
	case D3DRS_FOGVERTEXMODE:
		name=Get_DX8_Fog_Mode_Name(value);
		break;

	case D3DRS_FOGSTART:
	case D3DRS_FOGEND:
	case D3DRS_FOGDENSITY:
	case D3DRS_POINTSIZE:
	case D3DRS_POINTSIZE_MIN:
	case D3DRS_POINTSCALE_A:
	case D3DRS_POINTSCALE_B:
	case D3DRS_POINTSCALE_C:
	case D3DRS_PATCHSEGMENTS:
	case D3DRS_POINTSIZE_MAX:
	case D3DRS_TWEENFACTOR:
		name.Format("%f",std::bit_cast<float>(value));
		break;

	case D3DRS_ZBIAS:
	case D3DRS_STENCILREF:
		name.Format("%d",value);
		break;

	case D3DRS_STENCILFAIL:
	case D3DRS_STENCILZFAIL:
	case D3DRS_STENCILPASS:
		name=Get_DX8_Stencil_Op_Name(value);
		break;

	case D3DRS_WRAP0:
	case D3DRS_WRAP1:
	case D3DRS_WRAP2:
	case D3DRS_WRAP3:
	case D3DRS_WRAP4:
	case D3DRS_WRAP5:
	case D3DRS_WRAP6:
	case D3DRS_WRAP7:
		name="0";
		if (value&D3DWRAP_U) name+="|D3DWRAP_U";
		if (value&D3DWRAP_V) name+="|D3DWRAP_V";
		if (value&D3DWRAP_W) name+="|D3DWRAP_W";
		break;

	case D3DRS_DIFFUSEMATERIALSOURCE:
	case D3DRS_SPECULARMATERIALSOURCE:
	case D3DRS_AMBIENTMATERIALSOURCE:
	case D3DRS_EMISSIVEMATERIALSOURCE:
		name=Get_DX8_Material_Source_Name(value);
		break;

	case D3DRS_VERTEXBLEND:
		name=Get_DX8_Vertex_Blend_Flag_Name(value);
		break;

	case D3DRS_PATCHEDGESTYLE:
		name=Get_DX8_Patch_Edge_Style_Name(value);
		break;

	case D3DRS_DEBUGMONITORTOKEN:
		name=Get_DX8_Debug_Monitor_Token_Name(value);
		break;

	case D3DRS_COLORWRITEENABLE:
		name="0";
		if (value&D3DCOLORWRITEENABLE_RED) name+="|D3DCOLORWRITEENABLE_RED";
		if (value&D3DCOLORWRITEENABLE_GREEN) name+="|D3DCOLORWRITEENABLE_GREEN";
		if (value&D3DCOLORWRITEENABLE_BLUE) name+="|D3DCOLORWRITEENABLE_BLUE";
		if (value&D3DCOLORWRITEENABLE_ALPHA) name+="|D3DCOLORWRITEENABLE_ALPHA";
		break;
	case D3DRS_BLENDOP:
		name=Get_DX8_Blend_Op_Name(value);
		break;
	default:
		name.Format("UNKNOWN (%d)",value);
		break;
	}
}

void DX8Wrapper::Get_DX8_Texture_Stage_State_Value_Name(StringClass& name, D3DTEXTURESTAGESTATETYPE state, unsigned value)
{
	switch (state) {
	case D3DTSS_COLOROP:
	case D3DTSS_ALPHAOP:
		name=Get_DX8_Texture_Op_Name(value);
		break;

	case D3DTSS_COLORARG0:
	case D3DTSS_COLORARG1:
	case D3DTSS_COLORARG2:
	case D3DTSS_ALPHAARG0:
	case D3DTSS_ALPHAARG1:
	case D3DTSS_ALPHAARG2:
	case D3DTSS_RESULTARG:
		name=Get_DX8_Texture_Arg_Name(value);
		break;

	case D3DTSS_ADDRESSU:
	case D3DTSS_ADDRESSV:
	case D3DTSS_ADDRESSW:
		name=Get_DX8_Texture_Address_Name(value);
		break;

	case D3DTSS_MAGFILTER:
	case D3DTSS_MINFILTER:
	case D3DTSS_MIPFILTER:
		name=Get_DX8_Texture_Filter_Name(value);
		break;

	case D3DTSS_TEXTURETRANSFORMFLAGS:
		name=Get_DX8_Texture_Transform_Flag_Name(value);
		break;

	// Floating point values
	case D3DTSS_MIPMAPLODBIAS:
	case D3DTSS_BUMPENVMAT00:
	case D3DTSS_BUMPENVMAT01:
	case D3DTSS_BUMPENVMAT10:
	case D3DTSS_BUMPENVMAT11:
	case D3DTSS_BUMPENVLSCALE:
	case D3DTSS_BUMPENVLOFFSET:
		name.Format("%f",std::bit_cast<float>(value));
		break;

	case D3DTSS_TEXCOORDINDEX:
		if ((value&0xffff0000)==D3DTSS_TCI_CAMERASPACENORMAL) {
			name.Format("D3DTSS_TCI_CAMERASPACENORMAL|%d",value&0xffff);
		}
		else if ((value&0xffff0000)==D3DTSS_TCI_CAMERASPACEPOSITION) {
			name.Format("D3DTSS_TCI_CAMERASPACEPOSITION|%d",value&0xffff);
		}
		else if ((value&0xffff0000)==D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR) {
			name.Format("D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR|%d",value&0xffff);
		}
		else {
			name.Format("%d",value);
		}
		break;

	// Integer value
	case D3DTSS_MAXMIPLEVEL:
	case D3DTSS_MAXANISOTROPY:
		name.Format("%d",value);
		break;
	// Hex values
	case D3DTSS_BORDERCOLOR:
		name.Format("0x%x",value);
		break;

	default:
		name.Format("UNKNOWN (%d)",value);
		break;
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Op_Name(unsigned value)
{
	switch (value) {
	case D3DTOP_DISABLE                      : return "D3DTOP_DISABLE";
	case D3DTOP_SELECTARG1                   : return "D3DTOP_SELECTARG1";
	case D3DTOP_SELECTARG2                   : return "D3DTOP_SELECTARG2";
	case D3DTOP_MODULATE                     : return "D3DTOP_MODULATE";
	case D3DTOP_MODULATE2X                   : return "D3DTOP_MODULATE2X";
	case D3DTOP_MODULATE4X                   : return "D3DTOP_MODULATE4X";
	case D3DTOP_ADD                          : return "D3DTOP_ADD";
	case D3DTOP_ADDSIGNED                    : return "D3DTOP_ADDSIGNED";
	case D3DTOP_ADDSIGNED2X                  : return "D3DTOP_ADDSIGNED2X";
	case D3DTOP_SUBTRACT                     : return "D3DTOP_SUBTRACT";
	case D3DTOP_ADDSMOOTH                    : return "D3DTOP_ADDSMOOTH";
	case D3DTOP_BLENDDIFFUSEALPHA            : return "D3DTOP_BLENDDIFFUSEALPHA";
	case D3DTOP_BLENDTEXTUREALPHA            : return "D3DTOP_BLENDTEXTUREALPHA";
	case D3DTOP_BLENDFACTORALPHA             : return "D3DTOP_BLENDFACTORALPHA";
	case D3DTOP_BLENDTEXTUREALPHAPM          : return "D3DTOP_BLENDTEXTUREALPHAPM";
	case D3DTOP_BLENDCURRENTALPHA            : return "D3DTOP_BLENDCURRENTALPHA";
	case D3DTOP_PREMODULATE                  : return "D3DTOP_PREMODULATE";
	case D3DTOP_MODULATEALPHA_ADDCOLOR       : return "D3DTOP_MODULATEALPHA_ADDCOLOR";
	case D3DTOP_MODULATECOLOR_ADDALPHA       : return "D3DTOP_MODULATECOLOR_ADDALPHA";
	case D3DTOP_MODULATEINVALPHA_ADDCOLOR    : return "D3DTOP_MODULATEINVALPHA_ADDCOLOR";
	case D3DTOP_MODULATEINVCOLOR_ADDALPHA    : return "D3DTOP_MODULATEINVCOLOR_ADDALPHA";
	case D3DTOP_BUMPENVMAP                   : return "D3DTOP_BUMPENVMAP";
	case D3DTOP_BUMPENVMAPLUMINANCE          : return "D3DTOP_BUMPENVMAPLUMINANCE";
	case D3DTOP_DOTPRODUCT3                  : return "D3DTOP_DOTPRODUCT3";
	case D3DTOP_MULTIPLYADD                  : return "D3DTOP_MULTIPLYADD";
	case D3DTOP_LERP                         : return "D3DTOP_LERP";
	default										     : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Arg_Name(unsigned value)
{
	switch (value) {
	case D3DTA_CURRENT			: return "D3DTA_CURRENT";
	case D3DTA_DIFFUSE			: return "D3DTA_DIFFUSE";
	case D3DTA_SELECTMASK		: return "D3DTA_SELECTMASK";
	case D3DTA_SPECULAR			: return "D3DTA_SPECULAR";
	case D3DTA_TEMP				: return "D3DTA_TEMP";
	case D3DTA_TEXTURE			: return "D3DTA_TEXTURE";
	case D3DTA_TFACTOR			: return "D3DTA_TFACTOR";
	case D3DTA_ALPHAREPLICATE	: return "D3DTA_ALPHAREPLICATE";
	case D3DTA_COMPLEMENT		: return "D3DTA_COMPLEMENT";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Filter_Name(unsigned value)
{
	switch (value) {
	case D3DTEXF_NONE				: return "D3DTEXF_NONE";
	case D3DTEXF_POINT			: return "D3DTEXF_POINT";
	case D3DTEXF_LINEAR			: return "D3DTEXF_LINEAR";
	case D3DTEXF_ANISOTROPIC	: return "D3DTEXF_ANISOTROPIC";
	case D3DTEXF_FLATCUBIC		: return "D3DTEXF_FLATCUBIC";
	case D3DTEXF_GAUSSIANCUBIC	: return "D3DTEXF_GAUSSIANCUBIC";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Address_Name(unsigned value)
{
	switch (value) {
	case D3DTADDRESS_WRAP		: return "D3DTADDRESS_WRAP";
	case D3DTADDRESS_MIRROR		: return "D3DTADDRESS_MIRROR";
	case D3DTADDRESS_CLAMP		: return "D3DTADDRESS_CLAMP";
	case D3DTADDRESS_BORDER		: return "D3DTADDRESS_BORDER";
	case D3DTADDRESS_MIRRORONCE: return "D3DTADDRESS_MIRRORONCE";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Transform_Flag_Name(unsigned value)
{
	switch (value) {
	case D3DTTFF_DISABLE			: return "D3DTTFF_DISABLE";
	case D3DTTFF_COUNT1			: return "D3DTTFF_COUNT1";
	case D3DTTFF_COUNT2			: return "D3DTTFF_COUNT2";
	case D3DTTFF_COUNT3			: return "D3DTTFF_COUNT3";
	case D3DTTFF_COUNT4			: return "D3DTTFF_COUNT4";
	case D3DTTFF_PROJECTED		: return "D3DTTFF_PROJECTED";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_ZBuffer_Type_Name(unsigned value)
{
	switch (value) {
	case D3DZB_FALSE				: return "D3DZB_FALSE";
	case D3DZB_TRUE				: return "D3DZB_TRUE";
	case D3DZB_USEW				: return "D3DZB_USEW";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Fill_Mode_Name(unsigned value)
{
	switch (value) {
	case D3DFILL_POINT			: return "D3DFILL_POINT";
	case D3DFILL_WIREFRAME		: return "D3DFILL_WIREFRAME";
	case D3DFILL_SOLID			: return "D3DFILL_SOLID";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Shade_Mode_Name(unsigned value)
{
	switch (value) {
	case D3DSHADE_FLAT			: return "D3DSHADE_FLAT";
	case D3DSHADE_GOURAUD		: return "D3DSHADE_GOURAUD";
	case D3DSHADE_PHONG			: return "D3DSHADE_PHONG";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Blend_Name(unsigned value)
{
	switch (value) {
	case D3DBLEND_ZERO                : return "D3DBLEND_ZERO";
	case D3DBLEND_ONE                 : return "D3DBLEND_ONE";
	case D3DBLEND_SRCCOLOR            : return "D3DBLEND_SRCCOLOR";
	case D3DBLEND_INVSRCCOLOR         : return "D3DBLEND_INVSRCCOLOR";
	case D3DBLEND_SRCALPHA            : return "D3DBLEND_SRCALPHA";
	case D3DBLEND_INVSRCALPHA         : return "D3DBLEND_INVSRCALPHA";
	case D3DBLEND_DESTALPHA           : return "D3DBLEND_DESTALPHA";
	case D3DBLEND_INVDESTALPHA        : return "D3DBLEND_INVDESTALPHA";
	case D3DBLEND_DESTCOLOR           : return "D3DBLEND_DESTCOLOR";
	case D3DBLEND_INVDESTCOLOR        : return "D3DBLEND_INVDESTCOLOR";
	case D3DBLEND_SRCALPHASAT         : return "D3DBLEND_SRCALPHASAT";
	case D3DBLEND_BOTHSRCALPHA        : return "D3DBLEND_BOTHSRCALPHA";
	case D3DBLEND_BOTHINVSRCALPHA     : return "D3DBLEND_BOTHINVSRCALPHA";
	default									 : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Cull_Mode_Name(unsigned value)
{
	switch (value) {
	case D3DCULL_NONE				: return "D3DCULL_NONE";
	case D3DCULL_CW				: return "D3DCULL_CW";
	case D3DCULL_CCW				: return "D3DCULL_CCW";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Cmp_Func_Name(unsigned value)
{
	switch (value) {
	case D3DCMP_NEVER          : return "D3DCMP_NEVER";
	case D3DCMP_LESS           : return "D3DCMP_LESS";
	case D3DCMP_EQUAL          : return "D3DCMP_EQUAL";
	case D3DCMP_LESSEQUAL      : return "D3DCMP_LESSEQUAL";
	case D3DCMP_GREATER        : return "D3DCMP_GREATER";
	case D3DCMP_NOTEQUAL       : return "D3DCMP_NOTEQUAL";
	case D3DCMP_GREATEREQUAL   : return "D3DCMP_GREATEREQUAL";
	case D3DCMP_ALWAYS         : return "D3DCMP_ALWAYS";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Fog_Mode_Name(unsigned value)
{
	switch (value) {
	case D3DFOG_NONE				: return "D3DFOG_NONE";
	case D3DFOG_EXP				: return "D3DFOG_EXP";
	case D3DFOG_EXP2				: return "D3DFOG_EXP2";
	case D3DFOG_LINEAR			: return "D3DFOG_LINEAR";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Stencil_Op_Name(unsigned value)
{
	switch (value) {
	case D3DSTENCILOP_KEEP		: return "D3DSTENCILOP_KEEP";
	case D3DSTENCILOP_ZERO		: return "D3DSTENCILOP_ZERO";
	case D3DSTENCILOP_REPLACE	: return "D3DSTENCILOP_REPLACE";
	case D3DSTENCILOP_INCRSAT	: return "D3DSTENCILOP_INCRSAT";
	case D3DSTENCILOP_DECRSAT	: return "D3DSTENCILOP_DECRSAT";
	case D3DSTENCILOP_INVERT	: return "D3DSTENCILOP_INVERT";
	case D3DSTENCILOP_INCR		: return "D3DSTENCILOP_INCR";
	case D3DSTENCILOP_DECR		: return "D3DSTENCILOP_DECR";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Material_Source_Name(unsigned value)
{
	switch (value) {
	case D3DMCS_MATERIAL			: return "D3DMCS_MATERIAL";
	case D3DMCS_COLOR1			: return "D3DMCS_COLOR1";
	case D3DMCS_COLOR2			: return "D3DMCS_COLOR2";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Vertex_Blend_Flag_Name(unsigned value)
{
	switch (value) {
	case D3DVBF_DISABLE			: return "D3DVBF_DISABLE";
	case D3DVBF_1WEIGHTS			: return "D3DVBF_1WEIGHTS";
	case D3DVBF_2WEIGHTS			: return "D3DVBF_2WEIGHTS";
	case D3DVBF_3WEIGHTS			: return "D3DVBF_3WEIGHTS";
	case D3DVBF_TWEENING			: return "D3DVBF_TWEENING";
	case D3DVBF_0WEIGHTS			: return "D3DVBF_0WEIGHTS";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Patch_Edge_Style_Name(unsigned value)
{
	switch (value) {
	case D3DPATCHEDGE_DISCRETE	: return "D3DPATCHEDGE_DISCRETE";
   case D3DPATCHEDGE_CONTINUOUS:return "D3DPATCHEDGE_CONTINUOUS";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Debug_Monitor_Token_Name(unsigned value)
{
	switch (value) {
	case D3DDMT_ENABLE			: return "D3DDMT_ENABLE";
	case D3DDMT_DISABLE			: return "D3DDMT_DISABLE";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Blend_Op_Name(unsigned value)
{
	switch (value) {
	case D3DBLENDOP_ADD			: return "D3DBLENDOP_ADD";
	case D3DBLENDOP_SUBTRACT	: return "D3DBLENDOP_SUBTRACT";
	case D3DBLENDOP_REVSUBTRACT: return "D3DBLENDOP_REVSUBTRACT";
	case D3DBLENDOP_MIN			: return "D3DBLENDOP_MIN";
	case D3DBLENDOP_MAX			: return "D3DBLENDOP_MAX";
	default							: return "UNKNOWN";
	}
}


//============================================================================
// DX8Wrapper::getBackBufferFormat
//============================================================================

WW3DFormat	DX8Wrapper::getBackBufferFormat()
{
	return WW3D_FORMAT_A8R8G8B8;
}
