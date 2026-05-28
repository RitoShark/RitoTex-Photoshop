////////////////////////////////////////////////////////////////////////////////
// RitoTex — CustomDropdown implementation
//
// See CustomDropdown.h. Closed control is a subclassed STATIC; the open list is
// a borderless WS_POPUP top-level window, fully owner-drawn. No Win32 COMBOBOX.
////////////////////////////////////////////////////////////////////////////////

#include "CustomDropdown.h"
#include "CustomSaveDialog.h"   // DarkTheme palette
#include <windowsx.h>

using namespace DarkTheme;

static const wchar_t* kPopupClass  = L"RitoTexDropdownPopup";
static const wchar_t* kClosedClass = L"RitoTexDropdownClosed";

//-------------------------------------------------------------------------------
CustomDropdown::~CustomDropdown()
{
	ClosePopup();
	// The closed control is a child of the parent dialog and is destroyed with it.
}

//-------------------------------------------------------------------------------
bool CustomDropdown::Create(HWND hParent, int controlId, int x, int y, int w, int h, HFONT hFont)
{
	m_hParent = hParent;
	m_id      = controlId;
	m_hFont   = hFont;

	EnsureClasses();

	m_hwnd = CreateWindowExW(
		0, kClosedClass, L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP,
		x, y, w, h,
		hParent, (HMENU)(INT_PTR)controlId,
		GetModuleHandleW(nullptr), this);

	return m_hwnd != nullptr;
}

void CustomDropdown::AddItem(const std::string& text, const std::string& hint, int userData)
{
	m_items.push_back({ text, hint, userData });
}

void CustomDropdown::ClearItems()
{
	m_items.clear();
	m_selected = 0;
	m_hover = -1;
}

const std::string& CustomDropdown::GetHint(int index) const
{
	static const std::string empty;
	if (index < 0 || index >= (int)m_items.size()) return empty;
	return m_items[index].hint;
}

int CustomDropdown::GetUserData(int index) const
{
	if (index < 0 || index >= (int)m_items.size()) return 0;
	return m_items[index].userData;
}

