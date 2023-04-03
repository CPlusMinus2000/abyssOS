#include "terminal_admin.h"
#include "../etl/circular_buffer.h"
#include "../etl/deque.h"
#include "../etl/queue.h"
#include "../server/global_pathing_server.h"
#include "../server/local_pathing_server.h"
#include "../server/track_server.h"
#include "../server/train_admin.h"
#include "../utils/buffer.h"
#include "../utils/printf.h"
#include "courier_pool.h"
#include <climits>
using namespace Terminal;
using namespace Message;

const int HANDLE_FAIL = -1;
const char SENSOR_LETTERS[] = "ABCDE";
constexpr char SPACES[] = "                                                                      ";

struct TerminalCommand {
	char cmd[CMD_LEN];
	int len;
};

int relu(int x) {
	return x > 0 ? x : 0;
}

void str_cpy(const char* source, char* target, int* index, int len, bool check_null_char = false) {
	for (int i = 0; i < len && (!check_null_char || source[i] != '\0'); i++) {
		target[*index] = source[i];
		(*index) += 1;
	}
}

// Attempts to read the ID of a track node from the buffer
// out_len is
int scan_sensor_id(const char str[], int* out_len, const char track_id = 'a') {
	using namespace Planning;

	// Track ID should be lowercase a or b
	if (lower(track_id) != 'a' && lower(track_id) != 'b') {
		return READ_INT_FAIL;
	}

	// First character should be an alphabetic character, a-e or m
	if (!is_alpha(str[0])) {
		return READ_INT_FAIL;
	}

	// Now we work through a few cases
	// 1. The first character is A-E, and the second character is a digit
	// 2. The first character is B or M, the second character is R, and the third character is a digit
	// 3. The first character is E, the second character is N or X, and the third character is a digit
	if (lower(str[0]) >= 'a' && lower(str[0]) <= 'e' && is_digit(str[1])) {
		// We have a sensor ID
		int id = scan_int(str + 1, out_len, 2);
		if (id == READ_INT_FAIL || id < 1 || id > SENSORS_PER_LETTER) {
			return READ_INT_FAIL;
		}

		*out_len += 1;
		return (lower(str[0]) - 'a') * SENSORS_PER_LETTER + (id - 1);
	} else if (lower(str[0]) == 'b' || lower(str[0]) == 'm') {
		if (lower(str[1]) != 'r' || !is_digit(str[2])) {
			return READ_INT_FAIL;
		}

		int id = scan_int(str + 2, out_len, 3);
		if (id == READ_INT_FAIL || get_switch_id(id) == Train::NO_SWITCH) {
			return READ_INT_FAIL;
		}

		*out_len += 2;
		int ind = Train::get_switch_id(id);
		return (lower(str[0]) == 'b') ? TRACK_BRANCHES[ind] : TRACK_MERGES[ind];
	} else if (lower(str[0]) == 'e') {
		if ((lower(str[1]) != 'n' && lower(str[1]) != 'x') || !is_digit(str[2])) {
			return READ_INT_FAIL;
		}

		int orig_out_len = *out_len;
		int id = scan_int(str + 2, out_len, 3);
		if (id == READ_INT_FAIL) {
			return READ_INT_FAIL;
		}

		// Now we need to use the track data, because the entrances and exits are where tracks differ
		if (id < 1 || id > Planning::NUM_ENTER_EXIT) {
			*out_len = orig_out_len;
			return READ_INT_FAIL;
		} else if (lower(track_id) == 'b' && contains<int>(TRACK_B_MISSING, TRACK_B_MISSING_SIZE, id)) {
			*out_len = orig_out_len;
			return READ_INT_FAIL;
		}

		*out_len += 2;
		if (lower(track_id) == 'b') {
			id -= (id > TRACK_B_MISSING[1]);
			id -= (id > TRACK_B_MISSING[0]);
		}

		return (lower(str[1]) == 'n') ? TRACK_ENTRANCES[id - 1] : TRACK_EXITS[id - 1];
	} else {
		return READ_INT_FAIL;
	}
}

void log_time(char buf[], const uint32_t ticks) {
	char to = '0' + ticks % 10;

	uint32_t seconds = (ticks / 10) % 60;
	char so = '0' + seconds % 10;
	char st = '0' + (seconds / 10) % 10;

	uint32_t minutes = ticks / 600;
	char mo = '0' + minutes % 10;
	char mt = '0' + (minutes / 10) % 10;
	char mh = '0' + (minutes / 100) % 10;

	sprintf(buf, "%c%c%c:%c%c.%c", mh, mt, mo, st, so, to);
}

// Given a switch number, find the cursor position
// of the switch in the UI
void sw_to_cursor_pos(char sw, int* r, int* c) {
	if (sw < 19) {
		*r = 9 + 2 * ((sw - 1) / 6);
		*c = 5 + 6 * ((sw - 1) % 6);
	} else {
		*r = 15;
		*c = 8 + 9 * (sw - 19);
	}
}

// Given a train number and a UI request enum class, find the cursor position
// of the train in the UI and return the x and y position as a pair
etl::pair<int, int> train_to_cursor_pos(int train, TrainUIReq req) {
	int r = TRAIN_PRINTOUT_ROW + static_cast<int>(req) + 1;
	int tindex = Train::train_num_to_index(train);
	if (tindex == Train::NO_TRAIN) {
		return { -1, -1 };
	}

	int c = TRAIN_PRINTOUT_FIRST + tindex * TRAIN_PRINTOUT_WIDTH + TRAIN_PRINTOUT_UI_OFFSETS[tindex];
	if (train >= 10 && static_cast<int>(req) > 0) {
		c -= 1;
	}

	return { r, c };
}

int handle_tr(AddressBook& addr, const char cmd[]) {
	int i = 3;
	int out_len = 0;
	int train = scan_int(cmd + i, &out_len, 2);
	if (Train::train_num_to_index(train) == Train::NO_TRAIN) {
		return HANDLE_FAIL;
	}

	i += out_len + 1;
	if (cmd[i - 1] != ' ')
		return HANDLE_FAIL;

	int speed = scan_int(cmd + i, &out_len, 2);
	if (speed == -1) {
		return HANDLE_FAIL;
	}

	i += out_len;
	for (; cmd[i] != '\r'; ++i) {
		if (cmd[i] != ' ') {
			return HANDLE_FAIL;
		}
	}

	Train::TrainAdminReq req;
	req.header = RequestHeader::TRAIN_SPEED;
	req.body.command.id = train;
	req.body.command.action = speed;
	Send::SendNoReply(addr.train_admin_tid, reinterpret_cast<char*>(&req), sizeof(req));

	return 0;
}

