/******************************************************************************
*
*
* Notepad4
*
* MDPreview.cpp
*   Markdown preview pane rendered by WebView2.
*   Layout and styling follow https://mdviewer.net (fonts, tables, code blocks).
*   Markdown is parsed by md4c (https://github.com/mity/md4c).
*
* See License.txt for details about distribution and modification.
*
*
******************************************************************************/

#include <windows.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>

#include "WebView2.h"
#include "md4c/md4c-html.h"

#include "SciCall.h"
#include "Helpers.h"
#include "Notepad4.h"
#include "MDPreview.h"

extern HWND hwndMain;

namespace {

constexpr WCHAR kHostClassName[] = L"Notepad4MDPreviewHost";
constexpr WCHAR kVirtualHost[] = L"mdpreview.local";
constexpr int kUpdateTimerId = 0x4D44;	// 'MD'
constexpr UINT kUpdateDelayMs = 350;
constexpr size_t kMaxSourceBytes = 1 * 1024 * 1024;	// keep rendered HTML below NavigateToString limit

HWND s_hwndHost = nullptr;
bool s_visible = false;
bool s_ready = false;			// WebView2 controller fully initialized
bool s_pageAlive = false;		// a document is loaded in the webview
bool s_renderBusy = false;		// scroll capture/navigation chain in flight
bool s_dirty = false;			// content changed while a render is in flight
bool s_ownNavExpected = false;	// next NavigationStarting event is our own navigation
LONG s_lastScrollY = 0;

ICoreWebView2Environment *s_env = nullptr;
ICoreWebView2Controller *s_controller = nullptr;
ICoreWebView2 *s_webview = nullptr;
ICoreWebView2_3 *s_webview3 = nullptr;
EventRegistrationToken s_navStartingToken = {};
EventRegistrationToken s_navCompletedToken = {};

//=============================================================================
// Minimal COM callback base (no WRL, works with MinGW).
//
// MinGW has no __uuidof() support for the MIDL-generated WebView2 interfaces,
// so the IIDs (from WebView2.h) are defined here and passed to the base class.

constexpr IID kIID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler = {0x4e8a3389, 0xc9d8, 0x4bd2, {0xb6,0xb5,0x12,0x4f,0xee,0x6c,0xc1,0x4d}};
constexpr IID kIID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler = {0x6c4819f3, 0xc9b7, 0x4260, {0x81,0x27,0xc9,0xf5,0xbd,0xe7,0xf6,0x8c}};
constexpr IID kIID_ICoreWebView2NavigationStartingEventHandler = {0x9adbe429, 0xf36d, 0x432b, {0x9d,0xdc,0xf8,0x88,0x1f,0xbd,0x76,0xe3}};
constexpr IID kIID_ICoreWebView2NavigationCompletedEventHandler = {0xd33a35bf, 0x1c49, 0x4f98, {0x93,0xab,0x00,0x6e,0x05,0x33,0xfe,0x1c}};
constexpr IID kIID_ICoreWebView2ExecuteScriptCompletedHandler = {0x49511172, 0xcc67, 0x4bca, {0x99,0x23,0x13,0x71,0x12,0xf4,0xc4,0xcc}};
constexpr IID kIID_ICoreWebView2_3 = {0xA0D6DF20, 0x3B92, 0x416D, {0xAA,0x0C,0x43,0x7A,0x9C,0x72,0x78,0x57}};

template <class Itf, const IID *piid>
class ComHandlerBase : public Itf {
	LONG m_ref = 1;

public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
		if (ppvObject == nullptr) {
			return E_POINTER;
		}
		if (riid == __uuidof(IUnknown) || riid == *piid) {
			*ppvObject = static_cast<Itf *>(this);
			AddRef();
			return S_OK;
		}
		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override {
		return ::InterlockedIncrement(&m_ref);
	}
	ULONG STDMETHODCALLTYPE Release() override {
		const ULONG ref = ::InterlockedDecrement(&m_ref);
		if (ref == 0) {
			delete this;
		}
		return ref;
	}
};

class EnvironmentHandler final : public ComHandlerBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, &kIID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
	HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Environment *environment) override;
};

