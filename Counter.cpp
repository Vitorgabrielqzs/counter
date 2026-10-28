// Counter - contador que mora na bandeja do Windows (ao lado do relógio)
// C++ nativo (Win32 + Direct2D/DirectWrite): poucos MB de RAM e zero processamento enquanto parado
// (o programa só acorda quando o Windows manda uma mensagem: clique, atalho, troca de tema).
// - O ícone da bandeja é o próprio número: fonte do Windows (Segoe UI Variable Text), negrito, centralizado
// - Clique no número: menu com "Resetar" e "Fechar"; com ele aberto, → soma 1 e ← tira 1
// - Atalhos em qualquer programa: Ctrl+Shift+→ soma 1, Ctrl+Shift+← tira 1
// - O valor é salvo a cada mudança em %APPDATA%\Counter\config.json (com backup)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr UINT WM_TRAY = WM_APP + 1;               // mensagem que o ícone da bandeja manda para a janela
    constexpr UINT CMD_RESET = 1, CMD_CLOSE = 2;       // opções do menu
    constexpr int HOTKEY_PLUS = 1, HOTKEY_MINUS = 2;   // atalhos globais
    constexpr float DIGIT_HEIGHT = 0.75f;              // altura dos algarismos em relação ao ícone (3/4)

    HWND window;
    NOTIFYICONDATAW tray{};
    HICON trayIcon;
    long long value;
    UINT taskbarCreated;                               // avisa quando o Explorer reinicia (o ícone precisa voltar)
    ID2D1Factory* d2d;
    IDWriteFactory* dwrite;
    wchar_t fontName[64];                              // fonte encontrada (Segoe UI Variable Text ou Segoe UI)
    float capRatio = 0.7f;                             // altura dos algarismos / tamanho da fonte
    wchar_t folder[MAX_PATH], filePath[MAX_PATH], backupPath[MAX_PATH], tempPath[MAX_PATH];

    // ---------------------------------------------------------------
    // Arquivo de dados: {"Value":123}. Grava num temporário e troca de uma vez
    // (se o PC desligar no meio, nada se corrompe); a versão anterior fica como backup.
    // ---------------------------------------------------------------
    bool ReadValue(const wchar_t* path, long long& out)
    {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        char text[256];
        DWORD read = 0;
        BOOL ok = ReadFile(f, text, sizeof text - 1, &read, nullptr);
        CloseHandle(f);
        if (!ok) return false;
        text[read] = 0;
        const char* key = strstr(text, "\"Value\"");
        const char* colon = key ? strchr(key, ':') : nullptr;
        if (!colon) return false;
        out = _strtoi64(colon + 1, nullptr, 10);
        if (out < 0) out = 0;
        return true;
    }

    void Save()
    {
        CreateDirectoryW(folder, nullptr);
        char text[48];
        int length = sprintf_s(text, "{\"Value\":%lld}", value);
        HANDLE f = CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;   // disco cheio ou bloqueado: tenta de novo na próxima mudança
        DWORD written;
        BOOL ok = WriteFile(f, text, length, &written, nullptr) && FlushFileBuffers(f);
        CloseHandle(f);
        if (!ok) return;
        if (GetFileAttributesW(filePath) != INVALID_FILE_ATTRIBUTES)
            ReplaceFileW(filePath, tempPath, backupPath, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr);
        else
            MoveFileW(tempPath, filePath);
    }

    // Barra de tarefas clara (número preto) ou escura (número branco), como o Windows guarda no registro
    bool TaskbarIsLight()
    {
        DWORD light = 0, size = sizeof light;
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        return light == 1;
    }

    // Windows 11: Segoe UI Variable Text (fonte do relógio da barra); Windows 10: Segoe UI.
    // Guarda também a proporção da altura dos algarismos, para acertar o tamanho em pixels.
    void FindFont()
    {
        IDWriteFontCollection* fonts;
        if (FAILED(dwrite->GetSystemFontCollection(&fonts))) return;
        static const wchar_t* const names[] = { L"Segoe UI Variable Text", L"Segoe UI" };
        for (const wchar_t* name : names)
        {
            UINT32 index;
            BOOL exists = FALSE;
            if (FAILED(fonts->FindFamilyName(name, &index, &exists)) || !exists) continue;
            IDWriteFontFamily* family;
            IDWriteFont* font;
            if (SUCCEEDED(fonts->GetFontFamily(index, &family)))
            {
                if (SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)))
                {
                    DWRITE_FONT_METRICS metrics;
                    font->GetMetrics(&metrics);
                    capRatio = static_cast<float>(metrics.capHeight) / metrics.designUnitsPerEm;
                    font->Release();
                }
                family->Release();
            }
            wcscpy_s(fontName, name);
            break;
        }
        fonts->Release();
    }

    // ---------------------------------------------------------------
    // Desenha o número como ícone, no tamanho exato em pixels da bandeja (20 px a 125%, 24 a 150%...),
    // com DirectWrite (mesmo motor de texto da barra de tarefas): negrito, 3/4 da altura, centralizado.
    // ---------------------------------------------------------------
    HICON DrawIcon()
    {
        UINT dpi = GetDpiForSystem();
        int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);

        // Bitmap de 32 bits (com transparência) onde o Direct2D desenha
        BITMAPINFO info{};
        info.bmiHeader = { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB };   // altura negativa = de cima para baixo
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP color = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ previous = SelectObject(dc, color);

        // Número em texto; mesmo tamanho até 2 algarismos, depois diminui até caber na largura
        wchar_t text[24];
        int length = swprintf_s(text, L"%lld", value);
        float fontPx = size * DIGIT_HEIGHT / capRatio;
        IDWriteTextFormat* format = nullptr;
        IDWriteTextLayout* layout = nullptr;
        DWRITE_TEXT_METRICS metrics{};
        while (true)
        {
            dwrite->CreateTextFormat(fontName, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, fontPx, L"", &format);
            dwrite->CreateTextLayout(text, length, format, 1000, 1000, &layout);
            layout->GetMetrics(&metrics);
            if (metrics.widthIncludingTrailingWhitespace <= size - 1 || fontPx <= 6) break;
            layout->Release();
            format->Release();
            fontPx -= 0.5f;
        }

        // Centraliza pelos algarismos (não pela linha, que tem espaço para acentos), em pixel inteiro
        DWRITE_LINE_METRICS line;
        UINT32 lines;
        layout->GetLineMetrics(&line, 1, &lines);
        float baseline = floorf(size / 2.0f + fontPx * capRatio / 2 + 0.5f);
        float x = floorf((size - metrics.widthIncludingTrailingWhitespace) / 2 + 0.5f);

        // Direct2D desenhando no bitmap: 96 DPI = 1 unidade por pixel; tons de cinza (fundo transparente).
        // SOFTWARE = desenha no processador: para 20x20 pixels é instantâneo e não carrega o driver
        // da placa de vídeo (que sozinho ocuparia dezenas de MB de RAM)
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        ID2D1DCRenderTarget* target;
        if (SUCCEEDED(d2d->CreateDCRenderTarget(&props, &target)))
        {
            RECT area{ 0, 0, size, size };
            target->BindDC(dc, &area);
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            ID2D1SolidColorBrush* brush;
            target->CreateSolidColorBrush(TaskbarIsLight() ? D2D1::ColorF(0, 0, 0) : D2D1::ColorF(1, 1, 1), &brush);
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0, 0, 0, 0));
            target->DrawTextLayout(D2D1::Point2F(x, baseline - line.baseline), layout, brush);
            target->EndDraw();
            brush->Release();
            target->Release();
        }
        layout->Release();
        format->Release();

        // O Direct2D grava a cor já multiplicada pela opacidade; o ícone do Windows espera a cor pura
        auto* pixels = static_cast<BYTE*>(bits);
        for (int i = 0; i < size * size * 4; i += 4)
        {
            BYTE alpha = pixels[i + 3];
            if (alpha == 0 || alpha == 255) continue;
            for (int c = 0; c < 3; c++) pixels[i + c] = static_cast<BYTE>(pixels[i + c] * 255 / alpha);
        }

        // Bitmap → ícone (a máscara em branco é exigida, mas a transparência vem do próprio bitmap)
        SelectObject(dc, previous);
        void* zeros = calloc(((size + 15) / 16) * 2, size);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros);
        ICONINFO icon{ TRUE, 0, 0, mask, color };
        HICON result = CreateIconIndirect(&icon);
        DeleteObject(mask);
        free(zeros);
        DeleteObject(color);
        DeleteDC(dc);
        return result;
    }

    // Redesenha o ícone e o texto que aparece ao passar o mouse
    void RefreshTray(DWORD action = NIM_MODIFY)
    {
        HICON old = trayIcon;
        trayIcon = DrawIcon();
        tray.hIcon = trayIcon;
        swprintf_s(tray.szTip, L"Counter: %lld", value);
        Shell_NotifyIconW(action, &tray);
        if (action == NIM_ADD)
        {
            tray.uVersion = NOTIFYICON_VERSION_4;   // cliques chegam como NIN_SELECT / WM_CONTEXTMENU
            Shell_NotifyIconW(NIM_SETVERSION, &tray);
        }
        if (old) DestroyIcon(old);
    }

    // +1 / -1 (não fica negativo)
    void Step(int delta)
    {
        if (value + delta < 0) return;
        value += delta;
        Save();
        RefreshTray();
    }

    // Enquanto o menu está aberto, o Windows passa as teclas por aqui: ← e → mudam o número sem fechar o menu
    LRESULT CALLBACK MenuKeys(int code, WPARAM wParam, LPARAM lParam)
    {
        auto* msg = reinterpret_cast<MSG*>(lParam);
        if (code == MSGF_MENU && msg->message == WM_KEYDOWN && (msg->wParam == VK_LEFT || msg->wParam == VK_RIGHT))
        {
            Step(msg->wParam == VK_RIGHT ? 1 : -1);
            return 1;   // tecla usada: o menu não recebe
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // Menu do clique: "Resetar" (o 1º clique pede confirmação, abrindo o menu de novo) e "Fechar"
    void ShowMenu(int x, int y, bool confirmReset = false)
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, CMD_RESET, confirmReset ? L"Resetar? Clique de novo" : L"Resetar");
        AppendMenuW(menu, MF_STRING, CMD_CLOSE, L"Fechar");

        SetForegroundWindow(window);   // sem isso o menu não fecha ao clicar fora
        HHOOK hook = SetWindowsHookExW(WH_MSGFILTER, MenuKeys, nullptr, GetCurrentThreadId());
        UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_CENTERALIGN, x, y, window, nullptr);
        UnhookWindowsHookEx(hook);
        DestroyMenu(menu);
        PostMessageW(window, WM_NULL, 0, 0);

        if (command == CMD_RESET)
        {
            if (!confirmReset) { ShowMenu(x, y, true); return; }
            value = 0;
            Save();
            RefreshTray();
        }
        else if (command == CMD_CLOSE)
        {
            DestroyWindow(window);
        }
    }

    // Menus no tema escuro quando o Windows está no escuro. São funções internas do Windows (as mesmas
    // que o Explorer usa), sem nome público: achadas pelo número. Se não existirem, o menu só fica claro.
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
    using FlushMenuThemesFn = void(WINAPI*)();
    FlushMenuThemesFn flushMenuThemes;

    void EnableDarkMenus()
    {
        HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!uxtheme) return;
        if (auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135))))
            setMode(1);   // 1 = acompanha o tema do Windows
        if (auto allow = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(133))))
            allow(window, true);
        flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
        if (flushMenuThemes) flushMenuThemes();
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_TRAY:
            switch (LOWORD(lParam))
            {
            case NIN_SELECT:       // clique esquerdo
            case NIN_KEYSELECT:    // Enter com o ícone selecionado pelo teclado
            case WM_CONTEXTMENU:   // clique direito
                ShowMenu(GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
                break;
            }
            return 0;
        case WM_HOTKEY:
            Step(wParam == HOTKEY_PLUS ? 1 : -1);
            return 0;
        case WM_SETTINGCHANGE:    // trocou o tema claro/escuro
        case WM_DISPLAYCHANGE:    // trocou a tela ou a escala
            if (flushMenuThemes) flushMenuThemes();
            RefreshTray();
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &tray);   // tira o ícone na hora (senão fica um "fantasma")
            PostQuitMessage(0);
            return 0;
        }
        if (msg == taskbarCreated && msg != 0)
        {
            RefreshTray(NIM_ADD);   // o Explorer reiniciou: põe o ícone de volta
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // tamanhos em pixels reais (nítido)

    // Só uma cópia por vez: duas teriam dois números na bandeja salvando um por cima do outro
    CreateMutexW(nullptr, TRUE, L"Local\\Counter.UmaCopiaSo");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // %APPDATA%\Counter\config.json (o mesmo arquivo da versão anterior: o número continua)
    wchar_t appData[MAX_PATH];
    GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    swprintf_s(folder, L"%s\\Counter", appData);
    swprintf_s(filePath, L"%s\\config.json", folder);
    swprintf_s(backupPath, L"%s\\config.backup.json", folder);
    swprintf_s(tempPath, L"%s\\config.json.tmp", folder);
    if (!ReadValue(filePath, value)) ReadValue(backupPath, value);

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwrite));
    wcscpy_s(fontName, L"Segoe UI");
    FindFont();

    // Janela invisível: só recebe as mensagens (cliques no ícone, atalhos, troca de tema)
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = L"CounterTray";
    RegisterClassW(&cls);
    window = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"Counter", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    EnableDarkMenus();

    tray.cbSize = sizeof tray;
    tray.hWnd = window;
    tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    tray.uCallbackMessage = WM_TRAY;
    RefreshTray(NIM_ADD);

    // Ctrl+Shift+→ e Ctrl+Shift+← em qualquer programa. Enquanto o Counter está aberto, o Windows
    // entrega essas teclas só para ele (nos outros programas elas deixam de selecionar palavras)
    RegisterHotKey(window, HOTKEY_PLUS, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_RIGHT);
    RegisterHotKey(window, HOTKEY_MINUS, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_LEFT);

    // Fica parado esperando mensagens (não gasta processamento enquanto nada acontece)
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