int handle_rv(Courier::CourierPool<TerminalCourierMessage>& pool, const char cmd[]) {
	int out_len = 0;
	int i = 3;
	int train = scan_int(cmd + i, &out_len, 2);
	if (Train::train_num_to_index(train) == Train::NO_TRAIN) {
		return HANDLE_FAIL;
	}

	i += out_len;
	for (; cmd[i] != '\r'; ++i) {
		if (cmd[i] != ' ') {
			return HANDLE_FAIL;
		}
	}

	TerminalCourierMessage req = { RequestHeader::TERM_COUR_REV, train };
	pool.request(&req);
	return 0;
}

int handle_sw(AddressBook& addr, const char cmd[]) {
	char snum = 0;
	char status = 0;
	int i = 3;

	int out_len = 0;
	int switch_num = scan_int(cmd + i, &out_len, 3);
	if (switch_num == READ_INT_FAIL || switch_num < 1) {
		return HANDLE_FAIL;
	} else {
		snum = switch_num;
	}

	i += out_len + 2;
	if (cmd[i - 2] != ' ')
		return HANDLE_FAIL;

	status = lower(cmd[i - 1]);
	if (status != 'c' && status != 's')
		return HANDLE_FAIL;

	for (; cmd[i] != '\r'; ++i) {
		if (cmd[i] != ' ') {
			return HANDLE_FAIL;
		}
	}

	Track::TrackServerReq req = {};
	req.header = RequestHeader::TRACK_SWITCH;
	req.body.command.id = snum;
	req.body.command.action = status;
	Send::SendNoReply(addr.track_server_tid, reinterpret_cast<char*>(&req), sizeof(req));

	return 0;
}

/*
 * Handles a global pathing server command, which is one of:
 *  - go <train> <nodes...>
 *  - locate <train> <sensor>
 *  - init <train> <nodes...>
 * This is essentially a wrapper around sending a terminal courier and performing some error checking,
 * so it's pretty generic.
 */
int handle_global_pathing(Courier::CourierPool<TerminalCourierMessage>& pool, GenericCommand& cmd, RequestHeader header) {
	if (cmd.args.size() < 1 || (header == RequestHeader::TERM_COUR_LOCAL_LOCATE && cmd.args.size() != 1)) {
		return HANDLE_FAIL;
	}

	int arg = cmd.args.front();
	TerminalCourierMessage req;
	req.header = header;

	if (header == RequestHeader::TERM_COUR_LOCAL_INIT) {
		if (arg != 1 && arg != 2) {
			return HANDLE_FAIL;
		}
	} else {
		int index = Train::train_num_to_index(arg);
		if (index == Train::NO_TRAIN) {
			return HANDLE_FAIL;
		}
	}

	req.body.courier_body.num_args = cmd.args.size();
	for (int i = 0; !cmd.args.empty(); ++i) {
		req.body.courier_body.args[i] = cmd.args.front();
		cmd.args.pop();
	}

	pool.request(&req);
	return 0;
}

GenericCommand handle_generic(const char cmd[], const char which_track = 'a') {
	GenericCommand command = GenericCommand();
	int i = 0;

	for (; cmd[i] != ' ' && cmd[i] != '\r' && i < MAX_COMMAND_LEN - 1; ++i) {
		command.name[i] = cmd[i];
	}

	if (i == 0 || i == MAX_COMMAND_LEN - 1) {
		return command;
	}

	command.name[i] = '\0';
	while (cmd[i] == ' ') {
		i++;
	}

	int out_len = 0;
	while (cmd[i] != '\r') {
		int arg = scan_int(cmd + i, &out_len);
		if (arg == READ_INT_FAIL) {
			// Try reading it as a sensor instead
			arg = scan_sensor_id(cmd + i, &out_len, which_track);
			if (arg == READ_INT_FAIL) {
				return command;
			}
		}

		command.args.push(arg);
		i += out_len;
		while (cmd[i] == ' ') {
			i++;
		}
	}

	command.success = true;
	return command;
}

