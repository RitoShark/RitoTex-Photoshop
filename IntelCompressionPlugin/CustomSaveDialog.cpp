////////////////////////////////////////////////////////////////////////////////
// Copyright 2017 Intel Corporation
// Modified 2026 RitoShark - Custom Save Dialog with Dark Theme
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License.  You may obtain a copy
// of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
// License for the specific language governing permissions and limitations
// under the License.
////////////////////////////////////////////////////////////////////////////////

#include "CustomSaveDialog.h"
#include "resource.h"
#include "IntelPluginName.h"
#include "IntelPluginUIWin.h"
#include "PreviewDialog.h"
#include "PIUFile.h"
#include <vector>
#include <list>
#include <sstream>
#include <fstream>
#include <shlobj.h>
#include <algorithm>
#include <uxtheme.h>
#include <windowsx.h>
#pragma comment(lib, "uxtheme.lib")

using namespace std;

//-------------------------------------------------------------------------------
// Dark Theme Static Resources
//-------------------------------------------------------------------------------
HBRUSH CustomSaveDialog::s_brushDialogBg = nullptr;
HBRUSH CustomSaveDialog::s_brushEditBg = nullptr;
HPEN CustomSaveDialog::s_penBorder = nullptr;

//-------------------------------------------------------------------------------
// Helper Type Definitions (from SaveOptionsDialog.cpp)
//-------------------------------------------------------------------------------
typedef vector<string>(*VStringFunc)(void);

// Container for help button data
typedef struct {
	const int itemNum;		// WinForm ID of the help button
	VStringFunc func;		// Function to call to get the help text to display
} HelpButtonAndTextFunc;

// Struct to associate dropdown list with context text
typedef struct {
	const int itemNum;			// DropDown list WinForm ID
	const int contextItemNum;	// Context Text WinForm ID
} ComboAndContextStringID;

//-------------------------------------------------------------------------------
// Helper Function Declarations (from SaveOptionsDialog.cpp)
//-------------------------------------------------------------------------------
vector<string> GetCompressionHelpText(void);
vector<string> GetTextureTypeHelpText(void);
vector<string> GetPreCompressOpsHelpText(void);
vector<string> GetMipMapsHelpText(void);

// Help button mappings
HelpButtonAndTextFunc const helpButtonTextItem[] = {
	{ IDC_COMPRESSION_HELP, GetCompressionHelpText },
	{ IDC_TEXTURETYPE_HELP, GetTextureTypeHelpText },
	{ IDC_PRECOMPRESS_HELP, GetPreCompressOpsHelpText },
	{ IDC_MIPMAP_HELP, GetMipMapsHelpText }
};

// Combo and context string mappings
ComboAndContextStringID const gComboContextItems[] = {
	{ IDC_COMPRESSION_COMBO, IDC_COMPRESSION_HINT },
	{ IDC_TEXTURETYPE_COMBO, IDC_TEXTURETYPE_HINT },
	{ IDC_MIPMAP_COMBO, IDC_MIPMAPS_HINT }
};

//-------------------------------------------------------------------------------
// Constructor
//-------------------------------------------------------------------------------
CustomSaveDialog::CustomSaveDialog(IntelPlugin* plugin_)
	: hDlg(nullptr)
	, plugin(plugin_)
	, globalParams(plugin_->GetData())
	, MaxMipLevel(0)
	, m_closeHover(false)
	, m_ditheringChecked(true)
	, m_hTitleFont(nullptr)
	, m_hUIFont(nullptr)
{
	mPathToPresetDirectory.clear();

	// Calculate max mip level from image dimensions
	const FormatRecord* formatRecord = plugin->GetFormatRecord();
	MaxMipLevel = static_cast<int>(1 + floor(Log2(max(
		static_cast<double>(formatRecord->imageSize.h),
		static_cast<double>(formatRecord->imageSize.v)))));

	// Get path to Local per user configuration files (%USERPROFILE%\AppData\Local\Intel\PhotoshopDDSPlugin\)
	wchar_t* localAppData = 0;
	HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &localAppData);

	if (SUCCEEDED(hr))
	{
		// Convert wide char to char
		char str[MAX_PATH + 1] = {};
		wcstombs(str, localAppData, MAX_PATH);
		CoTaskMemFree(static_cast<void*>(localAppData));

		mPathToPresetDirectory = str;

		// Create directory if does not exist
		mPathToPresetDirectory.append("\\Intel");
		if (CreateDirectory(mPathToPresetDirectory.c_str(), NULL) || ERROR_ALREADY_EXISTS == GetLastError())
		{
			mPathToPresetDirectory.append("\\PhotoshopDDSPlugin\\");
			if (CreateDirectory(mPathToPresetDirectory.c_str(), NULL) || ERROR_ALREADY_EXISTS == GetLastError())
			{
				// Directory created successfully
			}
			else
			{
				mPathToPresetDirectory.clear();
			}
		}
		else
		{
			mPathToPresetDirectory.clear();
		}
	}
}

//-------------------------------------------------------------------------------
// Destructor
//-------------------------------------------------------------------------------
CustomSaveDialog::~CustomSaveDialog()
{
	if (m_hTitleFont) { DeleteObject(m_hTitleFont); m_hTitleFont = nullptr; }
	if (m_hUIFont) { DeleteObject(m_hUIFont); m_hUIFont = nullptr; }
	CleanupThemeResources();
}

//-------------------------------------------------------------------------------
// DoModal - Public Entry Point
//-------------------------------------------------------------------------------
int32 CustomSaveDialog::DoModal(IntelPlugin* plugin)
{
	CustomSaveDialog dialog(plugin);
	INT_PTR result = IDCANCEL;

	if (plugin->GetData()->queryForParameters)
	{
		// Interactive mode: show UI dialog
		result = DialogBoxParam(
			GetDLLInstance(),
			MAKEINTRESOURCE(IDD_MAINDIALOG),
			GetActiveWindow(),
			DialogProc,
			reinterpret_cast<LPARAM>(&dialog)
		);
	}
	else
	{
		// Batch mode: load preset without showing UI
		string presetName = reinterpret_cast<char*>(plugin->GetData()->presetBatchName) + 1;

		dialog.LoadPresets();
		dialog.InitDataFromPreset(presetName);
		result = dialog.mPresets.find(presetName) != dialog.mPresets.end() ? IDOK : IDCANCEL;
	}

	return static_cast<int32>(result);
}

//-------------------------------------------------------------------------------
// DialogProc - Static callback (bridges to instance method)
//-------------------------------------------------------------------------------
INT_PTR CALLBACK CustomSaveDialog::DialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	CustomSaveDialog* dialog = nullptr;

	if (msg == WM_INITDIALOG)
	{
		// Store instance pointer on first message
		dialog = reinterpret_cast<CustomSaveDialog*>(lParam);
		dialog->hDlg = hDlg;
		SetWindowLongPtr(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
		dialog->Init();
		return TRUE;
	}
	else
	{
		// Retrieve instance pointer for subsequent messages
		dialog = reinterpret_cast<CustomSaveDialog*>(GetWindowLongPtr(hDlg, DWLP_USER));
	}

	if (dialog)
		return dialog->WindowProc(msg, wParam, lParam);

	return FALSE;
}

