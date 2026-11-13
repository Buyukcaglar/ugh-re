/* host_win32.c - UGH!'s Windows x64 host (UGH64.exe, v1.0).
 *
 * The window shows the VGA picture scaled to the client area (4:3 aspect by default, optional integer
 * scaling and smoothing, the CRT scanlines of Civ64), passes the keyboard to the emulated PC as set-1
 * scancodes, a gamepad as the PC's analogue joystick and plays the sound through waveOut
 * (audio_win32.c).  The title bar, the system menu and the About box follow the system's light/dark
 * app mode, and change with it at once (the theme code is the one of the Civ64 project,
 * platform/win32/host_win32.c).  The system menu holds the host's commands: Full screen (Alt+Enter),
 * the display options, the sound (the DOS game's AdLib music or the Amiga version's music and
 * effects, Ctrl+Shift+A), Locate game files..., Locate Amiga disk..., About... and Close.
 * Settings and the high-score file live in %APPDATA%\UGH64. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../../hal/hal.h"
#include "audio_win32.h"
#include "../crt.h"
#include "../../audio/amiga_audio.h"
#include "../../audio/amiga_data.h"

#define APP_NAMEW L"UGH!"
#define APP_PRODUCTW L"UGH! 64bit"
#define APP_VERSIONW L"1.0"
#define ABOUT_URL L"https://github.com/Buyukcaglar/ugh-re"
#define INI_SECTION L"UGH64"

/* ---- state ---- */
static HWND g_hwnd;
static int g_quit;
static int g_running;               /* the emulation is up */
static char g_game_dir[MAX_PATH * 3];
static uint32_t g_frame[640 * 480];
static int g_fw = 320, g_fh = 200;
static int g_opt_crt = 0;   /* Display > CRT scanlines (crt.h) */
static int g_opt_aspect = 0, g_opt_integer = 1, g_opt_smooth = 0, g_opt_full = 0, g_opt_mute = 0, g_opt_pause_inactive = 1;
static int g_opt_amiga = 0;          /* Sound: the Amiga version's music and effects */
static char g_save_dir[MAX_PATH * 3];
static int g_inactive;               /* the window is not the foreground window */
static WINDOWPLACEMENT g_wp = {sizeof(WINDOWPLACEMENT)};
static int g_menu_open;             /* a modal loop (menu, About) runs: the game waits */
static uint8_t g_keys_down[512];    /* scancode | ext<<8 held */
static LARGE_INTEGER g_qpf;
static HANDLE g_timer;
static wchar_t g_ini[MAX_PATH];

/* ---- settings ---- */
static void ini_path(void)
{
    wchar_t dir[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir) != S_OK) {
        (void)GetModuleFileNameW(NULL, g_ini, MAX_PATH);
        *wcsrchr(g_ini, L'\\') = 0;
        (void)WideCharToMultiByte(CP_UTF8, 0, g_ini, -1, g_save_dir, (int)sizeof g_save_dir, NULL, NULL);
        (void)wcscat(g_ini, L"\\UGH64.ini");
        return;
    }
    (void)swprintf(g_ini, MAX_PATH, L"%ls\\UGH64", dir);
    (void)CreateDirectoryW(g_ini, NULL);
    (void)WideCharToMultiByte(CP_UTF8, 0, g_ini, -1, g_save_dir, (int)sizeof g_save_dir, NULL, NULL);
    (void)wcscat(g_ini, L"\\UGH64.ini");
}
const char *host_save_dir(void)
{
    return g_save_dir;
}
static int ini_int(const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(INI_SECTION, key, def, g_ini);
}
static void ini_set_int(const wchar_t *key, int v)
{
    wchar_t b[16];
    (void)swprintf(b, 16, L"%d", v);
    (void)WritePrivateProfileStringW(INI_SECTION, key, b, g_ini);
}
/* Display > scaling, one of three (a radio group): 4:3 aspect ratio (stretched to 4:3, sharp),
 * integer scaling (square pixels, whole multiples; the default) or smooth scaling (4:3, filtered) */
enum { SCALE_ASPECT, SCALE_INTEGER, SCALE_SMOOTH };
static int g_opt_scale = SCALE_INTEGER;
static void set_scale(int mode)
{
    if (mode < SCALE_ASPECT || mode > SCALE_SMOOTH) mode = SCALE_INTEGER;
    g_opt_scale = mode;
    g_opt_aspect = mode != SCALE_INTEGER;
    g_opt_integer = mode == SCALE_INTEGER;
    g_opt_smooth = mode == SCALE_SMOOTH;
}
static void save_options(void)
{
    ini_set_int(L"Scale", g_opt_scale);
    ini_set_int(L"Crt", g_opt_crt);
    ini_set_int(L"FullScreen", g_opt_full);
    ini_set_int(L"Mute", g_opt_mute);
    ini_set_int(L"PauseInactive", g_opt_pause_inactive);
    ini_set_int(L"AmigaSound", g_opt_amiga);
}

/* ---- the game folder ---- */
static int has_game(const wchar_t *dir)
{
    wchar_t p[MAX_PATH * 2];
    (void)swprintf(p, MAX_PATH * 2, L"%ls\\UGH.EXE", dir);
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return 1;
    (void)swprintf(p, MAX_PATH * 2, L"%ls\\UGH1.EXE", dir);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}
/* dir, or its UGH subfolder: the folder with UGH.EXE into out; 1 when found */
static int resolve_game_dir(const wchar_t *dir, wchar_t *out)
{
    wchar_t sub[MAX_PATH * 2];
    if (dir == NULL || dir[0] == 0) return 0;
    if (has_game(dir)) { wcsncpy(out, dir, MAX_PATH * 2 - 1); return 1; }
    (void)swprintf(sub, MAX_PATH * 2, L"%ls\\UGH", dir);
    if (has_game(sub)) { wcsncpy(out, sub, MAX_PATH * 2 - 1); return 1; }
    return 0;
}
static void set_game_dir(const wchar_t *w)
{
    (void)WideCharToMultiByte(CP_UTF8, 0, w, -1, g_game_dir, (int)sizeof g_game_dir, NULL, NULL);
}
const char *host_game_dir(void)
{
    return g_game_dir;
}

