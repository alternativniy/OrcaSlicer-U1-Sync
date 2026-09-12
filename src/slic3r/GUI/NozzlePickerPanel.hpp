#ifndef slic3r_NozzlePickerPanel_hpp_
#define slic3r_NozzlePickerPanel_hpp_

#include <functional>
#include <string>
#include <vector>

#include <wx/panel.h>

class wxBoxSizer;

// ComboBox/StaticBox/ScalableButton are declared at global scope (not in Slic3r::GUI), so
// forward-declare them here, before the namespace -- as Plater.hpp / AMSSetting.hpp do. Declaring
// them inside the namespace would make distinct Slic3r::GUI:: types that shadow the real ones and
// never complete.
class ComboBox;
class StaticBox;
class ScalableButton;

namespace Slic3r { namespace GUI {

// Sidebar panel that shows one nozzle-diameter dropdown per nozzle (dynamic N, e.g. the
// Snapmaker U1 toolchanger's 4 toolheads). Mirrors the Filament section's layout: a gradient title
// bar (nozzle icon + "Nozzles" label, bracketed by StaticLine separators) above a two-column grid of
// flush, compact inline rows -- each row is "Nozzle N" followed by its diameter dropdown, filled
// row-major (1 2 / 3 4 ...). Clicking the title bar collapses/expands the grid, same as Filament.
// Selections are independent and silent: changing a dropdown only updates pending state and reveals a
// checkmark button in the title bar (hidden while the selection matches the applied diameters).
// Clicking it invokes the on_apply callback with the per-nozzle diameters; the owner (Sidebar)
// performs the actual config write, extruder reconcile, and the mixed-nozzle line-width conversion
// offer. This widget owns no print config and no conversion logic.
class NozzlePickerPanel : public wxPanel
{
public:
    NozzlePickerPanel(wxWindow* parent);

    // Rebuild the dropdown grid to N = current.size(). Each combo offers `allowed` diameter strings
    // (e.g. "0.4") and is initialised from `current`. Safe to call repeatedly (printer switch /
    // nozzle-count change); existing combos are destroyed and recreated.
    void rebuild(const std::vector<double>& current, const std::vector<std::string>& allowed);

    // The per-nozzle diameters currently shown in the dropdowns (size == nozzle count).
    std::vector<double> pending_diameters() const;

    // Called when the title-bar checkmark is clicked, with pending_diameters(). The owner applies the change.
    void set_on_apply(std::function<void(const std::vector<double>&)> cb) { m_on_apply = std::move(cb); }

    size_t extruder_count() const { return m_selectors.size(); }

private:
    void on_apply_clicked();
    // Shows/hides the title-bar checkmark depending on whether any dropdown differs from m_original.
    void update_apply_visibility();
    // Shows/hides m_content and re-Fit()s so the panel's reserved height actually shrinks/grows --
    // shared by the title bar, its icon and its label so a click anywhere on the header (not just the
    // bit of bare background between them) collapses/expands.
    void toggle_content();

    StaticBox*               m_title{nullptr};        // title bar; click toggles m_content
    ScalableButton*          m_apply_icon{nullptr};    // title-bar checkmark; shown only while dirty
    wxPanel*                 m_content{nullptr};       // holds m_grid_sizer; hidden/shown by the title-bar click
    wxBoxSizer*              m_grid_sizer{nullptr}; // flush (no frame); holds NOZZLE_COLUMNS column sizers of compact "Nozzle N + combo" rows
    std::vector<ComboBox*>   m_selectors;           // one dropdown per nozzle (independent)
    std::vector<std::string> m_allowed;             // diameter strings offered in every combo
    std::vector<double>      m_original;            // round-tripped diameters as of the last rebuild()/Apply, for dirty tracking
    // Set before invoking m_on_apply and cleared by rebuild(): tells on_apply_clicked() whether the
    // owner's callback actually caused a rebuild (real config change) or was a no-op (e.g. the printer
    // was already on the requested profile). Without this, on_apply_clicked() would unconditionally
    // overwrite m_original/hide the checkmark with a stale snapshot taken before a nested rebuild()
    // already did that correctly with fresh data (or, in the no-op case, never happens at all).
    bool                     m_expect_rebuild{false};
    std::function<void(const std::vector<double>&)> m_on_apply;
};

}} // namespace Slic3r::GUI

#endif // slic3r_NozzlePickerPanel_hpp_
