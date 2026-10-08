/*
 * How it works:
 * The game uploads its camera as four consecutive vertex shader constants,
 * column-major, so each register is a row of the matrix and
 * clip.x = dot(register0, vertex). Scaling register 0 by sx scales NDC x
 * and nothing else.
 *
 * A matrix is a camera if neither its last row nor its last column is
 * (0,0,0,1), affine and orthographic matrices always have one or the other.
 * The 2D UI is drawn in screen space with a single constant rather than a
 * matrix, so it can never be caught by that test.
 *
 * Gameplay is detected by whether the scene camera is drawing meshes. In the
 * menus that camera is bound for a few background quads and never draws
 * indexed geometry; during a chart, and during the lead-in animation, it
 * draws the lane and the notes.
 *
 * Menu button prompts: the controller-aligned prompts at the bottom of the
 * menus always sit in the same region, so after each frame that strip is
 * copied out and stretched back in horizontally by the same sx. It eases off
 * as the field eases on, so it's never active during gameplay.
 *
 * Nothing is pattern-scanned and no addresses are hardcoded
 */

#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>

#if defined(_MSC_VER)
#pragma comment(lib, "d3d9.lib")
#endif

#ifndef COUNTOF
#define COUNTOF(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// IDirect3DDevice9 vtable indices, fixed by the COM interface definition.
enum : int {
    VT_ENDSCENE              = 42,
    VT_DRAWINDEXEDPRIMITIVE  = 82,
    VT_SETVERTEXSHADERCONSTF = 94,
    VT_COUNT                 = 119
};

static HMODULE g_self = nullptr;

static float         g_sx      = 1.165f;
static float         g_sy      = 1.0f;
static volatile LONG g_enabled = 1;
static volatile LONG g_gate    = 1;
static volatile LONG g_hotkeys = 1;
static volatile LONG g_menuStretch = 1;

// Which constant register holds the scene camera. Learned the first time a
// frame contains exactly one camera, that can only be gameplay, then kept
// in the ini so later sessions start out knowing it.
static int g_sceneReg = -1;

static UINT g_projRegs[8];
static int  g_projRegCount  = 0;
static int  g_activeProjReg = -1;
static int  g_sceneMeshDraws = 0;
static int  g_menuFrameRun   = 0;

// How strongly the effect is applied: 1 is full, 0 is off. Switching on is
// instant. Switching off eases since a hard cut would be visible. I didn't
// really know how to get rid of the cut so this is the lazy way to do it
static float     g_blend        = 0.0f;
static const int GATE_HOLD_FRAMES = 15;
static const int GATE_RAMP_FRAMES = 10;

// ---------------------------------------------------------------------------
static std::string SelfDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(g_self, path, MAX_PATH);
    std::string s(path);
    size_t slash = s.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string(".") : s.substr(0, slash);
}

// Small log for troubleshooting: load, settings, errors. Not per-frame.
static void Note(const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    std::string p = SelfDir() + "\\chusanwide.log";
    FILE* f = fopen(p.c_str(), "a");
    if (f) { fputs(line, f); fputc('\n', f); fclose(f); }
}

// ---------------------------------------------------------------------------
static std::string IniPath() { return SelfDir() + "\\chusanwide.ini"; }

static void WriteDefaultIni()
{
    std::string p = IniPath();
    if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    // Binary mode: this was previously text mode so "\r\n" would become "\r\r\n"
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return;
    fputs(
        "[chusanwide]\r\n"
        "enabled=1\r\n"
        "\r\n"
        "; sx is how much wider the playfield is drawn.\r\n"
        "; For 27 inch, set to 1.165. For 25 inch, set to 1.26. For 24 inch, set to 1.31.\r\n"
        "; If you have an unusual screen size / controller size, calculate sx = controller size (inches) / screen size (inches)\r\n"
        "sx=1.165\r\n"
        "; Vertical scale. Not recommended to edit this.\r\n"
        "sy=1.0\r\n"
        "\r\n"
        "; Only widen during gameplay\r\n"
        "gate=1\r\n"
        "\r\n"
        "; Stretch the button prompts at the bottom of the menus to line up with the controller\r\n"
        "menu_stretch=1\r\n"
        "\r\n"
        "; Left and Right arrow keys adjust sx by 0.005.\r\n"
        "; Set to 0 if you don\'t want this.\r\n"
        "hotkeys=1\r\n"
        "; Detected automatically on first play. Leave this alone.\r\n"
        "scene_register=-1\r\n", f);
    fclose(f);
}

