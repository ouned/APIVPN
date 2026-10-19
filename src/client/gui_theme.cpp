#include "gui_win32_internal.hpp"

// Purpose: Theme palettes and ImGui style application.

namespace apivpn::client {

void applyThemeSizes() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding     = ImVec2(12.0f, 12.0f);
    style.FramePadding      = ImVec2(10.0f, 6.0f);
    style.CellPadding       = ImVec2(10.0f, 7.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 6.0f);
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 10.0f;

    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 8.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;

    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
}

PillColors pillColors(PillTone tone) {
    switch (tone) {
        case PillTone::Green:     return { g_pal.pill_green_bg, g_pal.pill_green_fg };
        case PillTone::TealGreen: return { g_pal.pill_teal_bg, g_pal.pill_teal_fg };
        case PillTone::Blue:      return { g_pal.pill_blue_bg, g_pal.pill_blue_fg };
        case PillTone::DeepBlue:  return { g_pal.pill_deepblue_bg, g_pal.pill_deepblue_fg };
        case PillTone::Slate:     return { g_pal.pill_slate_bg, g_pal.pill_slate_fg };
        case PillTone::Muted:     return { g_pal.pill_muted_bg, g_pal.pill_muted_fg };
        case PillTone::Amber:     return { g_pal.pill_amber_bg, g_pal.pill_amber_fg };
        case PillTone::Orange:    return { g_pal.pill_orange_bg, g_pal.pill_orange_fg };
        case PillTone::Red:       return { g_pal.pill_red_bg, g_pal.pill_red_fg };
    }
    return { g_pal.pill_slate_bg, g_pal.pill_slate_fg };
}