void Terminal::terminal_admin() {
	Name::RegisterAs(TERMINAL_ADMIN);

	LocalPathing::LocalPathingReq req_to_local_train;
	req_to_local_train.header = Message::RequestHeader::LOCAL_PATH_SET_TRAIN;
	for (int i = 0; i < Train::NUM_TRAINS; ++i) {
		int tid = Task::Create(Priority::TERMINAL_PRIORITY, &LocalPathing::local_pathing_worker);
		req_to_local_train.body.train_num = Train::TRAIN_NUMBERS[i];
		Message::Send::SendNoReply(tid, reinterpret_cast<char*>(&req_to_local_train), sizeof(req_to_local_train));
	}

	AddressBook addr = getAddressBook();
	int from;
	TerminalServerReq req;

	Courier::CourierPool<TerminalCourierMessage> courier_pool
		= Courier::CourierPool<TerminalCourierMessage>(&terminal_courier, Priority::TERMINAL_PRIORITY);

	UART::Puts(addr.term_trans_tid, 0, START_PROMPT, sizeof(START_PROMPT) - 1);

	Task::Create(Priority::TERMINAL_PRIORITY, &terminal_clock_courier);
	Task::Create(Priority::TERMINAL_PRIORITY, &sensor_query_courier);
	Task::Create(Priority::TERMINAL_PRIORITY, &idle_time_courier);
	Task::Create(Priority::TERMINAL_PRIORITY, &user_input_courier);
	Task::Create(Priority::TERMINAL_PRIORITY, &switch_state_courier);
	Task::Create(Priority::TERMINAL_PRIORITY, &train_state_courier);

	bool isRunning = false;
	bool isDebug = false;

	char printing_buffer[UART::UART_MESSAGE_LIMIT]; // 512 is good for now
	int printing_index = 0;
	char buf[TERM_A_BUFLEN];
	etl::circular_buffer<TerminalCommand, CMD_HISTORY_LEN> cmd_history = etl::circular_buffer<TerminalCommand, CMD_HISTORY_LEN>();
	cmd_history.push(TerminalCommand { { 0 }, 0 });
	size_t cmd_history_index = 0;
	TAState escape_status = TAState::TA_DEFAULT_ARROW_STATE;

	bool isSensorModified = false;
	char sensor_state[Sensor::NUM_SENSOR_BYTES];
	etl::deque<etl::pair<int, int>, RECENT_SENSOR_COUNT> recent_sensor = etl::deque<etl::pair<int, int>, RECENT_SENSOR_COUNT>();
	bool sensor_table[Sensor::NUM_SENSOR_BYTES][CHAR_BIT] = { false };

	uint64_t idle_time, total_time;
	bool isIdleTimeModified = false;
	int char_count = 0;
	int ticks = 0;

	bool isSwitchStateModified = false;
	char switch_state[Train::NUM_SWITCHES];

	bool isTrainStateModified = false;
	Train::TrainRaw train_state[Train::NUM_TRAINS];
	GlobalTrainInfo global_train_info[Train::NUM_TRAINS];

	char which_track = 'a';
	track_node track[TRACK_MAX];
	init_tracka(track);

	// This is used to keep track of number of activated sensors

	auto trigger_print = [&]() {
		if (isRunning) {
			printing_index = 0;
			str_cpy(SAVE_CURSOR, printing_buffer, &printing_index, sizeof(SAVE_CURSOR) - 1);
			log_time(buf, ticks);
			str_cpy(buf, printing_buffer, &printing_index, 8);
			if (isIdleTimeModified) {
				isIdleTimeModified = false;
				uint64_t leading = idle_time * 100 / total_time;
				uint64_t trailing = (idle_time * 100000) / total_time % 1000;

				sprintf(buf, "\033[1;60HPercent: %llu.%03llu", leading, trailing);
				str_cpy(buf, printing_buffer, &printing_index, TERM_A_BUFLEN, true);
			}

			if (isSensorModified) {
				isSensorModified = false;
				str_cpy(SENSOR_CURSOR, printing_buffer, &printing_index, sizeof(SENSOR_CURSOR) - 1);
				for (int i = 0; i < Sensor::NUM_SENSOR_BYTES; i++) {
					for (int j = 1; j <= CHAR_BIT; j++) {
						if (sensor_state[i] & (1 << (CHAR_BIT - j)) && !sensor_table[i][j - 1]) {
							if (recent_sensor.size() == recent_sensor.max_size()) {
								// Full deque, remove last element
								recent_sensor.pop_back();
							}
							recent_sensor.push_front(etl::pair<int, int> { i, j });
						}

						sensor_table[i][j - 1] = sensor_state[i] & (1 << (CHAR_BIT - j));
					}
				}

				// Print every sensor that has been activated
				for (auto& it : recent_sensor) {
					const char l = SENSOR_LETTERS[it.first / 2];
					int pos = CHAR_BIT * (it.first % 2);
					char ones = '0' + ((it.second + pos) % 10);
					char write[4] = { l, ((it.second + pos > 9) ? '1' : '0'), ones, ' ' };
					str_cpy(write, printing_buffer, &printing_index, 4);
				}
			}

			if (isSwitchStateModified) {
				isSwitchStateModified = false;
				int r;
				int c;
				for (uint64_t i = 0; i < sizeof(switch_state); i++) {
					sw_to_cursor_pos(i + 1, &r, &c);
					sprintf(buf, "\033[%d;%dH%c", r, c, switch_state[i]);
					str_cpy(buf, printing_buffer, &printing_index, TERM_A_BUFLEN, true);
				}
			}

			str_cpy(RESTORE_CURSOR, printing_buffer, &printing_index, sizeof(RESTORE_CURSOR) - 1);
			UART::Puts(addr.term_trans_tid, 0, printing_buffer, printing_index);
			printing_index = 0;
			str_cpy(SAVE_CURSOR, printing_buffer, &printing_index, sizeof(SAVE_CURSOR) - 1);

			if (isTrainStateModified) {
				isTrainStateModified = false;
				for (int i = 0; i < Train::NUM_TRAINS; ++i) {
					int train_num = Train::TRAIN_NUMBERS[i];
					for (int j = static_cast<int>(TrainUIReq::TrainUISpeedDir); j != static_cast<int>(TrainUIReq::DEFAULT); ++j) {
						etl::pair<int, int> pos = train_to_cursor_pos(train_num, static_cast<TrainUIReq>(j));
						int len = sprintf(buf, MOVE_CURSOR_F, pos.first, pos.second);
						str_cpy(buf, printing_buffer, &printing_index, len);

						switch (static_cast<TrainUIReq>(j)) {
						case TrainUIReq::TrainUISpeedDir: {
							int speed = train_state[i].speed;
							char dir = train_state[i].direction ? 'S' : 'R';

							long vel = global_train_info[i].velocity;
							sprintf(buf, TRAIN_PRINTOUT[j], speed, dir, relu(vel) / 100, relu(vel) % 100);
							break;
						}
						case TrainUIReq::TrainUINextPrev: {
							int next = global_train_info[i].next_sensor;
							int prev = global_train_info[i].prev_sensor;

							char nc = 'X', pc = 'X';
							int nnum = 0, pnum = 0;
							if (next != Planning::NO_SENSOR && next > 0 && next < Planning::TOTAL_SENSORS) {
								nc = SENSOR_LETTERS[next / Planning::SENSORS_PER_LETTER];
								nnum = (next % Planning::SENSORS_PER_LETTER) + 1;
							}

							if (prev != Planning::NO_SENSOR && prev > 0 && prev < Planning::TOTAL_SENSORS) {
								pc = SENSOR_LETTERS[prev / Planning::SENSORS_PER_LETTER];
								pnum = (prev % Planning::SENSORS_PER_LETTER) + 1;
							}

							sprintf(buf, TRAIN_PRINTOUT[j], nc, nnum, pc, pnum);
							break;
						}
						case TrainUIReq::TrainUITimeDist: {
							int t = global_train_info[i].time_to_next_sensor;
							int d = global_train_info[i].dist_to_next_sensor;

							sprintf(buf, TRAIN_PRINTOUT[j], relu(t) % FOUR_DIGITS, relu(d) % FOUR_DIGITS);
							break;
						}
						case TrainUIReq::TrainUISrcDst: {
							int src = global_train_info[i].path_src;
							int dst = global_train_info[i].path_dest;

							src = (src == Planning::NO_SENSOR) ? 0 : src;
							dst = (dst == Planning::NO_SENSOR) ? 0 : dst;

							// This should never happen, but you never know
							if (src < 0 || src > TRACK_MAX || dst < 0 || dst > TRACK_MAX) {
								Task::_KernelCrash("TrainUI: Invalid src/dst");
							}

							sprintf(buf, TRAIN_PRINTOUT[j], track[src].name, track[dst].name);
							break;
						}
						case TrainUIReq::TrainUIBarge: {
							int barge_count = global_train_info[i].barge_count;
							int barge_weight = global_train_info[i].barge_weight;

							sprintf(buf, TRAIN_PRINTOUT[j], barge_count, barge_weight % THREE_DIGITS);
							break;
						}
						default: {
							break;
						}

						} // switch
						str_cpy(buf, printing_buffer, &printing_index, TERM_A_BUFLEN, true);
					}

					str_cpy(RESTORE_CURSOR, printing_buffer, &printing_index, sizeof(RESTORE_CURSOR) - 1);
					UART::Puts(addr.term_trans_tid, 0, printing_buffer, printing_index);
					printing_index = 0;
					str_cpy(SAVE_CURSOR, printing_buffer, &printing_index, sizeof(SAVE_CURSOR) - 1);
				}
			}

			str_cpy(RESTORE_CURSOR, printing_buffer, &printing_index, sizeof(RESTORE_CURSOR) - 1);
			if (printing_index >= UART::UART_MESSAGE_LIMIT) {
				Task::_KernelCrash("Too much printing\r\n");
			}
			UART::Puts(addr.term_trans_tid, 0, printing_buffer, printing_index);
		}
	};

	while (true) {
		Receive::Receive(&from, reinterpret_cast<char*>(&req), sizeof(TerminalServerReq));
		switch (req.header) {
		case RequestHeader::TERM_CLOCK: {
			// 100ms clock update
			Reply::EmptyReply(from);
			ticks += 1;
			trigger_print();
			break;
		}
		case RequestHeader::TERM_SENSORS: {
			// Should be 10 bytes of sensor data.
			// Print out all the sensors, in a fancy UI way.
			Reply::EmptyReply(from);
			for (int i = 0; i < Sensor::NUM_SENSOR_BYTES; i++) {
				sensor_state[i] = req.body.worker_msg.msg[i];
			}
			isSensorModified = true;
			break;
		}
		case RequestHeader::TERM_IDLE: {
			Reply::EmptyReply(from);
			Clock::IdleStats(&idle_time, &total_time);
			isIdleTimeModified = true;
			break;
		}
		case RequestHeader::TERM_START: {
			Reply::EmptyReply(from);
			printing_index = 0;
			str_cpy(CLEAR_SCREEN, printing_buffer, &printing_index, sizeof(CLEAR_SCREEN) - 1);
			str_cpy(TOP_LEFT, printing_buffer, &printing_index, sizeof(TOP_LEFT) - 1);
			str_cpy(CYAN_CURSOR, printing_buffer, &printing_index, sizeof(CYAN_CURSOR) - 1);
			str_cpy(WELCOME_MSG, printing_buffer, &printing_index, sizeof(WELCOME_MSG) - 1);
			str_cpy(RED_CURSOR, printing_buffer, &printing_index, sizeof(RED_CURSOR) - 1);
			str_cpy(SENSOR_DATA, printing_buffer, &printing_index, sizeof(SENSOR_DATA) - 1);
			str_cpy(BLUE_CURSOR, printing_buffer, &printing_index, sizeof(WHITE_CURSOR) - 1);
			for (int i = 0; i < Terminal::SWITCH_UI_LEN; ++i) {
				str_cpy(Terminal::SWITCH_UI[i], printing_buffer, &printing_index, UART::UART_MESSAGE_LIMIT, true);
			}

			str_cpy(WHITE_CURSOR, printing_buffer, &printing_index, sizeof(WHITE_CURSOR) - 1);
			UART::Puts(addr.term_trans_tid, 0, printing_buffer, printing_index);
			printing_index = 0;

			str_cpy(SAVE_CURSOR_NO_JUMP, printing_buffer, &printing_index, sizeof(SAVE_CURSOR_NO_JUMP) - 1);
			for (int t = 0; t < Terminal::TRAIN_UI_LEN; ++t) {
				sprintf(buf, "\033[%d;%dH", TRAIN_PRINTOUT_ROW + t, TRAIN_PRINTOUT_COLUMN);
				str_cpy(buf, printing_buffer, &printing_index, TERM_A_BUFLEN, true);
				str_cpy(Terminal::TRAIN_UI[t], printing_buffer, &printing_index, UART::UART_MESSAGE_LIMIT, true);

				UART::Puts(addr.term_trans_tid, 0, printing_buffer, printing_index);
				printing_index = 0;
				Clock::Delay(addr.clock_tid, 2);
			}

			str_cpy(RESTORE_CURSOR, printing_buffer, &printing_index, sizeof(RESTORE_CURSOR) - 1);
			str_cpy(DELIMINATION, printing_buffer, &printing_index, sizeof(DELIMINATION) - 1);
			str_cpy(SAVE_CURSOR_NO_JUMP, printing_buffer, &printing_index, sizeof(SAVE_CURSOR_NO_JUMP) - 1);

			int len = sprintf(buf, SETUP_SCROLL, SCROLL_TOP, SCROLL_BOTTOM);
			str_cpy(buf, printing_buffer, &printing_index, len);
			str_cpy(RESTORE_CURSOR, printing_buffer, &printing_index, sizeof(RESTORE_CURSOR) - 1);
			str_cpy("\r\n", printing_buffer, &printing_index, 2);
			str_cpy(PROMPT, printing_buffer, &printing_index, sizeof(PROMPT) - 1);
			str_cpy("\r\n\r\n", printing_buffer, &printing_index, 4);
			str_cpy(DELIMINATION, printing_buffer, &printing_index, sizeof(DELIMINATION) - 1);
			str_cpy("\r\n", printing_buffer, &printing_index, 2);
			str_cpy(DEBUG_TITLE, printing_buffer, &printing_index, sizeof(DEBUG_TITLE) - 1);
			str_cpy(HIDE_CURSOR, printing_buffer, &printing_index, sizeof(HIDE_CURSOR) - 1);

			UART::Puts(addr.term_trans_tid, 0, printing_buffer, printing_index);
			isRunning = true;
			break;
		}
		case RequestHeader::TERM_DEBUG_START: {
			// Instead of the fancy start menu, just put a simple prompt '>'.
			Reply::EmptyReply(from);
			printing_index = 0;

			str_cpy(CLEAR_SCREEN, printing_buffer, &printing_index, sizeof(CLEAR_SCREEN) - 1);
			str_cpy(TOP_LEFT, printing_buffer, &printing_index, sizeof(TOP_LEFT) - 1);

			UART::Puts(addr.term_trans_tid, 0, printing_buffer, printing_index);
			isDebug = true;
			break;
		}
		case RequestHeader::TERM_REVERSE_COMPLETE: {
			courier_pool.receive(from);
			break;
		}
		case RequestHeader::TERM_LOCAL_COMPLETE: {
			courier_pool.receive(from);
			break;
		}
		case RequestHeader::TERM_SWITCH: {
			Reply::EmptyReply(from);
			for (uint64_t i = 0; i < sizeof(switch_state); i++) {
				switch_state[i] = req.body.worker_msg.msg[i];
			}
			isSwitchStateModified = true;
			break;
		}
		case RequestHeader::TERM_TRAIN_STATUS: {
			Reply::EmptyReply(from);
			Train::TrainRaw* body = reinterpret_cast<Train::TrainRaw*>(req.body.worker_msg.msg);
			for (int i = 0; i < Train::NUM_TRAINS; ++i) {
				isTrainStateModified = isTrainStateModified || (train_state[i] != body[i]);
				train_state[i] = body[i];
			}
			break;
		}
		case RequestHeader::TERM_TRAIN_STATUS_MORE: {
			Reply::EmptyReply(from);
			GlobalTrainInfo* body = req.body.train_info;
			for (int i = 0; i < TERM_NUM_TRAINS; ++i) {
				isTrainStateModified = isTrainStateModified || (global_train_info[i] != body[i]);
				global_train_info[i] = body[i];
			}
			break;
		}
		case RequestHeader::TERM_PUTC: {
			Reply::EmptyReply(from);
			char c = req.body.regular_msg;
			int result = 0;
			int printing_index = 0;

			if (!isDebug) {
				str_cpy(SAVE_CURSOR_NO_JUMP, printing_buffer, &printing_index, sizeof(SAVE_CURSOR_NO_JUMP) - 1);
				sprintf(buf, PROMPT_CURSOR, sizeof(PROMPT_NNL) + char_count);
				str_cpy(buf, printing_buffer, &printing_index, TERM_A_BUFLEN, true);
			}

			if (char_count > CMD_LEN) {
				sprintf(buf, "\033M\r%s", ERROR);
				str_cpy(buf, printing_buffer, &printing_index, TERM_A_BUFLEN, true);
				char_count = 0;

				if (!isDebug) {
					str_cpy(PROMPT_NNL, printing_buffer, &printing_index, sizeof(PROMPT_NNL) - 1);
				}
			} else if (escape_status == TAState::TA_FOUND_ESCAPE) {
				escape_status = (c == '[') ? TAState::TA_FOUND_BRACKET : TAState::TA_DEFAULT_ARROW_STATE;
			} else if (escape_status == TAState::TA_FOUND_BRACKET) {
				switch (c) {
				case 'A': { // up arrow
					if (cmd_history_index > 0) {
						cmd_history_index--;
						str_cpy("\r", printing_buffer, &printing_index, 1);
						if (char_count > 0) {
							str_cpy(SPACES, printing_buffer, &printing_index, char_count + sizeof(PROMPT_NNL) - 1);
							str_cpy("\r", printing_buffer, &printing_index, 1);
						}

						if (!isDebug) {
							str_cpy(PROMPT_NNL, printing_buffer, &printing_index, sizeof(PROMPT_NNL) - 1);
						}
						str_cpy(cmd_history[cmd_history_index].cmd, printing_buffer, &printing_index, cmd_history[cmd_history_index].len);
						char_count = cmd_history[cmd_history_index].len;
					}
					break;
				}
				case 'B': { // down arrow
					if (cmd_history_index < cmd_history.size() - 1) {
						cmd_history_index++;
						str_cpy("\r", printing_buffer, &printing_index, 1);
						if (char_count > 0) {
							str_cpy(SPACES, printing_buffer, &printing_index, char_count + sizeof(PROMPT_NNL) - 1);
							str_cpy("\r", printing_buffer, &printing_index, 1);
						}

						if (!isDebug) {
							str_cpy(PROMPT_NNL, printing_buffer, &printing_index, sizeof(PROMPT_NNL) - 1);
						}
						str_cpy(cmd_history[cmd_history_index].cmd, printing_buffer, &printing_index, cmd_history[cmd_history_index].len);

						char_count = cmd_history[cmd_history_index].len;
					}
					break;
				}
				case 'C': { // right arrow
					if (cmd_history[cmd_history_index].cmd[char_count] != '\0') {
						char_count++;
					}
					break;
				}
				case 'D': { // left arrow
					if (char_count > 0) {
						char_count--;
					}
					break;
				}
				default: {
					char buf_3[3] = { '\033', '[', c };
					str_cpy(buf_3, printing_buffer, &printing_index, 3);
				}
				} // switch

				escape_status = TAState::TA_DEFAULT_ARROW_STATE;
			} else if (c == '\b') {
				if (char_count > 0) {
					cmd_history[cmd_history_index].cmd[char_count] = '\0';
					char_count--;
					str_cpy("\b \b", printing_buffer, &printing_index, 3);
				}
			} else if (c == '\r') {
				cmd_history[cmd_history_index].cmd[char_count] = '\r';
				GenericCommand cmd_parsed = handle_generic(cmd_history[cmd_history_index].cmd);

				// Restore the cursor so functions can use debug printing unimpeded
				// UART::PutsNullTerm(addr.term_trans_tid, 0, RESTORE_CURSOR, sizeof(RESTORE_CURSOR) - 1);

				if (strncmp(cmd_parsed.name, "tr", MAX_COMMAND_LEN) == 0) {
					result = handle_tr(addr, cmd_history[cmd_history_index].cmd);
				} else if (strncmp(cmd_parsed.name, "rv", MAX_COMMAND_LEN) == 0) {
					result = handle_rv(courier_pool, cmd_history[cmd_history_index].cmd);
				} else if (strncmp(cmd_parsed.name, "sw", MAX_COMMAND_LEN) == 0) {
					result = handle_sw(addr, cmd_history[cmd_history_index].cmd);
				} else if (strncmp(cmd_parsed.name, "q", MAX_COMMAND_LEN) == 0) {
					int i = 1;
					while (cmd_history[cmd_history_index].cmd[i] == ' ' && i < CMD_LEN) {
						++i;
					}

					if (cmd_history[cmd_history_index].cmd[i] == '\r') {
						UART::Puts(addr.term_trans_tid, 0, QUIT, sizeof(QUIT) - 1);
						char command[2] = { 15, 0 };
						for (int k = 0; k < Train::NUM_TRAINS; ++k) {
							// Stop all trains
							sprintf(buf, "Stopping train %d\r\n", TRAIN_NUMBERS[k]);
							UART::PutsNullTerm(addr.term_trans_tid, 0, buf, TERM_A_BUFLEN);

							command[1] = TRAIN_NUMBERS[k];
							UART::Puts(addr.train_trans_tid, TRAIN_UART_CHANNEL, command, 2);
							// revert then revert back
							UART::Puts(addr.train_trans_tid, TRAIN_UART_CHANNEL, command, 2);
						}

						Clock::Delay(addr.clock_tid, 200); // 2 seconds
						restart();
					} else {
						result = HANDLE_FAIL;
					}
				} else if (strncmp(cmd_parsed.name, "clear", MAX_COMMAND_LEN) == 0) {
					int top = isDebug ? 1 : SCROLL_TOP;
					for (int r = SCROLL_BOTTOM; r >= top; --r) {
						int len = sprintf(buf, MOVE_CURSOR_F, r, 1);
						UART::Puts(addr.term_trans_tid, 0, buf, len);
						UART::Puts(addr.term_trans_tid, 0, CLEAR_LINE, sizeof(CLEAR_LINE) - 1);
						Clock::Delay(addr.clock_tid, 1);
					}
				} else if (!cmd_parsed.success) {
					result = HANDLE_FAIL;
				} else if (strncmp(cmd_parsed.name, "go", MAX_COMMAND_LEN) == 0) {
					result = handle_global_pathing(courier_pool, cmd_parsed, RequestHeader::TERM_COUR_LOCAL_GO);
				} else if (strncmp(cmd_parsed.name, "locate", MAX_COMMAND_LEN) == 0) {
					result = handle_global_pathing(courier_pool, cmd_parsed, RequestHeader::TERM_COUR_LOCAL_LOCATE);
				} else if (strncmp(cmd_parsed.name, "loop", MAX_COMMAND_LEN) == 0) {
					result = handle_global_pathing(courier_pool, cmd_parsed, RequestHeader::TERM_COUR_LOCAL_LOOP);
				} else if (strncmp(cmd_parsed.name, "exloop", MAX_COMMAND_LEN) == 0) {
					result = handle_global_pathing(courier_pool, cmd_parsed, RequestHeader::TERM_COUR_LOCAL_EXLOOP);
				} else if (strncmp(cmd_parsed.name, "init", MAX_COMMAND_LEN) == 0) {
					if (cmd_parsed.args.size() > 0) {
						which_track = 'a' + cmd_parsed.args.front() - 1;
					}

					result = handle_global_pathing(courier_pool, cmd_parsed, RequestHeader::TERM_COUR_LOCAL_INIT);
					if (result != HANDLE_FAIL && which_track == 'a') {
						init_tracka(track);
					} else if (result != HANDLE_FAIL && which_track == 'b') {
						init_trackb(track);
					}

				} else if (strncmp(cmd_parsed.name, "cali", MAX_COMMAND_LEN) == 0) {
					result = handle_global_pathing(courier_pool, cmd_parsed, RequestHeader::TERM_COUR_LOCAL_CALI); // not working
				} else if (strncmp(cmd_parsed.name, "base", MAX_COMMAND_LEN) == 0) {
					result = handle_global_pathing(courier_pool, cmd_parsed, RequestHeader::TERM_COUR_LOCAL_CALI_BASE_SPEED);
				} else if (strncmp(cmd_parsed.name, "accele", MAX_COMMAND_LEN) == 0) {
					result = handle_global_pathing(courier_pool, cmd_parsed, RequestHeader::TERM_COUR_LOCAL_CALI_ACCELERATION);
				} else if (strncmp(cmd_parsed.name, "sdist", MAX_COMMAND_LEN) == 0) {
					result = handle_global_pathing(courier_pool, cmd_parsed, RequestHeader::TERM_COUR_LOCAL_CALI_STOPPING_DIST);
				} else if (strncmp(cmd_parsed.name, "dest", MAX_COMMAND_LEN) == 0) {
					result = handle_global_pathing(courier_pool, cmd_parsed, RequestHeader::TERM_COUR_LOCAL_DEST);
				} else if (strncmp(cmd_parsed.name, "rng", MAX_COMMAND_LEN) == 0) {
					result = handle_global_pathing(courier_pool, cmd_parsed, RequestHeader::TERM_COUR_LOCAL_RNG);
				} else if (strncmp(cmd_parsed.name, "bund", MAX_COMMAND_LEN) == 0) {
					result = handle_global_pathing(courier_pool, cmd_parsed, RequestHeader::TERM_COUR_LOCAL_BUN_DIST);
				} else {
					result = HANDLE_FAIL;
				}

				cmd_history[cmd_history_index].len = char_count;
				char_count = 0;
				cmd_history.push(TerminalCommand { { 0 }, 0 });
				if (cmd_history_index < cmd_history.max_size() - 1) {
					cmd_history_index++;
				}

				if (!isDebug) {
					if (result == HANDLE_FAIL) {
						sprintf(buf, "\033M\r%s", ERROR);
						str_cpy(buf, printing_buffer, &printing_index, TERM_A_BUFLEN, true);
					} else {
						sprintf(buf, "\033M\r%s\r\n", CLEAR_LINE);
						str_cpy(buf, printing_buffer, &printing_index, TERM_A_BUFLEN, true);
					}

					sprintf(buf, "%s%s", CLEAR_LINE, PROMPT_NNL);
					str_cpy(buf, printing_buffer, &printing_index, TERM_A_BUFLEN, true);
				} else {
					const char* s = (result == HANDLE_FAIL) ? ERROR : "";
					sprintf(buf, "%s\r\n", s);
					str_cpy(buf, printing_buffer, &printing_index, TERM_A_BUFLEN, true);
				}
			} else if (c == '\033') {
				// Escape sequence. Try to read an arrow key.
				escape_status = TAState::TA_FOUND_ESCAPE;
			} else {
				cmd_history[cmd_history_index].cmd[char_count++] = c;
				str_cpy(&c, printing_buffer, &printing_index, 1);
			}

			if (!isDebug) {
				str_cpy(RESTORE_CURSOR, printing_buffer, &printing_index, sizeof(RESTORE_CURSOR) - 1);
			}

			UART::Puts(addr.term_trans_tid, 0, printing_buffer, printing_index);
			break;
		}
		default: {
			Task::_KernelCrash("Illegal command passed to terminal admin: [%d]\r\n", req.header);
		}
		} // switch
	}
}

