#include "NozzlePickerPanel.hpp"

#include <algorithm>
#include <sstream>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statbox.h>

#include "Widgets/ComboBox.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/Label.hpp"
#include "wxExtensions.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/LocalesUtils.hpp"

namespace Slic3r { namespace GUI {

// Two-column grid, matching the Filament section. Nozzle i goes into column (i % NOZZLE_COLUMNS) so
// they read row-major (1 2 / 3 4); this fits the narrow printer sidebar and scales to more nozzles.
// The panel lives inside the sidebar's scrolled window, so extra rows scroll with it -- no wrapper here.
static const int NOZZLE_COLUMNS = 2;

// Spacing literals copied from SidebarProps (Plater.hpp) so the nozzle rows + title match the Filament
// section without pulling in the heavy Plater.hpp: TitlebarMargin()=8 (title icon inset),
// ElementSpacing()=5 (related controls, e.g. icon<->label and label<->combo).
static const int kTitlebarMargin = 8;
static const int kElementSpacing = 5;

// "0.4" / "0.4mm" -> 0.4 . Strict: after an optional trailing "mm" unit, the whole string must be a
// single number. Composite/combined variants like "0.4+0.6" are rejected (return 0), not silently
// truncated to their leading value. The "mm" suffix is accepted because the dropdowns display it.
//
// Must parse with a fixed decimal point regardless of the active UI language: plain std::stod/strtod
// read the process-wide LC_NUMERIC, which the Russian (and other comma-decimal) translations switch to
// a comma separator, making every "0.4"-style string here fail to parse (returns 0 for all nozzles).
// string_to_double_decimal_point (fast_float) always expects '.', matching config/format_diameter output.
static double parse_diameter(const wxString& value)
{
    std::string s = value.ToStdString();
    if (s.size() >= 2 && s.compare(s.size() - 2, 2, "mm") == 0)
        s.erase(s.size() - 2);
    size_t consumed = 0;
    double d = Slic3r::string_to_double_decimal_point(s, &consumed);
    return consumed == s.size() ? d : 0.0;
}

// Format a diameter as a trimmed "0.6mm" string for display in a dropdown. Always a '.' decimal point
// (see parse_diameter) -- float_to_string_decimal_point is locale-independent, unlike ostringstream.
static wxString format_diameter(double mm)
{
    std::string s = Slic3r::float_to_string_decimal_point(mm, 2);
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.') s += '0';
    }
    return wxString(s) + "mm";
}

// A printer_variant is offered as a per-extruder nozzle size only if it is a single diameter.
// A model can ship a *combined* variant such as "0.4+0.6" (the old single-preset way to describe a
// mixed-nozzle machine, which this picker replaces); one toolhead is "0.4" or "0.6", never "0.4+0.6",
// so combined variants are filtered out here. parse_diameter rejects them by returning 0.
static bool is_single_diameter(const std::string& variant)
{
    return parse_diameter(wxString(variant)) > 0.0;
}

NozzlePickerPanel::NozzlePickerPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    // Inherit the surrounding menu's background (the printer-content panel we're parented to) rather
    // than hardcoding a colour, so the area behind the nozzle rows is the same shade as the rest of the
    // sidebar by construction -- in any theme. Without this the panel kept wx's default grey and read as
    // a lighter block. The build site also runs UpdateDarkUI on this panel (as the sibling panels get).
    if (parent)
        SetBackgroundColour(parent->GetBackgroundColour());

    auto* root = new wxBoxSizer(wxVERTICAL);

    // Title bar matching the Filament section's `m_panel_filament_title` (Plater.cpp): a gradient
    // StaticBox holding a ScalableButton icon + a Label, bracketed top and bottom by thin StaticLine
    // separators. Icon = "single_nozzle_n" (the nozzle glyph) where filament uses "filament". Clicking
    // it toggles m_content, same as Filament's title toggles m_panel_filament_content.
    m_title = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    m_title->SetBackgroundColor(wxColour(248, 248, 248));
    m_title->SetBackgroundColor2(0xF1F1F1);

    auto* title_sizer = new wxBoxSizer(wxHORIZONTAL);
    // LB_PROPAGATE_MOUSE_EVENT: without it, a click on the label's own window never reaches m_title's
    // EVT_LEFT_UP handler below (each child window gets its own mouse events; only clicks landing on
    // m_title's bare background do), so only that background sliver -- not the visible "Nozzles" text
    // -- would toggle the section. Matches how Filament's title label is set up (Plater.cpp).
    auto* icon  = new ScalableButton(m_title, wxID_ANY, "single_nozzle_n");
    auto* label = new Label(m_title, _L("Nozzles"), LB_PROPAGATE_MOUSE_EVENT);
    title_sizer->Add(icon,  0, wxALIGN_CENTER | wxLEFT, FromDIP(kTitlebarMargin));
    title_sizer->Add(label, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(kElementSpacing));
    title_sizer->AddStretchSpacer(1);