// Last-write time of the ini, so edits made while the game runs can be picked
// up without a keypress, and so our own writes do not look like user edits.
static FILETIME g_iniStamp = {};

static bool IniStamp(FILETIME* out, DWORD* sizeLow)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(IniPath().c_str(), GetFileExInfoStandard, &d))
        return false;
    *out = d.ftLastWriteTime;
    if (sizeLow) *sizeLow = d.nFileSizeLow;
    return true;
}

static void RememberIniStamp()
{
    FILETIME ft;
    if (IniStamp(&ft, NULL)) g_iniStamp = ft;
}

static void SaveSx()
{
    char v[32];
    snprintf(v, sizeof(v), "%.4f", g_sx);
    WritePrivateProfileStringA("chusanwide", "sx", v, IniPath().c_str());
    RememberIniStamp();
}

// This adds new keys to inis created by an old version
static void AddMissingKey(const char* key, const char* comment, const char* value, const char* afterKey)
{
    char probe[8];
    GetPrivateProfileStringA("chusanwide", key, "\x01", probe, sizeof(probe), IniPath().c_str());
    if (probe[0] != '\x01') return;                            // already there

    std::string text;
    FILE* f = fopen(IniPath().c_str(), "rb");
    if (!f) return;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    // Match the old "\r\r\n"
    const bool doubled = text.find("\r\r\n") != std::string::npos;
    const char* nl = doubled ? "\r\r\n" : text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string insert = std::string("; ") + comment + nl + key + "=" + value + nl;

    // Find the start of the line after the `afterKey=` line.
    size_t at = std::string::npos;
    size_t afterLen = strlen(afterKey);
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\n', pos);
        size_t next = (end == std::string::npos) ? text.size() : end + 1;
        size_t k = pos;
        while (k < next && (text[k] == ' ' || text[k] == '\t')) ++k;
        if (_strnicmp(text.c_str() + k, afterKey, afterLen) == 0) {
            size_t e = k + afterLen;
            while (e < next && (text[e] == ' ' || text[e] == '\t')) ++e;
            if (e < next && text[e] == '=') { at = next; break; }
        }
        pos = next;
    }

    if (at == std::string::npos) {
        // Line not found (hand-edited ini); let Windows put it in the section.
        WritePrivateProfileStringA("chusanwide", key, value, IniPath().c_str());
    } else {
        if (at == text.size() && !text.empty() && text.back() != '\n') text += nl;
        if (!doubled) {
            insert = nl + insert;                               // blank line before
            if (at < text.size() && text[at] != '\r' && text[at] != '\n')
                insert += nl;                                   // and after, unless there is one
        }
        text.insert(at > text.size() ? text.size() : at, insert);

        std::string tmp = IniPath() + ".tmp";
        FILE* o = fopen(tmp.c_str(), "wb");
        if (!o) return;
        bool ok = fwrite(text.data(), 1, text.size(), o) == text.size();
        ok = (fclose(o) == 0) && ok;
        if (!ok || !MoveFileExA(tmp.c_str(), IniPath().c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileA(tmp.c_str());
            return;
        }
    }
    RememberIniStamp();
    Note("[chusanwide] added %s to the ini", key);
}

static float IniFloat(const char* key, float fallback)
{
    char buf[64] = {}, def[64];
    snprintf(def, sizeof(def), "%f", fallback);
    GetPrivateProfileStringA("chusanwide", key, def, buf, sizeof(buf), IniPath().c_str());
    float v = (float)atof(buf);
    return (v > 0.0f && v < 10.0f) ? v : fallback;
}