//-------------------------------------------------------------------------------
// WindowProc - Instance message handler
//-------------------------------------------------------------------------------
INT_PTR CustomSaveDialog::WindowProc(UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		// Dark theme color messages
		case WM_CTLCOLORDLG:
		case WM_CTLCOLORSTATIC:
		case WM_CTLCOLOREDIT:
		case WM_CTLCOLORLISTBOX:
		case WM_CTLCOLORBTN:
			return HandleColorMessages(msg, wParam, lParam);

		case WM_ERASEBKGND:
		{
			HDC hDC = (HDC)wParam;
			RECT rcClient;
			GetClientRect(hDlg, &rcClient);
			FillRect(hDC, &rcClient, s_brushDialogBg);
			return TRUE;
		}

		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hDC = BeginPaint(hDlg, &ps);
			RECT rcClient;
			GetClientRect(hDlg, &rcClient);

			// Fill main background
			FillRect(hDC, &rcClient, s_brushDialogBg);

			// Draw custom title bar
			DrawTitleBar(hDC, rcClient);

			// Draw subtle border around the entire window
			HPEN hPen = CreatePen(PS_SOLID, 1, DarkTheme::BORDER);
			HPEN hOldPen = (HPEN)SelectObject(hDC, hPen);
			HBRUSH hOldBrush = (HBRUSH)SelectObject(hDC, GetStockObject(NULL_BRUSH));
			Rectangle(hDC, rcClient.left, rcClient.top, rcClient.right, rcClient.bottom);
			SelectObject(hDC, hOldPen);
			SelectObject(hDC, hOldBrush);
			DeleteObject(hPen);

			EndPaint(hDlg, &ps);
			return TRUE;
		}

		case WM_LBUTTONDOWN:
		{
			int xPos = GET_X_LPARAM(lParam);
			int yPos = GET_Y_LPARAM(lParam);

			if (yPos < TITLEBAR_HEIGHT)
			{
				RECT rcClient;
				GetClientRect(hDlg, &rcClient);

				// Check if clicking close button
				if (xPos >= rcClient.right - CLOSE_BTN_WIDTH)
				{
					EndDialog(hDlg, IDCANCEL);
					return TRUE;
				}

				// Drag window by title bar
				ReleaseCapture();
				SendMessage(hDlg, WM_NCLBUTTONDOWN, HTCAPTION, 0);
				return TRUE;
			}
			return FALSE;
		}

		case WM_MOUSEMOVE:
		{
			int xPos = GET_X_LPARAM(lParam);
			int yPos = GET_Y_LPARAM(lParam);
			RECT rcClient;
			GetClientRect(hDlg, &rcClient);

			bool overClose = (yPos < TITLEBAR_HEIGHT && xPos >= rcClient.right - CLOSE_BTN_WIDTH);

			if (overClose != m_closeHover)
			{
				m_closeHover = overClose;
				// Invalidate just the title bar area
				RECT rcTitleBar = { 0, 0, rcClient.right, TITLEBAR_HEIGHT };
				InvalidateRect(hDlg, &rcTitleBar, FALSE);
			}

			// Track mouse leave
			TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hDlg, 0 };
			TrackMouseEvent(&tme);
			return FALSE;
		}

		case WM_MOUSELEAVE:
		{
			if (m_closeHover)
			{
				m_closeHover = false;
				RECT rcClient;
				GetClientRect(hDlg, &rcClient);
				RECT rcTitleBar = { 0, 0, rcClient.right, TITLEBAR_HEIGHT };
				InvalidateRect(hDlg, &rcTitleBar, FALSE);
			}
			return FALSE;
		}

		case WM_MEASUREITEM:
		{
			MeasureComboItem((LPMEASUREITEMSTRUCT)lParam);
			return TRUE;
		}

		case WM_DRAWITEM:
		{
			LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;

			// Check if this is our custom checkbox
			if (pDIS->CtlID == IDC_DITHERING_CHECK)
			{
				DrawCustomCheckbox(pDIS);
				return TRUE;
			}

			// Check if this is a combo box
			if (pDIS->CtlType == ODT_COMBOBOX)
			{
				DrawCustomComboItem(pDIS);
				return TRUE;
			}

			// Otherwise handle push buttons
			if (pDIS->CtlType == ODT_BUTTON)
			{
				// Get button state
				bool isPressed = (pDIS->itemState & ODS_SELECTED);
				bool isFocused = (pDIS->itemState & ODS_FOCUS);

				// Choose colors based on button type
				COLORREF bgColor;
				if (pDIS->CtlID == IDOK)
					bgColor = DarkTheme::ACCENT;
				else if (pDIS->CtlID == IDC_PREVIEW_BUTTON)
					bgColor = DarkTheme::ACCENT;
				else
					bgColor = DarkTheme::BUTTON_BG;

				if (isPressed)
				{
					int r = max(0, (int)GetRValue(bgColor) - 30);
					int g = max(0, (int)GetGValue(bgColor) - 30);
					int b = max(0, (int)GetBValue(bgColor) - 30);
					bgColor = RGB(r, g, b);
				}

				// Fill button background with rounded corners feel
				HBRUSH hBrush = CreateSolidBrush(bgColor);
				RECT rcBtn = pDIS->rcItem;
				FillRect(pDIS->hDC, &rcBtn, hBrush);
				DeleteObject(hBrush);

				// Draw button border
				COLORREF borderColor = (pDIS->CtlID == IDOK || pDIS->CtlID == IDC_PREVIEW_BUTTON)
					? DarkTheme::ACCENT : DarkTheme::BORDER;
				if (isFocused)
					borderColor = DarkTheme::ACCENT_HOVER;
				HPEN hPen = CreatePen(PS_SOLID, 1, borderColor);
				HPEN hOldPen = (HPEN)SelectObject(pDIS->hDC, hPen);
				HBRUSH hOldBrush = (HBRUSH)SelectObject(pDIS->hDC, GetStockObject(NULL_BRUSH));
				Rectangle(pDIS->hDC, rcBtn.left, rcBtn.top, rcBtn.right, rcBtn.bottom);
				SelectObject(pDIS->hDC, hOldPen);
				SelectObject(pDIS->hDC, hOldBrush);
				DeleteObject(hPen);

				// Draw button text
				char szText[256];
				GetWindowTextA(pDIS->hwndItem, szText, 256);
				HFONT hOldFont = nullptr;
				if (m_hUIFont)
					hOldFont = (HFONT)SelectObject(pDIS->hDC, m_hUIFont);
				SetTextColor(pDIS->hDC, DarkTheme::TEXT_PRIMARY);
				SetBkMode(pDIS->hDC, TRANSPARENT);

				RECT rcText = rcBtn;
				if (isPressed) {
					rcText.left++; rcText.top++;
				}

				DrawTextA(pDIS->hDC, szText, -1, &rcText,
				         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				if (hOldFont)
					SelectObject(pDIS->hDC, hOldFont);

				return TRUE;
			}

			return FALSE;
		}

		case WM_COMMAND:
		{
			// Special handling for our owner-drawn checkbox
			UINT ctlId = LOWORD(wParam);
			if (ctlId == IDC_DITHERING_CHECK)
			{
				// Toggle checkbox state
				m_ditheringChecked = !m_ditheringChecked;
				// Redraw the checkbox
				InvalidateRect(GetDlgItem(hDlg, IDC_DITHERING_CHECK), NULL, FALSE);
			}
			HandleCommand(wParam, lParam);
			return TRUE;
		}

		case WM_CLOSE:
			EndDialog(hDlg, IDCANCEL);
			return TRUE;

		default:
			return FALSE;
	}
}

//-------------------------------------------------------------------------------
// HandleColorMessages - Dark theme color handlers
//-------------------------------------------------------------------------------
INT_PTR CustomSaveDialog::HandleColorMessages(UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_CTLCOLORDLG:
			return (INT_PTR)s_brushDialogBg;

		case WM_CTLCOLORSTATIC:
		{
			HDC hDC = (HDC)wParam;
			SetTextColor(hDC, DarkTheme::TEXT_PRIMARY);
			SetBkColor(hDC, DarkTheme::DIALOG_BG);
			SetBkMode(hDC, TRANSPARENT);
			return (INT_PTR)s_brushDialogBg;
		}

		case WM_CTLCOLOREDIT:
		case WM_CTLCOLORLISTBOX:
		{
			HDC hDC = (HDC)wParam;
			SetTextColor(hDC, DarkTheme::TEXT_PRIMARY);
			SetBkColor(hDC, DarkTheme::EDIT_BG);
			return (INT_PTR)s_brushEditBg;
		}

		case WM_CTLCOLORBTN:
		{
			// Handle button colors
			HDC hDC = (HDC)wParam;
			SetTextColor(hDC, DarkTheme::TEXT_PRIMARY);
			SetBkMode(hDC, TRANSPARENT);
			return (INT_PTR)s_brushDialogBg;
		}

		default:
			return FALSE;
	}
}

//-------------------------------------------------------------------------------
// InitThemeResources - Create GDI resources for dark theme
//-------------------------------------------------------------------------------
void CustomSaveDialog::InitThemeResources()
{
	if (!s_brushDialogBg)
	{
		s_brushDialogBg = CreateSolidBrush(DarkTheme::DIALOG_BG);
		s_brushEditBg = CreateSolidBrush(DarkTheme::EDIT_BG);
		s_penBorder = CreatePen(PS_SOLID, 1, DarkTheme::BORDER);
	}

	// Create title bar font (semibold)
	if (!m_hTitleFont)
	{
		LOGFONT lf = {};
		lf.lfHeight = -14;
		lf.lfWeight = FW_SEMIBOLD;
		lf.lfCharSet = DEFAULT_CHARSET;
		lf.lfQuality = CLEARTYPE_QUALITY;
		strcpy(lf.lfFaceName, "Segoe UI");
		m_hTitleFont = CreateFontIndirect(&lf);
	}

	// Create UI font
	if (!m_hUIFont)
	{
		LOGFONT lf = {};
		lf.lfHeight = -12;
		lf.lfWeight = FW_NORMAL;
		lf.lfCharSet = DEFAULT_CHARSET;
		lf.lfQuality = CLEARTYPE_QUALITY;
		strcpy(lf.lfFaceName, "Segoe UI");
		m_hUIFont = CreateFontIndirect(&lf);
	}
}

//-------------------------------------------------------------------------------
// CleanupThemeResources - Delete GDI resources
//-------------------------------------------------------------------------------
void CustomSaveDialog::CleanupThemeResources()
{
	if (s_brushDialogBg)
	{
		DeleteObject(s_brushDialogBg);
		DeleteObject(s_brushEditBg);
		DeleteObject(s_penBorder);
		s_brushDialogBg = nullptr;
		s_brushEditBg = nullptr;
		s_penBorder = nullptr;
	}
}

//-------------------------------------------------------------------------------
// Log2 - Helper function
//-------------------------------------------------------------------------------
double CustomSaveDialog::Log2(double n)
{
	return log(n) / log(2.0);
}

