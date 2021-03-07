#include "modules/i3.hpp"

#include <sys/socket.h>

#include "absl/strings/numbers.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "drawtypes/iconset.hpp"
#include "drawtypes/label.hpp"
#include "modules/meta/base.inl"
#include "utils/factory.hpp"
#include "utils/file.hpp"

POLYBAR_NS

namespace modules {
  template class module<i3_module>;

  i3_module::i3_module(const bar_settings& bar, string name_) : event_module<i3_module>(bar, move(name_)) {
    m_router->register_action_with_data(EVENT_FOCUS, &i3_module::action_focus);
    m_router->register_action(EVENT_NEXT, &i3_module::action_next);
    m_router->register_action(EVENT_PREV, &i3_module::action_prev);

    auto socket_path = i3ipc::get_socketpath();

    if (!file_util::exists(socket_path)) {
      throw module_error("Could not find socket: " + (socket_path.empty() ? "<empty>" : socket_path));
    }

    m_ipc = factory_util::unique<i3ipc::connection>();

    // Load configuration values
    m_click = m_conf.get(name(), "enable-click", m_click);
    m_scroll = m_conf.get(name(), "enable-scroll", m_scroll);
    m_revscroll = m_conf.get(name(), "reverse-scroll", m_revscroll);
    m_wrap = m_conf.get(name(), "wrapping-scroll", m_wrap);
    m_indexsort = m_conf.get(name(), "index-sort", m_indexsort);
    m_pinworkspaces = m_conf.get(name(), "pin-workspaces", m_pinworkspaces);
    m_show_urgent = m_conf.get(name(), "show-urgent", m_show_urgent);
    m_strip_wsnumbers = m_conf.get(name(), "strip-wsnumbers", m_strip_wsnumbers);
    m_fuzzy_match = m_conf.get(name(), "fuzzy-match", m_fuzzy_match);
    m_workspaces_max_count = m_conf.get(name(), "workspaces-max-count", m_workspaces_max_count);
    m_workspaces_max_width = m_conf.get(name(), "workspaces-max-width", m_workspaces_max_width);

    m_conf.warn_deprecated(name(), "wsname-maxlen", "%name:min:max%");

    // Add formats and create components
    m_formatter->add(DEFAULT_FORMAT, DEFAULT_TAGS, {TAG_LABEL_STATE, TAG_LABEL_MODE});

    if (m_formatter->has(TAG_LABEL_STATE)) {
      m_statelabels.insert(
          make_pair(state::FOCUSED, load_optional_label(m_conf, name(), "label-focused", DEFAULT_WS_LABEL)));
      m_statelabels.insert(
          make_pair(state::UNFOCUSED, load_optional_label(m_conf, name(), "label-unfocused", DEFAULT_WS_LABEL)));
      m_statelabels.insert(
          make_pair(state::VISIBLE, load_optional_label(m_conf, name(), "label-visible", DEFAULT_WS_LABEL)));
      m_statelabels.insert(
          make_pair(state::URGENT, load_optional_label(m_conf, name(), "label-urgent", DEFAULT_WS_LABEL)));
      m_statelabels.insert(
          make_pair(state::INACTIVE_GROUP, load_optional_label(m_conf, name(), "label-inactive-group", DEFAULT_WS_LABEL)));
      m_statelabels.insert(
          make_pair(state::DUMMY_ELLIPSIS, load_optional_label(m_conf, name(), "label-ellipsis", DEFAULT_WS_LABEL)));
    }

    if (m_formatter->has(TAG_LABEL_MODE)) {
      m_modelabel = load_optional_label(m_conf, name(), "label-mode", "%mode%");
    }

    m_labelseparator = load_optional_label(m_conf, name(), "label-separator", "");

    m_icons = factory_util::shared<iconset>();
    m_icons->add(DEFAULT_WS_ICON, factory_util::shared<label>(m_conf.get(name(), DEFAULT_WS_ICON, ""s)));

    for (const auto& workspace : m_conf.get_list<string>(name(), "ws-icon", {})) {
      auto vec = string_util::tokenize(workspace, ';');
      if (vec.size() == 2) {
        m_icons->add(vec[0], factory_util::shared<label>(vec[1]));
      }
    }

    try {
      if (m_modelabel) {
        m_ipc->on_mode_event = [this](const i3ipc::mode_t& mode) {
          m_modeactive = (mode.change != DEFAULT_MODE);
          if (m_modeactive) {
            m_modelabel->reset_tokens();
            m_modelabel->replace_token("%mode%", mode.change);
          }
        };
      }
      m_ipc->subscribe(i3ipc::ET_WORKSPACE | i3ipc::ET_MODE);
    } catch (const exception& err) {
      throw module_error(err.what());
    }
  }