class ControllerHandler final : public ComHandlerBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, &kIID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
	HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Controller *controller) override;
};

class NavigationStartingHandler final : public ComHandlerBase<ICoreWebView2NavigationStartingEventHandler, &kIID_ICoreWebView2NavigationStartingEventHandler> {
public:
	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2NavigationStartingEventArgs *args) override;
};

class NavigationCompletedHandler final : public ComHandlerBase<ICoreWebView2NavigationCompletedEventHandler, &kIID_ICoreWebView2NavigationCompletedEventHandler> {
public:
	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) override;
};

class ScrollCaptureHandler final : public ComHandlerBase<ICoreWebView2ExecuteScriptCompletedHandler, &kIID_ICoreWebView2ExecuteScriptCompletedHandler> {
public:
	HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, LPCWSTR resultObjectAsJson) override;
};

//=============================================================================
// Document text access and markdown rendering.

std::string GetDocumentText() {
	const Sci_Position length = SciCall_GetLength();
	if (length <= 0) {
		return std::string();
	}
	std::string text(static_cast<size_t>(length) + 1, '\0');
	SciCall_GetText(length + 1, text.data());
	text.resize(static_cast<size_t>(length));
	return text;
}

void AppendMdOutput(const MD_CHAR *data, MD_SIZE size, void *userdata) {
	static_cast<std::string *>(userdata)->append(data, size);
}

void HtmlEscape(LPCWSTR text, std::wstring &out) {
	for (LPCWSTR p = text; *p != L'\0'; ++p) {
		switch (*p) {
		case L'&': out += L"&amp;"; break;
		case L'<': out += L"&lt;"; break;
		case L'>': out += L"&gt;"; break;
		case L'"': out += L"&quot;"; break;
		default: out += *p; break;
		}
	}
}

std::wstring Utf8ToWide(const std::string &text) {
	if (text.empty()) {
		return std::wstring();
	}
	const int cch = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	std::wstring wide(static_cast<size_t>(cch), L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), cch);
	return wide;
}