void applyThemeExtraColors(bool light, ImVec4* colors) {
    if (!light) {
        g_pal.accent    = v4(0.55f, 0.82f, 1.00f, 1.0f);
        g_pal.success   = v4(0.45f, 0.95f, 0.72f, 1.0f);
        g_pal.warning   = v4(0.98f, 0.80f, 0.42f, 1.0f);
        g_pal.danger    = v4(0.98f, 0.50f, 0.50f, 1.0f);
        g_pal.neutral   = v4(0.72f, 0.78f, 0.86f, 1.0f);
        g_pal.muted     = v4(0.75f, 0.82f, 0.92f, 1.0f);
        g_pal.rate_tx   = v4(0.45f, 0.90f, 0.68f, 1.0f);
        g_pal.rate_rx   = v4(0.48f, 0.76f, 1.00f, 1.0f);
        g_pal.plot_tx   = v4(0.22f, 0.78f, 0.56f, 1.0f);
        g_pal.plot_rx   = v4(0.28f, 0.65f, 0.98f, 1.0f);
        g_pal.chat_vip  = v4(0.45f, 0.56f, 0.70f, 1.0f);
        g_pal.chat_self = v4(0.28f, 0.88f, 0.64f, 1.0f);
        g_pal.chat_other = v4(0.38f, 0.72f, 1.00f, 1.0f);
        g_pal.mention_text = v4(1.00f, 0.86f, 0.55f, 1.0f);
        g_pal.toast_bg_ok  = v4(0.10f, 0.28f, 0.22f, 0.96f);
        g_pal.toast_bg_err = v4(0.38f, 0.12f, 0.14f, 0.96f);
        g_pal.toast_fg_ok  = v4(0.55f, 0.98f, 0.78f, 1.0f);
        g_pal.toast_fg_err = v4(1.00f, 0.65f, 0.65f, 1.0f);
        g_pal.vip_btn_bg        = v4(0.10f, 0.25f, 0.22f, 1.0f);
        g_pal.vip_btn_hover     = v4(0.14f, 0.34f, 0.29f, 1.0f);
        g_pal.vip_btn_bg_off    = v4(0.14f, 0.18f, 0.24f, 1.0f);
        g_pal.vip_btn_hover_off = v4(0.18f, 0.24f, 0.32f, 1.0f);
        g_pal.btn_blue          = v4(0.14f, 0.45f, 0.88f); g_pal.btn_blue_hover = v4(0.20f, 0.54f, 0.96f); g_pal.btn_blue_active = v4(0.11f, 0.37f, 0.76f);
        g_pal.btn_green         = v4(0.12f, 0.55f, 0.36f); g_pal.btn_green_hover = v4(0.16f, 0.66f, 0.44f); g_pal.btn_green_active = v4(0.10f, 0.48f, 0.31f);
        g_pal.btn_red           = v4(0.72f, 0.18f, 0.20f); g_pal.btn_red_hover = v4(0.85f, 0.24f, 0.26f); g_pal.btn_red_active = v4(0.62f, 0.15f, 0.17f);

        g_pal.sb_selected_bg  = c32(32, 48, 72, 235);
        g_pal.sb_selected_bar = c32(59, 130, 246, 255);
        g_pal.sb_hover_bg     = c32(24, 34, 48, 180);
        g_pal.sb_title_sel    = c32(255, 255, 255, 255);
        g_pal.sb_title        = c32(220, 228, 240, 255);
        g_pal.sb_subtitle     = c32(142, 158, 182, 255);
        g_pal.mention_bg      = c32(217, 119, 6, 40);

        g_pal.pill_green_bg    = c32(16, 92, 68, 220);   g_pal.pill_green_fg    = c32(110, 245, 190, 255);
        g_pal.pill_teal_bg     = c32(18, 82, 64, 225);   g_pal.pill_teal_fg     = c32(125, 240, 195, 255);
        g_pal.pill_blue_bg     = c32(20, 78, 128, 230);  g_pal.pill_blue_fg     = c32(130, 215, 255, 255);
        g_pal.pill_deepblue_bg = c32(24, 72, 124, 225);  g_pal.pill_deepblue_fg = c32(145, 215, 255, 255);
        g_pal.pill_slate_bg    = c32(48, 72, 96, 220);   g_pal.pill_slate_fg    = c32(190, 215, 240, 255);
        g_pal.pill_muted_bg    = c32(60, 68, 82, 180);   g_pal.pill_muted_fg    = c32(170, 180, 195, 255);
        g_pal.pill_amber_bg    = c32(180, 95, 12, 230);  g_pal.pill_amber_fg    = c32(255, 215, 140, 255);
        g_pal.pill_orange_bg   = c32(195, 78, 12, 235);  g_pal.pill_orange_fg   = c32(255, 210, 150, 255);
        g_pal.pill_red_bg      = c32(135, 36, 42, 220);  g_pal.pill_red_fg      = c32(255, 165, 165, 255);

        colors[ImGuiCol_TitleBgCollapsed]          = v4(0.06f, 0.08f, 0.11f, 1.00f);
        colors[ImGuiCol_SeparatorHovered]          = v4(0.25f, 0.42f, 0.65f, 1.00f);
        colors[ImGuiCol_SeparatorActive]           = v4(0.35f, 0.60f, 0.90f, 1.00f);
        colors[ImGuiCol_ResizeGrip]                = v4(0.22f, 0.28f, 0.38f, 0.60f);
        colors[ImGuiCol_ResizeGripHovered]         = v4(0.28f, 0.36f, 0.48f, 0.80f);
        colors[ImGuiCol_ResizeGripActive]          = v4(0.35f, 0.44f, 0.58f, 1.00f);
        colors[ImGuiCol_TabSelectedOverline]       = v4(0.23f, 0.51f, 0.96f, 1.00f);
        colors[ImGuiCol_TabDimmed]                 = v4(0.08f, 0.11f, 0.15f, 1.00f);
        colors[ImGuiCol_TabDimmedSelected]         = v4(0.12f, 0.22f, 0.38f, 1.00f);
        colors[ImGuiCol_TabDimmedSelectedOverline] = v4(0.23f, 0.51f, 0.96f, 0.60f);
        colors[ImGuiCol_PlotHistogram]             = v4(0.23f, 0.65f, 0.98f, 1.00f);
        colors[ImGuiCol_PlotHistogramHovered]      = v4(0.38f, 0.78f, 1.00f, 1.00f);
        colors[ImGuiCol_TextLink]                  = v4(0.30f, 0.68f, 1.00f, 1.00f);
        colors[ImGuiCol_TextSelectedBg]            = v4(0.23f, 0.51f, 0.96f, 0.35f);
        colors[ImGuiCol_DragDropTarget]            = v4(1.00f, 1.00f, 0.00f, 0.90f);
        colors[ImGuiCol_NavCursor]                 = v4(0.23f, 0.51f, 0.96f, 0.80f);
        colors[ImGuiCol_NavWindowingHighlight]     = v4(1.00f, 1.00f, 1.00f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg]         = v4(0.05f, 0.07f, 0.10f, 0.60f);
        colors[ImGuiCol_ModalWindowDimBg]          = v4(0.03f, 0.04f, 0.06f, 0.55f);
    } else {
        g_pal.accent    = v4(0.09f, 0.33f, 0.68f, 1.0f);
        g_pal.success   = v4(0.04f, 0.47f, 0.30f, 1.0f);
        g_pal.warning   = v4(0.66f, 0.42f, 0.00f, 1.0f);
        g_pal.danger    = v4(0.75f, 0.12f, 0.14f, 1.0f);
        g_pal.neutral   = v4(0.38f, 0.42f, 0.48f, 1.0f);
        g_pal.muted     = v4(0.40f, 0.44f, 0.51f, 1.0f);
        g_pal.rate_tx   = v4(0.04f, 0.47f, 0.30f, 1.0f);
        g_pal.rate_rx   = v4(0.09f, 0.33f, 0.68f, 1.0f);
        g_pal.plot_tx   = v4(0.09f, 0.52f, 0.34f, 1.0f);
        g_pal.plot_rx   = v4(0.12f, 0.38f, 0.80f, 1.0f);
        g_pal.chat_vip  = v4(0.42f, 0.47f, 0.55f, 1.0f);
        g_pal.chat_self = v4(0.04f, 0.47f, 0.30f, 1.0f);
        g_pal.chat_other = v4(0.09f, 0.33f, 0.68f, 1.0f);
        g_pal.mention_text = v4(0.55f, 0.34f, 0.00f, 1.0f);
        g_pal.toast_bg_ok  = v4(0.82f, 0.95f, 0.87f, 0.98f);
        g_pal.toast_bg_err = v4(0.99f, 0.86f, 0.86f, 0.98f);
        g_pal.toast_fg_ok  = v4(0.03f, 0.35f, 0.22f, 1.0f);
        g_pal.toast_fg_err = v4(0.60f, 0.10f, 0.12f, 1.0f);
        g_pal.vip_btn_bg        = v4(0.80f, 0.93f, 0.86f, 1.0f);
        g_pal.vip_btn_hover     = v4(0.72f, 0.89f, 0.80f, 1.0f);
        g_pal.vip_btn_bg_off    = v4(0.88f, 0.90f, 0.93f, 1.0f);
        g_pal.vip_btn_hover_off = v4(0.82f, 0.85f, 0.90f, 1.0f);
        g_pal.btn_blue          = v4(0.64f, 0.79f, 0.97f); g_pal.btn_blue_hover = v4(0.55f, 0.73f, 0.96f); g_pal.btn_blue_active = v4(0.46f, 0.66f, 0.93f);
        g_pal.btn_green         = v4(0.62f, 0.86f, 0.72f); g_pal.btn_green_hover = v4(0.52f, 0.81f, 0.65f); g_pal.btn_green_active = v4(0.44f, 0.76f, 0.58f);
        g_pal.btn_red           = v4(0.96f, 0.72f, 0.72f); g_pal.btn_red_hover = v4(0.94f, 0.63f, 0.63f); g_pal.btn_red_active = v4(0.90f, 0.55f, 0.55f);

        g_pal.sb_selected_bg  = c32(214, 229, 250, 255);
        g_pal.sb_selected_bar = c32(37, 99, 235, 255);
        g_pal.sb_hover_bg     = c32(232, 238, 246, 255);
        g_pal.sb_title_sel    = c32(10, 25, 50, 255);
        g_pal.sb_title        = c32(30, 40, 55, 255);
        g_pal.sb_subtitle     = c32(105, 120, 140, 255);
        g_pal.mention_bg      = c32(255, 196, 0, 70);

        g_pal.pill_green_bg    = c32(204, 240, 219, 255); g_pal.pill_green_fg    = c32(8, 102, 61, 255);
        g_pal.pill_teal_bg     = c32(204, 240, 224, 255); g_pal.pill_teal_fg     = c32(8, 102, 77, 255);
        g_pal.pill_blue_bg     = c32(209, 230, 252, 255); g_pal.pill_blue_fg     = c32(18, 74, 153, 255);
        g_pal.pill_deepblue_bg = c32(209, 232, 252, 255); g_pal.pill_deepblue_fg = c32(15, 71, 148, 255);
        g_pal.pill_slate_bg    = c32(224, 232, 240, 255); g_pal.pill_slate_fg    = c32(61, 79, 102, 255);
        g_pal.pill_muted_bg    = c32(232, 235, 240, 255); g_pal.pill_muted_fg    = c32(102, 112, 128, 255);
        g_pal.pill_amber_bg    = c32(252, 232, 191, 255); g_pal.pill_amber_fg    = c32(133, 79, 0, 255);
        g_pal.pill_orange_bg   = c32(252, 222, 184, 255); g_pal.pill_orange_fg   = c32(153, 69, 0, 255);
        g_pal.pill_red_bg      = c32(252, 217, 217, 255); g_pal.pill_red_fg      = c32(161, 23, 28, 255);

        colors[ImGuiCol_TitleBgCollapsed]          = v4(0.90f, 0.92f, 0.95f, 1.00f);
        colors[ImGuiCol_SeparatorHovered]          = v4(0.55f, 0.63f, 0.75f, 1.00f);
        colors[ImGuiCol_SeparatorActive]           = v4(0.35f, 0.50f, 0.75f, 1.00f);
        colors[ImGuiCol_ResizeGrip]                = v4(0.60f, 0.65f, 0.72f, 0.60f);
        colors[ImGuiCol_ResizeGripHovered]         = v4(0.50f, 0.56f, 0.66f, 0.80f);
        colors[ImGuiCol_ResizeGripActive]          = v4(0.40f, 0.47f, 0.60f, 1.00f);
        colors[ImGuiCol_TabSelectedOverline]       = v4(0.16f, 0.42f, 0.88f, 1.00f);
        colors[ImGuiCol_TabDimmed]                 = v4(0.90f, 0.92f, 0.95f, 1.00f);
        colors[ImGuiCol_TabDimmedSelected]         = v4(0.80f, 0.87f, 0.96f, 1.00f);
        colors[ImGuiCol_TabDimmedSelectedOverline] = v4(0.16f, 0.42f, 0.88f, 0.60f);
        colors[ImGuiCol_PlotHistogram]             = v4(0.16f, 0.42f, 0.88f, 1.00f);
        colors[ImGuiCol_PlotHistogramHovered]      = v4(0.10f, 0.34f, 0.78f, 1.00f);
        colors[ImGuiCol_TextLink]                  = v4(0.09f, 0.36f, 0.78f, 1.00f);
        colors[ImGuiCol_TextSelectedBg]            = v4(0.16f, 0.42f, 0.88f, 0.30f);
        colors[ImGuiCol_DragDropTarget]            = v4(0.90f, 0.70f, 0.00f, 0.90f);
        colors[ImGuiCol_NavCursor]                 = v4(0.16f, 0.42f, 0.88f, 0.80f);
        colors[ImGuiCol_NavWindowingHighlight]     = v4(0.20f, 0.25f, 0.35f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg]         = v4(0.90f, 0.92f, 0.95f, 0.60f);
        colors[ImGuiCol_ModalWindowDimBg]          = v4(0.60f, 0.62f, 0.66f, 0.45f);
    }
}

