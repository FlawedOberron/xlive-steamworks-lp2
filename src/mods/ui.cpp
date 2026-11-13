// LP2 mods: our own UI layer, drawn over the game every frame (Dear ImGui), instead of patching the game's Flash screens.
// Mods register layers (UiAddLayer); each draws what it wants with ImGui when the frame is presented. Nothing is drawn
// unless a layer chooses to, so stock screens, stock modes and the campaign look as they always did.
//
// The hooks: DX9 the device's Present (vtable 17) and Reset (16), DX11 the swap chain's Present (8) and ResizeBuffers (13).
// The vtables come from the game's own device: its D3D create import is hooked and patched as it's made (a class's
// vtable is shared by every object of it). Fallback, if that isn't caught within 5 s of the window: a throwaway device
// on a hidden window. mappics.cpp patches the same DX9 Present slot when it first writes
// a picture; whichever comes second calls the other.
#include "mods/lp2.h"

#include "core/config.h"
#include "core/log.h"

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>

#include "third-party/imgui/imgui.h"
#include "third-party/imgui/imgui_impl_dx11.h"
#include "third-party/imgui/imgui_impl_dx9.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

namespace xls::mods {

namespace {

struct Layer {
	std::string name;
	UiDrawFn draw;
};
std::mutex g_layersMutex;
std::vector<Layer> g_layers;

ImFont* g_fontNormal = nullptr;
ImFont* g_fontLarge = nullptr;
bool g_context = false;
DWORD g_lastFrame = 0;

typedef HRESULT(STDMETHODCALLTYPE* Present9Fn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* Reset9Fn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* Present11Fn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Resize11Fn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
Present9Fn g_present9 = nullptr;
Reset9Fn g_reset9 = nullptr;
Present11Fn g_present11 = nullptr;
Resize11Fn g_resize11 = nullptr;

IDirect3DDevice9* g_dev9 = nullptr;
ID3D11Device* g_dev11 = nullptr;
ID3D11DeviceContext* g_ctx11 = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
float g_width = 1280, g_height = 720;
bool g_failed = false; // the backend could not start: the game goes on without our UI

// A system font's TrueType bytes through GDI (Wine keeps its fonts out of the prefix's Fonts folder, but serves them).
std::vector<char> FontBytes(const char* face, bool bold)
{
	std::vector<char> data;
	HMODULE gdi = LoadLibraryA("gdi32.dll");
	if (!gdi) {
		return data;
	}
	using CreateFontAFn = HFONT(WINAPI*)(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR);
	using CreateDCAFn = HDC(WINAPI*)(LPCSTR, LPCSTR, LPCSTR, const DEVMODEA*);
	using SelectObjectFn = HGDIOBJ(WINAPI*)(HDC, HGDIOBJ);
	using GetFontDataFn = DWORD(WINAPI*)(HDC, DWORD, DWORD, PVOID, DWORD);
	using GetTextFaceAFn = int(WINAPI*)(HDC, int, LPSTR);
	using DeleteObjectFn = BOOL(WINAPI*)(HGDIOBJ);
	using DeleteDCFn = BOOL(WINAPI*)(HDC);
	auto createFont = (CreateFontAFn)(void*)GetProcAddress(gdi, "CreateFontA");
	auto createDC = (CreateDCAFn)(void*)GetProcAddress(gdi, "CreateCompatibleDC");
	auto select = (SelectObjectFn)(void*)GetProcAddress(gdi, "SelectObject");
	auto getFontData = (GetFontDataFn)(void*)GetProcAddress(gdi, "GetFontData");
	auto getFace = (GetTextFaceAFn)(void*)GetProcAddress(gdi, "GetTextFaceA");
	auto deleteObject = (DeleteObjectFn)(void*)GetProcAddress(gdi, "DeleteObject");
	auto deleteDC = (DeleteDCFn)(void*)GetProcAddress(gdi, "DeleteDC");
	if (!createFont || !createDC || !select || !getFontData || !getFace || !deleteObject || !deleteDC) {
		return data;
	}
	HDC dc = ((HDC(WINAPI*)(HDC))(void*)createDC)(nullptr);
	HFONT font = createFont(-32, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, 0, ANTIALIASED_QUALITY, 0, face);
	if (dc && font) {
		HGDIOBJ old = select(dc, font);
		char got[64] = {};
		getFace(dc, sizeof got, got);
		DWORD size = getFontData(dc, 0, 0, nullptr, 0);
		if (size != GDI_ERROR && size > 0 && size < 64u << 20) {
			data.resize(size);
			if (getFontData(dc, 0, 0, data.data(), size) != size) {
				data.clear();
			}
		}
		XLS_LOG_INFO("mods: ui: font \"%s\"%s -> \"%s\", %u bytes.", face, bold ? " bold" : "", got, (unsigned)data.size());
		select(dc, old);
	}
	if (font) {
		deleteObject(font);
	}
	if (dc) {
		deleteDC(dc);
	}
	return data;
}

// Every TrueType family this PC has (Wine's and the prefix's): ModCache\fonts.txt in the game folder, one per line, for the
// editor's font menus (a mode's Show text). Any of them can be drawn with (UiFontNamed).
int CALLBACK AddFamily(const LOGFONTA* lf, const TEXTMETRICA*, DWORD type, LPARAM out)
{
	if ((type & TRUETYPE_FONTTYPE) && lf->lfFaceName[0] && lf->lfFaceName[0] != '@') {
		((std::set<std::string>*)out)->insert(lf->lfFaceName);
	}
	return 1;
}

void ListFonts()
{
	HMODULE gdi = LoadLibraryA("gdi32.dll");
	using EnumFn = int(WINAPI*)(HDC, LOGFONTA*, FONTENUMPROCA, LPARAM, DWORD);
	using CreateDCFn = HDC(WINAPI*)(HDC);
	using DeleteDCFn = BOOL(WINAPI*)(HDC);
	auto enumFonts = gdi ? (EnumFn)(void*)GetProcAddress(gdi, "EnumFontFamiliesExA") : nullptr;
	auto createDC = gdi ? (CreateDCFn)(void*)GetProcAddress(gdi, "CreateCompatibleDC") : nullptr;
	auto deleteDC = gdi ? (DeleteDCFn)(void*)GetProcAddress(gdi, "DeleteDC") : nullptr;
	if (!enumFonts || !createDC || !deleteDC) {
		return;
	}
	HDC dc = createDC(nullptr);
	std::set<std::string> names;
	LOGFONTA lf = {};
	lf.lfCharSet = DEFAULT_CHARSET;
	enumFonts(dc, &lf, AddFamily, (LPARAM)&names, 0);
	deleteDC(dc);
	char path[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	if (char* slash = strrchr(path, '\\')) {
		strcpy(slash + 1, "ModCache");
		CreateDirectoryA(path, nullptr);
		strcat(path, "\\fonts.txt");
	}
	if (FILE* f = fopen(path, "w")) {
		for (const std::string& n : names) {
			fprintf(f, "%s\n", n.c_str());
		}
		fclose(f);
	}
	XLS_LOG_INFO("mods: ui: %u fonts listed in %s.", (unsigned)names.size(), path);
}

// Fonts asked for by name: added to the atlas before the next frame (the backend then rebuilds its font texture)
std::mutex g_fontMutex;
std::map<std::string, std::pair<ImFont*, ImFont*>> g_named; // name -> normal, large (nullptr: not found)
std::set<std::string> g_wanted;
float g_fontScale = 1.0f;

bool LoadWanted()
{
	std::set<std::string> wanted;
	{
		std::lock_guard<std::mutex> lock(g_fontMutex);
		wanted.swap(g_wanted);
	}
	if (wanted.empty()) {
		return false;
	}
	ImGuiIO& io = ImGui::GetIO();
	for (const std::string& name : wanted) {
		std::vector<char> bytes = FontBytes(name.c_str(), false);
		std::pair<ImFont*, ImFont*> fonts = {nullptr, nullptr};
		if (!bytes.empty()) {
			ImFontConfig cfg;
			cfg.OversampleH = 2;
			for (int large = 0; large < 2; large++) {
				void* copy = IM_ALLOC(bytes.size());
				memcpy(copy, bytes.data(), bytes.size());
				ImFont* f = io.Fonts->AddFontFromMemoryTTF(copy, (int)bytes.size(), (large ? 34.0f : 20.0f) * g_fontScale, &cfg);
				(large ? fonts.second : fonts.first) = f;
			}
		}
		std::lock_guard<std::mutex> lock(g_fontMutex);
		g_named[name] = fonts;
	}
	io.Fonts->Build();
	return true;
}

void Fonts(float height)
{
	g_fontScale = height / 720.0f;
	ListFonts();
	ImGuiIO& io = ImGui::GetIO();
	float scale = height / 720.0f;
	ImFontConfig cfg;
	cfg.FontDataOwnedByAtlas = true;
	cfg.OversampleH = 2;
	for (const char* face : {"Arial", "Tahoma", "Liberation Sans", "DejaVu Sans"}) {
		std::vector<char> bytes = FontBytes(face, true);
		if (bytes.empty()) {
			continue;
		}
		for (int large = 0; large < 2; large++) {
			void* copy = IM_ALLOC(bytes.size()); // the atlas frees it
			memcpy(copy, bytes.data(), bytes.size());
			ImFont* f = io.Fonts->AddFontFromMemoryTTF(copy, (int)bytes.size(), (large ? 34.0f : 20.0f) * scale, &cfg);
			(large ? g_fontLarge : g_fontNormal) = f;
		}
		break;
	}
	if (!g_fontNormal) {
		XLS_LOG_WARN("mods: ui: no system font found, using ImGui's own.");
		ImFontConfig def;
		def.SizePixels = 13.0f * std::max(1.0f, scale);
		g_fontNormal = io.Fonts->AddFontDefault(&def);
		def.SizePixels = 26.0f * std::max(1.0f, scale);
		g_fontLarge = io.Fonts->AddFontDefault(&def);
	}
}

bool Context(float height)
{
	if (g_context) {
		return true;
	}
	g_context = true;
	IMGUI_CHECKVERSION();
	ImGui::SetAllocatorFunctions([](size_t size, void*) { return CountedMalloc(size); }, [](void* p, void*) { CountedFree(p); });
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowBorderSize = 0;
	style.WindowRounding = 6;
	Fonts(height);
	return true;
}

// One frame of every layer, between the backend's NewFrame and ImGui::Render.
void Frame()
{
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(g_width, g_height);
	DWORD now = GetTickCount();
	io.DeltaTime = g_lastFrame && now > g_lastFrame ? std::min((now - g_lastFrame) / 1000.0f, 0.25f) : 1.0f / 60.0f;
	g_lastFrame = now;
	io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX); // no input yet: layers only draw
	ImGui::NewFrame();
	std::vector<Layer> layers;
	{
		std::lock_guard<std::mutex> lock(g_layersMutex);
		layers = g_layers;
	}
	for (const Layer& l : layers) {
		l.draw(g_width, g_height);
	}
	ImGui::Render();
}

bool Anything()
{
	std::lock_guard<std::mutex> lock(g_layersMutex);
	return !g_layers.empty();
}

HRESULT STDMETHODCALLTYPE Present9(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
{
	if (!g_failed && Anything()) {
		if (!g_dev9) {
			g_dev9 = dev;
			IDirect3DSurface9* bb = nullptr;
			D3DSURFACE_DESC desc = {};
			if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
				bb->GetDesc(&desc);
				bb->Release();
				g_width = (float)desc.Width;
				g_height = (float)desc.Height;
			}
			Context(g_height);
			if (!ImGui_ImplDX9_Init(dev)) {
				g_failed = true;
				XLS_LOG_WARN("mods: ui: the DX9 backend did not start, no overlay.");
			}
			else {
				XLS_LOG_INFO("mods: ui: drawing on DX9 at %.0f x %.0f.", g_width, g_height);
			}
		}
		if (!g_failed && dev == g_dev9) {
			if (LoadWanted()) {
				ImGui_ImplDX9_InvalidateDeviceObjects(); // its NewFrame makes the font texture again
			}
			ImGui_ImplDX9_NewFrame();
			Frame();
			IDirect3DSurface9 *bb = nullptr, *was = nullptr;
			dev->GetRenderTarget(0, &was);
			if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
				dev->SetRenderTarget(0, bb);
				if (SUCCEEDED(dev->BeginScene())) {
					ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
					dev->EndScene();
				}
				bb->Release();
			}
			if (was) {
				dev->SetRenderTarget(0, was);
				was->Release();
			}
		}
	}
	return g_present9(dev, a, b, w, r);
}

HRESULT STDMETHODCALLTYPE Reset9(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
	if (g_dev9 == dev && !g_failed) {
		ImGui_ImplDX9_InvalidateDeviceObjects();
	}
	HRESULT hr = g_reset9(dev, pp);
	if (g_dev9 == dev && !g_failed && SUCCEEDED(hr)) {
		if (pp && pp->BackBufferWidth && pp->BackBufferHeight) {
			g_width = (float)pp->BackBufferWidth;
			g_height = (float)pp->BackBufferHeight;
		}
		ImGui_ImplDX9_CreateDeviceObjects();
	}
	return hr;
}

void ReleaseTarget11()
{
	if (g_rtv) {
		g_rtv->Release();
		g_rtv = nullptr;
	}
}

HRESULT STDMETHODCALLTYPE Present11(IDXGISwapChain* sc, UINT sync, UINT flags)
{
	if (!g_failed && Anything() && !(flags & DXGI_PRESENT_TEST)) {
		if (!g_dev11) {
			DXGI_SWAP_CHAIN_DESC desc = {};
			sc->GetDesc(&desc);
			if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev11)) && g_dev11) {
				g_dev11->GetImmediateContext(&g_ctx11);
				g_width = (float)desc.BufferDesc.Width;
				g_height = (float)desc.BufferDesc.Height;
				Context(g_height);
				if (!ImGui_ImplDX11_Init(g_dev11, g_ctx11)) {
					g_failed = true;
					XLS_LOG_WARN("mods: ui: the DX11 backend did not start (no d3dcompiler?), no overlay.");
				}
				else {
					XLS_LOG_INFO("mods: ui: drawing on DX11 at %.0f x %.0f (format %u).", g_width, g_height, (unsigned)desc.BufferDesc.Format);
				}
			}
			else {
				g_failed = true;
				XLS_LOG_WARN("mods: ui: the swap chain has no D3D11 device, no overlay.");
			}
		}
		if (!g_failed && !g_rtv) {
			ID3D11Texture2D* bb = nullptr;
			if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
				D3D11_TEXTURE2D_DESC td = {};
				bb->GetDesc(&td);
				g_width = (float)td.Width;
				g_height = (float)td.Height;
				if (FAILED(g_dev11->CreateRenderTargetView(bb, nullptr, &g_rtv))) {
					g_rtv = nullptr;
					static bool warned = false;
					if (!warned) {
						warned = true;
						XLS_LOG_WARN("mods: ui: no render target on the back buffer (format %u).", (unsigned)td.Format);
					}
				}
				bb->Release();
			}
		}
		if (!g_failed && g_rtv) {
			if (LoadWanted()) {
				ImGui_ImplDX11_InvalidateDeviceObjects();
			}
			ImGui_ImplDX11_NewFrame();
			Frame();
			ID3D11RenderTargetView* wasRtv = nullptr;
			ID3D11DepthStencilView* wasDsv = nullptr;
			g_ctx11->OMGetRenderTargets(1, &wasRtv, &wasDsv);
			g_ctx11->OMSetRenderTargets(1, &g_rtv, nullptr);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			g_ctx11->OMSetRenderTargets(1, &wasRtv, wasDsv);
			if (wasRtv) {
				wasRtv->Release();
			}
			if (wasDsv) {
				wasDsv->Release();
			}
		}
	}
	return g_present11(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE Resize11(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags)
{
	ReleaseTarget11(); // the back buffers can't change while a view holds one
	return g_resize11(sc, count, w, h, format, flags);
}

bool Patch(void** slot, void* hook, void** original)
{
	DWORD old;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
		return false;
	}
	*original = *slot;
	*slot = hook;
	VirtualProtect(slot, sizeof(void*), old, &old);
	return true;
}

