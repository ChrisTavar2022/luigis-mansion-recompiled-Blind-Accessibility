// ImGui's item hooks (IMGUI_ENABLE_TEST_ENGINE), compiled into the imgui library so that every
// program linking it has them. They only run while g.TestEngineHookItems is set, which
// src/recomp/access.cpp does for screen reader players. Docs: docs/accessibility/README.md.
#include "imgui.h"
#include "imgui_internal.h"

void (*lmImguiItemAdd)(ImGuiContext* ctx, ImGuiID id) = nullptr;
void (*lmImguiItemInfo)(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) = nullptr;

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect&, const ImGuiLastItemData*) {
    if (lmImguiItemAdd) lmImguiItemAdd(ctx, id);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
    if (lmImguiItemInfo) lmImguiItemInfo(ctx, id, label, flags);
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
