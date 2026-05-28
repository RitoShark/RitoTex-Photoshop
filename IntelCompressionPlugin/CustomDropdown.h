////////////////////////////////////////////////////////////////////////////////
// RitoTex — CustomDropdown
//
// A fully custom, owner-drawn dropdown control. There is NO Win32 COMBOBOX
// underneath: the closed state is a self-drawn box (label + chevron) and the
// open state is a borderless, top-level popup window whose list is drawn pixel
// by pixel (dark theme, hover highlight, accent on the selected row).
//
// Usage:
//   CustomDropdown dd;
//   dd.Create(hParent, controlId, x, y, w, h, hFont);   // pixel coords
//   dd.AddItem("BC7", "High quality RGBA");              // text + optional hint
//   dd.SetSelected(0);
//   dd.SetOnChange([&](int idx){ ... });
// The parent receives WM_COMMAND with CBN_SELCHANGE-style notifications too, so
// existing WM_COMMAND routing keeps working if a callback isn't set.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <functional>

class CustomDropdown
{
public:
	struct Item
	{
		std::string text;
		std::string hint;   // optional context/hint string (shown by host elsewhere)
		int         userData;
	};

	CustomDropdown() = default;
	~CustomDropdown();

	// Create the closed control as a child of hParent at pixel coords.
	// controlId is used for WM_COMMAND notifications back to the parent.
	bool Create(HWND hParent, int controlId, int x, int y, int w, int h, HFONT hFont);

	void AddItem(const std::string& text, const std::string& hint = "", int userData = 0);
	void ClearItems();

	int  GetSelected() const { return m_selected; }
	void SetSelected(int index);          // updates display, does NOT fire callback
	int  GetCount() const { return (int)m_items.size(); }
	const std::string& GetHint(int index) const;
	int  GetUserData(int index) const;

	void SetEnabled(bool enabled);
	bool IsEnabled() const { return m_enabled; }

	// Fired when the user picks an item (not when SetSelected is called).
	void SetOnChange(std::function<void(int)> cb) { m_onChange = std::move(cb); }

	HWND GetHwnd() const { return m_hwnd; }
	int  GetId() const { return m_id; }

private:
	// Closed control window proc (our own registered class — no comctl32 subclassing)
	static LRESULT CALLBACK ClosedProc(HWND, UINT, WPARAM, LPARAM);
	// Popup list window proc
	static LRESULT CALLBACK PopupProc(HWND, UINT, WPARAM, LPARAM);

	void   OpenPopup();
	void   ClosePopup();
	void   PaintClosed(HDC hdc);
	void   PaintPopup(HDC hdc);
	int    HitTestPopup(int y) const;
	void   CommitHover();

	static void EnsureClasses();

	HWND   m_hwnd     = nullptr;   // closed control (STATIC, subclassed)
	HWND   m_hParent  = nullptr;
	HWND   m_hPopup   = nullptr;   // borderless popup window (created on open)
	HFONT  m_hFont    = nullptr;
	int    m_id       = 0;
	int    m_selected = 0;
	int    m_hover    = -1;        // hovered row in popup, -1 = none
	bool   m_swallowFirstUp = false; // ignore one stray LBUTTONUP right after opening
	bool   m_enabled  = true;
	bool   m_pressed  = false;     // closed box pressed/open state (for chevron + border accent)

	std::vector<Item> m_items;
	std::function<void(int)> m_onChange;

	static constexpr int ROW_HEIGHT = 26;
	static constexpr int MAX_VISIBLE = 8;   // popup scrolls beyond this (rare here)
};