void Terminal::terminal_courier() {

	AddressBook addr = Message::getAddressBook();
	Terminal::TerminalCourierMessage req;
	Terminal::TerminalServerReq req_to_admin;
	Train::TrainAdminReq req_to_train;
	LocalPathing::LocalPathingReq req_to_local_pathing;
	Planning::PlanningServerReq req_to_global;

	auto local_server_redirect = [&](RequestHeader header) {
		req_to_local_pathing.header = header;
		req_to_local_pathing.body.command.num_args = req.body.courier_body.num_args - 1;
		for (uint32_t i = 1; i < req.body.courier_body.num_args; ++i) {
			req_to_local_pathing.body.command.args[i - 1] = req.body.courier_body.args[i];
		}

		int train = req.body.courier_body.args[0];
		int index = Train::train_num_to_index(train);
		int local_pathing_tid = addr.local_pathing_tids[index];
		Send::SendNoReply(local_pathing_tid, reinterpret_cast<char*>(&req_to_local_pathing), sizeof(req_to_local_pathing));

		req_to_admin = { RequestHeader::TERM_LOCAL_COMPLETE, '0' };
		Send::SendNoReply(addr.terminal_admin_tid, reinterpret_cast<char*>(&req_to_admin), sizeof(req_to_admin));
	};

	int from;
	while (true) {
		Receive::Receive(&from, reinterpret_cast<char*>(&req), sizeof(TerminalCourierMessage));
		Reply::EmptyReply(from); // unblock caller right away
		switch (req.header) {
		case RequestHeader::TERM_COUR_REV: {
			// it wait for about 4 seconds then send in the command to reverse and speed up
			req_to_train.header = RequestHeader::TRAIN_REV;
			req_to_train.body.command.id = req.body.regular_body;
			Send::SendNoReply(addr.train_admin_tid, reinterpret_cast<char*>(&req_to_train), sizeof(req_to_train));
			req_to_admin = { RequestHeader::TERM_REVERSE_COMPLETE, '0' };
			Send::SendNoReply(addr.terminal_admin_tid, reinterpret_cast<char*>(&req_to_admin), sizeof(req_to_admin));
			break;
		}
		case RequestHeader::TERM_COUR_LOCAL_GO: {
			local_server_redirect(RequestHeader::LOCAL_PATH_SET_PATH);
			break;
		}
		case RequestHeader::TERM_COUR_LOCAL_LOCATE: {
			local_server_redirect(RequestHeader::LOCAL_PATH_LOCATE);
			break;
		}
		case RequestHeader::TERM_COUR_LOCAL_LOOP: {
			local_server_redirect(RequestHeader::LOCAL_PATH_LOOP);
			break;
		}
		case RequestHeader::TERM_COUR_LOCAL_EXLOOP: {
			local_server_redirect(RequestHeader::LOCAL_PATH_EXLOOP);
			break;
		}
		case RequestHeader::TERM_COUR_LOCAL_INIT: {
			req_to_global.header = Message::RequestHeader::GLOBAL_SET_TRACK;
			req_to_global.body.info = req.body.courier_body.args[0];
			Send::SendNoReply(addr.global_pathing_tid, reinterpret_cast<char*>(&req_to_global), sizeof(req_to_global));
			req_to_admin = { RequestHeader::TERM_LOCAL_COMPLETE, '0' };
			Send::SendNoReply(addr.terminal_admin_tid, reinterpret_cast<char*>(&req_to_admin), sizeof(req_to_admin));
			break;
		}
		case RequestHeader::TERM_COUR_LOCAL_CALI: {
			local_server_redirect(RequestHeader::LOCAL_PATH_CALI);
			break;
		}
		case RequestHeader::TERM_COUR_LOCAL_CALI_BASE_SPEED: {
			local_server_redirect(RequestHeader::LOCAL_PATH_CALI_BASE_SPEED);
			break;
		}
		case RequestHeader::TERM_COUR_LOCAL_CALI_ACCELERATION: {
			local_server_redirect(RequestHeader::LOCAL_PATH_CALI_ACCELERATION);
			break;
		}
		case RequestHeader::TERM_COUR_LOCAL_CALI_STOPPING_DIST: {
			local_server_redirect(RequestHeader::LOCAL_PATH_CALI_STOPPING_DISTANCE);
			break;
		}
		case RequestHeader::TERM_COUR_LOCAL_DEST: {
			local_server_redirect(RequestHeader::LOCAL_PATH_DEST);
			break;
		}
		case RequestHeader::TERM_COUR_LOCAL_RNG: {
			local_server_redirect(RequestHeader::LOCAL_PATH_RNG);
			break;
		}
		case RequestHeader::TERM_COUR_LOCAL_BUN_DIST: {
			local_server_redirect(RequestHeader::LOCAL_PATH_BUNNY_DIST);
			break;
		}
		default: {
			Task::_KernelCrash("Term_A Train Courier illegal type: [%d]\r\n", req.header);
		}
		} // switch
	}
}