/* the folder picker; 1 when a folder with the game was chosen */
static int pick_game_dir(HWND owner, wchar_t *out)
{
    IFileOpenDialog *d = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    for (;;) {
        DWORD opts = 0;
        IShellItem *it = NULL;
        PWSTR path = NULL;
        (void)d->lpVtbl->GetOptions(d, &opts);
        (void)d->lpVtbl->SetOptions(d, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        (void)d->lpVtbl->SetTitle(d, L"Locate the UGH! game folder (the one with UGH.EXE)");
        if (FAILED(d->lpVtbl->Show(d, owner))) break;
        if (SUCCEEDED(d->lpVtbl->GetResult(d, &it)) && SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &path))) {
            ok = resolve_game_dir(path, out);
            CoTaskMemFree(path);
        }
        if (it) it->lpVtbl->Release(it);
        if (ok) break;
        if (MessageBoxW(owner, L"UGH.EXE was not found in that folder or in its UGH subfolder.\n\n"
                               L"Please choose the folder of your UGH! (DOS) installation.",
                        APP_NAMEW, MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY)
            break;
    }
    d->lpVtbl->Release(d);
    return ok;
}

/* a private build (build.cmd private) carries the game files as RCDATA 100 (tools/mkpak.py) */
static int load_embedded_pack(void)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(100), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g;
    const void *p;
    if (r == NULL || (g = LoadResource(NULL, r)) == NULL || (p = LockResource(g)) == NULL) return 0;
    if (!hal_set_pack((const uint8_t *)p, (size_t)SizeofResource(NULL, r))) return 0;
    (void)strcpy(g_game_dir, ".");
    return 1;
}

static int find_game_dir(const wchar_t *cmdline_dir)
{
    wchar_t found[MAX_PATH * 2], buf[MAX_PATH * 2], *slash;
    if (resolve_game_dir(cmdline_dir, found)) goto ok;
    (void)GetPrivateProfileStringW(INI_SECTION, L"GameDir", L"", buf, MAX_PATH * 2, g_ini);
    if (resolve_game_dir(buf, found)) goto ok;
    (void)GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
    if ((slash = wcsrchr(buf, L'\\')) != NULL) *slash = 0;
    if (resolve_game_dir(buf, found)) goto ok;
    (void)GetCurrentDirectoryW(MAX_PATH * 2, buf);
    if (resolve_game_dir(buf, found)) goto ok;
    MessageBoxW(NULL, L"UGH! needs the files of the original DOS game.\n\n"
                      L"In the next window, choose the folder of your UGH! installation "
                      L"(the folder with UGH.EXE, or the one above it).",
                APP_NAMEW, MB_OK | MB_ICONINFORMATION);
    if (!pick_game_dir(NULL, found)) return 0;
ok:
    (void)WritePrivateProfileStringW(INI_SECTION, L"GameDir", found, g_ini);
    set_game_dir(found);
    return 1;
}

/* ---- theme (the Civ64 code) ---- */
#define THEME_DARK_BG RGB(32, 32, 32)
#define THEME_DARK_FIELD RGB(43, 43, 43)
#define THEME_DARK_TEXT RGB(255, 255, 255)
#define THEME_DARK_LINK RGB(96, 205, 255)
typedef int(WINAPI *ThemeModeFn)(int);
typedef BOOL(WINAPI *ThemeWindowFn)(HWND, BOOL);
typedef void(WINAPI *ThemeVoidFn)(void);
static ThemeModeFn g_ux_mode;
static ThemeWindowFn g_ux_window;
static ThemeVoidFn g_ux_refresh;
static ThemeVoidFn g_ux_flush;
static int g_ux_tried;
static DWORD g_os_build;
static int g_dark;