HWND DummyWindow()
{
	WNDCLASSEXA wc = {sizeof wc};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(nullptr);
	wc.lpszClassName = "lp2ui_dummy";
	RegisterClassExA(&wc);
	return CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
}

bool Hook9(HWND wnd)
{
	HMODULE d3d9 = LoadLibraryA("d3d9.dll");
	using CreateFn = IDirect3D9*(WINAPI*)(UINT);
	auto create = d3d9 ? (CreateFn)(void*)GetProcAddress(d3d9, "Direct3DCreate9") : nullptr;
	IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
	if (!d3d) {
		return false;
	}
	D3DPRESENT_PARAMETERS pp = {};
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.BackBufferFormat = D3DFMT_UNKNOWN;
	pp.hDeviceWindow = wnd;
	IDirect3DDevice9* dev = nullptr;
	HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &dev);
	bool ok = false;
	if (SUCCEEDED(hr) && dev) {
		void** vt = *(void***)dev;
		ok = Patch(&vt[17], (void*)&Present9, (void**)&g_present9) && Patch(&vt[16], (void*)&Reset9, (void**)&g_reset9);
		dev->Release();
	}
	else {
		XLS_LOG_WARN("mods: ui: the DX9 test device failed (0x%08lx).", hr);
	}
	d3d->Release();
	return ok;
}

