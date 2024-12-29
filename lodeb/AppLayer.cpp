#include "AppLayer.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <tinyfiledialogs.h>
#include <lldb/API/LLDB.h>
#include <sstream>
#include <unordered_set>

#include <stdio.h>

#include "ParseCommand.hpp"
#include "Log.hpp"
#include "LLDBUtil.hpp"

namespace {
    using namespace lodeb;

    const char* COMMAND_BAR_POPUP_NAME = "Command Bar";
    const char* STATE_PATH = "lodeb.txt";

    bool ReadEntireFileInto(const char* path, std::string& into) {
        FILE* f = fopen(path, "rb");

        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);

        into.resize(size);

        rewind(f);
        size_t n = fread(into.data(), 1, size, f);

        fclose(f);

        return n == size;
    }
}

namespace lodeb {
    using namespace Scaffold;

    AppLayer::AppLayer() {
        state.Load(STATE_PATH);
    }
    
    void AppLayer::OnUpdate(float) {
        state.Update();
        state.Store(STATE_PATH);
    }

    void AppLayer::OnRenderUI(float) {
        WindowTargetSettings();
        WindowCommandBar();
        WindowSourceView();
        WindowProcessOutput();
        WindowDebug();
        WindowLocals();
        WindowFrames();
    }

    void AppLayer::WindowTargetSettings() {
        ImGui::Begin("Target Settings");

        ImGui::InputText("Exe Path", &state.target_settings.exe_path);
        ImGui::SameLine();

        if(ImGui::Button("Browse##exe_path")) {
            const char* path = tinyfd_openFileDialog(
                "Path to Executable", 
                nullptr, 
                0, 
                nullptr, 
                nullptr, 
                0
            );

            if(path) {
                state.target_settings.exe_path = path;
            }
        }

        ImGui::InputText("Working Dir", &state.target_settings.working_dir);
        ImGui::SameLine();

        if(ImGui::Button("Browse##working_dir")) {
            const char* path = tinyfd_selectFolderDialog("Working Directory", nullptr);

            if(path) {
                state.target_settings.working_dir = path;
            }
        }

        if(ImGui::Button("Load Target")) {
            state.events.push_back(LoadTargetEvent{});
        }

        ImGui::End();
    }

    void AppLayer::WindowCommandBar() {
        auto handle_parsed_command = [&](ParsedCommand& parsed) {
            auto& target_state = state.target_state;

            if(!target_state) {
                ImGui::Text("No target loaded");

                return;
            }

            auto* sym_search = std::get_if<LookForSymbolCommand>(&parsed);
            if(!sym_search) {
                return;
            }

            auto& ts = *target_state;

            if(!ts.sym_loc_cache) {
                if(ts.sym_loc_cache_future.wait_for(std::chrono::seconds::zero()) == std::future_status::ready) {
                    // This can _only be called once hence us wrapping this in the !ts.sym_loc_cache if
                    ts.sym_loc_cache = ts.sym_loc_cache_future.get();
                }

                ImGui::Text("Loading symbols...");
                return;
            }

            // TODO(Apaar): Handle file search
            ImGui::BeginChild("##symbols", {400, 300}, ImGuiChildFlags_NavFlattened);

            // ImGui doesn't handle std::string_view so we put match names in here
            std::string name_buf;

            size_t i = 0;

            ts.sym_loc_cache->ForEachMatch(sym_search->text, [&](const auto& match) {
                ImGui::PushID(i);

                name_buf = match.name;

                if(ImGui::Selectable(name_buf.c_str())) {
                    ViewSourceEvent event{*match.loc};

                    LogInfo("Pushing ViewSourceEvent {}", *match.loc);

                    state.events.push_back(std::move(event));

                    ImGui::CloseCurrentPopup();
                }

                ImGui::PopID();
                i += 1;
            });

            ImGui::EndChild();
        };

        auto& input = Application::GetInput();

        if(input.GetKeyState(KeyCode::P) == KeyState::Pressed &&
           (input.IsKeyDown(KeyCode::LeftControl) || 
            input.IsKeyDown(KeyCode::RightControl) ||
            input.IsKeyDown(KeyCode::LeftSuper) ||
            input.IsKeyDown(KeyCode::RightSuper))) {
            ImGui::OpenPopup(COMMAND_BAR_POPUP_NAME);
        }

        auto* viewport = ImGui::GetMainViewport();

        ImGui::SetNextWindowPos(
            {viewport->Pos.x + viewport->Size.x * 0.5f,
             viewport->Pos.y + 200},
            ImGuiCond_Appearing,
            {0.5f, 0.5f}
        );

        if(!ImGui::BeginPopup(COMMAND_BAR_POPUP_NAME)) {
            state.cmd_bar_state.reset();
            return;
        }

        if(!state.cmd_bar_state) {
            state.cmd_bar_state.emplace();
        }

        if(!state.cmd_bar_state->focused_text) {
            ImGui::SetKeyboardFocusHere();
            state.cmd_bar_state->focused_text = true;
        }

        ImGui::SetNextItemWidth(400.0f);

        ImGui::InputText("##command_bar_text", &state.cmd_bar_state->text);

        auto parsed = ParseCommand(state.cmd_bar_state->text);
        handle_parsed_command(parsed);

        ImGui::EndPopup();
    }