  i3_module::workspace::operator bool() {
    return label && *label;
  }

  void i3_module::stop() {
    try {
      if (m_ipc) {
        m_log.info("%s: Disconnecting from socket", name());
        shutdown(m_ipc->get_event_socket_fd(), SHUT_RDWR);
        shutdown(m_ipc->get_main_socket_fd(), SHUT_RDWR);
      }
    } catch (...) {
    }

    event_module::stop();
  }

  bool i3_module::has_event() {
    try {
      m_ipc->handle_event();
      return true;
    } catch (const exception& err) {
      try {
        m_log.warn("%s: Attempting to reconnect socket (reason: %s)", name(), err.what());
        m_ipc->connect_event_socket(true);
        m_log.info("%s: Reconnecting socket succeeded", name());
      } catch (const exception& err) {
        m_log.err("%s: Failed to reconnect socket (reason: %s)", name(), err.what());
      }
      return false;
    }
  }

  bool i3_module::update() {
    /*
     * update only populates m_workspaces and those are only needed when
     * <label-state> appears in the format
     */
    if (!m_formatter->has(TAG_LABEL_STATE)) {
      return true;
    }
    m_workspaces.clear();
    try {
      vector<unique_ptr<workspace>> all_workspaces = get_workspaces();

      auto max_workspaces_shown = all_workspaces.size();
      if (m_workspaces_max_count >= 0) {
        max_workspaces_shown = std::min<size_t>(max_workspaces_shown, m_workspaces_max_count);
      }
      // The simple case: if everything fits, no need for ellipsis and other
      // complications.
      if (max_workspaces_shown >= all_workspaces.size() && get_num_fitting_workspaces(all_workspaces) == all_workspaces.size()) {
        m_workspaces = std::move(all_workspaces);
        return true;
      }
      // If we're here, it means not all the workspaces should be displayed, and
      // we use an ellipsis to communicate that this is the case. This is
      // implemented using a dummy workspace object with its name set to "...".
      auto ellipsis_ws = create_ellipsis_workspace();
      // We now compute two lists: the workspaces displayed before and after the
      // ellipsis. The latter can contain only zero or one elements, depending
      // on whether the focused workspace fits in the list of workspaces before
      // the ellipsis. Below are examples for the workspaces displayed, with
      // stars wrapping the focused workspace:
      // ws1 |  ws2  | ... | *ws8* | ...
      // ws1 |  ws2  | ... | *ws8*
      // ws1 | *ws2* | ...
      // ...
      using ws_iterator = typename decltype(all_workspaces)::iterator;
      vector<ws_iterator> before_first_ellipsis;
      vector<ws_iterator> after_first_ellipsis;
      size_t displayed_ws_count = 0;
      int displayed_ws_width = 0;
      // Checks if the given workspace fits, and if so, returns its width.
      // Otherwise, returns 0.
      auto count_ws_if_fits = [&](const workspace& ws) -> bool {
        if (displayed_ws_count + 1 > max_workspaces_shown) {
          return false;
        }
        int label_width = string_util::char_len(ws.label->get());
        if (m_workspaces_max_width >= 0 && displayed_ws_width + label_width > m_workspaces_max_width) {
          return false;
        }
        displayed_ws_count += 1;
        displayed_ws_width += label_width;
        return true;
      };
      // Allocate space for two ellipsis workspaces from our "width budget". The
      // second one won't necessarily be needed, but we want to make sure we
      // never pass use more than m_workspaces_max_width chars.
      displayed_ws_width += 2*string_util::char_len(ellipsis_ws->label->get());
      const auto focused_ws_iter = std::find_if(
          all_workspaces.begin(), all_workspaces.end(), [](auto& ws) { return ws->state == state::FOCUSED; });
      if (focused_ws_iter != all_workspaces.end() && count_ws_if_fits(**focused_ws_iter)) {
        for (auto i = all_workspaces.begin(); i != all_workspaces.end(); ++i) {
          if (i != focused_ws_iter && !count_ws_if_fits(**i)) {
            break;
          }
          before_first_ellipsis.push_back(i);
        }
        if (std::find(before_first_ellipsis.begin(), before_first_ellipsis.end(), focused_ws_iter) ==
            before_first_ellipsis.end()) {
          after_first_ellipsis.push_back(focused_ws_iter);
        }
      }

      for (auto iter : before_first_ellipsis) {
        m_workspaces.emplace_back(std::move(*iter));
      }
      m_workspaces.emplace_back(std::move(ellipsis_ws));
      for (auto iter : after_first_ellipsis) {
        m_workspaces.emplace_back(std::move(*iter));
      }
      // If the last shown workspace is not the last one, we need to add another
      // ellipsis after it.
      if (!after_first_ellipsis.empty() && next(after_first_ellipsis.back()) != all_workspaces.end()) {
        m_workspaces.emplace_back(create_ellipsis_workspace());
      }
      return true;
    } catch (const exception& err) {
      m_log.err("%s: %s", name(), err.what());
      return false;
    }
  }