    // Checkmark: applies the pending diameters, like Apply used to. Hidden while the dropdowns match
    // m_original (nothing to apply) and revealed the moment a selection changes (update_apply_visibility).
    // "check_on" (green rounded square + white tick) is the same asset the CheckBox widget uses for its
    // checked state -- reused here as a plain icon button, not an actual checkbox.
    m_apply_icon = new ScalableButton(m_title, wxID_ANY, "check_on");
    m_apply_icon->SetToolTip(_L("Apply nozzle diameters"));
    m_apply_icon->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_apply_clicked(); });
    m_apply_icon->Hide();
    title_sizer->Add(m_apply_icon, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(kTitlebarMargin));
    title_sizer->SetMinSize(-1, FromDIP(30));
    m_title->SetSizer(title_sizer);

    // Collapse/expand on title click, excluding the checkmark's area -- same fix Filament's titlebar
    // needed ("to fix undesired collapse when user spams del filament button"): a child button's click
    // still reaches this handler unless explicitly excluded by position.
    m_title->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
        if (m_apply_icon->IsShown() && e.GetPosition().x > m_apply_icon->GetPosition().x - FromDIP(10))
            return;
        toggle_content();
    });
    // The nozzle glyph is a native button (ScalableButton : wxButton): on GTK a real button consumes
    // its raw mouse-down/up internally and only ever emits wxEVT_BUTTON, so it can't use the
    // LB_PROPAGATE_MOUSE_EVENT trick above (there is no raw event to propagate) -- wire its own click
    // to the same toggle instead.
    icon->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { toggle_content(); });

    auto* sep_top = new StaticLine(this);
    sep_top->SetLineColour("#A6A9AA");
    root->Add(sep_top, 0, wxEXPAND);
    root->Add(m_title, 0, wxEXPAND);
    auto* sep_bottom = new StaticLine(this);
    sep_bottom->SetLineColour("#A6A9AA");
    root->Add(sep_bottom, 0, wxEXPAND);

    // Rows sit flush on m_content -- no inner frame -- like the Filament section. The grid sizer holds
    // NOZZLE_COLUMNS vertical column sizers side by side, built in rebuild(). m_content is a separate
    // window (not `this`) so the title-bar click can hide/show the rows without touching the title bar.
    m_content = new wxPanel(this, wxID_ANY);
    m_content->SetBackgroundColour(GetBackgroundColour());
    auto* content_sizer = new wxBoxSizer(wxVERTICAL);
    m_grid_sizer = new wxBoxSizer(wxHORIZONTAL);
    content_sizer->Add(m_grid_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(kElementSpacing));
    m_content->SetSizer(content_sizer);
    root->Add(m_content, 0, wxEXPAND);

    SetSizer(root);
}