static DWORD os_build(void)
{
    typedef LONG(WINAPI * VersionFn)(OSVERSIONINFOW *);
    OSVERSIONINFOW v;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    VersionFn fn = nt != NULL ? (VersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (fn == NULL || fn(&v) != 0 || v.dwMajorVersion < 10u) return 0u;
    return v.dwBuildNumber;
}
static void theme_load(void)
{
    HMODULE ux;
    if (g_ux_tried) return;
    g_ux_tried = 1;
    g_os_build = os_build();
    if (g_os_build < 18362u || (ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)) == NULL) return;
    g_ux_mode = (ThemeModeFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    g_ux_window = (ThemeWindowFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    g_ux_refresh = (ThemeVoidFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(104));
    g_ux_flush = (ThemeVoidFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (g_ux_mode == NULL || g_ux_window == NULL || g_ux_refresh == NULL || g_ux_flush == NULL) {
        g_ux_mode = NULL; g_ux_window = NULL; g_ux_refresh = NULL; g_ux_flush = NULL;
    }
}
static int theme_wanted(void)
{
    DWORD v = 1u, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &n) != ERROR_SUCCESS)
        return 0;
    return v == 0u;
}
static int theme_setting(LPARAM l)
{
    return l != 0 && CompareStringOrdinal((LPCWCH)l, -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL;
}
static void theme_window(HWND h, int dark)
{
    BOOL on = dark ? TRUE : FALSE;
    theme_load();
    if (g_ux_window != NULL) (void)g_ux_window(h, on);
    if (g_os_build >= 17763u) (void)DwmSetWindowAttribute(h, g_os_build >= 18985u ? 20u : 19u, &on, sizeof on);
}
static void theme_apply(int redraw)
{
    theme_load();
    g_dark = theme_wanted();
    if (g_ux_mode != NULL) {
        (void)g_ux_mode(g_dark ? 2 : 3);
        g_ux_refresh();
        g_ux_flush();
    }
    if (g_hwnd != NULL) {
        theme_window(g_hwnd, g_dark);
        if (redraw)
            (void)SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

/* ---- the About box ---- */
#define IDC_ABOUT_ICON 201
#define IDC_ABOUT_LINK 202
#define IDC_ABOUT_LICENSE 203
static const wchar_t g_license[] =
    L"MIT License\r\n\r\n"
    L"Copyright (c) 2026 Onur BUYUKCAGLAR\r\n\r\n"
    L"Permission is hereby granted, free of charge, to any person obtaining a copy of this "
    L"software and associated documentation files (the \"Software\"), to deal in the Software "
    L"without restriction, including without limitation the rights to use, copy, modify, merge, "
    L"publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons "
    L"to whom the Software is furnished to do so, subject to the following conditions:\r\n\r\n"
    L"The above copyright notice and this permission notice shall be included in all copies or "
    L"substantial portions of the Software.\r\n\r\n"
    L"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, "
    L"INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR "
    L"PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE "
    L"FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR "
    L"OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER "
    L"DEALINGS IN THE SOFTWARE.\r\n\r\n"
    L"The licence covers this port's own code (" APP_PRODUCTW L"). UGH! (Play Byte, 1992), its DOS "
    L"and Amiga programs, music and data belong to their owners; they are not part of this software, "
    L"which needs a copy of the original game to run.\r\n\r\n"
    L"The YM3812 (AdLib) emulation and the CRT scanlines filter come from the Civ64 project by the "
    L"same author, under the same licence.";

static int g_about_dark;
static HBRUSH g_about_bg, g_about_field;

typedef struct Tmpl {
    WORD buf[1024];
    size_t n;
} Tmpl;
static void t_word(Tmpl *t, WORD w) { if (t->n < 1024u) t->buf[t->n++] = w; }
static void t_str(Tmpl *t, const wchar_t *s) { do { t_word(t, (WORD)*s); } while (*s++ != L'\0'); }
static void t_align(Tmpl *t) { if ((t->n & 1u) != 0u) t_word(t, 0); }
static void t_item(Tmpl *t, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t *text)
{
    t_align(t);
    t_word(t, LOWORD(style)); t_word(t, HIWORD(style));
    t_word(t, 0); t_word(t, 0);
    t_word(t, (WORD)x); t_word(t, (WORD)y); t_word(t, (WORD)cx); t_word(t, (WORD)cy);
    t_word(t, id);
    t_word(t, 0xFFFF); t_word(t, cls);
    t_str(t, text);
    t_word(t, 0);
}

static void about_theme(HWND dlg)
{
    HWND c = GetDlgItem(dlg, IDC_ABOUT_LICENSE);
    g_about_dark = theme_wanted();
    theme_window(dlg, g_about_dark);
    if (c != NULL) {
        if (g_ux_window != NULL) (void)g_ux_window(c, g_about_dark ? TRUE : FALSE);
        (void)SetWindowTheme(c, g_about_dark ? L"DarkMode_Explorer" : NULL, NULL);
        (void)SendMessageW(c, WM_THEMECHANGED, 0u, 0);
    }
    if ((c = GetDlgItem(dlg, IDOK)) != NULL) (void)InvalidateRect(c, NULL, TRUE);
}

/* the OK button, owner-drawn: no theme draws a dark push button */
static void box_button(const DRAWITEMSTRUCT *di, int dark, HBRUSH bg)
{
    RECT r = di->rcItem;
    wchar_t text[32];
    int pressed = (di->itemState & ODS_SELECTED) != 0u;
    int focus = (di->itemState & ODS_FOCUS) != 0u;
    HFONT font = (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0u, 0);
    HGDIOBJ old = font != NULL ? SelectObject(di->hDC, font) : NULL;
    text[0] = L'\0';
    (void)GetWindowTextW(di->hwndItem, text, 32);
    if (dark) {
        HBRUSH face = CreateSolidBrush(pressed ? RGB(45, 45, 45) : RGB(60, 60, 60));
        HBRUSH edge = CreateSolidBrush(focus ? THEME_DARK_LINK : RGB(100, 100, 100));
        if (bg != NULL) (void)FillRect(di->hDC, &r, bg);
        if (face != NULL) { (void)FillRect(di->hDC, &r, face); (void)DeleteObject(face); }
        if (edge != NULL) { (void)FrameRect(di->hDC, &r, edge); (void)DeleteObject(edge); }
        (void)SetTextColor(di->hDC, THEME_DARK_TEXT);
    } else {
        HTHEME th = OpenThemeData(di->hwndItem, L"BUTTON");
        if (th != NULL) {
            (void)FillRect(di->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
            (void)DrawThemeBackground(th, di->hDC, BP_PUSHBUTTON, pressed ? PBS_PRESSED : PBS_DEFAULTED, &r, NULL);
            (void)CloseThemeData(th);
        } else {
            (void)DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pressed ? DFCS_PUSHED : 0u));
        }
        (void)SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
        if (focus && (di->itemState & ODS_NOFOCUSRECT) == 0u) {
            RECT f = r;
            (void)InflateRect(&f, -3, -3);
            (void)DrawFocusRect(di->hDC, &f);
        }
    }
    (void)SetBkMode(di->hDC, TRANSPARENT);
    if (pressed) (void)OffsetRect(&r, 1, 1);
    (void)DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | ((di->itemState & ODS_NOACCEL) != 0u ? DT_HIDEPREFIX : 0u));
    if (old != NULL) (void)SelectObject(di->hDC, old);
}

static HICON g_about_icon;   /* the About box's large icon (64 px at 96 dpi) */

static INT_PTR CALLBACK about_proc(HWND dlg, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_INITDIALOG: {
        int px = MulDiv(64, (int)GetDpiForWindow(dlg), 96);
        g_about_icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, 0);
        if (g_about_icon != NULL) (void)SendDlgItemMessageW(dlg, IDC_ABOUT_ICON, STM_SETICON, (WPARAM)g_about_icon, 0);
        (void)SetDlgItemTextW(dlg, IDC_ABOUT_LICENSE, g_license);
        g_about_bg = CreateSolidBrush(THEME_DARK_BG);
        g_about_field = CreateSolidBrush(THEME_DARK_FIELD);
        about_theme(dlg);
        (void)SetFocus(GetDlgItem(dlg, IDOK));
        return FALSE;
    }
    case WM_DESTROY:
        if (g_about_bg != NULL) { (void)DeleteObject(g_about_bg); g_about_bg = NULL; }
        if (g_about_field != NULL) { (void)DeleteObject(g_about_field); g_about_field = NULL; }
        if (g_about_icon != NULL) { (void)DestroyIcon(g_about_icon); g_about_icon = NULL; }
        break;
    case WM_SETTINGCHANGE:
        if (theme_setting(l)) {
            about_theme(dlg);
            (void)RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        break;
    case WM_DRAWITEM:
        if (w == IDOK) { box_button((const DRAWITEMSTRUCT *)l, g_about_dark, g_about_bg); return TRUE; }
        break;
    case WM_CTLCOLORDLG:
        if (g_about_dark && g_about_bg != NULL) return (INT_PTR)g_about_bg;
        break;
    case WM_CTLCOLORSTATIC: {
        int dark = g_about_dark && g_about_bg != NULL && g_about_field != NULL;
        if ((HWND)l == GetDlgItem(dlg, IDC_ABOUT_LINK)) {
            (void)SetTextColor((HDC)w, dark ? THEME_DARK_LINK : RGB(0, 0, 204));
            (void)SetBkMode((HDC)w, TRANSPARENT);
            return (INT_PTR)(dark ? g_about_bg : GetSysColorBrush(COLOR_BTNFACE));
        }
        if (dark) {
            int field = (HWND)l == GetDlgItem(dlg, IDC_ABOUT_LICENSE);
            (void)SetTextColor((HDC)w, THEME_DARK_TEXT);
            (void)SetBkColor((HDC)w, field ? THEME_DARK_FIELD : THEME_DARK_BG);
            return (INT_PTR)(field ? g_about_field : g_about_bg);
        }
        break;
    }
    case WM_SETCURSOR:
        if ((HWND)w == GetDlgItem(dlg, IDC_ABOUT_LINK)) {
            (void)SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32649)));
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_ABOUT_LINK && HIWORD(w) == STN_CLICKED) {
            (void)ShellExecuteW(dlg, L"open", ABOUT_URL, NULL, NULL, SW_SHOWNORMAL);
            return TRUE;
        }
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) { EndDialog(dlg, 1); return TRUE; }
        break;
    default:
        break;
    }
    return FALSE;
}

