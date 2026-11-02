#include "mods/lp2.h"

#include "core/log.h"

#include <windows.h>
#include <d3d9.h>
#include <d3d10.h>
#include <d3d11.h>

#include <stdint.h>
#include <string.h>

#include <mutex>
#include <vector>

// Live create-match pictures. The mod slots' pictures (flash\dds\ID221_SC14_ID / SC15_ID, packages.cpp
// AddMapPictures) load the first time the Flash shows them and stay loaded (the title archives are resident), so a
// rebind while scrolling the map list wouldn't change them: after a rebind we write the new picture into the loaded
// texture.
//
// sResource (DX11 [0x18d0ccc], DX9 [0x18c0c2c]) vf11 (vtable +0x2c, DX11 0x49fe10, DX9 0x49ec50): thiscall
// (DTI, path, flags) -> the loaded resource (a reference added), or with flags bit 7 none rather than a load; vf13
// (+0x34) releases one. rTexture (DTI DX11 0x18e16b0, DX9 0x18cf5d0) +0xcc: its texture object, whose +0x30 is the
// D3D texture (IDirect3DTexture9 / ID3D11Texture2D; +0x34 too when +0x28 has 0x4000, double buffered), as the
// loader (rTexture::vf7) fills it. Pictures are 512 x 512 DXT5, one mip: an rTexture file's last 512 * 512 bytes.
// DX9: written in the device's Present (the render thread). DX11: under the device's multithread lock
// (ID3D10Multithread on the immediate context, switched on the first time).