static void LoadIni()
{
    WriteDefaultIni();
    AddMissingKey("menu_stretch",
                  "Stretch the button prompts at the bottom of the menus to line up with the controller",
                  "1", "gate");

    // Fall back to whatever is already loaded rather than to a constant, so a
    // half-written file cannot snap a tuned value back to the default.
    g_sx = IniFloat("sx", g_sx);
    g_sy = IniFloat("sy", g_sy);
    InterlockedExchange(&g_enabled,
        GetPrivateProfileIntA("chusanwide", "enabled", (int)g_enabled, IniPath().c_str()) ? 1 : 0);
    InterlockedExchange(&g_gate,
        GetPrivateProfileIntA("chusanwide", "gate", (int)g_gate, IniPath().c_str()) ? 1 : 0);
    InterlockedExchange(&g_hotkeys,
        GetPrivateProfileIntA("chusanwide", "hotkeys", (int)g_hotkeys, IniPath().c_str()) ? 1 : 0);
    InterlockedExchange(&g_menuStretch,
        GetPrivateProfileIntA("chusanwide", "menu_stretch", (int)g_menuStretch, IniPath().c_str()) ? 1 : 0);
    g_sceneReg = GetPrivateProfileIntA("chusanwide", "scene_register", g_sceneReg, IniPath().c_str());

    RememberIniStamp();
    Note("[chusanwide] sx=%.4f sy=%.4f enabled=%d gate=%d menu_stretch=%d hotkeys=%d scene_register=%d",
         g_sx, g_sy, (int)g_enabled, (int)g_gate, (int)g_menuStretch, (int)g_hotkeys, g_sceneReg);
}

// ---------------------------------------------------------------------------
static inline bool IsUnit(float a, float b, float c, float d)
{
    return a == 0.0f && b == 0.0f && c == 0.0f && d == 1.0f;
}

static inline bool IsProjective(const float* m)
{
    return !IsUnit(m[12], m[13], m[14], m[15]) &&
           !IsUnit(m[3],  m[7],  m[11], m[15]);
}

static void RecordProjectiveRegister(UINT reg)
{
    for (int i = 0; i < g_projRegCount; ++i)
        if (g_projRegs[i] == reg) return;
    if (g_projRegCount < COUNTOF(g_projRegs))
        g_projRegs[g_projRegCount++] = reg;
}

// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI* tSetVSConstF)(IDirect3DDevice9*, UINT, const float*, UINT);
typedef HRESULT(WINAPI* tEndScene)(IDirect3DDevice9*);
typedef HRESULT(WINAPI* tDrawIndexed)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

static tSetVSConstF oSetVSConstF = nullptr;
static tEndScene    oEndScene    = nullptr;
static tDrawIndexed oDrawIndexed = nullptr;

static HRESULT WINAPI hkSetVSConstF(IDirect3DDevice9* dev, UINT startReg,
                                    const float* data, UINT count)
{
    if (data && count == 4 && IsProjective(data)) {
        RecordProjectiveRegister(startReg);
        g_activeProjReg = (int)startReg;

        const bool gate    = InterlockedCompareExchange(&g_gate, 1, 1) != 0;
        const bool isScene = (g_sceneReg < 0) || ((int)startReg == g_sceneReg);
        const float blend  = gate ? (isScene ? g_blend : 0.0f) : 1.0f;

        if (InterlockedCompareExchange(&g_enabled, 1, 1) && blend > 0.0f) {
            const float sx = 1.0f + (g_sx - 1.0f) * blend;
            const float sy = 1.0f + (g_sy - 1.0f) * blend;
            float m[16];
            memcpy(m, data, sizeof(m));
            // Register i is row i of the matrix.
            //   clip.x' = sx * clip.x
            //   clip.y' = sy * clip.y + (sy - 1) * clip.w
            // The second term anchors the bottom edge of the screen, so the
            // field grows upward from the controller rather than from the
            // middle of the screen.
            for (int i = 0; i < 4; ++i) {
                m[i]     = sx * data[i];
                m[4 + i] = sy * data[4 + i] + (sy - 1.0f) * data[12 + i];
            }
            return oSetVSConstF(dev, startReg, m, count);
        }
    }
    return oSetVSConstF(dev, startReg, data, count);
}

static HRESULT WINAPI hkDrawIndexed(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t,
                                    INT base, UINT minIdx, UINT numV,
                                    UINT startIdx, UINT primCount)
{
    if (g_activeProjReg >= 0 && (g_sceneReg < 0 || g_activeProjReg == g_sceneReg))
        g_sceneMeshDraws++;
    return oDrawIndexed(dev, t, base, minIdx, numV, startIdx, primCount);
}

// Menu button prompts. The region is 1273x84 centered at the bottom on 1080p
// Scaled to whatever size the backbuffer really is
static const float MENU_W = 1273.0f, MENU_H = 84.0f;