  bool i3_module::build(builder* builder, const string& tag) const {
    if (tag == TAG_LABEL_MODE && m_modeactive) {
      builder->node(m_modelabel);
    } else if (tag == TAG_LABEL_STATE && !m_workspaces.empty()) {
      if (m_scroll) {
        builder->action(mousebtn::SCROLL_DOWN, *this, m_revscroll ? EVENT_NEXT : EVENT_PREV, "");
        builder->action(mousebtn::SCROLL_UP, *this, m_revscroll ? EVENT_PREV : EVENT_NEXT, "");
      }

      bool first = true;
      for (auto&& ws : m_workspaces) {
        /*
         * The separator should only be inserted in between the workspaces, so
         * we insert it in front of all workspaces except the first one.
         */
        if (first) {
          first = false;
        } else if (*m_labelseparator) {
          builder->node(m_labelseparator);
        }

        if (m_click) {
          builder->action(mousebtn::LEFT, *this, EVENT_FOCUS, ws->name, ws->label);
        } else {
          builder->node(ws->label);
        }
      }

      if (m_scroll) {
        builder->action_close();
        builder->action_close();
      }
    } else {
      return false;
    }

    return true;
  }

  void i3_module::action_focus(const string& ws) {
    const i3_util::connection_t conn{};
    m_log.info("%s: Sending workspace focus command to ipc handler", name());
    conn.send_command(make_workspace_command(ws));
  }

  void i3_module::action_next() {
    focus_direction(true);
  }

  void i3_module::action_prev() {
    focus_direction(false);
  }

  void i3_module::focus_direction(bool next) {
    const i3_util::connection_t conn{};

    auto workspaces = i3_util::workspaces(ipc, m_bar.monitor->name);
    auto current_ws = std::find_if(workspaces.begin(), workspaces.end(), [](auto ws) { return ws->visible; });

    if (current_ws == workspaces.end()) {
      m_log.warn("%s: Current workspace not found", name());
      return;
    }

    if (next && (m_wrap || std::next(current_ws) != workspaces.end())) {
      if (!(*current_ws)->focused) {
        m_log.info("%s: Sending workspace focus command to ipc handler", name());
        conn.send_command(make_workspace_command((*current_ws)->name));
      }
      m_log.info("%s: Sending workspace next_on_output command to ipc handler", name());
      conn.send_command("workspace next_on_output");
    } else if (!next && (m_wrap || current_ws != workspaces.begin())) {
      if (!(*current_ws)->focused) {
        m_log.info("%s: Sending workspace focus command to ipc handler", name());
        conn.send_command(make_workspace_command((*current_ws)->name));
      }
      m_log.info("%s: Sending workspace prev_on_output command to ipc handler", name());
      conn.send_command("workspace prev_on_output");
    }
  }

  string i3_module::make_workspace_command(const string& workspace) {
    return "workspace \"" + workspace + "\"";
  }

  constexpr const auto g_SECTIONS_DELIM = u8"\u200b";

  i3_module::workspace_name_sections i3_module::parse_workspace_name(const string& workspace_name) {
    workspace_name_sections result;
    const vector<string> sections = absl::StrSplit(workspace_name, g_SECTIONS_DELIM);
    if (sections.size() != 5) {
      result.static_name = workspace_name;
      return result;
    }
    (void)absl::SimpleAtoi(absl::StripSuffix(sections[0], ":"), &result.global_number);
    result.group = std::string(absl::StripSuffix(sections[1], ":"));
    result.static_name = std::string(absl::StripPrefix(sections[2], ":"));
    result.dynamic_name = std::string(absl::StripPrefix(sections[3], ":"));
    (void)absl::SimpleAtoi(absl::StripPrefix(sections[4], ":"), &result.local_number);
    m_log.trace(
        "%s: Workspace name sections parsed: global_number=%d, group=%s, static_name=%s, dynamic_name=%s, "
        "local_number=%d",
        name(), result.global_number, result.group, result.static_name, result.dynamic_name, result.local_number);
    return result;
  }