void Terminal::terminal_clock_courier() {
	Name::RegisterAs(TERMINAL_CLOCK_COURIER_NAME);

	int repeat = CLOCK_UPDATE_FREQUENCY;
	int clock_tid = Name::WhoIs(Clock::CLOCK_SERVER_NAME);
	int terminal_tid = Name::WhoIs(Terminal::TERMINAL_ADMIN);
	int internal_timer = Clock::Time(clock_tid);
	Terminal::TerminalServerReq req = Terminal::TerminalServerReq(RequestHeader::TERM_CLOCK, internal_timer);

	while (true) {
		Send::SendNoReply(terminal_tid, reinterpret_cast<char*>(&req), sizeof(Terminal::TerminalServerReq));
		internal_timer += repeat;
		Clock::DelayUntil(clock_tid, internal_timer);
	}
}

void Terminal::sensor_query_courier() {
	Name::RegisterAs(TERMINAL_SENSOR_COURIER_NAME);

	int sensor_admin = Name::WhoIs(Sensor::SENSOR_ADMIN_NAME);
	int terminal_tid = Name::WhoIs(Terminal::TERMINAL_ADMIN);
	Sensor::SensorAdminReq req;
	req.header = RequestHeader::SENSOR_AWAIT_STATE;

	Terminal::TerminalServerReq treq;
	treq.header = RequestHeader::TERM_SENSORS;
	while (true) {
		Send::Send(sensor_admin, reinterpret_cast<char*>(&req), sizeof(Sensor::SensorAdminReq), treq.body.worker_msg.msg, Sensor::NUM_SENSOR_BYTES);
		Send::SendNoReply(terminal_tid, reinterpret_cast<char*>(&treq), sizeof(Terminal::TerminalServerReq));
	}
}

