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
