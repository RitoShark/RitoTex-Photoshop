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

#pragma once

#include "IntelPlugin.h"
#include "CustomDropdown.h"
#include <vector>
#include <map>
#include <string>
#include <memory>
#include <Windows.h>

//-------------------------------------------------------------------------------
// Host-adaptive Color Palette
//
// These are populated at runtime from Photoshop's interface colors (via the
// Property suite, propInterfaceColor) so the dialog matches whatever UI theme
// the user has selected — the dark themes AND the two light ones. If the host
// query fails we fall back to the previous hard-coded dark values, so the
// dialog always renders sanely. See CustomSaveDialog::InitHostTheme().
//-------------------------------------------------------------------------------
namespace DarkTheme {
	extern COLORREF DIALOG_BG;       // Main background
	extern COLORREF TITLEBAR_BG;     // Title bar (slightly offset from bg)
	extern COLORREF TEXT_PRIMARY;    // Primary text
	extern COLORREF TEXT_SECONDARY;  // Dimmed / disabled text
	extern COLORREF EDIT_BG;         // Edit / dropdown control bg
	extern COLORREF BUTTON_BG;       // Neutral button background
	extern COLORREF BORDER;          // Borders
	extern COLORREF ACCENT;          // Accent (primary action / selection)
	extern COLORREF ACCENT_HOVER;    // Accent hover
	extern COLORREF CLOSE_HOVER;     // Red close-button hover (constant)
	extern COLORREF CHECK_BG;        // Checkbox background
	extern COLORREF CHECK_MARK;      // Checkbox checked fill
	extern COLORREF COMBO_HIGHLIGHT; // Hovered dropdown item
	extern COLORREF SEPARATOR;       // Separator line

	// Populate the palette from the host's interface colors (idempotent).
	void InitFromHost();
}

// Dialog data struct (copied from SaveOptionsDialog.h)
struct DialogData
{
	// NOTE - assumed to be POD.
	std::string PresetName;           // The name of this preset
	uint32 CompressionTypeIndex;      // The current selection index of the combo box holding the compression
	TextureTypeEnum TextureTypeIndex; // Holds the texture type of the image
	MipmapEnum  MipMapTypeIndex;      // Holds the mip map generation method

	uint32 MipLevel;                  // only valid if SetMipLevel == true, specified the mip level to be exported when in cube map mode.

	bool SetMipLevel;                 // Specify that a specific mip level has to be exported. Only valid for cube map mode.
	bool Normalize;                   // Specify if normalization of values is needed. Only valid for Normal Maps
	bool FlipX;
	bool FlipY;
	bool UseDithering;                // Use error diffusion dithering for BC1/BC3
	bool UseUniformMetric;            // Use uniform error metric (false = perceptual)
};

// Custom save dialog class - Pure Win32 implementation (no PIDialog)
class CustomSaveDialog
{
public:
	explicit CustomSaveDialog(IntelPlugin* plugin);
	~CustomSaveDialog();

	// Public entry point (replaces OptionsDialog::DoModal)
	static int32 DoModal(IntelPlugin* plugin);

	// Batch mode preset loading
	bool LoadPresetNonUIMode(std::string nameOfPreset);

	// Dialog data sync (same interface as OptionsDialog)
	void FillGlobalStruct();
	void GetGlobalStruct();
	const DialogData& GetData() const { return mDialogData; }

private:
	// Custom title bar constants
	static constexpr int TITLEBAR_HEIGHT = 32; // pixels
	static constexpr int CLOSE_BTN_WIDTH = 46; // pixels

	// Core members
	HWND hDlg;
	IntelPlugin* plugin;
	IntelPlugin::Globals* globalParams;
	DialogData mDialogData;
	std::map<std::string, DialogData> mPresets;
	std::string mPathToPresetDirectory;
	uint32 MaxMipLevel;

	// Custom UI state
	bool m_closeHover;       // Close button hover state
	bool m_ditheringChecked; // Checkbox state (since we owner-draw it)
	HFONT m_hTitleFont;      // Bold font for title bar
	HFONT m_hUIFont;         // Regular font for controls

	// Fully custom dropdowns (no Win32 combo chrome)
	std::unique_ptr<CustomDropdown> m_ddCompression;
	std::unique_ptr<CustomDropdown> m_ddMipmap;
	std::unique_ptr<CustomDropdown> m_ddErrorMetric;