  string i3_module::create_display_name(const i3_module::workspace_name_sections& name_sections) {
    vector<string> components;
    if (!name_sections.group.empty()) {
      components.push_back(name_sections.group);
    }
    if (!name_sections.static_name.empty()) {
      components.push_back(name_sections.static_name);
    }
    if (!name_sections.dynamic_name.empty()) {
      components.push_back(name_sections.dynamic_name);
    }
    if (name_sections.local_number != MISSING_NUMBER) {
      components.push_back(to_string(name_sections.local_number));
    }
    return absl::StrJoin(components, ":");
  }

  vector<unique_ptr<i3_module::workspace>> i3_module::get_workspaces() {
    vector<shared_ptr<i3_util::workspace_t>> i3_workspaces;
    i3_util::connection_t ipc;

    if (m_pinworkspaces) {
      i3_workspaces = i3_util::workspaces(ipc, m_bar.monitor->name, m_show_urgent);
    } else {
      i3_workspaces = i3_util::workspaces(ipc);
    }

    if (m_indexsort) {
      sort(i3_workspaces.begin(), i3_workspaces.end(), i3_util::ws_numsort);
    }

    const string first_workspace_name = (*i3_workspaces.begin())->name;
    const string active_group = parse_workspace_name(first_workspace_name).group;

    vector<unique_ptr<workspace>> workspaces;
    for (size_t i = 0; i < i3_workspaces.size(); ++i) {
      const auto& ws = i3_workspaces[i];
      const auto name_sections = parse_workspace_name(ws->name);
      if (ws->num != name_sections.global_number) {
        m_log.warn("Mismatched workspace global number: %d vs %d", ws->num, name_sections.global_number);
      }

      state ws_state{state::NONE};

      if (ws->focused) {
        ws_state = state::FOCUSED;
      } else if (ws->urgent) {
        ws_state = state::URGENT;
      } else if (name_sections.group != active_group) {
        ws_state = state::INACTIVE_GROUP;
      } else if (ws->visible) {
        ws_state = state::VISIBLE;
      } else {
        ws_state = state::UNFOCUSED;
      }

      string ws_name{ws->name};

      // Remove workspace numbers "0:"
      if (m_strip_wsnumbers) {
        ws_name.erase(0, string_util::find_nth(ws_name, 0, ":", 1) + 1);
      }
      // Trim leading and trailing whitespace
      ws_name = string_util::trim(move(ws_name), ' ');

      auto icon = m_icons->get(ws->name, DEFAULT_WS_ICON, m_fuzzy_match);
      auto label = m_statelabels.find(ws_state)->second->clone();

      label->reset_tokens();
      label->replace_token("%output%", ws->output);
      label->replace_token("%name%", ws_name);
      label->replace_token("%icon%", icon->get());
      label->replace_token("%index%", to_string(ws->num));
      label->replace_token("%display_name%", create_display_name(name_sections));

      const int label_width = string_util::char_len(label->get());
      if (label_width == 0) {
        continue;
      }

      workspaces.emplace_back(factory_util::unique<workspace>(ws->name, ws_state, move(label)));
    }
    return workspaces;
  }

  size_t i3_module::get_num_fitting_workspaces(const vector<unique_ptr<workspace>>& workspaces) {
    if (m_workspaces_max_width < 0) {
      return workspaces.size();
    }
    int workspaces_label_width_total = 0;
    for (size_t i = 0; i < workspaces.size(); ++i) {
      int ws_width = string_util::char_len(workspaces[i]->label->get());
      if (workspaces_label_width_total + ws_width > m_workspaces_max_width) {
        return i;
      }
      workspaces_label_width_total += ws_width;
    }
    return workspaces.size();
  }

  unique_ptr<i3_module::workspace> i3_module::create_ellipsis_workspace() {
    auto label = m_statelabels.find(state::DUMMY_ELLIPSIS)->second->clone();
    label->reset_tokens();
    label->replace_token("%output%", "");
    label->replace_token("%name%", "...");
    label->replace_token("%icon%", "");
    label->replace_token("%index%", "");
    return factory_util::unique<workspace>("", state::DUMMY_ELLIPSIS, move(label));
  }
}  // namespace modules

POLYBAR_NS_END