void applyThemeColors(bool light) {
    ImVec4* colors = ImGui::GetStyle().Colors;
    applyThemeExtraColors(light, colors);
    if (!light) {
        colors[ImGuiCol_Text]                  = ImVec4(0.93f, 0.95f, 0.98f, 1.00f);
        colors[ImGuiCol_TextDisabled]          = ImVec4(0.54f, 0.60f, 0.70f, 1.00f);
        colors[ImGuiCol_WindowBg]              = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
        colors[ImGuiCol_ChildBg]               = ImVec4(0.09f, 0.12f, 0.16f, 1.00f);
        colors[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.13f, 0.18f, 0.98f);
        colors[ImGuiCol_Border]                = ImVec4(0.16f, 0.21f, 0.29f, 1.00f);
        colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg]               = ImVec4(0.06f, 0.08f, 0.12f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.11f, 0.15f, 0.22f, 1.00f);
        colors[ImGuiCol_FrameBgActive]         = ImVec4(0.14f, 0.20f, 0.29f, 1.00f);
        colors[ImGuiCol_TitleBg]               = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
        colors[ImGuiCol_TitleBgActive]         = ImVec4(0.08f, 0.11f, 0.15f, 1.00f);
        colors[ImGuiCol_MenuBarBg]             = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
        colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.06f, 0.08f, 0.11f, 0.60f);
        colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.22f, 0.28f, 0.38f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.28f, 0.36f, 0.48f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.35f, 0.44f, 0.58f, 1.00f);
        colors[ImGuiCol_CheckMark]             = ImVec4(0.23f, 0.51f, 0.96f, 1.00f);
        colors[ImGuiCol_SliderGrab]            = ImVec4(0.23f, 0.51f, 0.96f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.35f, 0.62f, 0.98f, 1.00f);
        colors[ImGuiCol_Button]                = ImVec4(0.15f, 0.21f, 0.30f, 1.00f);
        colors[ImGuiCol_ButtonHovered]         = ImVec4(0.21f, 0.29f, 0.41f, 1.00f);
        colors[ImGuiCol_ButtonActive]          = ImVec4(0.17f, 0.24f, 0.35f, 1.00f);
        colors[ImGuiCol_Header]                = ImVec4(0.15f, 0.22f, 0.32f, 1.00f);
        colors[ImGuiCol_HeaderHovered]         = ImVec4(0.20f, 0.29f, 0.42f, 1.00f);
        colors[ImGuiCol_HeaderActive]          = ImVec4(0.24f, 0.35f, 0.50f, 1.00f);
        colors[ImGuiCol_Separator]             = ImVec4(0.16f, 0.21f, 0.29f, 1.00f);
        colors[ImGuiCol_Tab]                   = ImVec4(0.11f, 0.15f, 0.21f, 1.00f);
        colors[ImGuiCol_TabHovered]            = ImVec4(0.20f, 0.36f, 0.62f, 1.00f);
        colors[ImGuiCol_TabSelected]           = ImVec4(0.18f, 0.34f, 0.58f, 1.00f);
        colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.07f, 0.09f, 0.13f, 1.00f);
        colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.16f, 0.21f, 0.29f, 1.00f);
        colors[ImGuiCol_TableBorderLight]      = ImVec4(0.12f, 0.16f, 0.22f, 1.00f);
        colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
        colors[ImGuiCol_PlotLines]             = ImVec4(0.23f, 0.65f, 0.98f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered]      = ImVec4(0.38f, 0.78f, 1.00f, 1.00f);
    } else {
        colors[ImGuiCol_Text]                  = ImVec4(0.10f, 0.13f, 0.18f, 1.00f);
        colors[ImGuiCol_TextDisabled]          = ImVec4(0.42f, 0.47f, 0.55f, 1.00f);
        colors[ImGuiCol_WindowBg]              = ImVec4(0.94f, 0.95f, 0.97f, 1.00f);
        colors[ImGuiCol_ChildBg]               = ImVec4(0.98f, 0.98f, 0.99f, 1.00f);
        colors[ImGuiCol_PopupBg]               = ImVec4(1.00f, 1.00f, 1.00f, 0.98f);
        colors[ImGuiCol_Border]                = ImVec4(0.78f, 0.81f, 0.86f, 1.00f);
        colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg]               = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.85f, 0.88f, 0.93f, 1.00f);
        colors[ImGuiCol_FrameBgActive]         = ImVec4(0.80f, 0.85f, 0.92f, 1.00f);
        colors[ImGuiCol_TitleBg]               = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
        colors[ImGuiCol_TitleBgActive]         = ImVec4(0.85f, 0.88f, 0.93f, 1.00f);
        colors[ImGuiCol_MenuBarBg]             = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
        colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.90f, 0.92f, 0.95f, 0.60f);
        colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.68f, 0.72f, 0.79f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.60f, 0.65f, 0.74f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.52f, 0.58f, 0.68f, 1.00f);
        colors[ImGuiCol_CheckMark]             = ImVec4(0.16f, 0.42f, 0.88f, 1.00f);
        colors[ImGuiCol_SliderGrab]            = ImVec4(0.16f, 0.42f, 0.88f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.10f, 0.34f, 0.78f, 1.00f);
        colors[ImGuiCol_Button]                = ImVec4(0.84f, 0.87f, 0.91f, 1.00f);
        colors[ImGuiCol_ButtonHovered]         = ImVec4(0.78f, 0.82f, 0.89f, 1.00f);
        colors[ImGuiCol_ButtonActive]          = ImVec4(0.72f, 0.77f, 0.86f, 1.00f);
        colors[ImGuiCol_Header]                = ImVec4(0.82f, 0.86f, 0.92f, 1.00f);
        colors[ImGuiCol_HeaderHovered]         = ImVec4(0.76f, 0.82f, 0.90f, 1.00f);
        colors[ImGuiCol_HeaderActive]          = ImVec4(0.70f, 0.77f, 0.88f, 1.00f);
        colors[ImGuiCol_Separator]             = ImVec4(0.78f, 0.81f, 0.86f, 1.00f);
        colors[ImGuiCol_Tab]                   = ImVec4(0.86f, 0.89f, 0.93f, 1.00f);
        colors[ImGuiCol_TabHovered]            = ImVec4(0.70f, 0.80f, 0.94f, 1.00f);
        colors[ImGuiCol_TabSelected]           = ImVec4(0.62f, 0.76f, 0.95f, 1.00f);
        colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.88f, 0.90f, 0.94f, 1.00f);
        colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.72f, 0.76f, 0.82f, 1.00f);
        colors[ImGuiCol_TableBorderLight]      = ImVec4(0.82f, 0.85f, 0.89f, 1.00f);
        colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_TableRowBgAlt]         = ImVec4(0.00f, 0.00f, 0.00f, 0.03f);
        colors[ImGuiCol_PlotLines]             = ImVec4(0.16f, 0.42f, 0.88f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered]      = ImVec4(0.10f, 0.34f, 0.78f, 1.00f);
    }
}