    void AppLayer::WindowSourceView() {
        auto& source_view_state = state.source_view_state;

        if(!source_view_state) {
            return;
        }

        auto cur_frame_loc = state.GetCurFrameLoc();

        if(!source_view_state->path.empty() &&
            source_view_state->text.empty()) {
            ReadEntireFileInto(source_view_state->path.c_str(), source_view_state->text);            
            LogInfo("Loaded file {}", source_view_state->path);
        }

        ImGui::Begin("Source View");

        ImGui::TextUnformatted(source_view_state->path.c_str());

        ImGui::BeginChild("##text", {-1, -1}, ImGuiChildFlags_Border, ImGuiWindowFlags_NoNav);;

        std::string line_buf;

        std::istringstream ss{source_view_state->text};

        FileLoc loc = {
            .path = source_view_state->path,
            .line = 0,
        };

        for(std::string line; (loc.line += 1), std::getline(ss, line);) {
            ImGui::PushID(loc.line);

            line_buf.clear();
            std::format_to(std::back_inserter(line_buf), "{:5} {}", loc.line, line);

            if(ImGui::InvisibleButton("##gutter", {20, 20})) {
                state.events.push_back(ToggleBreakpointEvent{loc});
            }

            ImGui::SameLine();

            bool has_bp = state.target_state && state.target_state->loc_to_breakpoint.contains(loc);

            if(has_bp) {
                auto* draw_list = ImGui::GetWindowDrawList();
                auto pos = ImGui::GetItemRectMin();
                draw_list->AddCircleFilled(
                    {pos.x + 10, pos.y + 10},
                    5,
                    ImGui::GetColorU32(ImVec4{1.0, 0.0, 0.0, 1.0})
                );

                ImGui::SameLine();
            }

            bool highlight = cur_frame_loc == loc;

            if(highlight) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImVec4{0.25, 0.5, 1.0, 1.0}));
            }


            ImGui::TextUnformatted(line_buf.c_str());

            if(highlight) {
                ImGui::PopStyleColor();
            }

            if(source_view_state->scroll_to_line == loc.line) {
                ImGui::SetScrollHereY();
            }

            ImGui::PopID();
        }

        source_view_state->scroll_to_line.reset();

        ImGui::EndChild();

        ImGui::End();
    }

    void AppLayer::WindowProcessOutput() {
        ImGui::Begin("Process Output");

        ImGui::BeginChild("##text", {-1, -1}, ImGuiChildFlags_Border);

        ImGui::TextUnformatted(state.process_output.c_str());

        ImGui::EndChild();
        ImGui::End();
    }

    void AppLayer::WindowDebug() {
        ImGui::Begin("Debug");

        if(!state.target_state) {
            ImGui::Text("No target loaded");

            ImGui::End();
            return;
        }

        if(!state.target_state->process_state) {
            if(ImGui::Button("Start")) {
                state.events.push_back(StartProcessEvent{});
            }
            
            ImGui::End();
            return;
        }

        auto& ps = state.target_state->process_state;

        if(ps->process.GetState() != lldb::eStateStopped) {
            ImGui::End();
            return;
        }

        if(ImGui::Button("Kill")) {
            state.events.push_back(ChangeDebugStateEvent{ChangeDebugStateEvent::Kill});
        }

        if(ImGui::Button("Step In")) {
            state.events.push_back(ChangeDebugStateEvent{ChangeDebugStateEvent::StepIn});
        }

        ImGui::SameLine();

        if(ImGui::Button("Step Over")) {
            state.events.push_back(ChangeDebugStateEvent{ChangeDebugStateEvent::StepOver});
        }

        ImGui::SameLine();

        if(ImGui::Button("Continue")) {
            state.events.push_back(ChangeDebugStateEvent{ChangeDebugStateEvent::Continue});
        }

        ImGui::End();
    }

    void AppLayer::WindowLocals() {
        ImGui::Begin("Locals");

        if(!state.target_state) {
            ImGui::Text("No target loaded");

            ImGui::End();
            return;
        }

        if(!state.target_state->process_state) {
            ImGui::Text("Process is not running");
            
            ImGui::End();
            return;
        }

        auto& ps = *state.target_state->process_state;

        if(ps.process.GetState() != lldb::eStateStopped) {
            ImGui::Text("Running...");
            
            ImGui::End();
            return; 
        }

        auto frame = ps.process.GetSelectedThread().GetSelectedFrame();

        lldb::SBVariablesOptions opts;
        
        opts.SetIncludeLocals(true);
        opts.SetIncludeArguments(true);
        opts.SetInScopeOnly(true);

        // HACK(Apaar): Caching this
        static uint32_t last_frame_id = (uint32_t)-1;
        static lldb::addr_t last_frame_pc = (lldb::addr_t)-1;

        static std::vector<std::string> var_names;
        static std::unordered_map<std::string, lldb::SBValue> name_to_value;
        static std::unordered_map<std::string, std::string> name_to_desc;

        if(frame.GetFrameID() != last_frame_id ||
           frame.GetPC() != last_frame_pc) {
            auto vars = frame.GetVariables(opts); 

            last_frame_id = frame.GetFrameID();
            last_frame_pc = frame.GetPC();

            LogDebug("Getting variables again");

            var_names.clear();
            name_to_value.clear();
            name_to_desc.clear();

            for(auto i = 0u; i < vars.GetSize(); ++i) {
                auto var = vars.GetValueAtIndex(i);
                auto addr = var.GetLoadAddress();

                if(addr == (lldb::addr_t)-1) {
                    continue;
                }

                auto name = std::format("{} at {:#010x}", var.GetName(), var.GetLoadAddress());

                var_names.emplace_back(name);
                name_to_value[name] = var;
            }
        }

        static lldb::SBStream stream;

        auto get_name_desc = [&](const std::string& var_name) -> const char* {
            auto found_desc = name_to_desc.find(var_name);

            if(found_desc != name_to_desc.end()) {
                return found_desc->second.c_str();
            }

            auto found_value = name_to_value.find(var_name);
            
            if(found_value == name_to_value.end()) {
                return nullptr;
            }

            stream.Clear();
            found_value->second.GetDescription(stream);

            std::string& s = name_to_desc[var_name];
            s = stream.GetData();

            return s.c_str();
        };
        

        ImGui::BeginChild("##vars", {-1, -1}, ImGuiChildFlags_Border);

        for(const auto& var_name : var_names) {
            if(ImGui::TreeNode(var_name.c_str())) {
                auto value = get_name_desc(var_name);

                if(value) {
                    ImGui::TextUnformatted(value);
                }

                ImGui::TreePop();
            }
        }

        ImGui::EndChild();

        ImGui::End();
    }

    void AppLayer::WindowFrames() {
        ImGui::Begin("Stack Frames");

        if(!state.target_state) {
            ImGui::Text("No target loaded");

            ImGui::End();
            return;
        }

        if(!state.target_state->process_state) {
            ImGui::Text("Process is not running");
            
            ImGui::End();
            return;
        }

        auto& ps = *state.target_state->process_state;

        auto thread = ps.process.GetSelectedThread();
        auto selected_frame = thread.GetSelectedFrame();

        ImGui::BeginChild("##frames");

        lldb::SBStream stream;

        for(auto i = 0u; i < thread.GetNumFrames(); ++i) {
            auto frame = thread.GetFrameAtIndex(i);

            stream.Clear();
            frame.GetDescription(stream);

            if(ImGui::Selectable(stream.GetData())) {
                state.events.push_back(SetSelectedFrameEvent{i});
            }
        }

        ImGui::EndChild();
        ImGui::End();
    }
}
