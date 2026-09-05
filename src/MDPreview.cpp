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
#include <commctrl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "WebView2.h"
#include "md4c/md4c-html.h"

#include "SciCall.h"
#include "Helpers.h"
#include "Dialogs.h"
#include "Notepad4.h"
#include "MDPreview.h"
#include "resource.h"

extern HWND hwndMain;

namespace {

constexpr WCHAR kHostClassName[] = L"Notepad4MDPreviewHost";
constexpr WCHAR kVirtualHost[] = L"mdpreview.local";
constexpr int kUpdateTimerId = 0x4D44;	// 'MD'
constexpr UINT kUpdateDelayMs = 350;
constexpr size_t kMaxSourceBytes = 1 * 1024 * 1024;	// keep rendered HTML below NavigateToString limit
constexpr size_t kMaxNavigateToStringChars = 1500000;	// NavigateToString() hard limit is 2 MB

HWND s_hwndHost = nullptr;
bool s_visible = false;
bool s_ready = false;			// WebView2 controller fully initialized
bool s_pageAlive = false;		// a document is loaded in the webview
bool s_renderBusy = false;		// scroll capture/navigation chain in flight
bool s_dirty = false;			// content changed while a render is in flight
bool s_ownNavExpected = false;	// next NavigationStarting event is our own navigation
bool s_printOnNavComplete = false;	// export PDF once the fresh render finished
std::wstring s_pdfTargetPath;	// output path of the pending PDF export
LONG s_lastScrollY = 0;

MDPreviewConfig s_config;

void MDPreviewConfig_SetDefaults(MDPreviewConfig &cfg) noexcept {
	cfg.bodyFont[0] = L'\0';
	cfg.bodySize = 16;
	cfg.headingFont[0] = L'\0';
	cfg.headingScale = 100;
	cfg.codeFont[0] = L'\0';
	cfg.codeScale = 88;
	cfg.tableHeadBg = RGB(0xF9, 0xF6, 0xF0);
	cfg.tableBorder = RGB(0xD7, 0xC4, 0xB2);
}

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
constexpr IID kIID_ICoreWebView2_7 = {0x79c24d83, 0x09a3, 0x45ae, {0x94,0x18,0x48,0x7f,0x32,0xa5,0x87,0x40}};
constexpr IID kIID_ICoreWebView2Environment6 = {0xe59ee362, 0xacbd, 0x4857, {0x9a,0x8e,0xd3,0x64,0x4d,0x94,0x59,0xa9}};
constexpr IID kIID_ICoreWebView2PrintSettings = {0x377f3721, 0xc74e, 0x48ca, {0x8d,0xb1,0xdf,0x68,0xe5,0x1d,0x60,0xe2}};
constexpr IID kIID_ICoreWebView2PrintToPdfCompletedHandler = {0xccf1ef04, 0xfd8e, 0x4d5f, {0xb2,0xde,0x09,0x83,0xe4,0x1b,0x8c,0x36}};

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

class PrintToPdfHandler final : public ComHandlerBase<ICoreWebView2PrintToPdfCompletedHandler, &kIID_ICoreWebView2PrintToPdfCompletedHandler> {
public:
	HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, BOOL isSuccessful) override;
};

void DoPrintToPdf();

//=============================================================================
// Document text access and markdown rendering.