void CustomDropdown::SetSelected(int index)
{
	if (index < 0 || index >= (int)m_items.size())
		index = 0;
	m_selected = index;
	if (m_hwnd)
		InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CustomDropdown::SetEnabled(bool enabled)
{
	m_enabled = enabled;
	if (m_hwnd)
	{
		EnableWindow(m_hwnd, enabled);
		InvalidateRect(m_hwnd, nullptr, FALSE);
	}
	if (!enabled)
		ClosePopup();
}

//-------------------------------------------------------------------------------
// Closed control: our own window class. We paint it and open the popup on click.
//-------------------------------------------------------------------------------
LRESULT CALLBACK CustomDropdown::ClosedProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	CustomDropdown* self = reinterpret_cast<CustomDropdown*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

	if (msg == WM_NCCREATE)
	{
		auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	if (!self)
		return DefWindowProc(hwnd, msg, wParam, lParam);

	switch (msg)
	{
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			self->PaintClosed(hdc);
			EndPaint(hwnd, &ps);
			return 0;
		}

		case WM_ERASEBKGND:
			return 1;   // we paint everything in WM_PAINT

		case WM_LBUTTONDOWN:
			SetFocus(hwnd);
			if (self->m_enabled)
			{
				if (self->m_hPopup)
					self->ClosePopup();
				else
					self->OpenPopup();
			}
			return 0;

		case WM_SETFOCUS:
		case WM_KILLFOCUS:
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;

		case WM_KEYDOWN:
			if (self->m_enabled &&
			    (wParam == VK_SPACE || wParam == VK_RETURN || wParam == VK_DOWN))
			{
				if (!self->m_hPopup) self->OpenPopup();
				return 0;
			}
			break;

		case WM_GETDLGCODE:
			return DLGC_WANTARROWS | DLGC_WANTCHARS;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

//-------------------------------------------------------------------------------
void CustomDropdown::PaintClosed(HDC hdc)
{
	RECT rc;
	GetClientRect(m_hwnd, &rc);

	// Background
	HBRUSH bg = CreateSolidBrush(EDIT_BG);
	FillRect(hdc, &rc, bg);
	DeleteObject(bg);

	// Border — accent when open/focused, normal otherwise
	bool focused = (GetFocus() == m_hwnd);
	COLORREF borderCol = (m_hPopup || focused) ? ACCENT : BORDER;
	HPEN pen = CreatePen(PS_SOLID, 1, borderCol);
	HPEN oldPen = (HPEN)SelectObject(hdc, pen);
	HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
	RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
	SelectObject(hdc, oldPen);
	SelectObject(hdc, oldBr);
	DeleteObject(pen);

	// Label text
	if (m_hFont) SelectObject(hdc, m_hFont);
	SetBkMode(hdc, TRANSPARENT);
	SetTextColor(hdc, m_enabled ? TEXT_PRIMARY : TEXT_SECONDARY);

	std::string label = (m_selected >= 0 && m_selected < (int)m_items.size())
		? m_items[m_selected].text : "";
	RECT rcText = rc;
	rcText.left += 10;
	rcText.right -= 26;   // leave room for chevron
	DrawTextA(hdc, label.c_str(), -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

	// Chevron (down triangle), accent-tinted
	int cx = rc.right - 16;
	int cy = (rc.top + rc.bottom) / 2;
	POINT tri[3] = { { cx - 4, cy - 2 }, { cx + 4, cy - 2 }, { cx, cy + 3 } };
	COLORREF chevCol = m_enabled ? (m_hPopup ? ACCENT_HOVER : TEXT_SECONDARY) : BORDER;
	HBRUSH chevBr = CreateSolidBrush(chevCol);
	HPEN chevPen = CreatePen(PS_SOLID, 1, chevCol);
	HBRUSH oldB2 = (HBRUSH)SelectObject(hdc, chevBr);
	HPEN oldP2 = (HPEN)SelectObject(hdc, chevPen);
	Polygon(hdc, tri, 3);
	SelectObject(hdc, oldB2);
	SelectObject(hdc, oldP2);
	DeleteObject(chevBr);
	DeleteObject(chevPen);
}

//-------------------------------------------------------------------------------
// Popup window
//-------------------------------------------------------------------------------
void CustomDropdown::EnsureClasses()
{
	static bool registered = false;
	if (registered) return;
	registered = true;

	HINSTANCE hInst = GetModuleHandleW(nullptr);

	WNDCLASSEXW closed = { sizeof(closed) };
	closed.lpfnWndProc   = ClosedProc;
	closed.hInstance     = hInst;
	closed.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	closed.hbrBackground = nullptr;            // we paint
	closed.lpszClassName = kClosedClass;
	RegisterClassExW(&closed);

	WNDCLASSEXW popup = { sizeof(popup) };
	popup.style          = CS_DROPSHADOW;      // subtle shadow under the list
	popup.lpfnWndProc    = PopupProc;
	popup.hInstance      = hInst;
	popup.hCursor        = LoadCursor(nullptr, IDC_ARROW);
	popup.hbrBackground  = nullptr;            // we paint
	popup.lpszClassName  = kPopupClass;
	RegisterClassExW(&popup);
}

void CustomDropdown::OpenPopup()
{
	if (m_items.empty() || m_hPopup) return;
	EnsureClasses();

	RECT rc;
	GetWindowRect(m_hwnd, &rc);   // screen coords of closed control
	int width  = rc.right - rc.left;
	int rows   = (int)m_items.size();
	int height = rows * ROW_HEIGHT + 2;   // +2 for the 1px frame top/bottom

	m_hover = m_selected;

	m_hPopup = CreateWindowExW(
		WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
		kPopupClass, L"",
		WS_POPUP,
		rc.left, rc.bottom + 2, width, height,
		m_hParent, nullptr, GetModuleHandleW(nullptr), this);

	if (!m_hPopup) return;

	SetWindowLongPtr(m_hPopup, GWLP_USERDATA, (LONG_PTR)this);
	ShowWindow(m_hPopup, SW_SHOWNOACTIVATE);
	SetCapture(m_hPopup);   // capture so clicks outside close the popup
	InvalidateRect(m_hwnd, nullptr, FALSE);   // redraw closed (open state)
}

void CustomDropdown::ClosePopup()
{
	if (!m_hPopup) return;
	if (GetCapture() == m_hPopup) ReleaseCapture();
	DestroyWindow(m_hPopup);
	m_hPopup = nullptr;
	m_hover = -1;
	if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

int CustomDropdown::HitTestPopup(int y) const
{
	int idx = (y - 1) / ROW_HEIGHT;
	if (idx < 0 || idx >= (int)m_items.size()) return -1;
	return idx;
}

void CustomDropdown::CommitHover()
{
	if (m_hover < 0 || m_hover >= (int)m_items.size()) { ClosePopup(); return; }
	int picked = m_hover;
	bool changed = (picked != m_selected);
	m_selected = picked;
	ClosePopup();
	InvalidateRect(m_hwnd, nullptr, FALSE);

	if (changed)
	{
		// Fire both: a callback (preferred) and a WM_COMMAND for legacy routing.
		if (m_onChange) m_onChange(picked);
		SendMessage(m_hParent, WM_COMMAND,
		            MAKEWPARAM(m_id, CBN_SELCHANGE), (LPARAM)m_hwnd);
	}
}

LRESULT CALLBACK CustomDropdown::PopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	CustomDropdown* self = reinterpret_cast<CustomDropdown*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (!self)
		return DefWindowProc(hwnd, msg, wParam, lParam);

	switch (msg)
	{
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			self->PaintPopup(hdc);
			EndPaint(hwnd, &ps);
			return 0;
		}

		case WM_ERASEBKGND:
			return 1;

		case WM_MOUSEMOVE:
		{
			int y = GET_Y_LPARAM(lParam);
			int x = GET_X_LPARAM(lParam);
			RECT rc; GetClientRect(hwnd, &rc);
			int hov = -1;
			if (x >= 0 && x < rc.right)
				hov = self->HitTestPopup(y);
			if (hov != self->m_hover)
			{
				self->m_hover = hov;
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			return 0;
		}

		case WM_LBUTTONUP:
		case WM_LBUTTONDOWN:
		{
			POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			RECT rc; GetClientRect(hwnd, &rc);
			if (pt.x < 0 || pt.y < 0 || pt.x >= rc.right || pt.y >= rc.bottom)
			{
				// Click outside the popup → dismiss without changing selection.
				self->ClosePopup();
				return 0;
			}
			if (msg == WM_LBUTTONUP)
			{
				self->m_hover = self->HitTestPopup(pt.y);
				self->CommitHover();
			}
			return 0;
		}

		case WM_CAPTURECHANGED:
			// Lost capture (e.g. user alt-tabbed) → close.
			if ((HWND)lParam != hwnd)
				self->ClosePopup();
			return 0;

		case WM_KEYDOWN:
			if (wParam == VK_ESCAPE) { self->ClosePopup(); return 0; }
			break;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

void CustomDropdown::PaintPopup(HDC hdc)
{
	RECT rc;
	GetClientRect(m_hPopup, &rc);

	// Frame fill
	HBRUSH bg = CreateSolidBrush(EDIT_BG);
	FillRect(hdc, &rc, bg);
	DeleteObject(bg);

	if (m_hFont) SelectObject(hdc, m_hFont);
	SetBkMode(hdc, TRANSPARENT);

	for (int i = 0; i < (int)m_items.size(); ++i)
	{
		RECT row = { 1, 1 + i * ROW_HEIGHT, rc.right - 1, 1 + (i + 1) * ROW_HEIGHT };

		bool isHover = (i == m_hover);
		bool isSel   = (i == m_selected);

		if (isHover)
		{
			HBRUSH hb = CreateSolidBrush(ACCENT);
			FillRect(hdc, &row, hb);
			DeleteObject(hb);
		}
		else if (isSel)
		{
			HBRUSH hb = CreateSolidBrush(COMBO_HIGHLIGHT);
			FillRect(hdc, &row, hb);
			DeleteObject(hb);
		}

		// Selected marker bar on the left
		if (isSel && !isHover)
		{
			RECT bar = { row.left, row.top, row.left + 3, row.bottom };
			HBRUSH bb = CreateSolidBrush(ACCENT);
			FillRect(hdc, &bar, bb);
			DeleteObject(bb);
		}

		SetTextColor(hdc, TEXT_PRIMARY);
		RECT rcText = row;
		rcText.left += 10;
		rcText.right -= 8;
		DrawTextA(hdc, m_items[i].text.c_str(), -1, &rcText,
		          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}

	// Outer border
	HPEN pen = CreatePen(PS_SOLID, 1, ACCENT);
	HPEN oldPen = (HPEN)SelectObject(hdc, pen);
	HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
	Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
	SelectObject(hdc, oldPen);
	SelectObject(hdc, oldBr);
	DeleteObject(pen);
}