void Terminal::idle_time_courier() {
	// technically idle_task will never need to yield to kernel
	// since there is no context swtich, only interrupt after this point
	int clock_tid = Name::WhoIs(Clock::CLOCK_SERVER_NAME);
	int terminal_tid = Name::WhoIs(Terminal::TERMINAL_ADMIN);

	Terminal::TerminalServerReq treq;
	treq.header = RequestHeader::TERM_IDLE;

	while (true) {
		Send::SendNoReply(terminal_tid, reinterpret_cast<char*>(&treq), sizeof(Terminal::TerminalServerReq));
		Clock::Delay(clock_tid, 200);
	}
}

void Terminal::user_input_courier() {
	Terminal::TerminalServerReq treq;
	treq.header = RequestHeader::TERM_START;

	int terminal_tid = Name::WhoIs(Terminal::TERMINAL_ADMIN);

	char c = UART::Getc(UART::UART_0_RECEIVER_TID, 0);
	if (c == 'D' || c == 'd') {
		treq.header = RequestHeader::TERM_DEBUG_START;
	}

	Send::SendNoReply(terminal_tid, reinterpret_cast<char*>(&treq), sizeof(Terminal::TerminalServerReq));
	treq.header = RequestHeader::TERM_PUTC;
	while (true) {
		treq.body.regular_msg = UART::Getc(UART::UART_0_RECEIVER_TID, 0);
		Send::SendNoReply(terminal_tid, reinterpret_cast<char*>(&treq), sizeof(Terminal::TerminalServerReq));
	}
}