std::string GetDocumentText() {
	const Sci_Position length = SciCall_GetLength();
	if (length <= 0) {
		return std::string();
	}
	std::string text(static_cast<size_t>(length) + 1, '\0');
	SciCall_GetText(length, text.data());
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
// All CSS here is original; only color values and font stacks reference the site.
// Fonts, sizes and table colors come from the user settings.

void FormatHexColor(COLORREF color, WCHAR (&text)[8]) {
	wsprintf(text, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
}

std::string ColorToCss(COLORREF color) {
	char buf[16];
	snprintf(buf, sizeof(buf), "#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
	return std::string(buf);
}

// font-family list: user chosen family first (may be empty), then fallbacks.
std::string FontStack(const WCHAR *userFont, const char *fallbacks) {
	std::string stack;
	if (userFont[0] != L'\0') {
		// convert the family name to UTF-8 and quote it
		const int cch = ::WideCharToMultiByte(CP_UTF8, 0, userFont, -1, nullptr, 0, nullptr, nullptr);
		std::string utf8(static_cast<size_t>(cch > 0 ? cch - 1 : 0), '\0');
		if (!utf8.empty()) {
			::WideCharToMultiByte(CP_UTF8, 0, userFont, -1, utf8.data(), cch, nullptr, nullptr);
		}
		stack += '\'';
		stack += utf8;
		stack += '\'';
		stack += ',';
	}
	stack += fallbacks;
	return stack;
}

std::string BuildPreviewStyle() {
	char buf[512];
	std::string css;
	const std::string headBg = ColorToCss(s_config.tableHeadBg);
	const std::string border = ColorToCss(s_config.tableBorder);

	css += ":root{--bg:#fcfbf8;--text:#2c2421;--text-secondary:#5c544f;--text-muted:#8c847f;";
	css += "--thead-bg:" + headBg + ";--border-medium:" + border + ";";
	css += "--border-subtle:#eae0d5;--border-strong:#cdb8a3;--code-bg:#f0e5d8;--pre-bg:#2c2421;--pre-color:#f5f1e6;";
	css += "--link:#1f6f9c;--inline-code-color:#9c4221;}";

	css += "html,body{background:var(--bg)}";
	css += "body{font-family:" + FontStack(s_config.bodyFont,
		"\"Inter\",\"Segoe UI\",system-ui,-apple-system,\"PingFang SC\",\"Microsoft YaHei\",sans-serif") + ";";
	css += "color:var(--text);";
	snprintf(buf, sizeof(buf), "font-size:%dpx;line-height:1.75;", s_config.bodySize);
	css += buf;
	css += "margin:0 auto;padding:36px 52px 72px;max-width:920px;-webkit-font-smoothing:antialiased;overflow-wrap:break-word}";

	const double scale = s_config.headingScale / 100.0;
	css += "h1,h2,h3,h4,h5,h6{font-family:" + FontStack(s_config.headingFont,
		"\"Playfair Display\",\"Georgia\",\"Times New Roman\",serif") + ";";
	css += "font-weight:700;color:#241b18;line-height:1.35;margin:1.5em 0 .55em}";
	snprintf(buf, sizeof(buf), "h1{font-size:%.2fem;margin-top:.35em;border-bottom:1px solid var(--border-subtle);padding-bottom:.32em}", 2.05 * scale);
	css += buf;
	snprintf(buf, sizeof(buf), "h2{font-size:%.2fem}h3{font-size:%.2fem}h4{font-size:%.2fem}h5{font-size:%.2fem}h6{font-size:%.2fem;color:var(--text-secondary)}",
			 1.60 * scale, 1.30 * scale, 1.12 * scale, 1.00 * scale, 0.95 * scale);
	css += buf;

	css += "p,ul,ol,pre,blockquote,table,hr{margin:1em 0}li{margin:.25em 0}ul ul,ol ol,ul ol,ol ul{margin:.1em 0}";
	css += "a{color:var(--link);text-decoration:none}a:hover{text-decoration:underline}";
	css += "table{border-collapse:collapse;width:100%;font-size:.95em}";
	css += "th,td{border:1px solid var(--border-medium);padding:10px 12px;text-align:left;vertical-align:top}";
	css += "thead th{background:var(--thead-bg);color:#241b18;font-weight:700;position:sticky;top:0;z-index:1}";
	css += "tbody tr:nth-child(2n){background:" + headBg + "55}";
	css += "pre{background:var(--pre-bg);color:var(--pre-color);border-radius:8px;padding:16px;overflow-x:auto;line-height:1.6}";
	css += "code{font-family:" + FontStack(s_config.codeFont,
		"\"JetBrains Mono\",\"Cascadia Mono\",\"Consolas\",\"Microsoft YaHei\",monospace") + ";";
	snprintf(buf, sizeof(buf), "font-size:%d%%}", s_config.codeScale);
	css += buf;
	css += ":not(pre)>code{background:var(--code-bg);color:var(--inline-code-color);border-radius:6px;padding:.16em .42em}";
	css += "pre>code{background:none;color:inherit;padding:0;font-size:1em}";
	css += "blockquote{border-left:3px solid var(--border-strong);background:" + headBg + "aa;color:var(--text-secondary);padding:6px 18px;border-radius:0 8px 8px 0}";
	css += "blockquote p{margin:.4em 0}";
	css += "img{max-width:100%;height:auto;border-radius:4px}";
	css += "hr{border:0;border-top:1px solid var(--border-subtle);margin:1.6em 0}";
	css += "input[type=checkbox]{margin-right:.45em;accent-color:#a8804f}";
	css += "mark{background:#f5dfa8;border-radius:3px;padding:0 .15em}";
	css += "del,s{color:var(--text-muted)}";
	css += "kbd{font-family:" + FontStack(s_config.codeFont,
		"\"JetBrains Mono\",\"Cascadia Mono\",\"Consolas\",\"Microsoft YaHei\",monospace") + ";font-size:.82em;background:var(--thead-bg);border:1px solid var(--border-medium);border-bottom-width:2px;border-radius:5px;padding:.1em .45em}";
	css += "::selection{background:#ecd9c0}";
	css += "::-webkit-scrollbar{width:10px;height:10px}::-webkit-scrollbar-thumb{background:#8c847f6b;border-radius:5px}";
	css += "::-webkit-scrollbar-thumb:hover{background:#5c544f85}::-webkit-scrollbar-track{background:transparent}";
	// print (PDF export): fixed A4 friendly layout, avoid splitting blocks
	css += "@media print{body{padding:0;max-width:none}thead th{position:static}";
	css += "pre,blockquote,tr,img{break-inside:avoid}h1,h2,h3,h4,h5,h6{break-after:avoid}a{color:inherit}}";
	return css;
}

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
	html += Utf8ToWide(BuildPreviewStyle());
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
	if (html.size() > kMaxNavigateToStringChars) {
		// NavigateToString() accepts at most 2 MB
		HRESULT hr = s_webview->NavigateToString(
			L"<!DOCTYPE html><html><body style=\"font-family:sans-serif;color:#5c544f;padding:40px\">"
			L"<p>Rendered document exceeds the 2 MB limit of the preview.</p></body></html>");
		if (SUCCEEDED(hr)) {
			return;
		}
		s_ownNavExpected = false;
		return;
	}
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
		return S_OK;
	}
	if (s_printOnNavComplete) {
		s_printOnNavComplete = false;
		DoPrintToPdf();
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
// Markdown preview settings (INI persistence).
//

bool ParseHexColor(LPCWSTR text, COLORREF &color) {
	while (*text == L' ' || *text == L'#') {
		text++;
	}
	if (::StrCmpNIW(text, L"0x", 2) == 0) {
		text += 2;
	}
	WCHAR *end = nullptr;
	const unsigned long value = ::wcstoul(text, &end, 16);
	if (end == text || *end != L'\0' || value > 0xFFFFFF) {
		return false;
	}
	color = RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
	return true;
}

void MDPreview_LoadSettings() noexcept {
	MDPreviewConfig_SetDefaults(s_config);
	WCHAR tch[LF_FACESIZE];
	::IniGetString(INI_SECTION_NAME_MD_PREVIEW, L"BodyFont", L"", tch, COUNTOF(tch));
	if (StrNotEmpty(tch)) {
		::StrCpyNW(s_config.bodyFont, tch, LF_FACESIZE);
	}
	s_config.bodySize = ::IniGetInt(INI_SECTION_NAME_MD_PREVIEW, L"BodyFontSize", 16);
	::IniGetString(INI_SECTION_NAME_MD_PREVIEW, L"HeadingFont", L"", tch, COUNTOF(tch));
	if (StrNotEmpty(tch)) {
		::StrCpyNW(s_config.headingFont, tch, LF_FACESIZE);
	}
	s_config.headingScale = ::IniGetInt(INI_SECTION_NAME_MD_PREVIEW, L"HeadingScale", 100);
	::IniGetString(INI_SECTION_NAME_MD_PREVIEW, L"CodeFont", L"", tch, COUNTOF(tch));
	if (StrNotEmpty(tch)) {
		::StrCpyNW(s_config.codeFont, tch, LF_FACESIZE);
	}
	s_config.codeScale = ::IniGetInt(INI_SECTION_NAME_MD_PREVIEW, L"CodeScale", 88);

	::IniGetString(INI_SECTION_NAME_MD_PREVIEW, L"TableHeadBg", L"#F9F6F0", tch, COUNTOF(tch));
	ParseHexColor(tch, s_config.tableHeadBg);
	::IniGetString(INI_SECTION_NAME_MD_PREVIEW, L"TableBorder", L"#D7C4B2", tch, COUNTOF(tch));
	ParseHexColor(tch, s_config.tableBorder);

	s_config.bodySize = std::clamp(s_config.bodySize, 9, 40);
	s_config.headingScale = std::clamp(s_config.headingScale, 50, 300);
	s_config.codeScale = std::clamp(s_config.codeScale, 40, 200);
}

void MDPreview_SaveSettings() noexcept {
	WCHAR tch[16];
	::IniSetString(INI_SECTION_NAME_MD_PREVIEW, L"BodyFont", s_config.bodyFont);
	wsprintf(tch, L"%d", s_config.bodySize);
	::IniSetString(INI_SECTION_NAME_MD_PREVIEW, L"BodyFontSize", tch);
	::IniSetString(INI_SECTION_NAME_MD_PREVIEW, L"HeadingFont", s_config.headingFont);
	wsprintf(tch, L"%d", s_config.headingScale);
	::IniSetString(INI_SECTION_NAME_MD_PREVIEW, L"HeadingScale", tch);
	::IniSetString(INI_SECTION_NAME_MD_PREVIEW, L"CodeFont", s_config.codeFont);
	wsprintf(tch, L"%d", s_config.codeScale);
	::IniSetString(INI_SECTION_NAME_MD_PREVIEW, L"CodeScale", tch);
	WCHAR szColor[8];
	FormatHexColor(s_config.tableHeadBg, szColor);
	::IniSetString(INI_SECTION_NAME_MD_PREVIEW, L"TableHeadBg", szColor);
	FormatHexColor(s_config.tableBorder, szColor);
	::IniSetString(INI_SECTION_NAME_MD_PREVIEW, L"TableBorder", szColor);
}

namespace {

//=============================================================================
// Markdown preview settings dialog.
//

void FillFontCombo(HWND hwndCombo, LPCWSTR current) {
	std::vector<std::wstring> names;
	HDC hdc = ::GetDC(hwndCombo);
	LOGFONTW lf;
	ZeroMemory(&lf, sizeof(lf));
	lf.lfCharSet = DEFAULT_CHARSET;
	::EnumFontFamiliesExW(hdc, &lf, [](const LOGFONTW *plf, const TEXTMETRICW *, DWORD, LPARAM lParam) -> int {
		auto list = reinterpret_cast<std::vector<std::wstring> *>(lParam);
		if (plf->lfFaceName[0] != L'@') {
			list->emplace_back(plf->lfFaceName);
		}
		return 1;
	}, reinterpret_cast<LPARAM>(&names), 0);
	::ReleaseDC(hwndCombo, hdc);

	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	::SendMessage(hwndCombo, CB_RESETCONTENT, 0, 0);
	bool found = false;
	for (const std::wstring &name : names) {
		const LPARAM index = ::SendMessageW(hwndCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
		if (!found && current[0] != L'\0' && ::StrCmpIW(name.c_str(), current) == 0) {
			::SendMessageW(hwndCombo, CB_SETCURSEL, index, 0);
			found = true;
		}
	}
	if (current[0] != L'\0') {
		// keep unknown font names selectable (font may not be installed yet)
		if (!found) {
			const LPARAM index = ::SendMessageW(hwndCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(current));
			::SendMessageW(hwndCombo, CB_SETCURSEL, index, 0);
		}
	} else {
		::SendMessageW(hwndCombo, CB_SETCURSEL, -1, 0);
	}
}

void GetFontComboText(HWND hwndCombo, WCHAR (&text)[LF_FACESIZE]) {
	const int index = static_cast<int>(::SendMessageW(hwndCombo, CB_GETCURSEL, 0, 0));
	if (index >= 0) {
		const int len = static_cast<int>(::SendMessageW(hwndCombo, CB_GETLBTEXTLEN, index, 0));
		if (len > 0 && len < LF_FACESIZE) {
			::SendMessageW(hwndCombo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text));
			return;
		}
	}
	::GetWindowTextW(hwndCombo, text, LF_FACESIZE);
}

bool PickColor(HWND hwnd, COLORREF &color) {
	static COLORREF custColors[16] = {};
	CHOOSECOLORW cc;
	ZeroMemory(&cc, sizeof(cc));
	cc.lStructSize = sizeof(cc);
	cc.hwndOwner = hwnd;
	cc.rgbResult = color;
	cc.lpCustColors = custColors;
	cc.Flags = CC_FULLOPEN | CC_RGBINIT;
	if (!::ChooseColorW(&cc)) {
		return false;
	}
	color = cc.rgbResult;
	return true;
}

void ReadDialogValues(HWND hwnd) noexcept {
	GetFontComboText(::GetDlgItem(hwnd, IDC_MDPREVIEW_BODYFONT), s_config.bodyFont);
	GetFontComboText(::GetDlgItem(hwnd, IDC_MDPREVIEW_HEADFONT), s_config.headingFont);
	GetFontComboText(::GetDlgItem(hwnd, IDC_MDPREVIEW_CODEFONT), s_config.codeFont);
	s_config.bodySize = std::clamp(static_cast<int>(::GetDlgItemInt(hwnd, IDC_MDPREVIEW_BODYSIZE, nullptr, FALSE)), 9, 40);
	s_config.headingScale = std::clamp(static_cast<int>(::GetDlgItemInt(hwnd, IDC_MDPREVIEW_HEADSCALE, nullptr, FALSE)), 50, 300);
	s_config.codeScale = std::clamp(static_cast<int>(::GetDlgItemInt(hwnd, IDC_MDPREVIEW_CODESCALE, nullptr, FALSE)), 40, 200);
	WCHAR tch[64];
	::GetDlgItemTextW(hwnd, IDC_MDPREVIEW_HEADBG, tch, COUNTOF(tch));
	if (!ParseHexColor(tch, s_config.tableHeadBg)) {
		s_config.tableHeadBg = RGB(0xF9, 0xF6, 0xF0);
	}
	::GetDlgItemTextW(hwnd, IDC_MDPREVIEW_BORDER, tch, COUNTOF(tch));
	if (!ParseHexColor(tch, s_config.tableBorder)) {
		s_config.tableBorder = RGB(0xD7, 0xC4, 0xB2);
	}
}

void WriteDialogValues(HWND hwnd, const MDPreviewConfig &cfg) {
	::SetDlgItemTextW(hwnd, IDC_MDPREVIEW_BODYFONT, cfg.bodyFont);
	::SetDlgItemInt(hwnd, IDC_MDPREVIEW_BODYSIZE, static_cast<UINT>(cfg.bodySize), FALSE);
	::SetDlgItemTextW(hwnd, IDC_MDPREVIEW_HEADFONT, cfg.headingFont);
	::SetDlgItemInt(hwnd, IDC_MDPREVIEW_HEADSCALE, static_cast<UINT>(cfg.headingScale), FALSE);
	::SetDlgItemTextW(hwnd, IDC_MDPREVIEW_CODEFONT, cfg.codeFont);
	::SetDlgItemInt(hwnd, IDC_MDPREVIEW_CODESCALE, static_cast<UINT>(cfg.codeScale), FALSE);
	WCHAR tch[8];
	FormatHexColor(cfg.tableHeadBg, tch);
	::SetDlgItemTextW(hwnd, IDC_MDPREVIEW_HEADBG, tch);
	FormatHexColor(cfg.tableBorder, tch);
	::SetDlgItemTextW(hwnd, IDC_MDPREVIEW_BORDER, tch);
}

INT_PTR CALLBACK MDPreviewSettingsDlgProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam) noexcept {
	switch (umsg) {
	case WM_INITDIALOG:
		WriteDialogValues(hwnd, s_config);
		::SendDlgItemMessageW(hwnd, IDC_MDPREVIEW_BODYSIZE_SPIN, UDM_SETRANGE32, 9, 40);
		::SendDlgItemMessageW(hwnd, IDC_MDPREVIEW_HEADSCALE_SPIN, UDM_SETRANGE32, 50, 300);
		::SendDlgItemMessageW(hwnd, IDC_MDPREVIEW_CODESCALE_SPIN, UDM_SETRANGE32, 40, 200);
		CenterDlgInParent(hwnd);
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDOK:
			ReadDialogValues(hwnd);
			::EndDialog(hwnd, IDOK);
			break;
		case IDCANCEL:
			::EndDialog(hwnd, IDCANCEL);
			break;
		case IDC_MDPREVIEW_RESTORE: {
			MDPreviewConfig defaults;
			MDPreviewConfig_SetDefaults(defaults);
			WriteDialogValues(hwnd, defaults);
		} break;
		case IDC_MDPREVIEW_HEADBG_BTN:
		case IDC_MDPREVIEW_BORDER_BTN: {
			const int editId = (LOWORD(wParam) == IDC_MDPREVIEW_HEADBG_BTN) ? IDC_MDPREVIEW_HEADBG : IDC_MDPREVIEW_BORDER;
			WCHAR tch[64];
			WCHAR szHex[8];
			COLORREF color;
			::GetDlgItemTextW(hwnd, editId, tch, COUNTOF(tch));
			if (!ParseHexColor(tch, color)) {
				color = (editId == IDC_MDPREVIEW_HEADBG) ? s_config.tableHeadBg : s_config.tableBorder;
			}
			if (PickColor(hwnd, color)) {
				FormatHexColor(color, szHex);
				::SetDlgItemTextW(hwnd, editId, szHex);
			}
		} break;
		}
		return TRUE;
	}
	return FALSE;
}

} // namespace

void MDPreview_SettingsDialog(HWND hwnd) noexcept {
	if (ThemedDialogBoxParam(g_hInstance, MAKEINTRESOURCEW(IDD_MDPREVIEW_SETTINGS), hwnd,
							 MDPreviewSettingsDlgProc, 0) == IDOK) {
		MDPreview_SaveSettings();
		MDPreview_RequestUpdate(false);		// apply immediately when the preview is visible
	}
}

//=============================================================================
// PDF export (WebView2 PrintToPdf, fixed A4 portrait layout).
//

namespace {

HRESULT STDMETHODCALLTYPE PrintToPdfHandler::Invoke(HRESULT errorCode, BOOL isSuccessful) {
	if (SUCCEEDED(errorCode) && isSuccessful) {
		MsgBoxInfo(MB_OK, IDS_MDPREVIEW_PDF_SAVED);
	} else {
		MsgBoxWarn(MB_OK, IDS_MDPREVIEW_PDF_FAILED);
	}
	return S_OK;
}

void DoPrintToPdf() {
	std::wstring path;
	path.swap(s_pdfTargetPath);
	if (!s_webview || path.empty()) {
		return;
	}

	ICoreWebView2PrintSettings *print = nullptr;
	ICoreWebView2Environment6 *env6 = nullptr;
	if (s_env && SUCCEEDED(s_env->QueryInterface(kIID_ICoreWebView2Environment6, reinterpret_cast<void **>(&env6))) &&
		env6 != nullptr) {
		env6->CreatePrintSettings(&print);
		env6->Release();
	}
	if (print == nullptr) {
		MsgBoxWarn(MB_OK, IDS_MDPREVIEW_PDF_FAILED);
		return;
	}

	print->put_Orientation(COREWEBVIEW2_PRINT_ORIENTATION_PORTRAIT);
	print->put_ScaleFactor(1.0);
	print->put_PageWidth(8.27);		// A4 portrait, inches
	print->put_PageHeight(11.69);
	print->put_MarginTop(0.4);
	print->put_MarginBottom(0.4);
	print->put_MarginLeft(0.4);
	print->put_MarginRight(0.4);
	print->put_ShouldPrintBackgrounds(TRUE);
	print->put_ShouldPrintHeaderAndFooter(FALSE);

	ICoreWebView2_7 *webview7 = nullptr;
	if (SUCCEEDED(s_webview->QueryInterface(kIID_ICoreWebView2_7, reinterpret_cast<void **>(&webview7))) &&
		webview7 != nullptr) {
		PrintToPdfHandler *handler = new PrintToPdfHandler();
		webview7->PrintToPdf(path.c_str(), print, handler);
		handler->Release();
		webview7->Release();
	} else {
		MsgBoxWarn(MB_OK, IDS_MDPREVIEW_PDF_FAILED);
	}
	print->Release();
}

} // namespace

void MDPreview_ExportPdf(HWND hwnd) noexcept {
	if (s_webview == nullptr) {
		MsgBoxWarn(MB_OK, IDS_MDPREVIEW_UNAVAILABLE);
		return;
	}

	WCHAR szFile[MAX_PATH];
	if (StrNotEmpty(szCurFile)) {
		lstrcpyn(szFile, szCurFile, COUNTOF(szFile));
		::PathRenameExtensionW(szFile, L".pdf");
	} else {
		lstrcpy(szFile, L"Markdown.pdf");
	}

	WCHAR szFilter[256];
	GetString(IDS_MDPREVIEW_PDF_FILTER, szFilter, COUNTOF(szFilter));
	for (LPWSTR p = szFilter; *p != L'\0'; ++p) {
		if (*p == L'|') {
			*p = L'\0';
		}
	}

	OPENFILENAMEW ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hwnd;
	ofn.lpstrFilter = szFilter;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = COUNTOF(szFile);
	ofn.lpstrDefExt = L"pdf";
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
	if (!::GetSaveFileNameW(&ofn)) {
		return;
	}

	s_pdfTargetPath = szFile;
	if (s_pageAlive && !s_renderBusy) {
		s_printOnNavComplete = false;
		DoPrintToPdf();
	} else {
		// re-render the current buffer first; the PDF is written on NavigationCompleted
		s_printOnNavComplete = true;
		if (!s_renderBusy) {
			s_renderBusy = true;
			if (s_pageAlive) {
				s_webview->ExecuteScript(L"window.scrollY|0", new ScrollCaptureHandler());
			} else {
				RenderNow();
			}
		}
	}
}

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
