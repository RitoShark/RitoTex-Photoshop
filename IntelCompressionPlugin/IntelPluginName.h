////////////////////////////////////////////////////////////////////////////////
// Copyright 2017 Intel Corporation
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

//Change this define to rename the plugin
//It changes the pipl photoshop resource and also the window UI on runtime.

//Be sure also to change the TargetName for the TEXExporter Project in "ConfigurationProperties->General->TargetName" (left click on TEXExporter),
//both for Win32 and x64 settigns.
#define TEXExporterPluginName "RitoTex"
#define TEXExporterPluginVersion " v2.0.2"

// ---------------------------------------------------------------------------
// Version — single source of truth. The update checker compares this against
// the latest GitHub release tag. Keep these three numbers in sync with the
// git release tag (e.g. tag "v2.0.0" -> 2,0,0).
// ---------------------------------------------------------------------------
#define RITOTEX_VERSION_MAJOR 2
#define RITOTEX_VERSION_MINOR 0
#define RITOTEX_VERSION_PATCH 2
#define RITOTEX_VERSION_STR   "2.0.2"

// GitHub repository the update checker queries (api.github.com/repos/<owner>/<repo>/releases/latest)
#define RITOTEX_GITHUB_OWNER  "RitoShark"
#define RITOTEX_GITHUB_REPO   "RitoTex-Photoshop"

// Terminology specific to this plug-in.
#define kPrompt				16100
#define kCreatorAndType		kPrompt+1

// scripting keys
#define keyPreset 'pres'
#define keyMipMap 'mipm'
#define keyAlphaS 'alps'