namespace xls::mods {

namespace {

struct Build {
	uint32_t manager, find, dti, vtable;
	uint32_t planetVtable; // sPlanetResource (the disc exes put this subclass there; its vf11 adds a flag and calls sResource's)
};
const Build kBuilds[kBuildCount] = {
	{0x14d0ccc, 0x9fe10, 0x14e16b0, 0x12e0df8, 0xf7a4f4}, // LP2DX11.exe
	{0x14c0c2c, 0x9ec50, 0x14cf5d0, 0x12d0d70, 0xf6a364}, // LP2DX9.exe
	{0x14d1ccc, 0x9fef0, 0x14e26b0, 0x12e11f8, 0xf7a4f4}, // LP2DX11.exe 1.1
	{0x14c0c2c, 0x9ee40, 0x14cf5d0, 0x12d0ff8, 0xf6a37c}, // LP2DX9.exe 1.1
};
const char* const kNames[2] = {"flash\\dds\\ID221_SC14_ID", "flash\\dds\\ID221_SC15_ID"};
const UINT kSize = 512, kBytes = kSize * kSize; // DXT5: a byte per pixel
const IID kIID_ID3D10Multithread = {0x9b7e4e00, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}};


typedef void*(__fastcall* FindFn)(void* self, void* edx, void* dti, const char* path, uint32_t flags);
typedef void(__fastcall* ReleaseFn)(void* self, void* edx, void* res);
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

unsigned char* g_base = nullptr;
const Build* g_build = nullptr;
bool g_dx11 = false;
PresentFn g_present = nullptr;
std::mutex g_mutex;
struct Pending {
	IDirect3DTexture9* tex;
	std::vector<uint8_t> data;
};
std::vector<Pending> g_pending; // DX9: for the next Present
bool g_protected = false;

void** Manager()
{
	// the build: the one whose resource manager is up, known by its vtable (sResource::vftable) or its lookup
	for (const Build& b : kBuilds) {
		if (GameBuild() >= 0 && &b - kBuilds != GameBuild()) {
			continue;
		}
		if (g_build && g_build != &b) {
			continue;
		}
		void** m = *(void***)(g_base + b.manager);
		if (!m || IsBadReadPtr(m, sizeof(void*)) || IsBadReadPtr(*m, 14 * sizeof(void*))) {
			continue;
		}
		void** vt = *(void***)m;
		if ((uint8_t*)vt == g_base + b.vtable || (uint8_t*)vt == g_base + b.planetVtable || (uint8_t*)vt[11] == g_base + b.find) {
			g_build = &b;
			g_dx11 = (&b - kBuilds) % 2 == 0;
			return m;
		}
	}
	static bool told = false;
	if (!told) {
		told = true;
		for (const Build& b : kBuilds) {
			if (GameBuild() >= 0 && &b - kBuilds != GameBuild()) {
				continue;
			}
			void** m = *(void***)(g_base + b.manager);
			void** vt = m && !IsBadReadPtr(m, sizeof(void*)) ? *(void***)m : nullptr;
			XLS_LOG_WARN("mods: maps: picture: manager at %p = %p, vtable %p (want %p), vf11 %p (want %p).", g_base + b.manager, m, vt,
				g_base + b.vtable, vt && !IsBadReadPtr(vt, 12 * sizeof(void*)) ? vt[11] : nullptr, g_base + b.find);
		}
	}
	return nullptr;
}

// the slot's loaded picture texture(s), with a reference each (Release them); none when it isn't loaded
int Textures(int k, IUnknown* out[2])
{
	void** m = Manager();
	if (!m) {
		XLS_LOG_WARN("mods: maps: picture: no resource manager found (unknown build?).");
		return 0;
	}
	// sResource's own lookup and release (its vtable), whatever subclass the manager is: sPlanetResource's
	// overrides run more of the game's code, which isn't safe from this thread
	void** vt = (void**)(g_base + g_build->vtable);
	uint8_t* res = (uint8_t*)((FindFn)vt[11])(m, nullptr, g_base + g_build->dti, kNames[k], 0x80);
	if (!res) {
		XLS_LOG_INFO("mods: maps: picture: %s isn't loaded (%s).", kNames[k], g_dx11 ? "DX11" : "DX9");
		return 0;
	}
	int n = 0;
	uint8_t* obj = *(uint8_t**)(res + 0xcc);
	if (obj) {
		bool two = (*(uint32_t*)(obj + 0x28) & 0x4000) != 0;
		for (int i = 0; i < (two ? 2 : 1); i++) {
			IUnknown* t = *(IUnknown**)(obj + 0x30 + 4 * i);
			if (t) {
				t->AddRef();
				out[n++] = t;
			}
		}
	}
	((ReleaseFn)vt[13])(m, nullptr, res);
	if (!n) {
		XLS_LOG_INFO("mods: maps: picture: %s is loaded but has no texture (object %p).", kNames[k], obj);
	}
	return n;
}

void CopyBlocks(uint8_t* dst, UINT pitch, const uint8_t* src)
{
	const UINT row = kSize / 4 * 16; // a row of 4x4 blocks
	for (UINT y = 0; y < kSize / 4; y++) {
		memcpy(dst + y * pitch, src + y * row, row);
	}
}

void Write9(IDirect3DTexture9* tex, const uint8_t* data)
{
	D3DSURFACE_DESC d;
	if (FAILED(tex->GetLevelDesc(0, &d)) || d.Width != kSize || d.Height != kSize || d.Format != D3DFMT_DXT5) {
		XLS_LOG_WARN("mods: maps: the create-match picture isn't 512x512 DXT5, not updated.");
		return;
	}
	D3DLOCKED_RECT r;
	if (d.Pool != D3DPOOL_DEFAULT || (d.Usage & D3DUSAGE_DYNAMIC)) {
		if (SUCCEEDED(tex->LockRect(0, &r, nullptr, 0))) {
			CopyBlocks((uint8_t*)r.pBits, r.Pitch, data);
			tex->UnlockRect(0);
		}
		return;
	}
	IDirect3DDevice9* dev = nullptr;
	IDirect3DTexture9* tmp = nullptr;
	if (SUCCEEDED(tex->GetDevice(&dev)) && SUCCEEDED(dev->CreateTexture(kSize, kSize, 1, 0, D3DFMT_DXT5, D3DPOOL_SYSTEMMEM, &tmp, nullptr))) {
		if (SUCCEEDED(tmp->LockRect(0, &r, nullptr, 0))) {
			CopyBlocks((uint8_t*)r.pBits, r.Pitch, data);
			tmp->UnlockRect(0);
			dev->UpdateTexture(tmp, tex);
		}
		tmp->Release();
	}
	if (dev) {
		dev->Release();
	}
}

HRESULT STDMETHODCALLTYPE Present9(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
{
	std::vector<Pending> todo;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		todo.swap(g_pending);
	}
	for (Pending& p : todo) {
		Write9(p.tex, p.data.data());
		p.tex->Release();
	}
	return g_present(dev, a, b, w, r);
}

bool HookPresent9(IDirect3DTexture9* tex)
{
	if (g_present) {
		return true;
	}
	IDirect3DDevice9* dev = nullptr;
	if (FAILED(tex->GetDevice(&dev))) {
		return false;
	}
	void** vt = *(void***)dev;
	DWORD old = 0;
	bool ok = VirtualProtect(&vt[17], sizeof(void*), PAGE_EXECUTE_READWRITE, &old) != 0;
	if (ok) {
		g_present = (PresentFn)vt[17];
		vt[17] = (void*)&Present9;
		VirtualProtect(&vt[17], sizeof(void*), old, &old);
	}
	dev->Release();
	return ok;
}

void Write11(ID3D11Texture2D* tex, const uint8_t* data)
{
	D3D11_TEXTURE2D_DESC d;
	tex->GetDesc(&d);
	if (d.Width != kSize || d.Height != kSize || (d.Format != DXGI_FORMAT_BC3_UNORM && d.Format != DXGI_FORMAT_BC3_UNORM_SRGB)) {
		XLS_LOG_WARN("mods: maps: the create-match picture isn't 512x512 BC3, not updated.");
		return;
	}
	if (d.Usage == D3D11_USAGE_IMMUTABLE || d.Usage == D3D11_USAGE_STAGING) {
		XLS_LOG_WARN("mods: maps: the create-match picture can't be written (usage %d).", (int)d.Usage);
		return;
	}
	ID3D11Device* dev = nullptr;
	tex->GetDevice(&dev);
	ID3D11DeviceContext* ctx = nullptr;
	dev->GetImmediateContext(&ctx);
	ID3D10Multithread* mt = nullptr; // the real interface: a local stand-in here got devirtualized to __cxa_pure_virtual (address 0)
	if (FAILED(ctx->QueryInterface(kIID_ID3D10Multithread, (void**)&mt))) {
		mt = nullptr;
	}
	if (mt && !g_protected) {
		// on for good: from now on the game's context calls take the lock too (a moment for any call under way)
		if (!mt->GetMultithreadProtected()) {
			mt->SetMultithreadProtected(TRUE);
			Sleep(50);
		}
		g_protected = true;
	}
	if (!mt) {
		XLS_LOG_WARN("mods: maps: no multithread lock on the D3D11 context, the create-match picture not updated.");
	}
	else {
		mt->Enter();
		if (d.Usage == D3D11_USAGE_DYNAMIC) {
			D3D11_MAPPED_SUBRESOURCE m;
			if (SUCCEEDED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
				CopyBlocks((uint8_t*)m.pData, m.RowPitch, data);
				ctx->Unmap(tex, 0);
			}
		}
		else {
			ctx->UpdateSubresource(tex, 0, nullptr, data, kSize / 4 * 16, 0);
		}
		mt->Leave();
		mt->Release();
	}
	ctx->Release();
	dev->Release();
}

}