// mdviewer.net (warm paper theme) inspired style: Inter/system body, JetBrains
// Mono code, serif headings, collapse tables with generous cell padding.
const char kPreviewStyle[] = R"CSS(
:root{--bg:#fcfbf8;--text:#2c2421;--text-secondary:#5c544f;--text-muted:#8c847f;
--border-subtle:#eae0d5;--border-medium:#d7c4b2;--border-strong:#cdb8a3;
--thead-bg:#f9f6f0;--code-bg:#f0e5d8;--pre-bg:#2c2421;--pre-color:#f5f1e6;
--link:#1f6f9c;--inline-code-color:#9c4221;
--font-body:"Inter","Segoe UI",system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;
--font-mono:"JetBrains Mono","Cascadia Mono","Consolas","Microsoft YaHei",monospace;
--font-heading:"Playfair Display","Georgia","Times New Roman",serif}
html,body{background:var(--bg)}
body{font-family:var(--font-body);color:var(--text);font-size:16px;line-height:1.75;
margin:0 auto;padding:36px 52px 72px;max-width:920px;-webkit-font-smoothing:antialiased;
overflow-wrap:break-word}
h1,h2,h3,h4,h5,h6{font-family:var(--font-heading);font-weight:700;color:#241b18;line-height:1.35;margin:1.5em 0 .55em}
h1{font-size:2.05em;margin-top:.35em;border-bottom:1px solid var(--border-subtle);padding-bottom:.32em}
h2{font-size:1.6em}h3{font-size:1.3em}h4{font-size:1.12em}h5{font-size:1em}h6{font-size:.95em;color:var(--text-secondary)}
p,ul,ol,pre,blockquote,table,hr{margin:1em 0}
li{margin:.25em 0}
ul ul,ol ol,ul ol,ol ul{margin:.1em 0}
a{color:var(--link);text-decoration:none}
a:hover{text-decoration:underline}
table{border-collapse:collapse;width:100%;font-size:.95em}
th,td{border:1px solid var(--border-medium);padding:10px 12px;text-align:left;vertical-align:top}
thead th{background:var(--thead-bg);color:#241b18;font-weight:700;position:sticky;top:0;z-index:1}
tbody tr:nth-child(2n){background:#f9f6f055}
pre{background:var(--pre-bg);color:var(--pre-color);border-radius:8px;padding:16px;overflow-x:auto;line-height:1.6}
code{font-family:var(--font-mono);font-size:.88em}
:not(pre)>code{background:var(--code-bg);color:var(--inline-code-color);border-radius:6px;padding:.16em .42em}
pre>code{background:none;color:inherit;padding:0;font-size:1em}
blockquote{border-left:3px solid var(--border-strong);background:#f9f6f0aa;color:var(--text-secondary);
padding:6px 18px;border-radius:0 8px 8px 0}
blockquote p{margin:.4em 0}
img{max-width:100%;height:auto;border-radius:4px}
hr{border:0;border-top:1px solid var(--border-subtle);margin:1.6em 0}
input[type=checkbox]{margin-right:.45em;accent-color:#a8804f}
mark{background:#f5dfa8;border-radius:3px;padding:0 .15em}
del,s{color:var(--text-muted)}
kbd{font-family:var(--font-mono);font-size:.82em;background:var(--thead-bg);border:1px solid var(--border-medium);
border-bottom-width:2px;border-radius:5px;padding:.1em .45em}
::selection{background:#ecd9c0}
::-webkit-scrollbar{width:10px;height:10px}
::-webkit-scrollbar-thumb{background:#8c847f6b;border-radius:5px}
::-webkit-scrollbar-thumb:hover{background:#5c544f85}
::-webkit-scrollbar-track{background:transparent}
)CSS";

void BuildHtml(const std::string &bodyHtml, LONG scrollY, std::wstring &html) {
	WCHAR title[MAX_PATH];
	WCHAR szFile[MAX_PATH];
	lstrcpy(title, L"Markdown Preview");
	lstrcpyn(szFile, szCurFile, COUNTOF(szFile));
	if (StrNotEmpty(szFile)) {
		LPCWSTR name = ::PathFindFileNameW(szFile);
		if (StrNotEmpty(name)) {
			std::wstring escaped;
			HtmlEscape(name, escaped);
			lstrcpyn(title, escaped.c_str(), COUNTOF(title));
		}
	}

	std::wstring baseTag;
	WCHAR szDir[MAX_PATH];
	lstrcpyn(szDir, szCurFile, COUNTOF(szDir));
	if (StrNotEmpty(szCurFile) && ::PathRemoveFileSpecW(szDir)) {
		baseTag = L"<base href=\"http://";
		baseTag += kVirtualHost;
		baseTag += L"/\">";
	}

	html.reserve(bodyHtml.size() * 2 + 8192);
	html += L"<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
	html += baseTag;
	html += L"<title>";
	html += title;
	html += L"</title><style>\n";
	html += Utf8ToWide(kPreviewStyle);
	html += L"\n</style></head><body>\n";
	html += Utf8ToWide(bodyHtml);
	html += L"\n<script>try{window.scrollTo(0,";
	WCHAR szScroll[32];
	wsprintf(szScroll, L"%ld", scrollY);
	html += szScroll;
	html += L");}catch(e){}</script></body></html>";
}

void NavigateToDocumentHtml(const std::wstring &html) {
	if (!s_webview) {
		return;
	}
	s_ownNavExpected = true;
	s_pageAlive = false;
	HRESULT hr = s_webview->NavigateToString(html.c_str());
	if (FAILED(hr)) {
		s_ownNavExpected = false;
	}
}

// Render the current Scintilla buffer into the preview.
void RenderNow() {
	if (!s_webview) {
		return;
	}

	const Sci_Position length = SciCall_GetLength();
	std::string bodyHtml;
	if (length <= 0) {
		bodyHtml = "<p style=\"color:#8c847f\">Empty document.</p>";
	} else if (static_cast<size_t>(length) > kMaxSourceBytes) {
		bodyHtml = "<p style=\"color:#8c847f\">Document too large for the preview (limit: 1 MB).</p>";
	} else {
		std::string source = GetDocumentText();
		if (source.find('\0') != std::string::npos) {
			bodyHtml = "<p style=\"color:#8c847f\">Binary content cannot be previewed.</p>";
		} else {
			std::string html;
			if (md_html(source.data(), static_cast<MD_SIZE>(source.size()), AppendMdOutput, &html,
						MD_DIALECT_GITHUB, 0) == 0) {
				bodyHtml.swap(html);
			} else {
				bodyHtml = "<p style=\"color:#8c847f\">Failed to parse Markdown.</p>";
			}
		}
	}

	// map the virtual host to the document folder so relative images resolve
	WCHAR szDir[MAX_PATH];
	lstrcpyn(szDir, szCurFile, COUNTOF(szDir));
	if (StrNotEmpty(szCurFile) && s_webview3 && ::PathRemoveFileSpecW(szDir)) {
		s_webview3->SetVirtualHostNameToFolderMapping(kVirtualHost, szDir,
													  COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
	}

	std::wstring html;
	BuildHtml(bodyHtml, s_lastScrollY, html);
	NavigateToDocumentHtml(html);
}

void RequestUpdateNow() {
	if (!s_visible || !s_ready || !s_webview) {
		return;
	}
	if (s_renderBusy) {
		s_dirty = true;
		return;
	}
	s_renderBusy = true;
	if (s_pageAlive) {
		// preserve the current scroll position across the re-render
		s_webview->ExecuteScript(L"window.scrollY|0", new ScrollCaptureHandler());
	} else {
		RenderNow();
	}
}

//=============================================================================
// WebView2 event handlers.

HRESULT STDMETHODCALLTYPE EnvironmentHandler::Invoke(HRESULT errorCode, ICoreWebView2Environment *environment) {
	if (FAILED(errorCode) || environment == nullptr) {
		if (!s_ready) {
			::MessageBoxW(hwndMain, L"WebView2 Runtime is required for the Markdown preview.",
						  L"Notepad4", MB_OK | MB_ICONWARNING);
		}
		return S_OK;
	}
	environment->AddRef();
	s_env = environment;
	environment->CreateCoreWebView2Controller(s_hwndHost, new ControllerHandler());
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ControllerHandler::Invoke(HRESULT errorCode, ICoreWebView2Controller *controller) {
	if (FAILED(errorCode) || controller == nullptr) {
		::MessageBoxW(hwndMain, L"Failed to create the WebView2 controller for the Markdown preview.",
					  L"Notepad4", MB_OK | MB_ICONWARNING);
		return S_OK;
	}

	controller->AddRef();
	s_controller = controller;
	ICoreWebView2 *webview = nullptr;
	if (FAILED(controller->get_CoreWebView2(&webview)) || webview == nullptr) {
		return S_OK;
	}
	s_webview = webview;

	ICoreWebView2Settings *settings = nullptr;
	if (SUCCEEDED(webview->get_Settings(&settings)) && settings != nullptr) {
		settings->put_AreDevToolsEnabled(FALSE);
		settings->put_IsStatusBarEnabled(FALSE);
		settings->put_AreDefaultContextMenusEnabled(FALSE);
		settings->put_IsZoomControlEnabled(FALSE);
		settings->Release();
	}

	ICoreWebView2_3 *webview3 = nullptr;
	if (SUCCEEDED(webview->QueryInterface(kIID_ICoreWebView2_3, reinterpret_cast<void **>(&webview3))) &&
		webview3 != nullptr) {
		s_webview3 = webview3;
	}

	NavigationStartingHandler *navHandler = new NavigationStartingHandler();
	webview->add_NavigationStarting(navHandler, &s_navStartingToken);
	navHandler->Release();
	NavigationCompletedHandler *navCompletedHandler = new NavigationCompletedHandler();
	webview->add_NavigationCompleted(navCompletedHandler, &s_navCompletedToken);
	navCompletedHandler->Release();

	RECT rc;
	::GetClientRect(s_hwndHost, &rc);
	controller->put_Bounds(rc);

	s_ready = true;
	if (s_visible) {
		RequestUpdateNow();
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE NavigationStartingHandler::Invoke(ICoreWebView2 *sender,
															ICoreWebView2NavigationStartingEventArgs *args) {
	if (s_ownNavExpected) {
		s_ownNavExpected = false;
		return S_OK;	// allow our own NavigateToString navigation
	}

	LPWSTR uri = nullptr;
	if (FAILED(args->get_Uri(&uri)) || uri == nullptr) {
		return S_OK;
	}

	BOOL cancel = TRUE;
	if (::StrCmpNIW(uri, L"about:", 6) == 0 || ::StrCmpNIW(uri, L"data:", 5) == 0) {
		cancel = FALSE;
	} else {
		// open external links and relative document links in the default browser/app
		::ShellExecuteW(hwndMain, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
	}
	::CoTaskMemFree(uri);
	args->put_Cancel(cancel);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE NavigationCompletedHandler::Invoke(ICoreWebView2 *sender,
															 ICoreWebView2NavigationCompletedEventArgs *args) {
	s_pageAlive = true;
	s_renderBusy = false;
	if (s_dirty) {
		s_dirty = false;
		s_renderBusy = true;
		s_webview->ExecuteScript(L"window.scrollY|0", new ScrollCaptureHandler());
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ScrollCaptureHandler::Invoke(HRESULT errorCode, LPCWSTR resultObjectAsJson) {
	if (SUCCEEDED(errorCode) && resultObjectAsJson != nullptr) {
		s_lastScrollY = static_cast<LONG>(::wcstol(resultObjectAsJson, nullptr, 10));
	}
	RenderNow();
	return S_OK;
}

//=============================================================================
// Host window.

LRESULT CALLBACK MDPreviewHostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_SIZE:
		if (s_controller != nullptr) {
			RECT rc;
			::GetClientRect(hwnd, &rc);
			s_controller->put_Bounds(rc);
		}
		return 0;

	case WM_TIMER:
		if (wParam == kUpdateTimerId) {
			::KillTimer(hwnd, kUpdateTimerId);
			RequestUpdateNow();
		}
		return 0;

	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT: {
		PAINTSTRUCT ps;
		::BeginPaint(hwnd, &ps);
		::EndPaint(hwnd, &ps);
	} return 0;
	}
	return ::DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace

//=============================================================================
// Public API.
//

void MDPreview_Create(HWND hwndParent, HINSTANCE hInstance) noexcept {
	WNDCLASSEX wcx;
	wcx.cbSize = sizeof(wcx);
	if (::GetClassInfoExW(hInstance, kHostClassName, &wcx) == FALSE) {
		wcx.style = CS_HREDRAW | CS_VREDRAW;
		wcx.lpfnWndProc = MDPreviewHostWndProc;
		wcx.cbClsExtra = 0;
		wcx.cbWndExtra = 0;
		wcx.hInstance = hInstance;
		wcx.hIcon = nullptr;
		wcx.hIconSm = nullptr;
		wcx.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
		wcx.hbrBackground = ::CreateSolidBrush(RGB(0xFC, 0xFB, 0xF8));
		wcx.lpszMenuName = nullptr;
		wcx.lpszClassName = kHostClassName;
		if (::RegisterClassExW(&wcx) == 0) {
			return;
		}
	}

	s_hwndHost = ::CreateWindowExW(0, kHostClassName, nullptr, WS_CHILD | WS_CLIPSIBLINGS,
								   0, 0, 0, 0, hwndParent, nullptr, hInstance, nullptr);
	if (s_hwndHost == nullptr) {
		return;
	}

	// WebView2Loader.dll ships beside Notepad4.exe
	WCHAR szExePath[MAX_PATH];
	::GetModuleFileNameW(nullptr, szExePath, COUNTOF(szExePath));
	::PathRemoveFileSpecW(szExePath);
	WCHAR szLoader[MAX_PATH];
	lstrcpyn(szLoader, szExePath, COUNTOF(szLoader));
	::PathAppendW(szLoader, L"WebView2Loader.dll");
	HMODULE hLoader = ::LoadLibraryExW(szLoader, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (hLoader == nullptr) {
		return;
	}

	using PFN_CreateEnvironment = HRESULT (STDMETHODCALLTYPE *)(
		PCWSTR browserExecutableFolder, PCWSTR userDataFolder, ICoreWebView2EnvironmentOptions *options,
		ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);
	const auto createEnvironment = reinterpret_cast<PFN_CreateEnvironment>(
		::GetProcAddress(hLoader, "CreateCoreWebView2EnvironmentWithOptions"));
	if (createEnvironment == nullptr) {
		return;
	}

	// per-user data folder for the browser profile
	WCHAR szUserData[MAX_PATH];
	PWSTR pszLocalAppData = nullptr;
	if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pszLocalAppData)) &&
		pszLocalAppData != nullptr) {
		lstrcpyn(szUserData, pszLocalAppData, COUNTOF(szUserData));
		::CoTaskMemFree(pszLocalAppData);
		::PathAppendW(szUserData, L"Notepad4\\WebView2");
		::SHCreateDirectoryExW(s_hwndHost, szUserData, nullptr);
	} else {
		lstrcpyn(szUserData, szExePath, COUNTOF(szUserData));
		::PathAppendW(szUserData, L"WebView2");
	}

	createEnvironment(nullptr, szUserData, nullptr, new EnvironmentHandler());
}

void MDPreview_Destroy() noexcept {
	if (s_hwndHost != nullptr) {
		::KillTimer(s_hwndHost, kUpdateTimerId);
	}
	if (s_webview != nullptr) {
		s_webview->remove_NavigationStarting(s_navStartingToken);
		s_webview->remove_NavigationCompleted(s_navCompletedToken);
	}
	if (s_controller != nullptr) {
		s_controller->Close();
		s_controller->Release();
		s_controller = nullptr;
	}
	if (s_webview != nullptr) {
		s_webview->Release();
		s_webview = nullptr;
	}
	if (s_webview3 != nullptr) {
		s_webview3->Release();
		s_webview3 = nullptr;
	}
	if (s_env != nullptr) {
		s_env->Release();
		s_env = nullptr;
	}
	s_ready = false;
	s_pageAlive = false;
	if (s_hwndHost != nullptr) {
		::DestroyWindow(s_hwndHost);
		s_hwndHost = nullptr;
	}
}

void MDPreview_SetVisible(bool bShow) noexcept {
	s_visible = bShow;
	if (s_hwndHost == nullptr) {
		return;
	}
	::ShowWindow(s_hwndHost, bShow ? SW_SHOW : SW_HIDE);
	if (s_controller != nullptr) {
		s_controller->put_IsVisible(bShow ? TRUE : FALSE);
	}
	if (bShow && s_ready) {
		RequestUpdateNow();
	}
}

bool MDPreview_IsReady() noexcept {
	return s_ready;
}

HWND MDPreview_GetHostWindow() noexcept {
	return s_hwndHost;
}

void MDPreview_RequestUpdate(bool bDelayed) noexcept {
	if (!s_visible || !s_ready) {
		return;
	}
	if (bDelayed) {
		::SetTimer(s_hwndHost, kUpdateTimerId, kUpdateDelayMs, nullptr);	// restart throttle window
	} else {
		RequestUpdateNow();
	}
}
