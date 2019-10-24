#pragma once

#include <i3ipc++/ipc.hpp>

#include "components/config.hpp"
#include "modules/meta/event_module.hpp"
#include "utils/i3.hpp"
#include "utils/io.hpp"

POLYBAR_NS

namespace modules {
  class i3_module : public event_module<i3_module> {
   public:
    enum class state {
      NONE,
      /**
       * \brief Active workspace on focused monitor
       */
      FOCUSED,
      /**
       * \brief Inactive workspace on any monitor
       */
      UNFOCUSED,
      /**
       * \brief Active workspace on unfocused monitor
       */
      VISIBLE,
      /**
       * \brief Workspace with urgency hint set
       */
      URGENT,
      /**
       * \brief Unfocused workspace in an inactive group
       */
      INACTIVE_GROUP,
      /**
       * \brief Dummy workspace to show ellipsis when the workspaces don't fit
       */
      DUMMY_ELLIPSIS,
    };

    struct workspace {
      explicit workspace(string name, enum state state_, label_t&& label)
          : name(name), state(state_), label(forward<label_t>(label)) {}

      operator bool();

      string name;
      enum state state;
      label_t label;
    };


   public:
    explicit i3_module(const bar_settings&, string);

    void stop() override;
    bool has_event();
    bool update();
    bool build(builder* builder, const string& tag) const;

    static constexpr auto TYPE = "internal/i3";

    static constexpr auto EVENT_FOCUS = "focus";
    static constexpr auto EVENT_NEXT = "next";
    static constexpr auto EVENT_PREV = "prev";

   protected:
    void action_focus(const string& ws);
    void action_next();
    void action_prev();

    void focus_direction(bool next);

   private:
    static string make_workspace_command(const string& workspace);

    static constexpr const char* DEFAULT_TAGS{"<label-state> <label-mode>"};
    static constexpr const char* DEFAULT_MODE{"default"};
    static constexpr const char* DEFAULT_WS_ICON{"ws-icon-default"};
    static constexpr const char* DEFAULT_WS_LABEL{"%icon% %name%"};

    static constexpr const char* TAG_LABEL_STATE{"<label-state>"};
    static constexpr const char* TAG_LABEL_MODE{"<label-mode>"};

    map<state, label_t> m_statelabels;
    vector<unique_ptr<workspace>> m_workspaces;
    iconset_t m_icons;

    label_t m_modelabel;
    bool m_modeactive{false};

    /**
     * Separator that is inserted in between workspaces
     */
    label_t m_labelseparator;

    bool m_click{true};
    bool m_scroll{true};
    bool m_revscroll{true};
    bool m_wrap{true};
    bool m_indexsort{false};
    bool m_pinworkspaces{false};
    bool m_strip_wsnumbers{false};
    bool m_fuzzy_match{false};
    int m_workspaces_max_count{-1};
    int m_workspaces_max_width{-1};

    unique_ptr<i3_util::connection_t> m_ipc;

    static constexpr const int MISSING_NUMBER = -1;

    struct workspace_name_sections {
      int global_number = MISSING_NUMBER;
      string group = "";
      string static_name = "";
      string dynamic_name = "";
      int local_number = MISSING_NUMBER;
    };

    workspace_name_sections parse_workspace_name(const string& workspace_name);
    string create_display_name(const workspace_name_sections& name_sections);
    vector<unique_ptr<workspace>> get_workspaces();
    size_t get_num_fitting_workspaces(const vector<unique_ptr<workspace>>& workspaces);
  };
}  // namespace modules

POLYBAR_NS_END