void MapPicsInstall(unsigned char* base)
{
	g_base = base;
}

void MapPicsUpdate(int k, const std::vector<uint8_t>& rtexture)
{
	if (!g_base || k < 0 || k > 1 || rtexture.size() < kBytes) {
		XLS_LOG_WARN("mods: maps: picture of slot %d not updated (%u bytes of picture).", 13 + k, (unsigned)rtexture.size());
		return;
	}
	IUnknown* tex[2] = {};
	int n = Textures(k, tex);
	const uint8_t* data = rtexture.data() + rtexture.size() - kBytes;
	for (int i = 0; i < n; i++) {
		if (g_dx11) {
			ID3D11Texture2D* t = nullptr;
			if (SUCCEEDED(tex[i]->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) {
				Write11(t, data);
				t->Release();
			}
		}
		else {
			IDirect3DTexture9* t = nullptr;
			if (SUCCEEDED(tex[i]->QueryInterface(__uuidof(IDirect3DTexture9), (void**)&t))) {
				if (HookPresent9(t)) {
					std::lock_guard<std::mutex> lock(g_mutex);
					g_pending.push_back({t, std::vector<uint8_t>(data, data + kBytes)});
					t = nullptr; // Present releases it
				}
				if (t) {
					t->Release();
				}
			}
		}
		tex[i]->Release();
	}
	if (n) {
		XLS_LOG_INFO("mods: maps: create-match picture of slot %d updated.", 13 + k);
	}
}

}