static void show_about(void)
{
    Tmpl t;
    DWORD style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    memset(&t, 0, sizeof t);
    t_word(&t, LOWORD(style)); t_word(&t, HIWORD(style));
    t_word(&t, 0); t_word(&t, 0);
    t_word(&t, 7);
    t_word(&t, 0); t_word(&t, 0); t_word(&t, 260); t_word(&t, 206);
    t_word(&t, 0); t_word(&t, 0);
    t_str(&t, L"About " APP_PRODUCTW L" v" APP_VERSIONW);
    t_word(&t, 9);
    t_str(&t, L"Segoe UI");
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_ICON | SS_REALSIZEIMAGE, 10, 8, 37, 34, IDC_ABOUT_ICON, 0x0082, L"");
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 56, 12, 194, 9, 0xFFFF, 0x0082, L"64bit version by Onur BUYUKCAGLAR");
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 56, 23, 194, 9, 0xFFFF, 0x0082, L"with Anthropic Claude AI assistance");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | SS_LEFT | SS_NOTIFY, 56, 34, 194, 9, IDC_ABOUT_LINK, 0x0082, ABOUT_URL);
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 58, 240, 9, 0xFFFF, 0x0082, L"Licenses:");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
           10, 69, 240, 108, IDC_ABOUT_LICENSE, 0x0081, L"");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 200, 184, 50, 15, IDOK, 0x0080, L"OK");
    g_menu_open++;
    (void)DialogBoxIndirectParamW(GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)t.buf, g_hwnd, about_proc, 0);
    g_menu_open--;
}

/* ---- the picture ---- */
static void dest_rect(int cw, int ch, RECT *r)
{
    double sw = g_fw, sh = g_fh, dw, dh;
    if (g_opt_aspect && g_fw * 3 != g_fh * 4) {   /* 320x200 and 640x400 shown 4:3 */
        sh = sw * 3.0 / 4.0;
    }
    if (g_opt_integer) {
        int k = 1;
        while ((k + 1) * g_fw <= cw && (int)((k + 1) * sh * g_fh / g_fh) <= ch) k++;
        dw = k * sw;
        dh = k * sh;
        if (dw > cw || dh > ch) { dw = sw; dh = sh; }
    } else {
        double s = (double)cw / sw < (double)ch / sh ? (double)cw / sw : (double)ch / sh;
        dw = sw * s;
        dh = sh * s;
    }
    r->left = (LONG)((cw - dw) / 2.0 + 0.5);
    r->top = (LONG)((ch - dh) / 2.0 + 0.5);
    r->right = r->left + (LONG)(dw + 0.5);
    r->bottom = r->top + (LONG)(dh + 0.5);
}

/* CRT scanlines: the scaled picture is made in a DIB section, filtered row by row (crt.h), then copied */
static HDC g_crt_dc;
static HBITMAP g_crt_bmp, g_crt_old;
static uint32_t *g_crt_bits;
static int g_crt_w, g_crt_h, g_crt_lines, g_crt_ph;
static uint8_t g_crt_lvl[8192];

/* the picture's height full screen on the window's monitor: the scanlines are spaced as there */
static int crt_period_rows(int dh)
{
    MONITORINFO mi;
    RECT f;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return dh;
    dest_rect(mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, &f);
    return f.bottom - f.top > 0 ? f.bottom - f.top : dh;
}

static int crt_surface(HDC dc, int w, int h)
{
    BITMAPINFO bi;
    void *bits = NULL;
    if (g_crt_bmp != NULL && g_crt_w == w && g_crt_h == h) return 1;
    if (g_crt_bmp != NULL) {
        (void)SelectObject(g_crt_dc, g_crt_old);
        (void)DeleteObject(g_crt_bmp);
        g_crt_bmp = NULL;
    }
    if (g_crt_dc == NULL && (g_crt_dc = CreateCompatibleDC(dc)) == NULL) return 0;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_crt_bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (g_crt_bmp == NULL) return 0;
    g_crt_old = (HBITMAP)SelectObject(g_crt_dc, g_crt_bmp);
    g_crt_bits = (uint32_t *)bits;
    g_crt_w = w;
    g_crt_h = h;
    g_crt_lines = 0;
    return 1;
}

static void paint(HDC dc)
{
    RECT cr, d;
    BITMAPINFO bi;
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT bar;
    (void)GetClientRect(g_hwnd, &cr);
    dest_rect(cr.right, cr.bottom, &d);
    bar = cr; bar.bottom = d.top; (void)FillRect(dc, &bar, black);
    bar = cr; bar.top = d.bottom; (void)FillRect(dc, &bar, black);
    bar = cr; bar.top = d.top; bar.bottom = d.bottom; bar.right = d.left; (void)FillRect(dc, &bar, black);
    bar = cr; bar.top = d.top; bar.bottom = d.bottom; bar.left = d.right; (void)FillRect(dc, &bar, black);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = g_fw;
    bi.bmiHeader.biHeight = -g_fh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    if (g_opt_crt && d.bottom - d.top <= (LONG)sizeof g_crt_lvl && d.right - d.left <= 8192 && crt_surface(dc, d.right - d.left, d.bottom - d.top)) {
        int w = d.right - d.left, h = d.bottom - d.top, y, lines = 2 * g_fh, ph = crt_period_rows(h);
        if (g_crt_lines != lines || g_crt_ph != ph) {
            crt_levels(g_crt_lvl, h, lines, ph);
            g_crt_lines = lines;
            g_crt_ph = ph;
        }
        (void)GdiFlush();
        if (g_opt_smooth) {                            /* GDI's smoothing, then every pixel through the filter */
            (void)SetStretchBltMode(g_crt_dc, HALFTONE);
            (void)SetBrushOrgEx(g_crt_dc, 0, 0, NULL);
            (void)StretchDIBits(g_crt_dc, 0, 0, w, h, 0, 0, g_fw, g_fh, g_frame, &bi, DIB_RGB_COLORS, SRCCOPY);
            (void)GdiFlush();
            for (y = 0; y < h; y++) crt_row(g_crt_bits + (size_t)y * (size_t)w, w, g_crt_lvl[y]);
        } else {
            /* sharp scaling: a source row is filtered once per weight (fw pixels) and widened by nearest
             * neighbour; a row equal to the one above (same source row and weight) is a copy */
            static uint32_t tmp[640];
            static int xmap[8192];
            int x, prev_sy = -1, prev_l = -1;
            for (x = 0; x < w; x++) xmap[x] = (int)(((int64_t)x * g_fw) / w);
            for (y = 0; y < h; y++) {
                int sy = (int)(((int64_t)y * g_fh) / h), l = g_crt_lvl[y];
                uint32_t *row = g_crt_bits + (size_t)y * (size_t)w;
                if (sy == prev_sy && l == prev_l) {
                    memcpy(row, row - w, (size_t)w * 4u);
                    continue;
                }
                memcpy(tmp, g_frame + (size_t)sy * (size_t)g_fw, (size_t)g_fw * 4u);
                crt_row(tmp, g_fw, l);
                for (x = 0; x < w; x++) row[x] = tmp[xmap[x]];
                prev_sy = sy;
                prev_l = l;
            }
        }
        (void)BitBlt(dc, d.left, d.top, w, h, g_crt_dc, 0, 0, SRCCOPY);
        return;
    }
    (void)SetStretchBltMode(dc, g_opt_smooth ? HALFTONE : COLORONCOLOR);
    if (g_opt_smooth) (void)SetBrushOrgEx(dc, 0, 0, NULL);
    (void)StretchDIBits(dc, d.left, d.top, d.right - d.left, d.bottom - d.top, 0, 0, g_fw, g_fh, g_frame, &bi,
                        DIB_RGB_COLORS, SRCCOPY);
}

