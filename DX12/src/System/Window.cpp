#include "System/Window.h"
#include "System/Log.h"

#include <shellscalingapi.h>
#pragma comment(lib, "Shcore.lib")

Window::WindowClass Window::WindowClass::wndClass;

// ---- UI-layer hook storage --------------------------------------------------
// Installed by the editor (or not at all for Game.exe).
namespace
{
    Window::WndMsgHook    g_msgHook            = nullptr;
    Window::WantCaptureFn g_wantCaptureMouse   = nullptr;
    Window::WantCaptureFn g_wantCaptureKeyboard= nullptr;

    inline bool UIWantsMouse()    { return g_wantCaptureMouse    && g_wantCaptureMouse();    }
    inline bool UIWantsKeyboard() { return g_wantCaptureKeyboard && g_wantCaptureKeyboard(); }
}

void Window::SetMessageHook       (WndMsgHook hook)    noexcept { g_msgHook             = hook; }
void Window::SetWantCaptureMouse  (WantCaptureFn fn)   noexcept { g_wantCaptureMouse    = fn;   }
void Window::SetWantCaptureKeyboard(WantCaptureFn fn)  noexcept { g_wantCaptureKeyboard = fn;   }

Window::WindowClass::WindowClass() noexcept:
	hInst(GetModuleHandle(nullptr))
{
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = HandleMsgSetup;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = GetInstance();
    wc.hIcon = nullptr;
    wc.hCursor = nullptr;
    wc.hbrBackground = nullptr;
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = GetName();
    wc.hIconSm = nullptr;
    RegisterClassEx(&wc);
}
Window::WindowClass::~WindowClass()
{
    UnregisterClass(wndClassName, GetInstance());
}
const wchar_t* Window::WindowClass::GetName() noexcept
{
    return wndClassName;
}

HINSTANCE Window::WindowClass::GetInstance() noexcept
{
    return wndClass.hInst;
}






Window::Window(int width, int height, const wchar_t* name) noexcept:
    width(width),height(height)
{
    // Per-monitor DPI awareness. Must be set BEFORE CreateWindow. Editor
    // (ImGui) and Game both benefit from correct DPI scaling. This was
    // previously achieved via ImGui_ImplWin32_EnableDpiAwareness but the
    // Win32 API call is equivalent and has no editor dependency.
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    constexpr DWORD kWindowStyle = WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_THICKFRAME;
    RECT wr;
    wr.left = 100;
    wr.right = width + wr.left;
    wr.top = 100;
    wr.bottom = height + wr.top;
    AdjustWindowRect(&wr, kWindowStyle, false);

    hwnd = CreateWindow(
        WindowClass::GetName(), name,
        kWindowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, WindowClass::GetInstance(), this
    );
    
    ShowWindow(hwnd, SW_SHOWDEFAULT);

    pGfx = std::make_unique<GraphicsDX12>(hwnd, width, height);

    // register mouse raw input device
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01; // mouse page
    rid.usUsage = 0x02; // mouse usage
    rid.dwFlags = 0;
    rid.hwndTarget = nullptr;
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid)) == FALSE)
    {
        throw CHWND_LAST_EXCEPT();
    }
}

Window::~Window()
{
    DestroyWindow(hwnd);
}

void Window::SetTitle(const std::wstring& title)
{
    if (SetWindowText(hwnd, title.c_str()) == 0)
    {
        
    }
}
void Window::EnableCursor() noexcept
{
    cursorEnabled = true;
    ShowCursor();
    FreeCursor();
}

void Window::DisableCursor() noexcept
{
    cursorEnabled = false;
    HideCursor();
    ConfineCursor();
}
bool Window::CursorEnabled() const noexcept
{
    return cursorEnabled;
}

void Window::SetFullscreen(bool fullscreen)
{
    
    pGfx->SetFullscreen(fullscreen);
}

std::optional<int> Window::ProcessMessage()
{
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            return msg.wParam;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return {};
}
void Window::ConfineCursor() noexcept
{
    RECT rect;
    GetClientRect(hwnd, &rect);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&rect), 2);
    ClipCursor(&rect);
}

void Window::FreeCursor() noexcept
{
    ClipCursor(nullptr);
}

void Window::HideCursor() noexcept
{
    while (::ShowCursor(FALSE) >= 0);
}

void Window::ShowCursor() noexcept
{
    while (::ShowCursor(TRUE) < 0);
}

