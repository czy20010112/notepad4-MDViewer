// This file is part of Notepad4.
// See License.txt for details about distribution and modification.
// Markdown preview pane rendered by WebView2, markdown parsed by md4c.

#pragma once

// Create the (initially hidden) preview host window as a child of the main window.
void MDPreview_Create(HWND hwndParent, HINSTANCE hInstance) noexcept;

// Tear down the preview pane and the WebView2 controller.
void MDPreview_Destroy() noexcept;

// Show or hide the preview pane. When shown, the document text is rendered.
void MDPreview_SetVisible(bool bShow) noexcept;

// Whether the preview pane is ready to display content (WebView2 initialized).
bool MDPreview_IsReady() noexcept;

// Host window handle used by the main window layout (MsgSize).
HWND MDPreview_GetHostWindow() noexcept;

// Re-render the current document. Safe to call when hidden or not ready.
// Calls from typing are throttled with a short timer.
void MDPreview_RequestUpdate(bool bDelayed) noexcept;

// Markdown preview appearance settings (persisted in the INI file).
struct MDPreviewConfig {
	WCHAR bodyFont[LF_FACESIZE];	// document body font family, empty = default stack
	int bodySize;					// base font size in px, default 16
	WCHAR headingFont[LF_FACESIZE];	// heading font family, empty = default serif stack
	int headingScale;				// heading size in percent of body size, default 100
	WCHAR codeFont[LF_FACESIZE];	// code font family, empty = default monospace stack
	int codeScale;					// code size in percent of body size, default 88
	COLORREF tableHeadBg;			// table header row background, default #F9F6F0
	COLORREF tableBorder;			// table cell border color, default #D7C4B2
};

void MDPreview_LoadSettings() noexcept;
void MDPreview_SaveSettings() noexcept;

// Show the preview settings dialog (fonts, sizes, table colors).
void MDPreview_SettingsDialog(HWND hwnd) noexcept;

// Render the current document and export it as PDF (A4, fixed layout).
void MDPreview_ExportPdf(HWND hwnd) noexcept;