typedef HRESULT(WINAPI* tReset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static tReset oReset = nullptr;

static IDirect3DSurface9* g_menuTmp      = nullptr;
static bool               g_menuOff      = false;   // gave up, see log
static bool               g_resetHooked  = false;
static int                g_menuFailRun  = 0;

static void ReleaseMenuTmp()
{
    if (g_menuTmp) { g_menuTmp->Release(); g_menuTmp = nullptr; }
}

// Our scratch surface lives in the default pool, which has to be released
// before Reset or the Reset fails.
static HRESULT WINAPI hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    ReleaseMenuTmp();
    return oReset(dev, pp);
}

// Hooked from the live device rather than the dummy one, so it's the right
// function even if the game's device is an Ex device with its own vtable.
static void HookReset(IDirect3DDevice9* dev)
{
    g_resetHooked = true;
    void* fn = (*reinterpret_cast<void***>(dev))[16];
    if (MH_CreateHook(fn, reinterpret_cast<LPVOID>(&hkReset), (LPVOID*)&oReset) != MH_OK ||
        MH_EnableHook(fn) != MH_OK) {
        g_menuOff = true;
        Note("[chusanwide] couldn't hook Reset, menu stretch disabled");
    }
}

static void StretchMenu(IDirect3DDevice9* dev)
{
    if (g_menuOff ||
        !InterlockedCompareExchange(&g_menuStretch, 1, 1) ||
        !InterlockedCompareExchange(&g_enabled, 1, 1)) return;

    // on in the menus, off in gameplay, same easing as the other thing
    const float s = 1.0f + (g_sx - 1.0f) * (1.0f - g_blend);
    if (s <= 1.001f) return;

    if (!g_resetHooked) { HookReset(dev); if (g_menuOff) return; }

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);

    const int W = (int)d.Width, H = (int)d.Height;
    const int w = (int)(MENU_W * W / 1920.0f + 0.5f);
    const int h = (int)(MENU_H * H / 1080.0f + 0.5f);
    int dw = (int)(w * s + 0.5f);
    if (dw > W) dw = W;

    RECT src = { (W - w) / 2,  H - h, (W - w) / 2 + w,   H };
    RECT dst = { (W - dw) / 2, H - h, (W - dw) / 2 + dw, H };

    if (g_menuTmp) {
        D3DSURFACE_DESC t;
        g_menuTmp->GetDesc(&t);
        if ((int)t.Width != w || (int)t.Height != h || t.Format != d.Format) ReleaseMenuTmp();
    }

    // A surface can't be stretched onto an overlapping part of itself, so
    // copy the strip out first, then stretch it back in.
    HRESULT hr = S_OK;
    if (!g_menuTmp)
        hr = dev->CreateRenderTarget(w, h, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_menuTmp, NULL);
    if (SUCCEEDED(hr)) hr = dev->StretchRect(bb, &src, g_menuTmp, NULL, D3DTEXF_NONE);
    if (SUCCEEDED(hr)) hr = dev->StretchRect(g_menuTmp, NULL, bb, &dst, D3DTEXF_LINEAR);
    bb->Release();

    if (SUCCEEDED(hr)) { g_menuFailRun = 0; return; }

    // Expected briefly while the device is lost; persistent means it can't work here.
    ReleaseMenuTmp();
    if (++g_menuFailRun == 600) {
        g_menuOff = true;
        Note("[chusanwide] menu stretch keeps failing (hr=0x%08X, format %d, msaa %d), disabled",
             (unsigned)hr, (int)d.Format, (int)d.MultiSampleType);
    }
}

static HRESULT WINAPI hkEndScene(IDirect3DDevice9* dev)
{
    // A frame with a single camera can only be gameplay, so it is a safe
    // moment to learn which register that camera uses.
    if (g_projRegCount == 1 && g_sceneReg != (int)g_projRegs[0]) {
        g_sceneReg = (int)g_projRegs[0];
        char v[16];
        snprintf(v, sizeof(v), "%d", g_sceneReg);
        WritePrivateProfileStringA("chusanwide", "scene_register", v, IniPath().c_str());
        RememberIniStamp();
        Note("[chusanwide] scene camera is register c%d", g_sceneReg);
    }

    if (g_sceneMeshDraws > 0) {
        g_menuFrameRun = 0;
        g_blend = 1.0f;                  // instant on
    } else {
        g_menuFrameRun++;
        // Only decay if we were actually playing, so the menus at startup
        // never begin at full scale and fade out.
        if (g_blend > 0.0f && g_menuFrameRun > GATE_HOLD_FRAMES) {
            g_blend = 1.0f - (float)(g_menuFrameRun - GATE_HOLD_FRAMES) / GATE_RAMP_FRAMES;
            if (g_blend < 0.0f) g_blend = 0.0f;
        }
    }

    g_projRegCount   = 0;
    g_sceneMeshDraws = 0;
    g_activeProjReg  = -1;

    HRESULT hr = oEndScene(dev);
    StretchMenu(dev);
    return hr;
}