void host_present(const HostFrame *f)
{
    HDC dc;
    g_fw = f->width;
    g_fh = f->height;
    memcpy(g_frame, f->pixels, (size_t)f->width * (size_t)f->height * 4u);
    if (g_hwnd == NULL || IsIconic(g_hwnd)) return;
    dc = GetDC(g_hwnd);
    paint(dc);
    (void)ReleaseDC(g_hwnd, dc);
}

/* ---- time ---- */
uint64_t host_time_us(void)
{
    LARGE_INTEGER c;
    (void)QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1000000.0 / (double)g_qpf.QuadPart);
}
void host_sleep_us(uint64_t us)
{
    if (us > 200000u) us = 200000u;
    if (g_timer != NULL) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(us * 10u);
        if (SetWaitableTimerEx(g_timer, &due, 0, NULL, NULL, NULL, 0)) {
            (void)WaitForSingleObject(g_timer, INFINITE);
            return;
        }
    }
    Sleep((DWORD)(us / 1000u));
}

void host_audio(const int16_t *s, int n)
{
    audio_push(s, n);
}

int host_quit_requested(void)
{
    return g_quit;
}

void hal_fatal(const char *msg)
{
    wchar_t w[600];
    (void)MultiByteToWideChar(CP_UTF8, 0, msg, -1, w, 600);
    MessageBoxW(g_hwnd, w, APP_NAMEW L" - internal error", MB_OK | MB_ICONERROR);
    ExitProcess(3);
}

