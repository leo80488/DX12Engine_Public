#pragma once

#include <Windows.h>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <optional>
#include <memory>
#include <vector>
#include <string>

#include "System/Exception_handle.h"
#include "System/Keyboard.h"
#include "System/Mouse.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"


class Window {
public:
	class Exception :public ExceptionHandle {
	public:
		Exception(int line, const char* file, HRESULT hr );
		const char* what() const override;
		virtual const char* GetType() const override;
		static std::string TranslateErrorCode(HRESULT hr)
		{
			char* pMsgBuf = nullptr;
			DWORD nMsgLen = FormatMessage(
				FORMAT_MESSAGE_ALLOCATE_BUFFER |
				FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				reinterpret_cast<LPWSTR>(&pMsgBuf), 0, nullptr
			);
			if (nMsgLen == 0) {
				return "Unidentified error code";
			}
			std::string errorString = pMsgBuf;
			LocalFree(pMsgBuf);
			return errorString;
		}
		HRESULT GetErrorCode() const;
		std::string GetErrorString() const;
	private:
		HRESULT hr;
	};

private:
	class WindowClass {
	public:
		static const wchar_t* GetName() noexcept;
		static HINSTANCE GetInstance() noexcept;
	private:
		WindowClass() noexcept;
		~WindowClass();
		WindowClass(const WindowClass&) = delete;
		WindowClass& operator = (const WindowClass&) = delete;
		static constexpr const wchar_t* wndClassName = L"Window_TEST";
		static WindowClass wndClass;
		HINSTANCE hInst;
	};
public:
	Window(int width, int height, const wchar_t* name) noexcept;
	~Window();
	Window(const Window&) = delete;
	Window& operator=(const Window&) = delete;
	void SetTitle(const std::wstring& title);
	void EnableCursor() noexcept;
	void DisableCursor() noexcept;
	bool CursorEnabled() const noexcept;

	/** Whether currently fullscreen (toggled by Alt+Enter). */
	bool IsFullscreen() const noexcept { return m_fullscreen; }
	void SetFullscreen(bool fullscreen);

	static std::optional<int> ProcessMessage();
	IGraphicsDevice& Gfx()
	{
		return *pGfx;
	}

	// ---- UI-layer hooks -----------------------------------------------------
	// Window itself has no knowledge of ImGui/any UI; it only calls these
	// optional function pointers if set. Editor installs them on startup;
	// Game leaves them null and the window becomes a pure input forwarder.
	using WndMsgHook    = LRESULT (*)(HWND, UINT, WPARAM, LPARAM);
	using WantCaptureFn = bool    (*)();
	static void SetMessageHook       (WndMsgHook hook)   noexcept;
	static void SetWantCaptureMouse  (WantCaptureFn fn)  noexcept;
	static void SetWantCaptureKeyboard(WantCaptureFn fn) noexcept;
private:
	void ConfineCursor() noexcept;
	void FreeCursor() noexcept;
	void ShowCursor()noexcept;
	void HideCursor()noexcept;
	static LRESULT CALLBACK HandleMsgSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept;
	static LRESULT CALLBACK HandleMsgThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept;
	LRESULT HandleMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept;
private:
	bool cursorEnabled = true;
	bool m_fullscreen = false;
	int width;
	int height;
	HWND hwnd;
	std::unique_ptr<GraphicsDX12> pGfx;
	std::vector<BYTE> rawBuffer;
};
#define CHWND_EXCEPT(hr) Window::Exception(__LINE__,__FILE__,hr)
#define CHWND_LAST_EXCEPT() Window::Exception(__LINE__,__FILE__,GetLastError())