// ---------------------------------------------------------------------------
// Every IDirect3DDevice9 shares one vtable, so reading it from a throwaway
// device is enough to hook the game's real one without intercepting device
// creation and without fighting chusanhook over it.
// ---------------------------------------------------------------------------
static bool GrabVTable(void** out)
{
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { Note("[chusanwide] Direct3DCreate9 failed"); return false; }

    HWND hwnd = CreateWindowExA(0, "STATIC", "cw", WS_OVERLAPPED, 0, 0, 1, 1,
                                NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) { d3d->Release(); Note("[chusanwide] dummy window failed"); return false; }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow    = hwnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                                   D3DCREATE_NOWINDOWCHANGES, &pp, &dev);
    if (FAILED(hr))
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                               D3DCREATE_NOWINDOWCHANGES, &pp, &dev);
    if (FAILED(hr) || !dev) {
        DestroyWindow(hwnd); d3d->Release();
        Note("[chusanwide] CreateDevice failed (hr=0x%08X)", hr);
        return false;
    }

    memcpy(out, *reinterpret_cast<void***>(dev), sizeof(void*) * VT_COUNT);
    dev->Release(); d3d->Release(); DestroyWindow(hwnd);
    return true;
}

static bool GameHasFocus()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI HotkeyThread(LPVOID)
{
    int watchTick = 0;

    for (;;) {
        // Arrow keys only, and only while the game itself has focus
        if (InterlockedCompareExchange(&g_hotkeys, 1, 1) && GameHasFocus()) {
            if (GetAsyncKeyState(VK_LEFT)  & 1) { g_sx -= 0.005f; SaveSx(); Note("[chusanwide] sx=%.4f", g_sx); }
            if (GetAsyncKeyState(VK_RIGHT) & 1) { g_sx += 0.005f; SaveSx(); Note("[chusanwide] sx=%.4f", g_sx); }
        }

        // Roughly twice a second, check whether the ini changed on disk.
        if (++watchTick >= 16) {
            watchTick = 0;

            FILETIME ft;
            DWORD sizeLow = 0;
            if (IniStamp(&ft, &sizeLow) && sizeLow > 0 &&
                CompareFileTime(&ft, &g_iniStamp) != 0) {
                // Editors commonly truncate then rewrite, avoid reading a half-written file
                Sleep(200);
                FILETIME ft2;
                DWORD size2 = 0;
                if (IniStamp(&ft2, &size2) && size2 > 0) {
                    LoadIni();
                    Note("[chusanwide] reloaded ini");
                }
            }
        }

        Sleep(30);
    }
}

static DWORD WINAPI Setup(LPVOID)
{
    LoadIni();

    void* vt[VT_COUNT] = {};
    if (!GrabVTable(vt)) return 0;
    if (MH_Initialize() != MH_OK) { Note("[chusanwide] MH_Initialize failed"); return 0; }

    struct { int idx; LPVOID fn; LPVOID* orig; const char* name; } hooks[] = {
        { VT_SETVERTEXSHADERCONSTF, reinterpret_cast<LPVOID>(&hkSetVSConstF), (LPVOID*)&oSetVSConstF, "SetVertexShaderConstantF" },
        { VT_ENDSCENE,              reinterpret_cast<LPVOID>(&hkEndScene),    (LPVOID*)&oEndScene,    "EndScene" },
        { VT_DRAWINDEXEDPRIMITIVE,  reinterpret_cast<LPVOID>(&hkDrawIndexed), (LPVOID*)&oDrawIndexed, "DrawIndexedPrimitive" },
    };

    for (int i = 0; i < COUNTOF(hooks); ++i)
        if (MH_CreateHook(vt[hooks[i].idx], hooks[i].fn, hooks[i].orig) != MH_OK)
            Note("[chusanwide] failed to hook %s", hooks[i].name);

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { Note("[chusanwide] MH_EnableHook failed"); return 0; }

    Note("[chusanwide] ready");
    CreateThread(NULL, 0, HotkeyThread, NULL, 0, NULL);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, Setup, NULL, 0, NULL);
    }
    return TRUE;
}
