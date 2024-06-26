/*
	vutrace - Hacky VU tracer/debugger.
	Copyright (C) 2020-2022 chaoticgd

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <map>
#include <array>
#include <string>
#include <vector>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <functional>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "pcsx2defs.h"
#include "pcsx2disassemble.h"
#include "gif.h"
#include "fonts.h"

static const int INSN_PAIR_SIZE = 8;
static int row_size_imgui = 4;
static int row_size = 16;
static int tick_rate = 1;
static bool show_as_hex = false;
static float font_size = 16.0f;
static bool use_default_font = false;
static bool require_font_update = false;
static ImFontConfig default_font_cfg = ImFontConfig();

struct Snapshot
{
	VURegs registers = {};
	u8 memory[VU1_MEMSIZE];
	u8 program[VU1_PROGSIZE];
	u32 read_addr = 0;
	u32 read_size = 0;
	u32 write_addr = 0;
	u32 write_size = 0;
};

struct Instruction
{
	bool is_executed = false;
	std::map<u32, std::size_t> branch_to_times;
	std::map<u32, std::size_t> branch_from_times;
	std::size_t times_executed = 0;
	std::string disassembly;
};

struct AppState
{
	std::size_t current_snapshot = 0;
	std::vector<Snapshot> snapshots;
	bool snapshots_scroll_to = false;
	bool disassembly_scroll_to = false;
	std::vector<Instruction> instructions;
	std::string disassembly_highlight;
	std::string trace_file_path;
	bool comments_loaded = false;
	std::string comment_file_path;
	std::array<std::string, VU1_PROGSIZE / INSN_PAIR_SIZE> comments;
};

struct MessageBoxState
{
	bool is_open = false;
	std::string text;
};

static MessageBoxState export_box;
static MessageBoxState comment_box;
static MessageBoxState save_to_file;
static MessageBoxState find_bytes;
static MessageBoxState go_to_box;

void update_gui(AppState &app);
void snapshots_window(AppState &app);
void registers_window(AppState &app);
void memory_window(AppState &app);
void disassembly_window(AppState &app);
void gs_packet_window(AppState &app);
bool walk_until_pc_equal(AppState &app, u32 target_pc, int step); // Add step to the current snapshot index until pc == target_pc, otherwise do nothing.
void walk_until_mem_access(AppState &app, u32 address); // Add 1 to the current snapshot index until a snapshot reads from/writes to address, otherwise do nothing.
void parse_trace(AppState &app, std::string trace_file_path);
void parse_comment_file(AppState &app, std::string comment_file_path);
void save_comment_file(AppState &app);
std::string disassemble(u8 *program, u32 address);
bool is_xgkick(u32 lower);
void init_gui(GLFWwindow **window);
void update_font();
void main_menu_bar();
void handle_shortcuts();
void begin_docking();
void create_dock_layout(GLFWwindow *window);
void alert(MessageBoxState &state, const char *title);
bool prompt(MessageBoxState &state, const char *title);
std::vector<u8> decode_hex(const std::string &in);
std::string to_hex(size_t n);
size_t from_hex(const std::string& in);

int main(int argc, char **argv)
{
	if(argc != 2 && argc != 3) {
		fprintf(stderr, "usage: %s <trace file> [comment file]\n", argv[0]);
		return 1;
	}
	
	GLFWwindow *window;
	int width, height;
	init_gui(&window);
	
	AppState app;
	parse_trace(app, argv[1]);
	
	if(argc == 3) {
		parse_comment_file(app, argv[2]);
	}
	
	ImGuiContext &g = *GImGui;

	while(!glfwWindowShouldClose(window)) {
		if (require_font_update) {
			update_font();
		}
		
		glfwPollEvents();

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		
		if(g.InputTextState.ID == 0 || g.InputTextState.ID != ImGui::GetActiveID()) {
			if(ImGui::IsKeyPressed(ImGuiKey_W) && app.current_snapshot > 0) {
				app.current_snapshot--;
				app.snapshots_scroll_to = true;
				app.disassembly_scroll_to = true;
			}
			if(ImGui::IsKeyPressed(ImGuiKey_S) && app.current_snapshot < app.snapshots.size() - 1) {
				app.current_snapshot++;
				app.snapshots_scroll_to = true;
				app.disassembly_scroll_to = true;
			}
			
			u32 pc = app.snapshots[app.current_snapshot].registers.VI[TPC].UL;
			if(ImGui::IsKeyPressed(ImGuiKey_A)) {
				walk_until_pc_equal(app, pc, -1);
			}
			if(ImGui::IsKeyPressed(ImGuiKey_D)) {
				walk_until_pc_equal(app, pc, 1);
			}
		}
		
		main_menu_bar();

		begin_docking();
		update_gui(app);
		static bool is_first_frame = true;
		if(is_first_frame) {
			create_dock_layout(window);
			is_first_frame = false;
		}
		ImGui::End(); // docking

		ImGui::Render();
		glfwMakeContextCurrent(window);
		glfwGetFramebufferSize(window, &width, &height);

		glViewport(0, 0, width, height);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		glfwMakeContextCurrent(window);
		glfwSwapBuffers(window);
	}
		
	glfwDestroyWindow(window);

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwTerminate();
}

void update_gui(AppState &app)
{
	if(ImGui::Begin("Snapshots"))   snapshots_window(app);   ImGui::End();
	if(ImGui::Begin("Registers"))   registers_window(app);   ImGui::End();
	if(ImGui::Begin("Memory"))      memory_window(app);      ImGui::End();
	if(ImGui::Begin("Disassembly")) disassembly_window(app); ImGui::End();
	if(ImGui::Begin("GS Packet"))   gs_packet_window(app);   ImGui::End();
}

void snapshots_window(AppState &app)
{
	
	ImGui::AlignTextToFramePadding();
	ImGui::Text("Iter:");
	ImGui::SameLine();
	u32 pc = app.snapshots[app.current_snapshot].registers.VI[TPC].UL;
	if(ImGui::Button(" < ")) {
		walk_until_pc_equal(app, pc, -1);
	}
	ImGui::SameLine();
	if(ImGui::Button(" > ")) {
		walk_until_pc_equal(app, pc, 1);
	}
	
	std::function<bool(Snapshot &)> filter;
	
	if(ImGui::BeginTabBar("tabs")) {
		if(ImGui::BeginTabItem("All")) {
			filter = [&](Snapshot &snapshot) { return true; };
			ImGui::EndTabItem();
		}
		if(ImGui::BeginTabItem("XGKICK")) {
			filter = [&](Snapshot &snapshot) {
				u32 pc = snapshot.registers.VI[TPC].UL;
				u32 lower = *(u32*) &snapshot.program[pc];
				return is_xgkick(lower);
			};
			ImGui::EndTabItem();
		}
		if(ImGui::BeginTabItem("Highlighted")) {
			filter = [&](Snapshot &snapshot) {
				std::string disassembly = disassemble(&snapshot.program[snapshot.registers.VI[TPC].SL], snapshot.registers.VI[TPC].SL);
				bool is_highlighted =
					app.disassembly_highlight.size() > 0 &&
					disassembly.find(app.disassembly_highlight) != std::string::npos;
				return is_highlighted;
			};
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	
	ImVec2 size = ImGui::GetContentRegionAvail();
	ImGui::PushItemWidth(-1);
	if(ImGui::BeginListBox("##snapshots", size)) {
		for(std::size_t i = 0; i < app.snapshots.size(); i++) {
			Snapshot& snap = app.snapshots[i];
			Snapshot next_snap;
			if(i < app.snapshots.size() - 1) {
				next_snap = app.snapshots[i + 1];
			}
			bool is_selected = i == app.current_snapshot;
			
			if(!filter(snap)) {
				continue;
			}
			
			std::stringstream ss;
			ss << i;
			if(next_snap.read_size > 0) {
				ss << " READ 0x" << std::hex << next_snap.read_addr;
			} else if(next_snap.write_size > 0) {
				ss << " WRITE 0x" << std::hex << next_snap.write_addr;
			}
			
			u32 pc = snap.registers.VI[TPC].UL;
			std::string disassembly = disassemble(&snap.program[pc], pc);
			
			bool is_highlighted =
			app.disassembly_highlight.size() > 0 &&
				disassembly.find(app.disassembly_highlight) != std::string::npos;
		
			if(is_highlighted) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImColor(255, 255, 0).Value);
			}
			if(ImGui::Selectable(ss.str().c_str(), is_selected)) {
				app.current_snapshot = i;
				app.disassembly_scroll_to = true;
			}
			if(is_highlighted) {
				ImGui::PopStyleColor();
			}
			
			if(app.snapshots_scroll_to && is_selected) {
				ImGui::SetScrollHereY(0.5);
				app.snapshots_scroll_to = false;
			}
		}
		ImGui::EndListBox();
	}
	ImGui::PopItemWidth();
}

void registers_window(AppState &app) {
	Snapshot &current = app.snapshots[app.current_snapshot];
	VURegs &regs = current.registers;
	
	static const char *integer_register_names[] = {
			"vi00", "vi01", "vi02", "vi03",
			"vi04", "vi05", "vi06", "vi07",
			"vi08", "vi09", "vi10", "vi11",
			"vi12", "vi13", "vi14", "vi15",
			"Status", "MACflag", "ClipFlag", "c2c19",
			"R", "I", "Q", "c2c23",
			"c2c24", "c2c25", "TPC", "CMSAR0",
			"FBRST", "VPU-STAT", "c2c30", "CMSAR1",
	};
	
	ImGui::BeginTable("Registers", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV |
										   ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable);

	for (int i = 0; i < 32; i++) {
		VECTOR value = regs.VF[i];
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);

		if (show_as_hex) {
			ImGui::Text("vf%02d = %08x %08x %08x %08x",
						i, value.UL[0], value.UL[1], value.UL[2], value.UL[3]);
		} else {
			ImGui::Text("vf%02d = %.4f %.4f %.4f %.4f",
						i, value.F[0], value.F[1], value.F[2], value.F[3]);
		}

		ImGui::TableSetColumnIndex(1);

		ImGui::Text("%s = 0x%x = %hd", integer_register_names[i], regs.VI[i].UL, regs.VI[i].UL);
	}

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	if (show_as_hex) {
		ImGui::Text("ACC = %08x %08x %08x %08x",
					regs.ACC.UL[0], regs.ACC.UL[1], regs.ACC.UL[2], regs.ACC.UL[3]);
	} else {
		ImGui::Text("ACC = %.4f %.4f %.4f %.4f",
					regs.ACC.F[0], regs.ACC.F[1], regs.ACC.F[2], regs.ACC.F[3]);
	}

	ImGui::EndTable();
}

void memory_window(AppState &app)
{
	Snapshot &current = app.snapshots[app.current_snapshot];
	Snapshot *last;
	if(app.current_snapshot > 0) {
		last = &app.snapshots[app.current_snapshot - 1];
	} else {
		last = &current;
	}
	
	static MessageBoxState found_bytes;
	alert(found_bytes, "Found Bytes");
	
	if(prompt(find_bytes, "Find Bytes") && !found_bytes.is_open) {
		std::vector<u8> value_raw = decode_hex(find_bytes.text);
		for(std::size_t i = 0; i < VU1_MEMSIZE - value_raw.size(); i++) {
			if(memcmp(value_raw.data(), &current.memory[i], value_raw.size()) == 0) {
				found_bytes.is_open = true;
				found_bytes.text = "Found match at 0x" + to_hex(i);
				break;
			}
		}
		if(!found_bytes.is_open) {
			found_bytes.is_open = true;
			found_bytes.text = "No match found";
		}
	}
	
	if(prompt(save_to_file, "Save to File")) {
		FILE* dump_file = fopen(save_to_file.text.c_str(), "wb");
		if(dump_file) {
			fwrite(&current.memory[0], sizeof(current.memory), 1, dump_file);
			fclose(dump_file);
		} else {
			fprintf(stderr, "Failed to open %s for writing.\n", save_to_file.text.c_str());
		}
	}
	
	static std::string scroll_to_address_str;
	s32 scroll_to_address = -1;

	if(prompt(go_to_box, "Scroll To Address"))
	{
		scroll_to_address = strtol(go_to_box.text.c_str(), NULL, 16);
	}
	
	ImGui::BeginChild("rows_outer");
	if(ImGui::BeginChild("rows")) {
		ImDrawList *dl = ImGui::GetWindowDrawList();
		
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(18, 4));
		
		for(int i = 0; i < VU1_MEMSIZE / row_size; i++) {
			ImGui::PushID(i);
			
			static ImColor row_header_col = ImColor(1.f, 1.f, 1.f);
			std::stringstream row_header;
			row_header << std::hex << std::setfill('0') << std::setw(5) << i * row_size;
			ImGui::Text("%s", row_header.str().c_str());
			ImGui::SameLine();
			
			for(int j = 0; j < row_size / 4; j++) {
				ImGui::PushID(j);
				const auto draw_byte = [&](int k) {
					ImGui::PushID(k);
					
					u32 address = i * row_size + j * 4 + k;
					u32 val = current.memory[address];
					u32 last_val = last->memory[address];
					std::stringstream hex;
					if(val < 0x10) hex << "0";
					hex << std::hex << val;
					ImVec4 hex_col = ImVec4(0.8f, 0.8f, 0.8f, 1.f);
					if(val != last_val) {
						hex_col = ImVec4(1.f, 0.5f, 0.5f, 1.f);
					}
					ImGui::PushStyleColor(ImGuiCol_Text, hex_col);
					if(ImGui::Button(hex.str().c_str())) {
						walk_until_mem_access(app, address);
					}
					ImGui::SameLine();
					ImGui::PopStyleColor();
					
					if(address == scroll_to_address) {
						ImGui::SetScrollHereY(0.5);
					}
					
					ImGui::PopID(); // k
				};
				
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
				draw_byte(0);
				draw_byte(1);
				draw_byte(2);
				ImGui::PopStyleVar();
				draw_byte(3);
				ImGui::PopID(); // j
			}
			ImGui::NewLine();
			
			ImGui::PopID(); // i
		}
		
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		ImGui::PopStyleVar();
	}
	ImGui::EndChild();
	ImGui::EndChild();
}

void disassembly_window(AppState &app)
{
	Snapshot &current = app.snapshots[app.current_snapshot];
	
	ImGui::PushItemWidth(ImGui::GetWindowWidth() - (ImGui::GetWindowWidth() * .75f));
	ImGui::InputText("Highlight", &app.disassembly_highlight);
	ImGui::PopItemWidth();
	
	if(prompt(comment_box, "Load Comment File")) {
		parse_comment_file(app, comment_box.text);
	}
	if(prompt(export_box, "Export Disassembly")) {
		std::ofstream disassembly_out_file(export_box.text);
		for(std::size_t i = 0; i < VU1_PROGSIZE; i+= INSN_PAIR_SIZE) {
			disassembly_out_file << disassemble(&current.program[i], i);
			if(app.comments.at(i / INSN_PAIR_SIZE).size() > 0) {
				disassembly_out_file << "; ";
			}
			disassembly_out_file << app.comments.at(i / INSN_PAIR_SIZE);
			disassembly_out_file << "\n";
		}
	}

	ImGui::BeginChild("disasm");

	ImGui::BeginTable("Instructions", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV |
									  ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable);

	for(std::size_t i = 0; i < VU1_PROGSIZE; i += INSN_PAIR_SIZE) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::PushID(i);

		Instruction instruction = app.instructions[i / INSN_PAIR_SIZE];
		bool is_pc = current.registers.VI[TPC].UL == i;
		ImGuiSelectableFlags flags = instruction.is_executed ?
									 ImGuiSelectableFlags_None :
									 ImGuiSelectableFlags_Disabled;

		std::string disassembly = instruction.disassembly; 

		if(instruction.branch_from_times.size() > 0) {
			std::stringstream addresses;
			std::size_t fallthrough_times = app.instructions[i / 8 + 1].times_executed;
			for(auto addrtimes = instruction.branch_from_times.begin(); addrtimes != instruction.branch_from_times.end(); addrtimes++) {
				addresses << std::hex << addrtimes->first << " (" << std::dec << addrtimes->second << ") ";
				fallthrough_times -= addrtimes->second;
			}
			ImGui::Text("  %s/ ft (%ld) ->", addresses.str().c_str(), fallthrough_times);
		}

		bool is_highlighted =
				app.disassembly_highlight.size() > 0 &&
				disassembly.find(app.disassembly_highlight) != std::string::npos;

		if(is_highlighted) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImColor(255, 255, 0).Value);
		}
		bool clicked = ImGui::Selectable(disassembly.c_str(), is_pc, flags);
		if(is_highlighted) {
			ImGui::PopStyleColor();
		}

		if(instruction.branch_to_times.size() > 0) {
			std::stringstream addresses;
			std::size_t fallthrough_times = instruction.times_executed;
			for(auto addrtimes = instruction.branch_to_times.begin(); addrtimes != instruction.branch_to_times.end(); addrtimes++) {
				addresses << std::hex << addrtimes->first << " (" << std::dec << addrtimes->second << ") ";
				fallthrough_times -= addrtimes->second;
			}
			ImGui::Text("  -> %s/ ft (%ld)", addresses.str().c_str(), fallthrough_times);
		}

		if(is_pc && app.disassembly_scroll_to) {
			ImGui::SetScrollHereY(0.5);
			app.disassembly_scroll_to = false;
		}

		if(!is_pc && clicked) {
			bool pc_changed = false;
			if(current.registers.VI[TPC].UL > i) {
				pc_changed = walk_until_pc_equal(app, i, -1);
				if(!pc_changed) {
					pc_changed = walk_until_pc_equal(app, i, 1);
				}
			} else {
				pc_changed = walk_until_pc_equal(app, i, 1);
				if(!pc_changed) {
					pc_changed = walk_until_pc_equal(app, i, -1);
				}
			}
			if(pc_changed) {
				app.disassembly_scroll_to = true;
			}
		}

		ImGui::TableSetColumnIndex(1);
		
		if(!is_pc) {
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
		}
		ImVec2 comment_size(ImGui::GetWindowSize().x - 768.f, 14.f);
		std::string &comment = app.comments.at(i / INSN_PAIR_SIZE);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
		ImGui::PushItemWidth(-1);
		ImGuiInputTextFlags comment_flags = app.comments_loaded ?
											ImGuiInputTextFlags_None :
											ImGuiInputTextFlags_ReadOnly;
		if(ImGui::InputText("##comment", &comment, comment_flags)) {
			save_comment_file(app);
		}
		ImGui::PopItemWidth();
		ImGui::PopStyleVar();
		if(!is_pc) {
			ImGui::PopStyleColor();
		}

		ImGui::PopID();
	}

	ImGui::EndTable();
	ImGui::EndChild();
}

void gs_packet_window(AppState &app)
{
	ImGui::Columns(2);
	
	static std::string address_hex;
	ImGui::InputText("Address", &address_hex);
	
	Snapshot &snap = app.snapshots[app.current_snapshot];
	
	std::size_t address;
	if(address_hex.size() == 0) {
		u32 pc = snap.registers.VI[TPC].UL;
		u32 lower = *(u32*) &snap.program[pc];
		if(is_xgkick(lower)) {
			u32 is = bit_range(lower, 11, 15);
			address = snap.registers.VI[is].UL * 0x10;
		} else {
			return;
		}
	} else {
		address = from_hex(address_hex);
	}
	
	if(address < 0) address = 0;
	if(address > VU1_MEMSIZE) address = VU1_MEMSIZE;
	
	GsPacket packet = read_gs_packet(&snap.memory[address], VU1_MEMSIZE - address);
	
	ImGui::BeginChild("primlist");
	
	static std::size_t selected_primitive = 0;
	for(std::size_t i = 0; i < packet.primitives.size(); i++) {
		std::string label = std::to_string(i);
		if(ImGui::Selectable(label.c_str(), i == selected_primitive)) {
			selected_primitive = i;
		}
	}
	if(selected_primitive >= packet.primitives.size()) {
		selected_primitive = 0;
	}
	
	ImGui::EndChild();
	ImGui::NextColumn();
	
	if(packet.primitives.size() < 1) {
		return;
	}
	const GsPrimitive &prim = packet.primitives[selected_primitive];
	const GifTag &tag = prim.tag;
	
	ImGui::TextWrapped("NLOOP=%x, EOP=%x, PRE=%x, FLAG=%s, NREG=%lx\n",
		tag.nloop, tag.eop, tag.pre, gif_flag_name(tag.flag), tag.regs.size());
	
	ImGui::TextWrapped("PRIM: PRIM=%s, IIP=%s, TME=%d, FGE=%d, ABE=%d, AA1=%d, FST=%s, CTXT=%s, FIX=%d",
		gs_primitive_type_name(tag.prim.prim),
		tag.prim.iip == GSSHADE_FLAT ? "FLAT" : "GOURAUD",
		tag.prim.tme, tag.prim.fge, tag.prim.abe, tag.prim.aa1,
		tag.prim.fst == GSFST_STQ ? "STQ" : "UV",
		tag.prim.ctxt == GSCTXT_1 ? "FIRST" : "SECOND",
		tag.prim.fix);
	
	
	ImGui::TextWrapped("REGS:");
	ImGui::SameLine();
	for(std::size_t i = 0; i < tag.regs.size(); i++) {
		ImGui::TextWrapped("%s", gs_register_name(tag.regs[i]));
		ImGui::SameLine();
	}
	ImGui::NewLine();
	
	ImGui::BeginChild("data");
	for(const GsPackedData& item : prim.packed_data) {
		ImGui::Text("%x: %6s", item.source_address, gs_register_name(item.reg));
		ImGui::SameLine();
		switch(item.reg) {
			case GSREG_AD: {
				ImGui::Text("%s <- %lx\n", gif_ad_register_name(item.ad.addr), item.ad.data);
				break;
			}
			case GSREG_XYZF2: {
				ImGui::Text("%d %d %d F=%d ADC=%d",
					item.xyzf2.x, item.xyzf2.y, item.xyzf2.z, item.xyzf2.f, item.xyzf2.adc);
				break;
			}
			default: {
				// Hex dump the raw data.
				for(std::size_t i = 0; i < 0x10; i += 4) {
					ImGui::Text("%02x%02x%02x%02x",
						item.buffer[i + 0],
						item.buffer[i + 1],
						item.buffer[i + 2],
						item.buffer[i + 3]);
					ImGui::SameLine();
				}
				ImGui::NewLine();
			}
		}
	}
	ImGui::EndChild();
}

bool walk_until_pc_equal(AppState &app, u32 target_pc, int step)
{
	std::size_t snapshot = app.current_snapshot;
	do {
		if(-step > (int) snapshot || snapshot + step >= app.snapshots.size()) {
			return false;
		}
		snapshot += step;
	} while(app.snapshots[snapshot].registers.VI[TPC].UL != target_pc);
	app.current_snapshot = snapshot;
	app.snapshots_scroll_to = true;
	return true;
}

void walk_until_mem_access(AppState &app, u32 address)
{
	std::size_t snapshot_index = app.current_snapshot;
	do {
		snapshot_index = (snapshot_index + 1) % app.snapshots.size();
		
		Snapshot &snap = app.snapshots[snapshot_index];
		if((snap.read_size > 0 && (snap.read_addr / 0x10 == address / 0x10)) ||
		   (snap.write_size > 0 && (snap.write_addr / 0x10 == address / 0x10))) {
			if(snapshot_index >= 1) {
				app.current_snapshot = snapshot_index - 1;
				app.snapshots_scroll_to = true;
				app.disassembly_scroll_to = true;
			}
			return;
		}
	} while(snapshot_index != app.current_snapshot);
}

enum VUTracePacketType {
	VUTRACE_NULLPACKET = 0,
	VUTRACE_PUSHSNAPSHOT = 'P',
	VUTRACE_SETREGISTERS = 'R',
	VUTRACE_SETMEMORY = 'M',
	VUTRACE_SETINSTRUCTIONS = 'I',
	VUTRACE_LOADOP = 'L',
	VUTRACE_STOREOP = 'S',
	VUTRACE_PATCHREGISTER = 'r',
	VUTRACE_PATCHMEMORY = 'm'
};

void parse_trace(AppState &app, std::string trace_file_path)
{
	std::vector<Snapshot> snapshots;
	
	app.trace_file_path = trace_file_path;
	
	FILE *trace = fopen(trace_file_path.c_str(), "rb");
	if(trace == nullptr) {
		fprintf(stderr, "Error: Failed to read trace!\n");
		exit(1);
	}
	
	app.instructions.resize(VU1_PROGSIZE / INSN_PAIR_SIZE);
	
	auto check_eof = [](int n) {
		if(n != 1) {
			fprintf(stderr, "Error: Unexpected end of file.\n");
			exit(1);
		}
	};
	
	app.snapshots = {};
	
	char magic[4];
	u32 version;
	check_eof(fread(magic, 4, 1, trace));
	if(memcmp(magic, "VUTR", 4) == 0) {
		check_eof(fread(&version, 4, 1, trace));
	} else {
		version = 1;
		fseek(trace, 0, SEEK_SET);
	}
	
	if(version > 3) {
		fprintf(stderr, "Format version too new!\n");
		exit(1);
	}
	
	Snapshot current;
	VUTracePacketType packet_type = VUTRACE_NULLPACKET;
	while(fread(&packet_type, 1, 1, trace) == 1) {
		switch(packet_type) {
			case VUTRACE_PUSHSNAPSHOT: {
				if(current.registers.VI[TPC].UL >= VU1_PROGSIZE || current.registers.VI[TPC].UL % INSN_PAIR_SIZE != 0) {
					fprintf(stderr, "Bad program counter value.\n");
					exit(1);
				}
				app.snapshots.push_back(current);
				
				u32 pc = current.registers.VI[TPC].UL;
				Instruction &instruction = app.instructions[pc / INSN_PAIR_SIZE];
				instruction.is_executed = true;
				
				if(app.snapshots.size() >= 2) {
					Snapshot &last = app.snapshots.at(app.snapshots.size() - 2);
					u32 last_pc = last.registers.VI[TPC].UL;
					if(last_pc + INSN_PAIR_SIZE != pc) {
						// A branch has taken place.
						app.instructions[last_pc / INSN_PAIR_SIZE].branch_to_times[pc]++;
						instruction.branch_from_times[last_pc]++;
					}
				}
				instruction.times_executed++;
				
				current.read_addr = 0;
				current.read_size = 0;
				current.write_addr = 0;
				current.write_size = 0;
				break;
			}
			case VUTRACE_SETREGISTERS: {
				if(version == 1) {
					old_pcsx2_structs_v1::VURegs old_regs = {};
					check_eof(fread(&old_regs, sizeof(old_regs), 1, trace));
					memcpy(current.registers.VF, old_regs.VF, sizeof(current.registers.VF));
					memcpy(current.registers.VI, old_regs.VI, sizeof(current.registers.VI));
					current.registers.ACC = old_regs.ACC;
					current.registers.q = old_regs.q;
					current.registers.p = old_regs.p;
				} else if(version == 2) {
					old_pcsx2_structs_v2::VURegs old_regs = {};
					check_eof(fread(&old_regs, sizeof(old_regs), 1, trace));
					memcpy(current.registers.VF, old_regs.VF, sizeof(current.registers.VF));
					memcpy(current.registers.VI, old_regs.VI, sizeof(current.registers.VI));
					current.registers.ACC = old_regs.ACC;
					current.registers.q = old_regs.q;
					current.registers.p = old_regs.p;
				} else {
					check_eof(fread(&current.registers.VF, sizeof(current.registers.VF), 1, trace));
					check_eof(fread(&current.registers.VI, sizeof(current.registers.VI), 1, trace));
					check_eof(fread(&current.registers.ACC, sizeof(current.registers.ACC), 1, trace));
					check_eof(fread(&current.registers.q, sizeof(current.registers.q), 1, trace));
					check_eof(fread(&current.registers.p, sizeof(current.registers.p), 1, trace));
				}
				break;
			}
			case VUTRACE_SETMEMORY: {
				check_eof(fread(current.memory, VU1_MEMSIZE, 1, trace));
				break;
			}
			case VUTRACE_SETINSTRUCTIONS: {
				check_eof(fread(current.program, VU1_PROGSIZE, 1, trace));
				break;
			}
			case VUTRACE_LOADOP: {
				check_eof(fread(&current.read_addr, sizeof(u32), 1, trace));
				check_eof(fread(&current.read_size, sizeof(u32), 1, trace));
				break;
			}
			case VUTRACE_STOREOP: {
				check_eof(fread(&current.write_addr, sizeof(u32), 1, trace));
				check_eof(fread(&current.write_size, sizeof(u32), 1, trace));
				break;
			}
			case VUTRACE_PATCHREGISTER: {
				u8 index = 0;
				u128 data = {};
				check_eof(fread(&index, sizeof(u8), 1, trace));
				check_eof(fread(&data, sizeof(u128), 1, trace));
				if(index < 32) {
					memcpy(&current.registers.VF[index], &data, 16);
				} else if(index < 64) {
					memcpy(&current.registers.VI[index - 32], &data, 16);
				} else if(index == 64) {
					memcpy(&current.registers.ACC, &data, 16);
				} else if(index == 65) {
					memcpy(&current.registers.q, &data, 16);
				} else if(index == 66) {
					memcpy(&current.registers.p, &data, 16);
				} else {
					fprintf(stderr, "Error: 'r' packet has bad register index.\n");
					exit(1);
				}
				break;
			}
			case VUTRACE_PATCHMEMORY: {
				u16 address = 0;
				u32 data = 0;
				check_eof(fread(&address, sizeof(u16), 1, trace));
				check_eof(fread(&data, sizeof(u32), 1, trace));
				if(address < VU1_MEMSIZE - 4) {
					memcpy(&current.memory[address], &data, sizeof(data));
				} else {
					fprintf(stderr, "Error: 'm' packet has address that is too big.\n");
					exit(1);
				}
				break;
			}
			default: {
				fprintf(stderr, "Error: Invalid packet type 0x%x in trace file at 0x%lx!\n",
					packet_type, ftell(trace));
				exit(1);
			}
		}
	}
	if(!feof(trace)) {
		fprintf(stderr, "Error: Failed to read trace!\n");
		exit(1);
	}
	
	fclose(trace);

	for(std::size_t i = 0; i < VU1_PROGSIZE; i += INSN_PAIR_SIZE) {
		app.instructions[i >> 3].disassembly = disassemble(&current.program[i], i);
	}    
}

void parse_comment_file(AppState &app, std::string comment_file_path) {
	app.comment_file_path = comment_file_path;
	std::ifstream comment_file(comment_file_path);
	if(comment_file) {
		std::string line;
		for(std::size_t i = 0; std::getline(comment_file, line) && i < VU1_PROGSIZE / INSN_PAIR_SIZE; i++) {
			app.comments[i] = line;
		}
		app.comments_loaded = true;
	}
}

void save_comment_file(AppState &app)
{
	std::ofstream comment_file(app.comment_file_path);
	for(std::size_t i = 0; i < app.comments.size(); i++) {
		comment_file << app.comments[i] << "\n";
	}
}

bool is_xgkick(u32 lower)
{
	return bit_range(lower, 0, 10) == 0b11011111100;
}

void init_gui(GLFWwindow **window)
{
	if(!glfwInit()) {
		fprintf(stderr, "Cannot load GLFW.");
		exit(1);
	}

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);

	*window = glfwCreateWindow(1280, 720, "vutrace", NULL, NULL);
	if(*window == nullptr) {
		fprintf(stderr, "Cannot create GLFW window.");
		exit(1);
	}

	glfwMaximizeWindow(*window);
	glfwMakeContextCurrent(*window);
	tick_rate = glfwGetVideoMode(glfwGetPrimaryMonitor())->refreshRate / 60;
	glfwSwapInterval(tick_rate); // vsync

	if(!gladLoadGLLoader((GLADloadproc) glfwGetProcAddress)) {
		fprintf(stderr, "Cannot load GLAD.");
		exit(1);
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	
	io.Fonts->AddFontFromMemoryCompressedTTF(&ProggyVectorRegular_compressed_data, ProggyVectorRegular_compressed_size, font_size);
	
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigDockingWithShift = true;
	io.FontAllowUserScaling = true;
	ImGui::StyleColorsDark();
	
	io.Fonts->Build();
	ImGui_ImplGlfw_InitForOpenGL(*window, true);
	ImGui_ImplOpenGL3_Init("#version 120");
}

void update_font() {
	ImGuiIO &io = ImGui::GetIO();
	
	io.Fonts->Clear();
	io.Fonts->ClearFonts();

	if(use_default_font) {
		default_font_cfg.SizePixels = font_size;
		io.FontDefault = io.Fonts->AddFontDefault(&default_font_cfg);
	} else {
		io.FontDefault = io.Fonts->AddFontFromMemoryCompressedTTF(&ProggyVectorRegular_compressed_data, ProggyVectorRegular_compressed_size, font_size);
	}

	io.Fonts->Build();
	ImGui_ImplOpenGL3_CreateFontsTexture();
	require_font_update = false;
}

void main_menu_bar() {
	handle_shortcuts();
	
	if (ImGui::BeginMainMenuBar()) {
		if(ImGui::BeginMenu("File")) {
			if(ImGui::MenuItem("Load Comments", "Ctrl+L")) {
				comment_box.is_open = true;
			}
			if(ImGui::MenuItem("Export Disassembly", "Ctrl+D")) {
				export_box.is_open = true;
			}
			ImGui::EndMenu();
		}
		if(ImGui::BeginMenu("System")) {
			
			if(ImGui::SliderInt("##tickrate", &tick_rate, 0, 5, "App Refresh Rate %d")) {
				
				glfwSwapInterval(tick_rate);
			}
			
			ImGui::SetItemTooltip("Limits the application's refresh rate to decrease impact on CPU. Assuming a 60Hz monitor, the default value (1) is enough.\n0 is unlimited, 60Hz / 2 = 30fps, 60Hz / 3 = 20fps, etc.");
			
			ImGui::EndMenu();
		}
		if(ImGui::BeginMenu("Registers")) {
			if(ImGui::MenuItem("Show as Hex", "Ctrl+Q", show_as_hex)) {
				show_as_hex = !show_as_hex;
			}
			ImGui::EndMenu();
		}
		if(ImGui::BeginMenu("Memory")) {
			if(ImGui::MenuItem("Search", "Ctrl+F")) {
				find_bytes.is_open = true;
			}
			if(ImGui::MenuItem("Dump", "Ctrl+T")) {
				save_to_file.is_open = true;
			}
			if(ImGui::MenuItem("Go To", "Ctrl+G")) {
				go_to_box.is_open = true;
			}
			if(ImGui::SliderInt("##rowsize", &row_size_imgui, 1, 8, "Line Width: %d")) {
				row_size = row_size_imgui * 4;
			}
			ImGui::EndMenu();
		}
		if(ImGui::BeginMenu("Font")) {
			if(ImGui::MenuItem("Use Default", "", use_default_font)) {
				use_default_font = !use_default_font;
				require_font_update = true;
			}
			if(ImGui::InputFloat("##Size", &font_size, 1.0f, 20.0f, "Size %1.0f")) {
				require_font_update = true;
			}
			ImGui::EndMenu();
		}
		
		ImGui::EndMainMenuBar();
	}
}

void handle_shortcuts() {
	if(ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
		if(ImGui::IsKeyPressed(ImGuiKey_F)) {
			find_bytes.is_open = !find_bytes.is_open;
		}
		if(ImGui::IsKeyPressed(ImGuiKey_T)) {
			save_to_file.is_open = !save_to_file.is_open;
		}
		if(ImGui::IsKeyPressed(ImGuiKey_D)) {
			export_box.is_open = !export_box.is_open;
		}
		if(ImGui::IsKeyPressed(ImGuiKey_L)) {
			comment_box.is_open = !comment_box.is_open;
		}
		if(ImGui::IsKeyPressed(ImGuiKey_G)) {
			go_to_box.is_open = !go_to_box.is_open;
		}
		if(ImGui::IsKeyPressed(ImGuiKey_Q)) {
			show_as_hex = !show_as_hex;
		}
	}
}

void begin_docking() {
	ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->Pos);
	ImGui::SetNextWindowSize(viewport->Size);
	ImGui::SetNextWindowViewport(viewport->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
	window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

	static bool p_open;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("dock_space", &p_open, window_flags);
	ImGui::PopStyleVar();
	
	ImGui::PopStyleVar(2);

	ImGuiID dockspace_id = ImGui::GetID("dock_space");
	ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
}

void create_dock_layout(GLFWwindow *window)
{
	ImGuiID dockspace_id = ImGui::GetID("dock_space");
	
	int width, height;
	glfwGetFramebufferSize(window, &width, &height);
	
	ImGui::DockBuilderRemoveNode(dockspace_id);
	ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(width, height));
	
	ImGuiID top, bottom;
	ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.75f, &top, &bottom);
	
	ImGuiID registers, middle;
	ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 1.f / 3.f, &registers, &middle);
	
	ImGuiID snapshots, disassembly;
	ImGui::DockBuilderSplitNode(middle, ImGuiDir_Left, 0.2f, &snapshots, &disassembly);
	
	ImGuiID memory, gs_packet;
	ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.5f, &memory, &gs_packet);
	
	ImGui::DockBuilderDockWindow("Registers", registers);
	ImGui::DockBuilderDockWindow("Snapshots", snapshots);
	ImGui::DockBuilderDockWindow("Disassembly", disassembly);
	ImGui::DockBuilderDockWindow("Memory", memory);
	ImGui::DockBuilderDockWindow("GS Packet", gs_packet);
}

void alert(MessageBoxState &state, const char *title)
{
	if(state.is_open) {
		ImGui::SetNextWindowSize(ImVec2(400, 100));
		ImGui::Begin(title);
		ImGui::Text("%s", state.text.c_str());
		if(ImGui::Button("Close")) {
			state.text = "";
			state.is_open = false;
		}
		ImGui::End();
	}
}

bool prompt(MessageBoxState &state, const char *title)
{
	bool result = false;
	if(state.is_open) {
		ImGui::SetNextWindowSize(ImVec2(400, 100));
		ImGui::Begin(title, &state.is_open);
		ImGui::InputText("##input", &state.text, ImGuiInputTextFlags_AutoSelectAll);
		if(ImGui::Button("Okay") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
			state.is_open = false;
			result = true;
		}
		ImGui::SameLine();
		if(ImGui::Button("Cancel")  || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			state.is_open = false;
		}
		ImGui::End();
	}
	return result;
}

std::vector<u8> decode_hex(const std::string &in)
{
	std::vector<u8> result;
	u8 current_byte;
	bool reading_second_nibble = false;
	for(char c : in) {
		u8 nibble = 0;
		if(c >= '0' && c <= '9') {
			nibble = c - '0';
		} else if(c >= 'A' && c <= 'Z') {
			nibble = c - 'A' + 0xa;
		} else if(c >= 'a' && c <= 'z') {
			nibble = c - 'a' + 0xa;
		} else {
			continue;
		}
		
		if(!reading_second_nibble) {
			current_byte = nibble << 4;
			reading_second_nibble = true;
		} else {
			current_byte |= nibble;
			result.push_back(current_byte);
			reading_second_nibble = false;
		}
		
	}
	return result;
}

std::string to_hex(size_t n)
{
	std::stringstream ss;
	ss << std::hex << n;
	return ss.str();
}

size_t from_hex(const std::string& in) {
	size_t result;
	std::stringstream ss;
	ss << std::hex << in;
	ss >> result;
	return result; 
}