/* ---- full screen ---- */
static void set_full(int on)
{
    DWORD st = (DWORD)GetWindowLongW(g_hwnd, GWL_STYLE);
    if (on) {
        MONITORINFO mi = {sizeof mi};
        if (!g_opt_full || (st & WS_OVERLAPPEDWINDOW)) (void)GetWindowPlacement(g_hwnd, &g_wp);
        (void)GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        (void)SetWindowLongW(g_hwnd, GWL_STYLE, (LONG)(st & ~(DWORD)WS_OVERLAPPEDWINDOW) | WS_POPUP);
        (void)SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                           mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else if (!(st & WS_OVERLAPPEDWINDOW)) {
        (void)SetWindowLongW(g_hwnd, GWL_STYLE, (LONG)((st & ~(DWORD)WS_POPUP) | WS_OVERLAPPEDWINDOW));
        (void)SetWindowPlacement(g_hwnd, &g_wp);
        (void)SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_opt_full = on;
    save_options();
}

/* ---- the system menu ---- */
#define SC_UGH_FULL 0x1010u
#define SC_UGH_ASPECT 0x1020u
#define SC_UGH_INTEGER 0x1030u
#define SC_UGH_SMOOTH 0x1040u
#define SC_UGH_MUTE 0x1050u
#define SC_UGH_LOCATE 0x1060u
#define SC_UGH_RESTART 0x1070u
#define SC_UGH_ABOUT 0x1080u
#define SC_UGH_PAUSE 0x1090u
#define SC_UGH_CRT 0x10A0u
#define SC_UGH_SND_DOS 0x10B0u
#define SC_UGH_SND_AMIGA 0x10C0u
#define SC_UGH_LOCATE_AMIGA 0x10D0u

static void update_menu(void)
{
    HMENU m = GetSystemMenu(g_hwnd, FALSE);
    if (m == NULL) return;
    (void)CheckMenuItem(m, SC_UGH_FULL, MF_BYCOMMAND | (g_opt_full ? MF_CHECKED : MF_UNCHECKED));
    (void)CheckMenuRadioItem(m, SC_UGH_ASPECT, SC_UGH_SMOOTH,
                             g_opt_scale == SCALE_ASPECT ? SC_UGH_ASPECT : g_opt_scale == SCALE_SMOOTH ? SC_UGH_SMOOTH : SC_UGH_INTEGER,
                             MF_BYCOMMAND);
    (void)CheckMenuItem(m, SC_UGH_CRT, MF_BYCOMMAND | (g_opt_crt ? MF_CHECKED : MF_UNCHECKED));
    (void)CheckMenuItem(m, SC_UGH_MUTE, MF_BYCOMMAND | (g_opt_mute ? MF_CHECKED : MF_UNCHECKED));
    (void)CheckMenuItem(m, SC_UGH_PAUSE, MF_BYCOMMAND | (g_opt_pause_inactive ? MF_CHECKED : MF_UNCHECKED));
    (void)CheckMenuRadioItem(m, SC_UGH_SND_DOS, SC_UGH_SND_AMIGA, g_opt_amiga ? SC_UGH_SND_AMIGA : SC_UGH_SND_DOS, MF_BYCOMMAND);
}

/* ---- the Amiga sound: its TFMX module, from the Amiga disk image (ADF) ----
 * Looked for: the saved path, then any *.adf in the game folder, beside the program, in an "amiga"
 * folder next to either, and in the folder above the game folder's. */
static int try_adf(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER n;
    uint8_t *b;
    DWORD got = 0;
    int ok = 0;
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (GetFileSizeEx(f, &n) && n.QuadPart > 0 && n.QuadPart <= 4 * 1024 * 1024 &&
        (b = (uint8_t *)malloc((size_t)n.QuadPart)) != NULL) {
        if (ReadFile(f, b, (DWORD)n.QuadPart, &got, NULL) && got == (DWORD)n.QuadPart)
            ok = amiga_data_load_adf(b, (size_t)got);
        free(b);
    }
    CloseHandle(f);
    return ok;
}
static int try_adf_dir(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t pat[MAX_PATH * 2], p[MAX_PATH * 2];
    HANDLE h;
    int ok = 0;
    (void)swprintf(pat, MAX_PATH * 2, L"%ls\\*.adf", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        (void)swprintf(p, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
        if (try_adf(p)) {
            (void)WritePrivateProfileStringW(INI_SECTION, L"AmigaDisk", p, g_ini);
            ok = 1;
        }
    } while (!ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}
static void find_amiga_disk_now(void)
{
    wchar_t buf[MAX_PATH * 2], gd[MAX_PATH * 2], sub[MAX_PATH * 2], *slash;
    if (amiga_audio_ready()) return;
    {   /* a private build may carry the disk in its pack */
        size_t n;
        const uint8_t *adf = hal_pack_find("AMIGA.ADF", &n);
        if (adf != NULL && amiga_data_load_adf(adf, n)) return;
    }
    (void)GetPrivateProfileStringW(INI_SECTION, L"AmigaDisk", L"", buf, MAX_PATH * 2, g_ini);
    if (buf[0] && try_adf(buf)) return;
    (void)MultiByteToWideChar(CP_UTF8, 0, g_game_dir, -1, gd, MAX_PATH * 2);
    if (try_adf_dir(gd)) return;
    (void)swprintf(sub, MAX_PATH * 2, L"%ls\\amiga", gd);
    if (try_adf_dir(sub)) return;
    if ((slash = wcsrchr(gd, L'\\')) != NULL) {
        *slash = 0;
        if (try_adf_dir(gd)) return;
        (void)swprintf(sub, MAX_PATH * 2, L"%ls\\amiga", gd);
        if (try_adf_dir(sub)) return;
    }
    (void)GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
    if ((slash = wcsrchr(buf, L'\\')) != NULL) *slash = 0;
    if (try_adf_dir(buf)) return;
    (void)swprintf(sub, MAX_PATH * 2, L"%ls\\amiga", buf);
    (void)try_adf_dir(sub);
}
/* the Amiga music runs beside the DOS music all the time (so that Ctrl+Shift+A switches at once);
 * a disk found after the game started its tune starts that tune now */
static void find_amiga_disk(void)
{
    int was = amiga_audio_ready();
    find_amiga_disk_now();
    if (!was && amiga_audio_ready()) game_amiga_resume();
}
static void pick_amiga_disk(HWND owner)
{
    IFileOpenDialog *d = NULL;
    COMDLG_FILTERSPEC spec[2] = {{L"Amiga disk images (*.adf)", L"*.adf"}, {L"All files", L"*.*"}};
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return;
    (void)d->lpVtbl->SetFileTypes(d, 2, spec);
    (void)d->lpVtbl->SetTitle(d, L"Locate the UGH! Amiga disk image (ADF)");
    if (SUCCEEDED(d->lpVtbl->Show(d, owner))) {
        IShellItem *it = NULL;
        PWSTR path = NULL;
        if (SUCCEEDED(d->lpVtbl->GetResult(d, &it)) && SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &path))) {
            int was = amiga_audio_ready();
            if (try_adf(path)) {
                (void)WritePrivateProfileStringW(INI_SECTION, L"AmigaDisk", path, g_ini);
                if (!was) game_amiga_resume();
            } else MessageBoxW(owner, L"That file is not the UGH! Amiga disk: its TFMX music (mdat.UGH!-All_Sounds.o "
                                    L"and smpl.UGH!-All_Sounds) was not found in it.", APP_NAMEW, MB_OK | MB_ICONWARNING);
            CoTaskMemFree(path);
        }
        if (it) it->lpVtbl->Release(it);
    }
    d->lpVtbl->Release(d);
}

/* the sound source: the DOS game's, or the Amiga version's (when its disk was found) */
static void set_amiga_sound(int on)
{
    if (on && !amiga_audio_ready()) {
        find_amiga_disk();
        if (!amiga_audio_ready()) {
            g_menu_open++;
            if (MessageBoxW(g_hwnd, L"The Amiga sound needs the UGH! Amiga disk image (an .adf file).\n\n"
                                    L"Locate it now?", APP_NAMEW, MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                pick_amiga_disk(g_hwnd);
            g_menu_open--;
            if (!amiga_audio_ready()) on = 0;
        }
    }
    g_opt_amiga = on;
    hal_set_audio_source(on);
    save_options();
    update_menu();
}

static void add_menu(void)
{
    HMENU m, disp;
    int n;
    (void)GetSystemMenu(g_hwnd, TRUE);
    m = GetSystemMenu(g_hwnd, FALSE);
    if (m == NULL) return;
    (void)DeleteMenu(m, SC_CLOSE, MF_BYCOMMAND);
    n = GetMenuItemCount(m);
    if (n > 0 && (GetMenuState(m, (UINT)(n - 1), MF_BYPOSITION) & MF_SEPARATOR) != 0u)
        (void)DeleteMenu(m, (UINT)(n - 1), MF_BYPOSITION);
    (void)AppendMenuW(m, MF_STRING, SC_UGH_FULL, L"&Full screen\tAlt+Enter");
    disp = CreatePopupMenu();
    (void)AppendMenuW(disp, MF_STRING, SC_UGH_ASPECT, L"&4:3 aspect ratio");
    (void)AppendMenuW(disp, MF_STRING, SC_UGH_INTEGER, L"&Integer scaling");
    (void)AppendMenuW(disp, MF_STRING, SC_UGH_SMOOTH, L"&Smooth scaling");
    (void)AppendMenuW(disp, MF_SEPARATOR, 0u, NULL);
    (void)AppendMenuW(disp, MF_STRING, SC_UGH_CRT, L"&CRT scanlines");
    (void)AppendMenuW(m, MF_POPUP, (UINT_PTR)disp, L"&Display");
    {
        HMENU snd = CreatePopupMenu();
        (void)AppendMenuW(snd, MF_STRING, SC_UGH_SND_DOS, L"&DOS (AdLib)\tCtrl+Shift+A");
        (void)AppendMenuW(snd, MF_STRING, SC_UGH_SND_AMIGA, L"&Amiga (TFMX)\tCtrl+Shift+A");
        (void)AppendMenuW(snd, MF_SEPARATOR, 0u, NULL);
        (void)AppendMenuW(snd, MF_STRING, SC_UGH_LOCATE_AMIGA, L"&Locate Amiga disk…");
        (void)AppendMenuW(m, MF_POPUP, (UINT_PTR)snd, L"&Sound");
    }
    (void)AppendMenuW(m, MF_STRING, SC_UGH_MUTE, L"&Mute sound");
    (void)AppendMenuW(m, MF_STRING, SC_UGH_PAUSE, L"&Pause when inactive");
    (void)AppendMenuW(m, MF_SEPARATOR, 0u, NULL);
    (void)AppendMenuW(m, MF_STRING, SC_UGH_RESTART, L"&Restart game");
    if (!hal_have_pack()) (void)AppendMenuW(m, MF_STRING, SC_UGH_LOCATE, L"&Locate game files…");
    (void)AppendMenuW(m, MF_SEPARATOR, 0u, NULL);
    (void)AppendMenuW(m, MF_STRING, SC_UGH_ABOUT, L"&About…");
    (void)AppendMenuW(m, MF_SEPARATOR, 0u, NULL);
    (void)AppendMenuW(m, MF_STRING, SC_CLOSE, L"&Close\tAlt+F4");
    (void)SetMenuDefaultItem(m, SC_CLOSE, FALSE);
    update_menu();
}

/* ---- keyboard ---- */
static void release_keys(void)
{
    int i;
    for (i = 0; i < 512; i++) {
        if (g_keys_down[i]) {
            g_keys_down[i] = 0;
            if (g_running) hal_key((uint8_t)(i & 0x7F), 0, i >= 256);
        }
    }
}

static void key_event(WPARAM vk, LPARAM l, int down)
{
    int sc = (int)((l >> 16) & 0x7F);
    int ext = (l & (1 << 24)) != 0;
    if (vk == VK_PAUSE) return;
    if (sc == 0) sc = (int)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    if (sc == 0x45 && !ext) ext = 0;
    g_keys_down[sc | (ext ? 256 : 0)] = (uint8_t)down;
    if (g_running) hal_key((uint8_t)sc, down, ext);
}

static int start_game(void)
{
    char err[512];
    release_keys();
    g_running = 0;
    if (!hal_init(err, sizeof err)) {
        wchar_t w[600];
        (void)MultiByteToWideChar(CP_UTF8, 0, err, -1, w, 600);
        MessageBoxW(g_hwnd, w, APP_NAMEW, MB_OK | MB_ICONERROR);
        return 0;
    }
    g_running = 1;
    return 1;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        (void)InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = 340;
        mm->ptMinTrackSize.y = 280;
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)l;
        if (!g_opt_full)
            (void)SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (theme_setting(l)) theme_apply(1);
        break;
    case WM_ACTIVATEAPP:
        if (!w) release_keys();
        g_inactive = !w;
        (void)SetWindowTextW(h, (g_inactive && g_opt_pause_inactive) ? APP_NAMEW L" (paused)" : APP_NAMEW);
        audio_set_muted(g_opt_mute || (g_inactive && g_opt_pause_inactive));
        break;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && g_opt_full) {
            (void)SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_KILLFOCUS:
        release_keys();
        break;
    case WM_ENTERMENULOOP: case WM_ENTERSIZEMOVE:
        g_menu_open++;
        release_keys();
        break;
    case WM_EXITMENULOOP: case WM_EXITSIZEMOVE:
        if (g_menu_open > 0) g_menu_open--;
        break;
    case WM_INITMENUPOPUP:
        update_menu();
        break;
    case WM_SYSCOMMAND:
        switch (w & 0xFFF0u) {
        case SC_UGH_FULL: set_full(!g_opt_full); update_menu(); return 0;
        case SC_UGH_ASPECT: set_scale(SCALE_ASPECT); save_options(); update_menu(); (void)InvalidateRect(h, NULL, FALSE); return 0;
        case SC_UGH_INTEGER: set_scale(SCALE_INTEGER); save_options(); update_menu(); (void)InvalidateRect(h, NULL, FALSE); return 0;
        case SC_UGH_SMOOTH: set_scale(SCALE_SMOOTH); save_options(); update_menu(); (void)InvalidateRect(h, NULL, FALSE); return 0;
        case SC_UGH_CRT: g_opt_crt = !g_opt_crt; save_options(); update_menu(); (void)InvalidateRect(h, NULL, FALSE); return 0;
        case SC_UGH_MUTE: g_opt_mute = !g_opt_mute; audio_set_muted(g_opt_mute || (g_inactive && g_opt_pause_inactive)); save_options(); update_menu(); return 0;
        case SC_UGH_ABOUT: show_about(); return 0;
        case SC_UGH_SND_DOS: set_amiga_sound(0); return 0;
        case SC_UGH_SND_AMIGA: set_amiga_sound(1); return 0;
        case SC_UGH_LOCATE_AMIGA:
            g_menu_open++;
            pick_amiga_disk(h);
            g_menu_open--;
            update_menu();
            return 0;
        case SC_UGH_PAUSE: g_opt_pause_inactive = !g_opt_pause_inactive; save_options(); update_menu(); return 0;
        case SC_UGH_RESTART:
            if (MessageBoxW(h, L"Restart the game from the beginning?", APP_NAMEW, MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                (void)start_game();
            return 0;
        case SC_UGH_LOCATE: {
            wchar_t dir[MAX_PATH * 2];
            g_menu_open++;
            if (pick_game_dir(h, dir)) {
                (void)WritePrivateProfileStringW(INI_SECTION, L"GameDir", dir, g_ini);
                set_game_dir(dir);
                (void)start_game();
            }
            g_menu_open--;
            return 0;
        }
        case SC_KEYMENU:
            if (l == 0 || l == VK_RETURN) return 0;   /* Alt alone, F10: the game's */
            break;
        default:
            break;
        }
        break;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) {
            if (!(l & (1 << 30))) { set_full(!g_opt_full); update_menu(); }
            return 0;
        }
        if (w == VK_F4 && (l & (1 << 29))) break;
        if (w == VK_SPACE && (l & (1 << 29))) break;
        key_event(w, l, 1);
        return 0;
    case WM_SYSKEYUP:
        key_event(w, l, 0);
        return 0;
    case WM_KEYDOWN:
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000)) {
            if (!(l & (1 << 30))) set_amiga_sound(!g_opt_amiga);
            return 0;
        }
        if (w == VK_F11) {
            if (!(l & (1 << 30))) { set_full(!g_opt_full); update_menu(); }
            return 0;
        }
        key_event(w, l, 1);
        return 0;
    case WM_KEYUP:
        if (w == VK_F11) return 0;
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000)) return 0;
        key_event(w, l, 0);
        return 0;
    case WM_CLOSE:
        g_quit = 1;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* ---- gamepad (XInput, loaded at run time): pressed as the keys of the original controls ----
 * UGH!'s first player keys: D-pad / left stick: the grey arrows (Up: pedal, lift); A: Up as well;
 * B and X: right Ctrl (the action key: pick up, drop the stone); Start: F1 (start the game);
 * Back: Esc (menu, quit); Y: Enter; RB: P; LB: F2 (password). */
typedef struct XPad {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
} XPad;
typedef DWORD(WINAPI *XGetStateFn)(DWORD, XPad *);
static XGetStateFn g_xget;
static int g_xtried;
static uint32_t g_pad_prev;

static const struct { uint32_t bit; uint8_t sc; int ext; } g_padmap[] = {
    {1u << 0, 0x48, 1}, {1u << 1, 0x50, 1}, {1u << 2, 0x4B, 1}, {1u << 3, 0x4D, 1},
    {1u << 4, 0x3B, 0}, {1u << 5, 0x01, 0}, {1u << 6, 0x3C, 0}, {1u << 7, 0x19, 0},
    {1u << 8, 0x48, 1}, {1u << 9, 0x1D, 1}, {1u << 10, 0x1C, 0},
};

static void pad_poll(void)
{
    XPad p;
    uint32_t now = 0, ch;
    size_t k;
    if (!g_xtried) {
        HMODULE x = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (x == NULL) x = LoadLibraryExW(L"xinput9_1_0.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        g_xget = x != NULL ? (XGetStateFn)(void *)GetProcAddress(x, "XInputGetState") : NULL;
        g_xtried = 1;
    }
    if (g_xget == NULL || !g_running || GetForegroundWindow() != g_hwnd) {
        now = 0;
    } else if (g_xget(0, &p) == ERROR_SUCCESS) {
        WORD b = p.buttons;
        if ((b & 0x0001u) || p.ly > 12000) now |= 1u << 0;            /* up */
        if ((b & 0x0002u) || p.ly < -12000) now |= 1u << 1;           /* down */
        if ((b & 0x0004u) || p.lx < -12000) now |= 1u << 2;           /* left */
        if ((b & 0x0008u) || p.lx > 12000) now |= 1u << 3;            /* right */
        if (b & 0x0010u) now |= 1u << 4;                                /* Start */
        if (b & 0x0020u) now |= 1u << 5;                                /* Back */
        if (b & 0x0100u) now |= 1u << 6;                                /* LB */
        if (b & 0x0200u) now |= 1u << 7;                                /* RB */
        if (b & 0x1000u) now |= 1u << 8;                                /* A */
        if (b & (0x2000u | 0x4000u)) now |= 1u << 9;                   /* B, X */
        if (b & 0x8000u) now |= 1u << 10;                               /* Y */
    }
    ch = now ^ g_pad_prev;
    if (ch == 0) return;
    {   /* the up arrow has two sources (d-pad and A): one key event for their union */
        const uint32_t up = (1u << 0) | (1u << 8);
        int was = (g_pad_prev & up) != 0, is = (now & up) != 0;
        if (was != is && g_running) hal_key(0x48, is, 1);
        ch &= ~up;
    }
    for (k = 0; k < sizeof g_padmap / sizeof g_padmap[0]; k++) {
        if (!(ch & g_padmap[k].bit)) continue;
        if (g_running) hal_key(g_padmap[k].sc, (now & g_padmap[k].bit) != 0, g_padmap[k].ext);
    }
    g_pad_prev = now;
}

void host_pump(void)
{
    MSG m;
    pad_poll();
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) { g_quit = 1; break; }
        (void)TranslateMessage(&m);
        (void)DispatchMessageW(&m);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    RECT r;
    DWORD style = WS_OVERLAPPEDWINDOW;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    UINT dpi;
    (void)prev;
    (void)cmd;
    (void)CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    (void)QueryPerformanceFrequency(&g_qpf);
    (void)timeBeginPeriod(1);
    g_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (g_timer == NULL) g_timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    ini_path();
    set_scale(ini_int(L"Scale", SCALE_INTEGER));
    g_opt_crt = ini_int(L"Crt", 0);
    g_opt_mute = ini_int(L"Mute", 0);
    g_opt_pause_inactive = ini_int(L"PauseInactive", 1);
    g_opt_amiga = ini_int(L"AmigaSound", 0);
    if (!load_embedded_pack() && !find_game_dir(argc > 1 ? argv[1] : NULL)) return 1;
    theme_apply(0);
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"UGH64Window";
    (void)RegisterClassExW(&wc);
    dpi = GetDpiForSystem();
    r.left = 0; r.top = 0;
    r.right = MulDiv(960, (int)dpi, 96);
    r.bottom = MulDiv(720, (int)dpi, 96);
    {
        RECT wa;
        (void)SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        while (r.bottom > (wa.bottom - wa.top) * 9 / 10 && r.bottom > 400) { r.right = r.right * 3 / 4; r.bottom = r.bottom * 3 / 4; }
    }
    (void)AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAMEW, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (g_hwnd == NULL) return 1;
    theme_apply(0);
    add_menu();
    (void)ShowWindow(g_hwnd, show);
    (void)UpdateWindow(g_hwnd);
    if (ini_int(L"FullScreen", 0)) { set_full(1); update_menu(); }
    (void)audio_open();
    audio_set_muted(g_opt_mute);
    if (!start_game()) return 1;
    find_amiga_disk();                 /* (quietly: without it, Amiga sound asks for it when chosen) */
    if (g_opt_amiga) g_opt_amiga = amiga_audio_ready();
    hal_set_audio_source(g_opt_amiga);
    update_menu();
    while (!g_quit) {
        if (!g_running || g_menu_open || IsIconic(g_hwnd) || (g_inactive && g_opt_pause_inactive)) {
            host_pump();
            host_sleep_us(15000);
            continue;
        }
        if (!hal_frame()) {
            if (hal_exited() && !g_quit) {
                if (g_hal_exit_msg[0] != 0 && strstr(g_hal_exit_msg, "FATAL") != NULL) {
                    wchar_t wm[300];
                    (void)MultiByteToWideChar(CP_ACP, 0, g_hal_exit_msg, -1, wm, 300);
                    MessageBoxW(g_hwnd, wm, APP_NAMEW, MB_OK | MB_ICONWARNING);
                }
                g_quit = 1;
            }
        }
    }
    audio_close();
    DestroyWindow(g_hwnd);
    LocalFree(argv);
    return 0;
}