// The game's own device: its import of the D3D create call is pointed at ours, so the vtable is patched from the
// game's real swap chain / device as it's made (the throwaway device above, made beside the game's, could leave
// Steam's overlay without its screenshot key).
std::atomic<bool> g_caught{false};

void** ImportSlot(const char* dll, const char* name)
{
	auto* base = (uint8_t*)GetModuleHandleA(nullptr);
	auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress) {
		return nullptr;
	}
	for (auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); desc->Name; desc++) {
		if (_stricmp((const char*)(base + desc->Name), dll) != 0 || !desc->OriginalFirstThunk) {
			continue;
		}
		auto* names = (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk);
		auto* slots = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
		for (; names->u1.AddressOfData; names++, slots++) {
			if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) && strcmp((const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, name) == 0) {
				return (void**)&slots->u1.Function;
			}
		}
	}
	return nullptr;
}

PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_create11 = nullptr;

HRESULT WINAPI Create11(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT count,
	UINT sdk, const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc, ID3D11Device** dev, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx)
{
	HRESULT hr = g_create11(adapter, type, sw, flags, levels, count, sdk, desc, sc, dev, level, ctx);
	if (SUCCEEDED(hr) && sc && *sc && !g_caught.exchange(true)) {
		void** vt = *(void***)*sc;
		bool ok = Patch(&vt[8], (void*)&Present11, (void**)&g_present11) && Patch(&vt[13], (void*)&Resize11, (void**)&g_resize11);
		XLS_LOG_INFO("mods: ui: overlay hooked on the game's swap chain (%s).", ok ? "ok" : "patch failed");
	}
	return hr;
}