void NozzlePickerPanel::rebuild(const std::vector<double>& current, const std::vector<std::string>& allowed)
{
    // Tell a caller further up the stack (on_apply_clicked()) that a rebuild happened during its call
    // to m_on_apply(), so it doesn't also stomp on the fresh m_original/checkmark state set below with
    // a stale pre-call snapshot.
    m_expect_rebuild = false;

    // Destroy the previous columns/rows and recreate to the new N. Clearing the sizer with
    // delete_windows=true destroys the child combos/labels, so stale pointers are not reused.
    m_selectors.clear();
    m_grid_sizer->Clear(true);

    // Keep only single-diameter entries from the variant list (see is_single_diameter).
    m_allowed.clear();
    for (const std::string& d : allowed)
        if (is_single_diameter(d))
            m_allowed.push_back(d);

    // NOZZLE_COLUMNS vertical column sizers side by side, each an equal share (proportion 1).
    std::vector<wxBoxSizer*> columns;
    for (int c = 0; c < NOZZLE_COLUMNS; ++c) {
        auto* col = new wxBoxSizer(wxVERTICAL);
        m_grid_sizer->Add(col, 1, wxEXPAND);
        columns.push_back(col);
    }

    std::vector<wxStaticText*> labels; // unified to a common width below, once all N are known
    labels.reserve(current.size());

    for (size_t i = 0; i < current.size(); ++i) {
        // One compact inline row, same shape as a filament row (label where filament puts its colour
        // badge, then a stretchy combo): "Nozzle N"  [ 0.4mm v ]. No stacking, no dividers -- that is
        // what kept the panel as short as the Filament section below it.
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* label = new wxStaticText(m_content, wxID_ANY,
            wxString::Format(_L("Nozzle %d"), static_cast<int>(i + 1)));
        labels.push_back(label);
        row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(kElementSpacing));

        // Orca's read-only ComboBox hides its inner text control, so it needs a real size; the filament
        // combo pins its height to 30 * em / 10. wxEXPAND makes it fill the row's full remaining width
        // (proportion 1 alone only reserves that width -- without wxEXPAND the combo sits at its own
        // natural size within it, so its width tracked its current text ("0.4mm" vs "0.35mm" etc)
        // instead of lining up into an even grid).
        auto* combo = new ComboBox(m_content, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   wxDefaultSize, 0, nullptr, wxCB_READONLY);
        for (const std::string& d : m_allowed)
            combo->Append(format_diameter(parse_diameter(wxString(d))));

        // Initialise to the current per-nozzle diameter (independent; no sync-all).
        combo->SetValue(format_diameter(current[i]));
        // Any selection change may make this dropdown differ from m_original -- reveal/hide the
        // title-bar checkmark accordingly instead of always showing an Apply button.
        combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { update_apply_visibility(); });

        m_selectors.push_back(combo);
        row->Add(combo, 1, wxALIGN_CENTER_VERTICAL | wxALL | wxEXPAND, FromDIP(2))
            ->SetMinSize(wxSize(-1, 30 * wxGetApp().em_unit() / 10));

        // Add the row flush (no border), like the Filament section. The only inter-row gap is the
        // combo's own FromDIP(2) top/bottom, so the rows pack as tightly as the filament list.
        columns[i % NOZZLE_COLUMNS]->Add(row, 0, wxEXPAND);
    }

    // Unify every "Nozzle N" label to the widest one's width (double-digit N, or a longer translation,
    // would otherwise nudge just that row's combo out of line with the rest) so the combo column starts
    // at the same x in every row, in both grid columns.
    int max_label_w = 0;
    for (wxStaticText* l : labels)
        max_label_w = std::max(max_label_w, l->GetBestSize().x);
    for (wxStaticText* l : labels)
        l->SetMinSize(wxSize(max_label_w, -1));

    // Round-trip through format/parse so the dirty comparison in update_apply_visibility() (which
    // compares against parse_diameter(combo->GetValue())) never flags a diff from rounding alone.
    m_original.clear();
    for (double d : current)
        m_original.push_back(parse_diameter(format_diameter(d)));
    m_apply_icon->Hide();

    // Report the new best height to the parent sizer. rebuild() can change the row count (e.g. a
    // 4-nozzle U1 vs an 8-nozzle machine = 2 vs 4 rows), and Layout() alone only arranges children
    // inside the existing panel rect -- it does not tell the parent sizer the panel now needs a
    // different height, so without this the panel keeps its stale size and the extra rows are clipped.
    GetSizer()->Fit(this);
    SetMinSize(wxSize(-1, GetSizer()->GetMinSize().y));
    Layout();
}

std::vector<double> NozzlePickerPanel::pending_diameters() const
{
    std::vector<double> out;
    out.reserve(m_selectors.size());
    for (const ComboBox* combo : m_selectors)
        out.push_back(parse_diameter(combo->GetValue()));
    return out;
}

void NozzlePickerPanel::on_apply_clicked()
{
    if (m_selectors.empty() || !m_on_apply)
        return;
    std::vector<double> pending = pending_diameters();
    // m_on_apply (Sidebar::apply_per_extruder_nozzles) writes the printer config, which typically
    // cascades back into a synchronous, nested rebuild() of this very panel (preset-changed ->
    // Sidebar::update() -> rebuild()) before this call even returns. Detect that so the code below
    // doesn't overwrite the fresh state rebuild() just set with this now-stale `pending` snapshot.
    m_expect_rebuild = true;
    m_on_apply(pending);
    if (m_expect_rebuild) {
        // No nested rebuild() happened (e.g. a no-op guard: the printer was already on the requested
        // profile) -- nothing else is coming to reset the dirty baseline, so do it here.
        m_expect_rebuild = false;
        m_original = pending;
        update_apply_visibility();
    }
}

void NozzlePickerPanel::update_apply_visibility()
{
    bool dirty = pending_diameters() != m_original;
    if (m_apply_icon->IsShown() != dirty) {
        m_apply_icon->Show(dirty);
        m_title->Layout();
    }
}

void NozzlePickerPanel::toggle_content()
{
    m_content->Show(!m_content->IsShown());
    // Recompute the panel's own reserved size for the new (collapsed/expanded) content height --
    // mirrors rebuild()'s trailer. Without this, Layout() below only repositions children inside the
    // panel's existing (still-expanded) rect; the forced SetMinSize from the last rebuild()/toggle
    // keeps pinning the panel to its old height, leaving dead space when collapsing.
    GetSizer()->Fit(this);
    SetMinSize(wxSize(-1, GetSizer()->GetMinSize().y));
    for (wxWindow* w = this; w; w = w->GetParent())
        w->Layout();
}

}} // namespace Slic3r::GUI