LRESULT Window::HandleMsgSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
{
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* const pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        Window* const pWnd = static_cast<Window*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pWnd));
        SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Window::HandleMsgThunk));
        return pWnd->HandleMsg(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT Window::HandleMsgThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
{
    Window* const pWnd = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    return pWnd->HandleMsg(hwnd, msg, wParam, lParam);
}

LRESULT Window::HandleMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
{
    // Let the UI layer (editor ImGui) see the message first. Returns non-zero
    // when it fully handled the message (e.g. ImGui was over a docked window).
    if (g_msgHook && g_msgHook(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
    {
        UINT w = (UINT)LOWORD(lParam);
        UINT h = (UINT)HIWORD(lParam);
        width = (int)w;
        height = (int)h;
        if (pGfx && w > 0 && h > 0)
        {
            pGfx->Resize(w, h);
        }
        return 0;
    }
    // clear keystate when windows loses focus to prevent input getting
    case WM_KILLFOCUS:
        Keyboard::GetInstance().ClearState();
        break;
    /*key message*/
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))
        {
            m_fullscreen = !m_fullscreen;
            SetFullscreen(m_fullscreen);
			LOG_INFO("Toggled fullscreen mode: {}", m_fullscreen ? "ON" : "OFF");
            return 0;
        }
        if (UIWantsKeyboard())
            break;
        if (!(lParam & 0x40000000) || Keyboard::GetInstance().AutorepeatIsEnabled())
            Keyboard::GetInstance().OnKeyPressed(static_cast<unsigned char>(wParam));
        break;
    case WM_SYSKEYUP:
    case WM_KEYUP:
        Keyboard::GetInstance().OnKeyReleased(static_cast<unsigned char>(wParam));
        break;
    case WM_CHAR:
        if (UIWantsKeyboard())
        {
            break;
        }
        Keyboard::GetInstance().OnChar(static_cast<char>(wParam));
        break;
    /*key message*/
    case WM_MOUSEMOVE:
    {
        if (UIWantsMouse())
        {
            break;
        }
        const POINTS pt=MAKEPOINTS(lParam);
        auto& mouse = Mouse::GetInstance();
        if (pt.x >= 0 && pt.x < width && pt.y>=0 && pt.y < height)
        {
            mouse.OnMouseMove(pt.x, pt.y);
            if (!mouse.IsInWindow())
            {
                SetCapture(hwnd);
                mouse.OnMouseEnter();
            }
        }
        else
        {
            if (mouse.LeftIsPressed() || mouse.RightIsPressed())
            {
                mouse.OnMouseMove(pt.x, pt.y);
            }
            else
            {
                ReleaseCapture();
                mouse.OnMouseLeave();
            }
        }
        
        break;
    }
    case WM_LBUTTONDOWN:
    {
        SetForegroundWindow(hwnd);
        if (!cursorEnabled)
        {
            ConfineCursor();
            HideCursor();
        }
        if (UIWantsMouse())
        {
            break;
        }
        const POINTS pt = MAKEPOINTS(lParam);
        Mouse::GetInstance().OnLeftPressed(pt.x, pt.y);
        break;
    }
    case WM_RBUTTONDOWN:
    {
        if (UIWantsMouse())
        {
            break;
        }
        const POINTS pt = MAKEPOINTS(lParam);
        Mouse::GetInstance().OnRightPressed(pt.x, pt.y);
        break;
    }
    case WM_LBUTTONUP:
    {
        if (UIWantsMouse())
        {
            break;
        }
        const POINTS pt = MAKEPOINTS(lParam);
        Mouse::GetInstance().OnLeftReleased(pt.x, pt.y);
        break;
    }
    case WM_RBUTTONUP:
    {
        if (UIWantsMouse())
        {
            break;
        }
        const POINTS pt = MAKEPOINTS(lParam);
        Mouse::GetInstance().OnRightReleased(pt.x, pt.y);
        break;
    }
    case WM_MOUSEWHEEL:
    {
        if (UIWantsMouse())
        {
            break;
        }
        const POINTS pt = MAKEPOINTS(lParam);
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        Mouse::GetInstance().OnWheelDelta(pt.x, pt.y, delta);
        break;
    }

    case WM_ACTIVATE:
    {
        // confine/free cursor on window to foreground/background if cursor disabled
        if (!cursorEnabled)
        {
            if (wParam & WA_ACTIVE )
            {
                
                ConfineCursor();
                HideCursor();
            }
            else
            {
                
                FreeCursor();
                ShowCursor();
            }
        }
        break;
    }
        /************** RAW MOUSE MESSAGES **************/
    case WM_INPUT:
    {
        UINT size;
        // first get the size of the input data
        if (GetRawInputData(
            reinterpret_cast<HRAWINPUT>(lParam),
            RID_INPUT,
            nullptr,
            &size,
            sizeof(RAWINPUTHEADER)) == -1)
        {
            // bail msg processing if error
            break;
        }
        rawBuffer.resize(size);
        // read in the input data
        if (GetRawInputData(
            reinterpret_cast<HRAWINPUT>(lParam),
            RID_INPUT,
            rawBuffer.data(),
            &size,
            sizeof(RAWINPUTHEADER)) != size)
        {
            // bail msg processing if error
            break;
        }
        // process the raw input data
        auto& ri = reinterpret_cast<const RAWINPUT&>(*rawBuffer.data());
        if (ri.header.dwType == RIM_TYPEMOUSE &&
            (ri.data.mouse.lLastX != 0 || ri.data.mouse.lLastY != 0))
        {
            Mouse::GetInstance().OnRawDelta(ri.data.mouse.lLastX, ri.data.mouse.lLastY);
        }
        break;
    }
    /************** END RAW MOUSE MESSAGES **************/
    }
    

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

//Exception
Window::Exception::Exception(int line, const char* file, HRESULT hr) :
    ExceptionHandle(line, file),hr(hr)
{
}

const char* Window::Exception::what() const
{
    std::ostringstream oss;
    oss << GetType() << std::endl
        << "[Error Code] " << GetErrorCode() << std::endl
        << "[Description] " << GetErrorString() << std::endl
        << GetOriginString();
    whatBuffer = oss.str();
    return whatBuffer.c_str();
}

const char* Window::Exception::GetType() const
{
    return "Window Exception";
}

HRESULT Window::Exception::GetErrorCode() const
{
    return hr;
}

std::string Window::Exception::GetErrorString() const
{
    return TranslateErrorCode(hr);
}