typedef HRESULT(STDMETHODCALLTYPE* CreateDevice9Fn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
CreateDevice9Fn g_createDevice9 = nullptr;
using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
Create9Fn g_create9 = nullptr;

HRESULT STDMETHODCALLTYPE CreateDevice9(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
	HRESULT hr = g_createDevice9(d3d, adapter, type, wnd, flags, pp, out);
	if (SUCCEEDED(hr) && out && *out && !g_caught.exchange(true)) {
		void** vt = *(void***)*out;
		bool ok = Patch(&vt[17], (void*)&Present9, (void**)&g_present9) && Patch(&vt[16], (void*)&Reset9, (void**)&g_reset9);
		XLS_LOG_INFO("mods: ui: overlay hooked on the game's device (%s).", ok ? "ok" : "patch failed");
	}
	return hr;
}

IDirect3D9* WINAPI Create9(UINT sdk)
{
	IDirect3D9* d3d = g_create9(sdk);
	if (d3d && !g_createDevice9) {
		void** vt = *(void***)d3d;
		Patch(&vt[16], (void*)&CreateDevice9, (void**)&g_createDevice9);
	}
	return d3d;
}

// once the import slot holds the real function (SteamStub fills the Steam exes' imports in after our DllMain)
bool HookCreate(bool dx11)
{
	void** slot = dx11 ? ImportSlot("d3d11.dll", "D3D11CreateDeviceAndSwapChain") : ImportSlot("d3d9.dll", "Direct3DCreate9");
	HMODULE dll = GetModuleHandleA(dx11 ? "d3d11.dll" : "d3d9.dll");
	HMODULE owner = nullptr;
	if (!slot || !dll || !*slot ||
	    !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)*slot, &owner) ||
	    owner == GetModuleHandleA(nullptr)) {
		return false;
	}
	return dx11 ? Patch(slot, (void*)&Create11, (void**)&g_create11) : Patch(slot, (void*)&Create9, (void**)&g_create9);
}