//-------------------------------------------------------------------------------
// DrawTitleBar - Custom title bar with close button
//-------------------------------------------------------------------------------
void CustomSaveDialog::DrawTitleBar(HDC hDC, const RECT& rcClient)
{
	// Title bar background
	RECT rcTitleBar = { 0, 0, rcClient.right, TITLEBAR_HEIGHT };
	HBRUSH hBrushTitleBg = CreateSolidBrush(DarkTheme::TITLEBAR_BG);
	FillRect(hDC, &rcTitleBar, hBrushTitleBg);
	DeleteObject(hBrushTitleBg);

	// Separator line below title bar
	HPEN hSepPen = CreatePen(PS_SOLID, 1, DarkTheme::SEPARATOR);
	HPEN hOldPen = (HPEN)SelectObject(hDC, hSepPen);
	MoveToEx(hDC, 0, TITLEBAR_HEIGHT - 1, NULL);
	LineTo(hDC, rcClient.right, TITLEBAR_HEIGHT - 1);
	SelectObject(hDC, hOldPen);
	DeleteObject(hSepPen);

	// Title text
	HFONT hOldFont = nullptr;
	if (m_hTitleFont)
		hOldFont = (HFONT)SelectObject(hDC, m_hTitleFont);
	SetTextColor(hDC, DarkTheme::TEXT_PRIMARY);
	SetBkMode(hDC, TRANSPARENT);

	RECT rcTitleText = { 12, 0, rcClient.right - CLOSE_BTN_WIDTH - 8, TITLEBAR_HEIGHT };
	DrawTextA(hDC, "RitoTex - Export Texture", -1, &rcTitleText,
	         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

	if (hOldFont)
		SelectObject(hDC, hOldFont);

	// Close button
	RECT rcCloseBtn = {
		rcClient.right - CLOSE_BTN_WIDTH,
		0,
		rcClient.right,
		TITLEBAR_HEIGHT
	};

	if (m_closeHover)
	{
		HBRUSH hBrushClose = CreateSolidBrush(DarkTheme::CLOSE_HOVER);
		FillRect(hDC, &rcCloseBtn, hBrushClose);
		DeleteObject(hBrushClose);
	}

	// Draw X glyph
	SetTextColor(hDC, DarkTheme::TEXT_PRIMARY);
	SetBkMode(hDC, TRANSPARENT);

	// Use a clean font for the X
	LOGFONT lfClose = {};
	lfClose.lfHeight = -14;
	lfClose.lfWeight = FW_NORMAL;
	lfClose.lfCharSet = DEFAULT_CHARSET;
	lfClose.lfQuality = CLEARTYPE_QUALITY;
	strcpy(lfClose.lfFaceName, "Segoe MDL2 Assets");
	HFONT hCloseFont = CreateFontIndirect(&lfClose);

	if (hCloseFont)
	{
		HFONT hPrevFont = (HFONT)SelectObject(hDC, hCloseFont);
		// MDL2 Assets "\xEE\x80\xA8" = E0A8 = ChromeClose, fallback to simple X
		DrawTextW(hDC, L"\xE8BB", 1, &rcCloseBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(hDC, hPrevFont);
		DeleteObject(hCloseFont);
	}
	else
	{
		// Fallback: draw a simple X with lines
		int cx = (rcCloseBtn.left + rcCloseBtn.right) / 2;
		int cy = (rcCloseBtn.top + rcCloseBtn.bottom) / 2;
		HPEN hXPen = CreatePen(PS_SOLID, 1, DarkTheme::TEXT_PRIMARY);
		HPEN hPrevPen = (HPEN)SelectObject(hDC, hXPen);
		MoveToEx(hDC, cx - 5, cy - 5, NULL); LineTo(hDC, cx + 6, cy + 6);
		MoveToEx(hDC, cx + 5, cy - 5, NULL); LineTo(hDC, cx - 6, cy + 6);
		SelectObject(hDC, hPrevPen);
		DeleteObject(hXPen);
	}
}

//-------------------------------------------------------------------------------
// DrawCustomCheckbox - Owner-drawn checkbox with dark theme
//-------------------------------------------------------------------------------
void CustomSaveDialog::DrawCustomCheckbox(LPDRAWITEMSTRUCT pDIS)
{
	HDC hDC = pDIS->hDC;
	RECT rc = pDIS->rcItem;

	// Fill background
	HBRUSH hBgBrush = CreateSolidBrush(DarkTheme::DIALOG_BG);
	FillRect(hDC, &rc, hBgBrush);
	DeleteObject(hBgBrush);

	// Checkbox box dimensions
	int boxSize = 16;
	int boxY = rc.top + (rc.bottom - rc.top - boxSize) / 2;
	RECT rcBox = { rc.left, boxY, rc.left + boxSize, boxY + boxSize };

	// Draw checkbox box
	COLORREF boxBg = m_ditheringChecked ? DarkTheme::CHECK_MARK : DarkTheme::CHECK_BG;
	HBRUSH hBoxBrush = CreateSolidBrush(boxBg);
	FillRect(hDC, &rcBox, hBoxBrush);
	DeleteObject(hBoxBrush);

	// Draw box border
	COLORREF borderColor = m_ditheringChecked ? DarkTheme::CHECK_MARK : DarkTheme::BORDER;
	HPEN hPen = CreatePen(PS_SOLID, 1, borderColor);
	HPEN hOldPen = (HPEN)SelectObject(hDC, hPen);
	HBRUSH hOldBrush = (HBRUSH)SelectObject(hDC, GetStockObject(NULL_BRUSH));
	RoundRect(hDC, rcBox.left, rcBox.top, rcBox.right, rcBox.bottom, 3, 3);
	SelectObject(hDC, hOldPen);
	SelectObject(hDC, hOldBrush);
	DeleteObject(hPen);

	// Draw checkmark if checked
	if (m_ditheringChecked)
	{
		HPEN hCheckPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
		HPEN hOldCheckPen = (HPEN)SelectObject(hDC, hCheckPen);

		// Checkmark shape (L shape rotated)
		int x = rcBox.left + 3;
		int y = rcBox.top + boxSize / 2;
		MoveToEx(hDC, x + 1, y, NULL);
		LineTo(hDC, x + 4, y + 3);
		LineTo(hDC, x + 10, y - 4);

		SelectObject(hDC, hOldCheckPen);
		DeleteObject(hCheckPen);
	}

	// Draw label text
	HFONT hOldFont = nullptr;
	if (m_hUIFont)
		hOldFont = (HFONT)SelectObject(hDC, m_hUIFont);

	SetTextColor(hDC, (pDIS->itemState & ODS_DISABLED)
		? DarkTheme::TEXT_SECONDARY : DarkTheme::TEXT_PRIMARY);
	SetBkMode(hDC, TRANSPARENT);

	RECT rcLabel = { rcBox.right + 6, rc.top, rc.right, rc.bottom };
	char szText[256];
	GetWindowTextA(pDIS->hwndItem, szText, 256);
	DrawTextA(hDC, szText, -1, &rcLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	if (hOldFont)
		SelectObject(hDC, hOldFont);
}

//-------------------------------------------------------------------------------
// DrawCustomComboItem - Owner-drawn combo box items with dark theme
//-------------------------------------------------------------------------------
void CustomSaveDialog::DrawCustomComboItem(LPDRAWITEMSTRUCT pDIS)
{
	if (pDIS->itemID == (UINT)-1)
		return; // Nothing to draw

	HDC hDC = pDIS->hDC;
	RECT rc = pDIS->rcItem;

	bool isSelected = (pDIS->itemState & ODS_SELECTED);
	bool isFocused = (pDIS->itemState & ODS_FOCUS);

	// Background color
	COLORREF bgColor;
	if (isSelected)
		bgColor = DarkTheme::ACCENT;
	else
		bgColor = DarkTheme::EDIT_BG;

	HBRUSH hBrush = CreateSolidBrush(bgColor);
	FillRect(hDC, &rc, hBrush);
	DeleteObject(hBrush);

	// Get item text
	char szText[256] = {};
	SendMessage(pDIS->hwndItem, CB_GETLBTEXT, pDIS->itemID, (LPARAM)szText);

	// Draw text
	HFONT hOldFont = nullptr;
	if (m_hUIFont)
		hOldFont = (HFONT)SelectObject(hDC, m_hUIFont);

	SetTextColor(hDC, DarkTheme::TEXT_PRIMARY);
	SetBkMode(hDC, TRANSPARENT);

	RECT rcText = rc;
	rcText.left += 6;
	DrawTextA(hDC, szText, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	if (hOldFont)
		SelectObject(hDC, hOldFont);

	// Focus rectangle
	if (isFocused)
		DrawFocusRect(hDC, &rc);
}

//-------------------------------------------------------------------------------
// MeasureComboItem - Set combo box item height
//-------------------------------------------------------------------------------
void CustomSaveDialog::MeasureComboItem(LPMEASUREITEMSTRUCT pMIS)
{
	pMIS->itemHeight = 24; // Comfortable item height
}

// ==============================================================================
// LoadPresetNonUIMode - Load preset without showing dialog (batch mode)
// ==============================================================================
bool CustomSaveDialog::LoadPresetNonUIMode(string nameOfPreset)
{
	// If not presets directory return with error
	if (mPathToPresetDirectory.empty())
		return false;

	// Initialize with defaults
	InitDataNoPreset(mDialogData);

	// This loads all the settings and the last-used setting
	LoadPresets();

	InitDataFromPreset(nameOfPreset);

	InitComboItems();

	return true;
}

// ==============================================================================
// FillGlobalStruct - Fill global plugin struct with UI data
// ==============================================================================
void CustomSaveDialog::FillGlobalStruct()
{
	// Map CompressionTypeIndex directly to encoding.
	// ExtractDataFromUI sets: 0 = BGRA (Uncompressed), 1 = BC3 (DXT5).
	// We do NOT index into gComboItems here because that vector is dynamically
	// filtered by texture type and its size may not match CompressionTypeIndex.
	switch (mDialogData.CompressionTypeIndex)
	{
		case 0: // BGRA / Uncompressed
			globalParams->encoding_g = DXGI_FORMAT_B8G8R8A8_UNORM;
			break;
		case 1: // BC3 / DXT5
			globalParams->encoding_g = DXGI_FORMAT_BC3_UNORM;
			break;
		default:
			globalParams->encoding_g = DXGI_FORMAT_BC3_UNORM;
			break;
	}

	globalParams->TextureTypeIndex = mDialogData.TextureTypeIndex; //Col,Col+alpha,CubeFrmLayera,CubefromCross,NM
	globalParams->MipMapTypeIndex = mDialogData.MipMapTypeIndex;  //None,Autogen,FromLayers
	globalParams->MipLevel = mDialogData.MipLevel;			   // only valid if SetMipLevel == true
	globalParams->SetMipLevel = mDialogData.SetMipLevel;
	globalParams->Normalize = mDialogData.Normalize;
	globalParams->FlipX = mDialogData.FlipX;
	globalParams->FlipY = mDialogData.FlipY;
	globalParams->useDithering = mDialogData.UseDithering;
	globalParams->useUniformMetric = mDialogData.UseUniformMetric;

	//presetBatchName is a PString (first byte is the size), convert C to PString
	CToPStr(mDialogData.PresetName.c_str(), reinterpret_cast<char*>(globalParams->presetBatchName));
}

// ==============================================================================
// GetGlobalStruct - Fill UI data struct with global plugin struct
// ==============================================================================
void CustomSaveDialog::GetGlobalStruct()
{
	int CompressionTypeIndex = 0;
	CompressionTypeEnum compressionID;

	// Find compression ID
	switch (globalParams->encoding_g)
	{
		case DXGI_FORMAT_BC1_UNORM:
			compressionID = CompressionTypeEnum::BC1;
			break;
		case DXGI_FORMAT_BC3_UNORM:
			compressionID = CompressionTypeEnum::BC3;
			break;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			compressionID = CompressionTypeEnum::UNCOMPRESSED;
			break;
		default:
			compressionID = CompressionTypeEnum::BC1;
			break;
	}

	// Iteration over all available compression for this texture type and find the compression combobox index
	// Compression types which are not available are not show in the combo box, therefore the index is not incremented if matrix is false
	for (int i = 0; i < CompressionTypeEnum::COMPRESSION_TYPE_COUNT; i++)
	{
		// If this compression mode is available for this texture type
		if (IntelPlugin::IsCombinationValid(globalParams->TextureTypeIndex, static_cast<CompressionTypeEnum>(i)))
		{
			// If its the selected encoding and this encoding is available then break out
			if (i == compressionID)
				break;

			CompressionTypeIndex++;
		}
	}

	mDialogData.CompressionTypeIndex = CompressionTypeIndex;
	mDialogData.TextureTypeIndex = globalParams->TextureTypeIndex;  //Col,Col+alpha,CubeFrmLayera,CubefromCross,NM
	mDialogData.MipMapTypeIndex = globalParams->MipMapTypeIndex;   //None,Autogen,FromLayers
	mDialogData.MipLevel = globalParams->MipLevel;			// only valid if SetMipLevel == true
	mDialogData.SetMipLevel = globalParams->SetMipLevel;
	mDialogData.Normalize = globalParams->Normalize;
	mDialogData.FlipX = globalParams->FlipX;
	mDialogData.FlipY = globalParams->FlipY;
	mDialogData.UseDithering = globalParams->useDithering;
	mDialogData.UseUniformMetric = globalParams->useUniformMetric;
}

// ==============================================================================
// InitComboItems - Fills in Combo items structures for Presets, Compression, Textype, and MipMap generation
// ==============================================================================
void CustomSaveDialog::InitComboItems()
{
	gComboItems.reserve(NUMBEROF_COMBOS);

	gComboItems.push_back(ComboData(IDC_PRESET_COMBO));
	GetPresetNames(gComboItems[PRESETS_COMBO]);

	gComboItems.push_back(ComboData(IDC_COMPRESSION_COMBO));
	GetCompressionNames(gComboItems[COMPRESSION_COMBO]);

	gComboItems.push_back(ComboData(IDC_TEXTURETYPE_COMBO));
	GetTextureTypeNames(gComboItems[TEXTURETYPE_COMBO]);

	gComboItems.push_back(ComboData(IDC_MIPMAP_COMBO));
	GetMipMapNames(gComboItems[MIPMAP_COMBO]);
}

// ==============================================================================
// LoadPresets - Scan preset directory and read .preset files
// ==============================================================================
void CustomSaveDialog::LoadPresets(void)
{
	mPresets.clear();

	// List all preset files
	string fullPath = mPathToPresetDirectory + "*.preset";

	// Find the first file in the directory
	WIN32_FIND_DATA ffd;
	HANDLE hFind = FindFirstFile(fullPath.c_str(), &ffd);

	if (INVALID_HANDLE_VALUE == hFind)
	{
		return;
	}

	// Walk the list of files in the dir that match our pattern
	list<string> fileNames;

	do
	{
		if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			string fname(ffd.cFileName);
			fileNames.push_back(fname);
		}

	} while (FindNextFile(hFind, &ffd) != 0);

	DWORD dwError = GetLastError();
	if (dwError != ERROR_NO_MORE_FILES)
	{
		return;
	}

	FindClose(hFind);

	// Now, read the rest of the files and load up the presets menu with them
	for (auto it = fileNames.begin(); it != fileNames.end(); ++it)
	{
		string presetPath = mPathToPresetDirectory + *it;
		ReadPreset(presetPath);
	}
}

// ==============================================================================
// ReadPreset - Fills in the DialogData structs which are stored in the mPresets array
// ==============================================================================
void CustomSaveDialog::ReadPreset(string fname)
{
	ifstream fileIn;
	fileIn.open(fname);

	if (fileIn.is_open())
	{
		string line;
		uint32 lineNum = 0;

		DialogData dd;

		size_t foundBSlash = fname.rfind("\\");
		string justName = fname;

		if (foundBSlash != string::npos)
		{
			justName = fname.substr(foundBSlash + 1);
		}
		else
		{
			size_t foundFSlash = fname.rfind("/");	// handle those other OSes
			if (foundFSlash != string::npos)
			{
				justName = fname.substr(foundFSlash + 1);
			}
		}

		size_t foundDot = justName.rfind(".");

		if (foundDot != string::npos)
		{
			dd.PresetName = justName.substr(0, foundDot);
		}
		else
		{
			dd.PresetName = justName;
		}

		try
		{
			while (getline(fileIn, line))
			{
				bool done = false;
				switch (lineNum - 1)
				{
					case -1: if (atoi(line.c_str()) != PRESET_FILE_VERSION)		// version check
						throw std::exception();

					case 0: dd.CompressionTypeIndex = atoi(line.c_str());    break;
					case 1: dd.TextureTypeIndex = static_cast<TextureTypeEnum>(atoi(line.c_str()));        break;
					case 2: dd.MipMapTypeIndex = static_cast<MipmapEnum>(atoi(line.c_str()));         break;
					case 3: dd.MipLevel = atoi(line.c_str());                break;
					case 4: dd.SetMipLevel = atoi(line.c_str()) == 1;        break;
					case 5: dd.Normalize = atoi(line.c_str()) == 1;          break;
					case 6: dd.FlipX = atoi(line.c_str()) == 1;              break;

					case 7: dd.FlipY = atoi(line.c_str()) == 1;
						break;

					case 8: dd.UseDithering = atoi(line.c_str()) == 1;    break;

					case 9: dd.UseUniformMetric = atoi(line.c_str()) == 1;
						done = true;
						break;

					default:
						done = true;
				}

				++lineNum;

				if (done)
				{
					break;
				}
			}

			if (dd.PresetName.compare(LAST_SETTINGS_PRESET_NAME) == 0)
			{
				mDialogData = dd;
			}
			else
			{
				mPresets[dd.PresetName] = dd;
			}
		}
		catch (std::exception&)
		{
			// bad settings read
		}

		fileIn.close();
	}
}

// ==============================================================================
// SaveNewPreset - Writes new preset to file and updates UI
// ==============================================================================
void CustomSaveDialog::SaveNewPreset(string presetName, DialogData dd)
{
	bool fileWriteSucceeded = true;
	dd.PresetName = presetName;

	// path to new preset file
	string fullPath = mPathToPresetDirectory + presetName + ".preset";

	ofstream fileOut;
	fileOut.open(fullPath);

	if (fileOut.is_open())
	{
		fileOut << PRESET_FILE_VERSION << "\n";
		fileOut << dd.CompressionTypeIndex << "\n";
		fileOut << dd.TextureTypeIndex << "\n";
		fileOut << dd.MipMapTypeIndex << "\n";
		fileOut << dd.MipLevel << "\n";
		fileOut << (dd.SetMipLevel ? "1" : "0") << "\n";
		fileOut << (dd.Normalize ? "1" : "0") << "\n";
		fileOut << (dd.FlipX ? "1" : "0") << "\n";
		fileOut << (dd.FlipY ? "1" : "0") << "\n";
		fileOut << (dd.UseDithering ? "1" : "0") << "\n";
		fileOut << (dd.UseUniformMetric ? "1" : "0") << "\n";

		fileOut.close();
	}
	else
	{
		fileWriteSucceeded = false;
		errorMessage("Can not save " + fullPath, "Preset save error");
	}

	if (fileWriteSucceeded)
	{
		if ((presetName.compare(LAST_SETTINGS_PRESET_NAME) == 0) && (!mPresets.empty()))
		{
			// TODO
		}
		else
		{
			mPresets[presetName] = dd;
		}

		// Update the main working data
		InitDataFromPreset(presetName);

		mDialogData.PresetName = presetName;

		// rebuild combo items data list for presets
		ComboData& cd = gComboItems[PRESETS_COMBO];
		cd.itemAndContextStrings.clear();
		GetPresetNames(cd);

		// rebuild combobox for presets -- making sure it has the right selected item (newly created preset)
		InitComboFromItems(PRESETS_COMBO);

		// Update UI to reflect new Preset
		SetUIFromData();
	}
}

// ==============================================================================
// UpdatePreset - Updates existing preset file
// ==============================================================================
void CustomSaveDialog::UpdatePreset(string presetName, DialogData dd)
{
	dd.PresetName = presetName;

	// path to existing preset file
	string fullPath = mPathToPresetDirectory + presetName + ".preset";

	ofstream fileOut;
	fileOut.open(fullPath);

	// Save change into preset file
	if (fileOut.is_open())
	{
		fileOut << PRESET_FILE_VERSION << "\n";
		fileOut << dd.CompressionTypeIndex << "\n";
		fileOut << dd.TextureTypeIndex << "\n";
		fileOut << dd.MipMapTypeIndex << "\n";
		fileOut << dd.MipLevel << "\n";
		fileOut << (dd.SetMipLevel ? "1" : "0") << "\n";
		fileOut << (dd.Normalize ? "1" : "0") << "\n";
		fileOut << (dd.FlipX ? "1" : "0") << "\n";
		fileOut << (dd.FlipY ? "1" : "0") << "\n";
		fileOut << (dd.UseDithering ? "1" : "0") << "\n";
		fileOut << (dd.UseUniformMetric ? "1" : "0") << "\n";

		fileOut.close();
	}
	else
	{
		errorMessage("Can not save " + fullPath, "Preset save error");
	}

	// Save change also into mPresets array
	mPresets[presetName] = dd;
}

// ==============================================================================
// DeletePreset - Remove a preset from the list and delete the .preset file
// ==============================================================================
void CustomSaveDialog::DeletePreset(string presetName)
{
	if (mPresets.erase(presetName))
	{
		// rebuild combo items data list for presets
		ComboData& cd = gComboItems[PRESETS_COMBO];
		cd.itemAndContextStrings.clear();
		GetPresetNames(cd);

		// rebuild combobox for presets -- making sure it has the right selected item
		InitComboFromItems(PRESETS_COMBO);

		// Update UI to reflect change
		SetUIFromData();

		// path to presets file
		string fullPath = mPathToPresetDirectory + presetName + ".preset";

		BOOL fileDeleted = DeleteFile(fullPath.c_str());

		if (!fileDeleted)
		{
			errorMessage("Can not delete " + fullPath, "Preset delete error");
		}
	}
}

// ==============================================================================
// InitDataNoPreset - Initialize a DialogData structure with default settings
// ==============================================================================
void CustomSaveDialog::InitDataNoPreset(DialogData& dd)
{
	dd.PresetName = LAST_SETTINGS_PRESET_NAME;
	dd.CompressionTypeIndex = 0;
	dd.TextureTypeIndex = TextureTypeEnum::COLOR;
	dd.MipMapTypeIndex = MipmapEnum::NONE;

	dd.SetMipLevel = false;

	dd.MipLevel = 0;

	dd.Normalize = false;
	dd.FlipX = false;
	dd.FlipY = false;
	dd.UseDithering = true;
	dd.UseUniformMetric = false;
}

// ==============================================================================
// InitDataFromPreset - Populate mDialogData with settings from specified Preset
// ==============================================================================
void CustomSaveDialog::InitDataFromPreset(string presetName)
{
	auto item = mPresets.find(presetName);
	if (item != mPresets.end())
		mDialogData = item->second;
}

// ==============================================================================
// InitComboFromItems - Fill combo controls with strings stored in ComboData structs
// ==============================================================================
void CustomSaveDialog::InitComboFromItems(int32 comboItemsIndex)
{
	ComboData& cd = gComboItems[comboItemsIndex];

	// Clear combo box
	SendDlgItemMessage(hDlg, cd.itemNum, CB_RESETCONTENT, 0, 0);

	if (!cd.itemAndContextStrings.empty())
	{
		// Add the description strings to the Combo box
		for (size_t i = 0; i < cd.itemAndContextStrings.size(); ++i)
		{
			SendDlgItemMessage(hDlg, cd.itemNum, CB_ADDSTRING, 0, (LPARAM)cd.itemAndContextStrings[i].itemText.c_str());
		}

		// Set the selected item
		uint32 selectedIndex = cd.startIndex;
		SendDlgItemMessage(hDlg, cd.itemNum, CB_SETCURSEL, selectedIndex, 0);

		// Iterate over only the 3 combo boxes and set the text of the according Context Control
		// Compression, Textype, and MipMap
		for (uint32 j = 0; j < sizeof(gComboContextItems) / sizeof(ComboAndContextStringID); ++j)
		{
			if (cd.itemNum == gComboContextItems[j].itemNum)
			{
				SetDlgItemTextA(hDlg, gComboContextItems[j].contextItemNum, cd.itemAndContextStrings[selectedIndex].itemContextInfo.c_str());
				break;
			}
		}
	}
}

// ==============================================================================
// SetFontCompressionCombo - Override combo box font for alignment
// ==============================================================================
void CustomSaveDialog::SetFontCompressionCombo()
{
	HWND comboCompression = GetDlgItem(hDlg, IDC_COMPRESSION_COMBO);
	HWND comboTexType = GetDlgItem(hDlg, IDC_TEXTURETYPE_COMBO);
	HWND comboMipMap = GetDlgItem(hDlg, IDC_MIPMAP_COMBO);

	LOGFONT lf = {}; // to define the font
	lf.lfHeight = 14;
	lf.lfWeight = FW_NORMAL;
	lf.lfCharSet = ANSI_CHARSET;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = PROOF_QUALITY;
	lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
	strcpy(lf.lfFaceName, "Consolas");

	if (auto hFont = ::CreateFontIndirect(&lf))
	{
		SendMessage(comboCompression, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
		SendMessage(comboTexType, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
		SendMessage(comboMipMap, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
	}
}

// ==============================================================================
// Init - Main initialization of the UI (SIMPLIFIED for new UI)
// ==============================================================================
void CustomSaveDialog::Init(void)
{
	// Initialize theme resources
	InitThemeResources();

	// Disable visual styles on all themed controls so our dark theme
	// text colors (via WM_CTLCOLORSTATIC) are actually respected.
	// Without this, Windows draws radio/checkbox/groupbox text in theme
	// colors (black) which is invisible on our dark background.
	static const int themedControls[] = {
		IDC_COMPRESSION_BC3,   // Radio button
		IDC_COMPRESSION_BGRA,  // Radio button
		IDC_DITHERING_CHECK,   // Checkbox
		IDC_COMPRESSION_HINT,  // Static text
		IDC_ERRORMETRIC_LABEL, // Static text
		IDC_ERRORMETRIC_COMBO, // Combo box
		IDC_MIPMAP_COMBO,      // Combo box
	};
	for (int id : themedControls)
	{
		HWND hwndCtrl = GetDlgItem(hDlg, id);
		if (hwndCtrl)
			SetWindowTheme(hwndCtrl, L"", L"");
	}

	// Also disable visual styles on groupboxes (they are anonymous static controls with id -1)
	// Enumerate child windows to find groupboxes and static text controls
	EnumChildWindows(hDlg, [](HWND hwndChild, LPARAM lParam) -> BOOL {
		char className[64];
		GetClassNameA(hwndChild, className, sizeof(className));
		if (_stricmp(className, "Button") == 0)
		{
			DWORD style = GetWindowLong(hwndChild, GWL_STYLE);
			DWORD btnType = style & BS_TYPEMASK;
			// Groupboxes, radio buttons, checkboxes
			if (btnType == BS_GROUPBOX || btnType == BS_RADIOBUTTON ||
			    btnType == BS_AUTORADIOBUTTON || btnType == BS_CHECKBOX ||
			    btnType == BS_AUTOCHECKBOX || btnType == BS_3STATE ||
			    btnType == BS_AUTO3STATE)
			{
				SetWindowTheme(hwndChild, L"", L"");
			}
		}
		else if (_stricmp(className, "Static") == 0)
		{
			SetWindowTheme(hwndChild, L"", L"");
		}
		else if (_stricmp(className, "ComboBox") == 0)
		{
			SetWindowTheme(hwndChild, L"", L"");
		}
		return TRUE;
	}, 0);

	// Initialize defaults
	InitDataNoPreset(mDialogData);

	// Force Color+Alpha as texture type (index 1 = Color+Alpha)
	mDialogData.TextureTypeIndex = COLOR_ALPHA;

	// Force BC3 as default compression (index 1 = BC3 in our simplified list)
	// BC3 = DXGI_FORMAT_BC3_UNORM, BGRA = DXGI_FORMAT_B8G8R8A8_UNORM
	mDialogData.CompressionTypeIndex = 1; // BC3

	// Set compression radio buttons - BC3 selected by default
	SendDlgItemMessage(hDlg, IDC_COMPRESSION_BC3, BM_SETCHECK, BST_CHECKED, 0);
	SendDlgItemMessage(hDlg, IDC_COMPRESSION_BGRA, BM_SETCHECK, BST_UNCHECKED, 0);

	// Update compression hint text
	SetDlgItemTextA(hDlg, IDC_COMPRESSION_HINT,
		"BC3: 8 bits/pixel with full alpha. Best for most textures.");

	// Initialize mipmap combo
	SendDlgItemMessage(hDlg, IDC_MIPMAP_COMBO, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessage(hDlg, IDC_MIPMAP_COMBO, CB_ADDSTRING, 0, (LPARAM)"None");
	SendDlgItemMessage(hDlg, IDC_MIPMAP_COMBO, CB_ADDSTRING, 0, (LPARAM)"Auto Generate");
	SendDlgItemMessage(hDlg, IDC_MIPMAP_COMBO, CB_SETCURSEL, mDialogData.MipMapTypeIndex, 0);

	// Set dithering checkbox (enabled by default for better quality)
	mDialogData.UseDithering = true;
	m_ditheringChecked = true;
	InvalidateRect(GetDlgItem(hDlg, IDC_DITHERING_CHECK), NULL, FALSE);

	// Initialize error metric combo (Perceptual is default - better quality)
	SendDlgItemMessage(hDlg, IDC_ERRORMETRIC_COMBO, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessage(hDlg, IDC_ERRORMETRIC_COMBO, CB_ADDSTRING, 0, (LPARAM)"Perceptual (Better Quality)");
	SendDlgItemMessage(hDlg, IDC_ERRORMETRIC_COMBO, CB_ADDSTRING, 0, (LPARAM)"Uniform");
	SendDlgItemMessage(hDlg, IDC_ERRORMETRIC_COMBO, CB_SETCURSEL, 0, 0);
	mDialogData.UseUniformMetric = false;

	// Enable/disable quality options based on compression format
	UpdateQualityControlsState();
}

// ==============================================================================
// UpdateQualityControlsState - Enable/disable quality controls based on compression
// ==============================================================================
void CustomSaveDialog::UpdateQualityControlsState()
{
	// Check if BC3 is selected
	bool isBC3 = (SendDlgItemMessage(hDlg, IDC_COMPRESSION_BC3, BM_GETCHECK, 0, 0) == BST_CHECKED);

	// Enable/disable quality controls
	EnableWindow(GetDlgItem(hDlg, IDC_DITHERING_CHECK), isBC3);
	EnableWindow(GetDlgItem(hDlg, IDC_ERRORMETRIC_LABEL), isBC3);
	EnableWindow(GetDlgItem(hDlg, IDC_ERRORMETRIC_COMBO), isBC3);

	// Update hint text based on selection
	if (isBC3)
	{
		SetDlgItemTextA(hDlg, IDC_COMPRESSION_HINT,
			"BC3: 8 bits/pixel with full alpha. Best for most textures.");
	}
	else
	{
		SetDlgItemTextA(hDlg, IDC_COMPRESSION_HINT,
			"BGRA: Lossless, no compression. Larger file size but highest quality.");
	}
}

// ==============================================================================
// SetUIFromData - Update the UI state based on current mDialogData
// ==============================================================================
void CustomSaveDialog::SetUIFromData()
{
	// Change entries in compression/mipmap combo based on texture type
	UpdateCompressionCombo();
	UpdateMipMapCombo();

	// Initialize compression combo from mDialogData.CompressionTypeIndex
	SendDlgItemMessage(hDlg, IDC_COMPRESSION_COMBO, CB_SETCURSEL, mDialogData.CompressionTypeIndex, 0);
	// Update context string based on selected entry
	SetContextString(IDC_COMPRESSION_HINT, IDC_COMPRESSION_COMBO);

	// Initialize texture type combo from mDialogData.TextureTypeIndex
	SendDlgItemMessage(hDlg, IDC_TEXTURETYPE_COMBO, CB_SETCURSEL, mDialogData.TextureTypeIndex, 0);
	// Update context string based on selected entry
	SetContextString(IDC_TEXTURETYPE_HINT, IDC_TEXTURETYPE_COMBO);

	// Initialize mip map creation combo from mDialogData.MipMapTypeIndex
	SendDlgItemMessage(hDlg, IDC_MIPMAP_COMBO, CB_SETCURSEL, mDialogData.MipMapTypeIndex, 0);
	// Update context string based on selected entry
	SetContextString(IDC_MIPMAPS_HINT, IDC_MIPMAP_COMBO);

	// Handle mip level checkbox
	bool wasMipLevelChecked = (SendDlgItemMessage(hDlg, IDC_CUBEMIPLEVEL_CHECK, BM_GETCHECK, 0, 0) == BST_CHECKED);
	SendDlgItemMessage(hDlg, IDC_CUBEMIPLEVEL_CHECK, BM_SETCHECK, mDialogData.SetMipLevel ? BST_CHECKED : BST_UNCHECKED, 0);

	if (wasMipLevelChecked && !mDialogData.SetMipLevel)
	{
		SendDlgItemMessage(hDlg, IDC_MIPLEVEL_COMBO, CB_RESETCONTENT, 0, 0);
	}
	else if (!wasMipLevelChecked && mDialogData.SetMipLevel)
	{
		PopulateMipLevelsCombo();
	}
	else if (wasMipLevelChecked && mDialogData.SetMipLevel)
	{
		SendDlgItemMessage(hDlg, IDC_MIPLEVEL_COMBO, CB_SETCURSEL, mDialogData.MipLevel, 0);
	}

	// Set normalize checkbox
	SendDlgItemMessage(hDlg, IDC_NORMALIZE_CHECK, BM_SETCHECK, mDialogData.Normalize ? BST_CHECKED : BST_UNCHECKED, 0);

	// Set flip X checkbox
	SendDlgItemMessage(hDlg, IDC_FLIPX_CHECK, BM_SETCHECK, mDialogData.FlipX ? BST_CHECKED : BST_UNCHECKED, 0);

	// Set flip Y checkbox
	SendDlgItemMessage(hDlg, IDC_FLIPY_CHECK, BM_SETCHECK, mDialogData.FlipY ? BST_CHECKED : BST_UNCHECKED, 0);

	// Set dithering checkbox (owner-drawn, use member variable)
	m_ditheringChecked = mDialogData.UseDithering;
	InvalidateRect(GetDlgItem(hDlg, IDC_DITHERING_CHECK), NULL, FALSE);

	// Set error metric combo
	SendDlgItemMessage(hDlg, IDC_ERRORMETRIC_COMBO, CB_SETCURSEL, mDialogData.UseUniformMetric ? 1 : 0, 0);

	// Disable any controls they do not apply based on the chosen texture type
	DisableUnavailableControls();
}

// ==============================================================================
// HandleCommand - Command handling (WM_COMMAND messages)
// ==============================================================================
void CustomSaveDialog::HandleCommand(WPARAM wParam, LPARAM lParam)
{
	UINT controlID = LOWORD(wParam);
	UINT notifyCode = HIWORD(wParam);

	// Shows MessageBox for help [?] buttons
	for (uint32 i = 0; i < sizeof(helpButtonTextItem) / sizeof(HelpButtonAndTextFunc); ++i)
	{
		if (controlID == helpButtonTextItem[i].itemNum)
		{
			vector<string> captionAndText = helpButtonTextItem[i].func();
			MessageBox(GetActiveWindow(), captionAndText[0].c_str(), captionAndText[1].c_str(), 0);
			return;
		}
	}

	// One of the Combo boxes was changed, update context string
	if (notifyCode == CBN_SELCHANGE)
	{
		for (uint32 i = 0; i < sizeof(gComboContextItems) / sizeof(ComboAndContextStringID); ++i)
		{
			if (controlID == gComboContextItems[i].itemNum)
			{
				// Update context string based on selected entry
				SetContextString(gComboContextItems[i].contextItemNum, controlID);
				break;		// no return - want this to fall through to the end block that backs up the change
			}
		}
	}

	// Handle specific controls
	switch (controlID)
	{
		case IDC_COMPRESSION_BC3:
		case IDC_COMPRESSION_BGRA:
			// Radio button changed - update UI state
			UpdateQualityControlsState();
			ExtractDataFromUI(mDialogData);
			break;

		case IDC_DITHERING_CHECK:
			// Dithering checkbox changed
			ExtractDataFromUI(mDialogData);
			break;

		case IDC_ERRORMETRIC_COMBO:
			if (notifyCode == CBN_SELCHANGE)
				ExtractDataFromUI(mDialogData);
			break;

		case IDC_MIPMAP_COMBO:
			if (notifyCode == CBN_SELCHANGE)
				ExtractDataFromUI(mDialogData);
			break;

		case IDC_CUBEMIPLEVEL_CHECK:
			OnCubeMipLevelCheck();
			break;

		case IDC_PRESET_COMBO:
			if (notifyCode == CBN_SELCHANGE)
				OnPresetComboChange();
			break;

		case IDC_PRESETDELETE_BUTTON:
			OnPresetDelete();
			return;

		case IDC_PRESETSAVE_BUTTON:
			OnPresetSave();
			return;

		case IDC_PREVIEW_BUTTON:
			OnPreview();
			return;

		case IDOK:
			OnOK();
			EndDialog(hDlg, IDOK);
			return;

		case IDCANCEL:
			EndDialog(hDlg, IDCANCEL);
			return;

		default:
			break;
	}

	// Some other changes happened to the UI which did not have custom actions just update the struct
	auto old = mDialogData;
	ExtractDataFromUI(mDialogData);

	// Rebuild CompressionTypes Combo box and disable not applicable controls
	// If Texture or MipMap type changed (apply last to have mDialog updated)
	if (old.TextureTypeIndex != mDialogData.TextureTypeIndex || old.MipMapTypeIndex != mDialogData.MipMapTypeIndex)
	{
		// Update compression/mipmap combo box
		UpdateCompressionCombo();
		UpdateMipMapCombo();

		// Disable any controls they do not apply based on the chosen texture type
		DisableUnavailableControls();
	}
}

// ==============================================================================
// OnOK - Handle OK button pressed
// ==============================================================================
void CustomSaveDialog::OnOK()
{
	// Get preset name from combo
	int presetIndex = SendDlgItemMessage(hDlg, IDC_PRESET_COMBO, CB_GETCURSEL, 0, 0);
	if (presetIndex != CB_ERR)
	{
		char buffer[256];
		SendDlgItemMessage(hDlg, IDC_PRESET_COMBO, CB_GETLBTEXT, presetIndex, (LPARAM)buffer);
		mDialogData.PresetName = buffer;
	}

	ExtractDataFromUI(mDialogData);

	// We have the none preset set so save only to the none preset so that it remember the next time it loads
	if (mDialogData.PresetName.compare(LAST_SETTINGS_PRESET_NAME) == 0)
	{
		UpdatePreset(LAST_SETTINGS_PRESET_NAME, mDialogData);
	}
	else
	{
		// Update currently set preset and none preset so that it remember the next time it loads

#ifdef AUTO_SAVE_PRESETS
		UpdatePreset(mDialogData.PresetName, mDialogData);
#endif

		UpdatePreset(LAST_SETTINGS_PRESET_NAME, mDialogData);
	}

	// CRITICAL FIX: Sync dialog data to globals before compression
	// Without this, dithering and other settings don't reach CompressToScratchImage()
	FillGlobalStruct();
}

// ==============================================================================
// OnPreview - Handle Preview button pressed
// ==============================================================================
void CustomSaveDialog::OnPreview()
{
	// Copy over UI to global struct for preview routine
	FillGlobalStruct();

	// Show previewUI (modal does not return until OK is pressed)
	PreviewDialog dlg(plugin);
	dlg.Modal();

	// Copy any changes back into UI struct (preview can change compression)
	GetGlobalStruct();

	// Update UI from struct
	SetUIFromData();
}

// ==============================================================================
// OnPresetSave - Handle Preset Save button pressed
// ==============================================================================
void CustomSaveDialog::OnPresetSave()
{
	// Get current preset name from combo
	int presetIndex = SendDlgItemMessage(hDlg, IDC_PRESET_COMBO, CB_GETCURSEL, 0, 0);
	string newPresetName;
	if (presetIndex != CB_ERR)
	{
		char buffer[256];
		SendDlgItemMessage(hDlg, IDC_PRESET_COMBO, CB_GETLBTEXT, presetIndex, (LPARAM)buffer);
		newPresetName = buffer;
	}

	newPresetName = GetPresetName(newPresetName, GetActiveWindow());

	if (!newPresetName.empty())
	{
		bool doSave = mPresets.find(newPresetName) == mPresets.end();
		if (!doSave)	// already exists?
		{
			doSave = IDOK == MessageBox(GetActiveWindow(), "That Preset name exists - save over it?", "Confirm Save", MB_OKCANCEL);
		}

		if (doSave)
		{
			DialogData dd;
			ExtractDataFromUI(dd);

			SaveNewPreset(newPresetName, dd);
		}
	}
}

// ==============================================================================
// OnPresetDelete - Handle Preset Delete button pressed
// ==============================================================================
void CustomSaveDialog::OnPresetDelete()
{
	// Get selected preset from combo
	int presetIndex = SendDlgItemMessage(hDlg, IDC_PRESET_COMBO, CB_GETCURSEL, 0, 0);
	string selectedPreset;
	if (presetIndex != CB_ERR)
	{
		char buffer[256];
		SendDlgItemMessage(hDlg, IDC_PRESET_COMBO, CB_GETLBTEXT, presetIndex, (LPARAM)buffer);
		selectedPreset = buffer;
	}

	if (selectedPreset.compare(LAST_SETTINGS_PRESET_NAME) == 0)
	{
		MessageBox(GetActiveWindow(), "You must select a preset from the dropdown to be deleted", "Delete Preset", MB_OK);
		return;
	}

	stringstream ss;
	ss << "Are you sure you want to delete the preset: " << selectedPreset << "?";

	int delResult = MessageBox(GetActiveWindow(), ss.str().c_str(), "Delete Preset", MB_OKCANCEL);

	if (delResult == IDOK)
	{
		DeletePreset(selectedPreset);
	}
}

// ==============================================================================
// OnCubeMipLevelCheck - Handle cube map mip level checkbox change
// ==============================================================================
void CustomSaveDialog::OnCubeMipLevelCheck()
{
	bool checked = (SendDlgItemMessage(hDlg, IDC_CUBEMIPLEVEL_CHECK, BM_GETCHECK, 0, 0) == BST_CHECKED);

	if (checked)
	{
		PopulateMipLevelsCombo();
	}
	else
	{
		SendDlgItemMessage(hDlg, IDC_MIPLEVEL_COMBO, CB_RESETCONTENT, 0, 0);
	}
}

// ==============================================================================
// OnPresetComboChange - Handle preset combo box selection change
// ==============================================================================
void CustomSaveDialog::OnPresetComboChange()
{
	// Get the string entry of the selected item in the combo box
	int presetIndex = SendDlgItemMessage(hDlg, IDC_PRESET_COMBO, CB_GETCURSEL, 0, 0);
	string selectedItem;
	if (presetIndex != CB_ERR)
	{
		char buffer[256];
		SendDlgItemMessage(hDlg, IDC_PRESET_COMBO, CB_GETLBTEXT, presetIndex, (LPARAM)buffer);
		selectedItem = buffer;
	}

	// Did we select a different preset
	if (selectedItem.compare(mDialogData.PresetName) != 0)
	{
		// Store changes in current preset before swapping

#ifdef AUTO_SAVE_PRESETS
		UpdatePreset(mDialogData.PresetName, mDialogData);
#endif

		// Load new data in struct
		InitDataFromPreset(selectedItem);

		// Update UI from struct
		SetUIFromData();

		// If we want a callback or notify that a preset has changed, here is where to place that call
	}
}

// ==============================================================================
// ExtractDataFromUI - Interrogate UI elements and copy to DialogData struct (SIMPLIFIED)
// ==============================================================================
void CustomSaveDialog::ExtractDataFromUI(DialogData& dd)
{
	// Read compression format from radio buttons
	bool isBGRA = (SendDlgItemMessage(hDlg, IDC_COMPRESSION_BGRA, BM_GETCHECK, 0, 0) == BST_CHECKED);

	// Map to compression index:
	// 0 = Uncompressed (BGRA8) = DXGI_FORMAT_B8G8R8A8_UNORM
	// 1 = BC3 (DXT5) = DXGI_FORMAT_BC3_UNORM
	dd.CompressionTypeIndex = isBGRA ? 0 : 1;

	// Always Color+Alpha texture type
	dd.TextureTypeIndex = COLOR_ALPHA;

	// Read mipmap generation method
	dd.MipMapTypeIndex = static_cast<MipmapEnum>(GetSelectedItem(IDC_MIPMAP_COMBO));

	// Read quality settings (only apply to BC3) - dithering checkbox is owner-drawn
	dd.UseDithering = m_ditheringChecked;
	dd.UseUniformMetric = (GetSelectedItem(IDC_ERRORMETRIC_COMBO) == 1);

	// Default values for hidden/removed controls
	dd.SetMipLevel = false;
	dd.MipLevel = 0;
	dd.Normalize = false;
	dd.FlipX = false;
	dd.FlipY = false;
}

// ==============================================================================
// DisableUnavailableControls - Enables or disables controls depending on texture type
// ==============================================================================
void CustomSaveDialog::DisableUnavailableControls()
{
	HWND normalizeCheck = GetDlgItem(hDlg, IDC_NORMALIZE_CHECK);
	HWND flipXCheck = GetDlgItem(hDlg, IDC_FLIPX_CHECK);
	HWND flipYCheck = GetDlgItem(hDlg, IDC_FLIPY_CHECK);
	HWND mipLevelCheck = GetDlgItem(hDlg, IDC_CUBEMIPLEVEL_CHECK);
	HWND mipLevelCombo = GetDlgItem(hDlg, IDC_MIPLEVEL_COMBO);

	// if texture type normal map then enable/disable relevant checkboxes
	if (mDialogData.TextureTypeIndex != TextureTypeEnum::NORMALMAP)
	{
		// disable and clear normalize checkbox
		EnableWindow(normalizeCheck, false);
		SendMessage(normalizeCheck, BM_SETCHECK, BST_UNCHECKED, 0);
		// disable and clear flipx checkbox
		EnableWindow(flipXCheck, false);
		SendMessage(flipXCheck, BM_SETCHECK, BST_UNCHECKED, 0);
		// disable and clear flipy checkbox
		EnableWindow(flipYCheck, false);
		SendMessage(flipYCheck, BM_SETCHECK, BST_UNCHECKED, 0);
	}
	else
	{
		// enable normalize, flipx,y checkboxes
		EnableWindow(normalizeCheck, true);
		EnableWindow(flipXCheck, true);
		EnableWindow(flipYCheck, true);
	}

	// if texture type is/isnot cubemap then enable/disable combo+checkbox
	if (mDialogData.TextureTypeIndex != TextureTypeEnum::CUBEMAP_CROSSED &&
		mDialogData.TextureTypeIndex != TextureTypeEnum::CUBEMAP_LAYERS)
	{
		// disable miplevel checkbox and combo
		EnableWindow(mipLevelCheck, false);
		EnableWindow(mipLevelCombo, false);
		// uncheck and clear
		SendMessage(mipLevelCheck, BM_SETCHECK, BST_UNCHECKED, 0);
		SendDlgItemMessage(hDlg, IDC_MIPLEVEL_COMBO, CB_RESETCONTENT, 0, 0);
	}
	else
	{
		// enable miplevel checkbox and combo
		EnableWindow(mipLevelCheck, true);
		EnableWindow(mipLevelCombo, true);
	}

	// Dithering is only useful for BC1/BC3 - disable when Uncompressed is selected
	HWND ditheringCheck = GetDlgItem(hDlg, IDC_DITHERING_CHECK);
	HWND errorMetricCombo = GetDlgItem(hDlg, IDC_ERRORMETRIC_COMBO);

	// Derive compression type directly from radio buttons (IDC_COMPRESSION_BGRA).
	// 0 = BGRA/Uncompressed, 1 = BC3. Do NOT use gComboItems since the compression
	// combo was replaced by radio buttons and the vector size may not match.
	bool isBGRASelected = (SendDlgItemMessage(hDlg, IDC_COMPRESSION_BGRA, BM_GETCHECK, 0, 0) == BST_CHECKED);
	int compressionID = isBGRASelected ? CompressionTypeEnum::UNCOMPRESSED : CompressionTypeEnum::BC3;

	if (compressionID == CompressionTypeEnum::UNCOMPRESSED)
	{
		EnableWindow(ditheringCheck, false);
		SendMessage(ditheringCheck, BM_SETCHECK, BST_UNCHECKED, 0);
		EnableWindow(errorMetricCombo, false);
	}
	else
	{
		EnableWindow(ditheringCheck, true);
		EnableWindow(errorMetricCombo, true);
	}
}

// ==============================================================================
// UpdateCompressionCombo - Update compression combo box based on texture type
// ==============================================================================
void CustomSaveDialog::UpdateCompressionCombo()
{
	// Fill new entries into struct
	GetCompressionNames(gComboItems[COMPRESSION_COMBO]);

	// Clear and fill combo box from struct
	InitComboFromItems(COMPRESSION_COMBO);
}

// ==============================================================================
// UpdateMipMapCombo - Update mipmap combo box based on texture type
// ==============================================================================
void CustomSaveDialog::UpdateMipMapCombo()
{
	// Fill new entries into struct
	GetMipMapNames(gComboItems[MIPMAP_COMBO]);

	// Clear and fill combo box from struct
	InitComboFromItems(MIPMAP_COMBO);
}

// ==============================================================================
// GetPresetNames - Populate data struct for Presets dropdown list
// ==============================================================================
void CustomSaveDialog::GetPresetNames(ComboData& comboItem)
{
	comboItem.startIndex = -1;
	comboItem.itemAndContextStrings.clear();

	// push info for stored presets
	for (auto it = mPresets.begin(); it != mPresets.end(); ++it)
	{
		comboItem.itemAndContextStrings.push_back(ComboItemAndContext(it->first, ""));
		if (it->first.compare(mDialogData.PresetName) == 0)
		{
			comboItem.startIndex = static_cast<uint32>(comboItem.itemAndContextStrings.size() - 1);
		}
	}
}

// ==============================================================================
// GetCompressionNames - Populate compression combo data
// ==============================================================================
void CustomSaveDialog::GetCompressionNames(ComboData& comboItem)
{
	comboItem.itemAndContextStrings.clear();

	if (IntelPlugin::IsCombinationValid(mDialogData.TextureTypeIndex, CompressionTypeEnum::BC1))
		comboItem.itemAndContextStrings.push_back(ComboItemAndContext("BC1   4bpp  (DXT1)", "DXT1 compression. No Alpha Channel.", CompressionTypeEnum::BC1));

	if (IntelPlugin::IsCombinationValid(mDialogData.TextureTypeIndex, CompressionTypeEnum::BC3))
		comboItem.itemAndContextStrings.push_back(ComboItemAndContext("BC3   8bpp  (DXT5)", "DXT5 compression. Supports Alpha.", CompressionTypeEnum::BC3));

	if (IntelPlugin::IsCombinationValid(mDialogData.TextureTypeIndex, CompressionTypeEnum::UNCOMPRESSED))
		comboItem.itemAndContextStrings.push_back(ComboItemAndContext("none  32bpp", "Lossless, no compression applied.", CompressionTypeEnum::UNCOMPRESSED));

	if (mDialogData.CompressionTypeIndex < comboItem.itemAndContextStrings.size())
	{
		comboItem.startIndex = mDialogData.CompressionTypeIndex;
	}
	else	// Somehow have an index off the end of the list?  Reset to 0... best to be safe
	{
		comboItem.startIndex = 0;
	}
}

// ==============================================================================
// GetTextureTypeNames - Populate texture type combo data
// ==============================================================================
void CustomSaveDialog::GetTextureTypeNames(ComboData& comboItem)
{
	comboItem.itemAndContextStrings.push_back(ComboItemAndContext("Color", "Export only the RGB channels."));
	comboItem.itemAndContextStrings.push_back(ComboItemAndContext("Color + Alpha", "Export RGB and alpha channel."));
	comboItem.itemAndContextStrings.push_back(ComboItemAndContext("Cube Map from Layers", "Export a cube map using layers for faces (6 layers required)."));
	comboItem.itemAndContextStrings.push_back(ComboItemAndContext("Cube Map from crossed image", "Export a cube map from faces arranged in a horizontal or vertical crossed format."));
	comboItem.itemAndContextStrings.push_back(ComboItemAndContext("Normal Map", "Export as a normal map."));

	if (mDialogData.TextureTypeIndex < comboItem.itemAndContextStrings.size())
	{
		comboItem.startIndex = mDialogData.TextureTypeIndex;
	}
	else	// Somehow have an index off the end of the list?  Reset to 0... best to be safe
	{
		comboItem.startIndex = 0;
	}
}

// ==============================================================================
// GetMipMapNames - Populate mipmap combo data
// ==============================================================================
void CustomSaveDialog::GetMipMapNames(ComboData& comboItem)
{
	comboItem.itemAndContextStrings.clear();

	comboItem.itemAndContextStrings.push_back(ComboItemAndContext("None", "No Mip Maps."));
	comboItem.itemAndContextStrings.push_back(ComboItemAndContext("Auto Generate", "Autogenerate Mip Maps."));

	// If cube maps user can not specify its own mip levels
	if (mDialogData.TextureTypeIndex != TextureTypeEnum::CUBEMAP_CROSSED && mDialogData.TextureTypeIndex != TextureTypeEnum::CUBEMAP_LAYERS)
		comboItem.itemAndContextStrings.push_back(ComboItemAndContext("From Layers", "Mip Maps are specified by the user, stored in layers."));

	if (mDialogData.MipMapTypeIndex < comboItem.itemAndContextStrings.size())
	{
		comboItem.startIndex = mDialogData.MipMapTypeIndex;
	}
	else	// Somehow have an index off the end of the list?  Reset to 0... best to be safe
	{
		comboItem.startIndex = 0;
	}
}

// ==============================================================================
// PopulateMipLevelsCombo - Fills the combo for selecting mip level for cube map export
// ==============================================================================
void CustomSaveDialog::PopulateMipLevelsCombo()
{
	SendDlgItemMessage(hDlg, IDC_MIPLEVEL_COMBO, CB_RESETCONTENT, 0, 0);

	for (uint32 i = 0; i <= MaxMipLevel; ++i)
	{
		stringstream s;
		s << i;
		SendDlgItemMessage(hDlg, IDC_MIPLEVEL_COMBO, CB_ADDSTRING, 0, (LPARAM)s.str().c_str());
	}

	uint32 selectedIndex = mDialogData.MipLevel;	// Convert level to index
	SendDlgItemMessage(hDlg, IDC_MIPLEVEL_COMBO, CB_SETCURSEL, selectedIndex, 0);
}

// ==============================================================================
// SetContextString - Changes the context Text field of the combo box
// ==============================================================================
void CustomSaveDialog::SetContextString(uint32 contextStringID, uint32 controlID)
{
	// Get the index straight from control
	int combo_index = SendDlgItemMessage(hDlg, controlID, CB_GETCURSEL, 0, 0);

	// find the struct with the combo box data for the selected combo box
	// search the array where combo items are stored
	for (uint32 j = 0; j < gComboItems.size(); ++j)
	{
		// Correct combo box
		if (gComboItems[j].itemNum == controlID)
		{
			SetDlgItemTextA(hDlg, contextStringID, gComboItems[j].itemAndContextStrings[combo_index].itemContextInfo.c_str());
			return;
		}
	}
}

// ==============================================================================
// GetSelectedItem - Get selected item index from combo box
// ==============================================================================
uint32 CustomSaveDialog::GetSelectedItem(uint32 comboBoxID)
{
	// Get the index straight from control
	int index = SendDlgItemMessage(hDlg, comboBoxID, CB_GETCURSEL, 0, 0);
	return index;
}

// ==============================================================================
// GetSelectedMipLevelIndex - Get selected mip level index
// ==============================================================================
uint32 CustomSaveDialog::GetSelectedMipLevelIndex()
{
	int index = SendDlgItemMessage(hDlg, IDC_MIPLEVEL_COMBO, CB_GETCURSEL, 0, 0);
	if (index != CB_ERR)
	{
		char buffer[32];
		SendDlgItemMessage(hDlg, IDC_MIPLEVEL_COMBO, CB_GETLBTEXT, index, (LPARAM)buffer);
		return atoi(buffer);
	}
	return 0;
}

// Note: Help text functions (GetCompressionHelpText, etc.) are defined in SaveOptionsDialog.cpp
