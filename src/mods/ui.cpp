// LP2 mods: our own UI layer, drawn over the game every frame (Dear ImGui), instead of patching the game's Flash screens.
// Mods register layers (UiAddLayer); each draws what it wants with ImGui when the frame is presented. Nothing is drawn
// unless a layer chooses to, so stock screens, stock modes and the campaign look as they always did.
//
// The hooks: DX9 the device's Present (vtable 17) and Reset (16), DX11 the swap chain's Present (8) and ResizeBuffers (13).
// The vtables come from a throwaway device made on a hidden window once the game's window is up (a class's vtable is
// shared by every object of it, the game's included). mappics.cpp patches the same DX9 Present slot when it first writes
// a picture; whichever comes second calls the other.
#include "mods/lp2.h"

#include "core/log.h"

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>

#include "third-party/imgui/imgui.h"
#include "third-party/imgui/imgui_impl_dx11.h"
#include "third-party/imgui/imgui_impl_dx9.h"

#include <atomic>
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

void Fonts(float height)
{
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
	Sleep(500);
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

void UiInstall(int build)
{
	if (build < 0) {
		return;
	}
	bool dx11 = build % 2 == 0;
	HANDLE thread = CreateThread(nullptr, 0, HookThread, dx11 ? (void*)1 : nullptr, 0, nullptr);
	if (thread) {
		CloseHandle(thread);
	}
}

}