bool g_uiOff = false, g_createHooked = false;
int g_uiDx = -2; // 0 DX11, 1 DX9, -1 neither, -2 not checked yet

bool Hook11(HWND wnd)
{
	HMODULE d3d11 = LoadLibraryA("d3d11.dll");
	auto create = d3d11 ? (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(void*)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain") : nullptr;
	if (!create) {
		return false;
	}
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 1;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.Width = 64;
	sd.BufferDesc.Height = 64;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = wnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	IDXGISwapChain* sc = nullptr;
	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
	bool ok = false;
	if (SUCCEEDED(hr) && sc) {
		void** vt = *(void***)sc;
		ok = Patch(&vt[8], (void*)&Present11, (void**)&g_present11) && Patch(&vt[13], (void*)&Resize11, (void**)&g_resize11);
	}
	else {
		XLS_LOG_WARN("mods: ui: the DX11 test device failed (0x%08lx).", hr);
	}
	if (sc) {
		sc->Release();
	}
	if (ctx) {
		ctx->Release();
	}
	if (dev) {
		dev->Release();
	}
	return ok;
}

BOOL CALLBACK FindGameWindow(HWND wnd, LPARAM found)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(wnd, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(wnd)) {
		*(HWND*)found = wnd;
		return FALSE;
	}
	return TRUE;
}