	// Compression formats shown in the dropdown, in display order.
	// Index into this table is what the dropdown selection returns.
	struct CompressionFormat
	{
		const char* label;        // shown in the dropdown
		const char* hint;         // hint line under the dropdown
		DXGI_FORMAT encoding;     // what we hand the codec
		CompressionTypeEnum type; // for IsCombinationValid checks
	};
	static const CompressionFormat kCompressionFormats[];
	static const int kCompressionFormatCount;

	void BuildCustomDropdowns();              // create + populate the 3 dropdowns
	void OnCompressionDropdownChange(int idx);
	int  EncodingToFormatIndex(DXGI_FORMAT enc) const;

	// Combo box data structures (REUSE from SaveOptionsDialog)
	struct ComboItemAndContext
	{
		std::string itemText;
		std::string itemContextInfo;
		int itemUserData;

		ComboItemAndContext(std::string text, std::string contextInfo, int userData = 0)
			: itemText(text), itemContextInfo(contextInfo), itemUserData(userData) { }
	};

	struct ComboData
	{
		const uint32 itemNum;                       // WinForm ID of the dropdown list for this particular dropdown
		uint32 startIndex;                          // Which item in the dropdown to select when creating the list - 0 unless a preset is being loaded.
		std::vector<ComboItemAndContext> itemAndContextStrings; // list of strings and any corresponding context info for the dropdown list (context could be blank)

		explicit ComboData(int num) : itemNum(num) { }
	};

	enum { PRESETS_COMBO, COMPRESSION_COMBO, TEXTURETYPE_COMBO, MIPMAP_COMBO, NUMBEROF_COMBOS };

	std::vector<ComboData> gComboItems;

	// Dark theme GDI resources (static - shared across instances)
	static HBRUSH s_brushDialogBg;
	static HBRUSH s_brushEditBg;
	static HPEN s_penBorder;

	// Message handling (Win32 pattern)
	static INT_PTR CALLBACK DialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
	INT_PTR WindowProc(UINT msg, WPARAM wParam, LPARAM lParam);

	// Initialization and event handlers
	void Init();
	void HandleCommand(WPARAM wParam, LPARAM lParam);
	void OnOK();
	void OnCancel();
	void OnPresetSave();
	void OnPresetDelete();
	void OnComboChange(UINT controlID);
	void OnCheckboxChange(UINT controlID);
	void OnHelpButton(UINT controlID);
	void OnCubeMipLevelCheck();
	void OnPresetComboChange();

	// Dark theme methods
	void InitThemeResources();
	void CleanupThemeResources();
	INT_PTR HandleColorMessages(UINT msg, WPARAM wParam, LPARAM lParam);
	void SetFontCompressionCombo();

	// Custom drawing methods
	void CenterDialog();
	void DrawTitleBar(HDC hDC, const RECT& rcClient);
	void DrawCustomCheckbox(LPDRAWITEMSTRUCT pDIS);
	void DrawCustomComboItem(LPDRAWITEMSTRUCT pDIS);
	void MeasureComboItem(LPMEASUREITEMSTRUCT pMIS);

	// Preset management (MIGRATE from SaveOptionsDialog)
	void LoadPresets(void);
	void ReadPreset(std::string fname);
	void SaveNewPreset(std::string presetName, DialogData dd);
	void UpdatePreset(std::string presetName, DialogData dd);
	void DeletePreset(std::string presetName);

	// UI data sync (MIGRATE from SaveOptionsDialog)
	void InitDataNoPreset(DialogData& dd);
	void InitDataFromPreset(std::string presetName);
	void SetUIFromData();
	void ExtractDataFromUI(DialogData& dd);

	// Control initialization (MIGRATE from SaveOptionsDialog)
	void GetPresetNames(ComboData& comboItem);
	void GetCompressionNames(ComboData& comboItem);
	void GetTextureTypeNames(ComboData& comboItem);
	void GetMipMapNames(ComboData& comboItem);
	void PopulateMipLevelsCombo();
	void DisableUnavailableControls();
	void UpdateCompressionCombo();
	void UpdateMipMapCombo();
	void InitComboItems();
	void InitComboFromItems(int32 comboItemsIndex);
	uint32 GetSelectedItem(uint32 comboBoxID);
	uint32 GetSelectedMipLevelIndex();
	void SetContextString(uint32 contextStringID, uint32 index);
	void UpdateQualityControlsState();

	// Helper method
	double Log2(double n);
};

#define PRESET_FILE_VERSION (int)101
#define LAST_SETTINGS_PRESET_NAME "-last-used-"