void Terminal::switch_state_courier() {
	Terminal::TerminalServerReq req_to_terminal;
	Track::TrackServerReq req_to_track = {};
	AddressBook addr = getAddressBook();

	req_to_track.header = RequestHeader::TRACK_SWITCH_SUBSCRIBE;
	req_to_terminal.header = RequestHeader::TERM_SWITCH;
	req_to_terminal.body.worker_msg.msg_len = Train::NUM_SWITCHES;
	int update_frequency = 100; // update once a second
	while (true) {
		Send::Send(addr.track_server_tid,
				   reinterpret_cast<char*>(&req_to_track),
				   sizeof(req_to_track),
				   req_to_terminal.body.worker_msg.msg,
				   req_to_terminal.body.worker_msg.msg_len);
		Send::SendNoReply(addr.terminal_admin_tid, reinterpret_cast<char*>(&req_to_terminal), sizeof(Terminal::TerminalServerReq));
		Clock::Delay(addr.clock_tid, update_frequency);
	}
}

void Terminal::train_state_courier() {
	Terminal::TerminalServerReq req_to_terminal;
	Train::TrainAdminReq req_to_train;
	Planning::PlanningServerReq req_to_global_planning;
	AddressBook addr = getAddressBook();

	req_to_train.header = RequestHeader::TRAIN_OBSERVE;
	req_to_global_planning.header = RequestHeader::GLOBAL_OBSERVE;
	int update_frequency = 100; // update once a second
	while (true) {
		req_to_terminal.header = RequestHeader::TERM_TRAIN_STATUS;
		req_to_terminal.body.worker_msg.msg_len = Train::NUM_TRAINS * sizeof(Train::TrainRaw);
		Send::Send(addr.train_admin_tid,
				   reinterpret_cast<char*>(&req_to_train),
				   sizeof(Train::TrainAdminReq),
				   req_to_terminal.body.worker_msg.msg,
				   req_to_terminal.body.worker_msg.msg_len);
		Send::SendNoReply(addr.terminal_admin_tid, reinterpret_cast<char*>(&req_to_terminal), sizeof(Terminal::TerminalServerReq));

		req_to_terminal.header = RequestHeader::TERM_TRAIN_STATUS_MORE;
		Send::Send(addr.global_pathing_tid,
				   reinterpret_cast<char*>(&req_to_global_planning),
				   sizeof(Planning::PlanningServerReq),
				   reinterpret_cast<char*>(&req_to_terminal.body),
				   sizeof(RequestBody));
		Send::SendNoReply(addr.terminal_admin_tid, reinterpret_cast<char*>(&req_to_terminal), sizeof(Terminal::TerminalServerReq));
		Clock::Delay(addr.clock_tid, update_frequency);
	}
}