bool systemPrefersLightTheme() {
    DWORD use_light = 0;
    DWORD size = sizeof(use_light);
    if (RegGetValueA(HKEY_CURRENT_USER,
                     "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     "AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &use_light, &size) == ERROR_SUCCESS) {
        return use_light != 0;
    }
    return false;
}

bool resolvedLightTheme(const GuiState& st) {
    return (st.theme_mode == ThemeMode::Light) ||
           (st.theme_mode == ThemeMode::System && systemPrefersLightTheme());
}

void applyTheme(GuiState& st) {
    applyThemeSizes();
    st.resolved_light = resolvedLightTheme(st);
    applyThemeColors(st.resolved_light);
    ImGui::GetStyle().ScaleAllSizes(st.dpi_scale > 0.0f ? st.dpi_scale : 1.0f);
}

void applyThemeAndWindow(HWND hWnd, GuiState& st) {
    applyTheme(st);
    if (hWnd && IsWindow(hWnd)) {
        BOOL dark = st.resolved_light ? FALSE : TRUE;
        DwmSetWindowAttribute(hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    }
}

void drawPillBadge(const GuiState& st, const char* text, ImU32 bg_col, ImU32 text_col) {
    ImVec2 text_sz = ImGui::CalcTextSize(text);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float pad_x = st.S(8.0f);
    float pad_y = st.S(2.5f);
    ImVec2 rect_min = pos;
    ImVec2 rect_max = ImVec2(pos.x + text_sz.x + pad_x * 2.0f, pos.y + text_sz.y + pad_y * 2.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(rect_min, rect_max, bg_col, st.S(10.0f));
    dl->AddText(ImVec2(pos.x + pad_x, pos.y + pad_y), text_col, text);
    ImGui::Dummy(ImVec2(text_sz.x + pad_x * 2.0f, text_sz.y + pad_y * 2.0f));
}


} // namespace apivpn::client