DWORD WINAPI HookThread(void* param)
{
	bool dx11 = param != nullptr;
	HWND game = nullptr;
	for (int i = 0; i < 1200 && !game; i++) { // the game's window: its device comes with it
		EnumWindows(FindGameWindow, (LPARAM)&game);
		Sleep(100);
	}
	if (!game) {
		XLS_LOG_WARN("mods: ui: no game window within 2 minutes, no overlay.");
		return 0;
	}
	for (int i = 0; i < 50 && !g_caught; i++) { // the game's own device, through the import hook
		Sleep(100);
	}
	if (g_caught.exchange(true)) {
		return 0;
	}
	XLS_LOG_WARN("mods: ui: the game's device wasn't caught, using a throwaway one.");
	HWND wnd = DummyWindow();
	bool ok = wnd && (dx11 ? Hook11(wnd) : Hook9(wnd));
	if (wnd) {
		DestroyWindow(wnd);
	}
	if (ok) {
		XLS_LOG_INFO("mods: ui: overlay hooked (%s).", dx11 ? "DX11 swap chain" : "DX9 device");
	}
	else {
		XLS_LOG_WARN("mods: ui: overlay not hooked (%s).", dx11 ? "DX11" : "DX9");
	}
	return 0;
}

}

void UiAddLayer(const char* name, UiDrawFn draw)
{
	std::lock_guard<std::mutex> lock(g_layersMutex);
	g_layers.push_back({name, draw});
}

::ImFont* UiFont(bool large)
{
	return large ? g_fontLarge : g_fontNormal;
}

::ImFont* UiFontNamed(const char* name, bool large)
{
	if (!name || !*name) {
		return UiFont(large);
	}
	std::lock_guard<std::mutex> lock(g_fontMutex);
	auto it = g_named.find(name);
	if (it == g_named.end()) {
		g_wanted.insert(name); // from the next frame
		return UiFont(large);
	}
	ImFont* f = large ? it->second.second : it->second.first;
	return f ? f : UiFont(large);
}

void UiEarly()
{
	if (g_createHooked || g_uiOff) {
		return;
	}
	if (g_uiDx == -2) {
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		const wchar_t* name = wcsrchr(path, L'\\');
		name = name ? name + 1 : path;
		g_uiDx = _wcsnicmp(name, L"LP2DX11", 7) == 0 ? 0 : _wcsnicmp(name, L"LP2DX9", 6) == 0 ? 1 : -1;
		g_uiOff = GetFileAttributesW((xls::Cfg().modCacheDir + L"no-ui.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
	}
	if (g_uiDx < 0 || g_uiOff) {
		return;
	}
	if (HookCreate(g_uiDx == 0)) {
		g_createHooked = true;
		XLS_LOG_INFO("mods: ui: the game's %s create import hooked.", g_uiDx == 0 ? "D3D11" : "D3D9");
	}
}

void UiInstall(int build)
{
	if (build < 0) {
		return;
	}
	if (GetFileAttributesW((xls::Cfg().modCacheDir + L"no-ui.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
		XLS_LOG_INFO("mods: ui: ModCache\\no-ui.txt: overlay off (no Present hooks, no throwaway device).");
		return;
	}
	bool dx11 = build % 2 == 0;
	UiEarly();
	if (!g_createHooked) {
		XLS_LOG_WARN("mods: ui: the game's D3D create import not hooked.");
	}
	HANDLE thread = CreateThread(nullptr, 0, HookThread, dx11 ? (void*)1 : nullptr, 0, nullptr);
	if (thread) {
		CloseHandle(thread);
	}
}

}
