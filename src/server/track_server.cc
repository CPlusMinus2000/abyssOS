
#include "track_server.h"
#include "../etl/queue.h"
#include "../etl/unordered_set.h"
#include "../routing/track_data_new.h"
#include "train_admin.h"
using namespace Track;
using namespace Message;
using namespace Routing;
using namespace Task;

char get_switch_index_to_id(int index) {
	if (0 <= index && index <= 17) {
		return '\0' + index + 1;
	} else {
		return '\0' + index - 18 + 153;
	}
}

int get_switch_id_to_index(int id) {
	if (1 <= id && id <= 18) {
		return id - 1;
	} else if (153 <= id && id <= 156) {
		return id - 153 + 18;
	} else {
		return 0;
	}
}

char get_rev_switch_id(char id) {
	if (id == 153)
		return 154;
	else if (id == 154)
		return 153;
	else if (id == 155)
		return 156;
	else if (id == 156)
		return 155;
	else
		_KernelCrash("invalid reverse id");
	return 0;
}

char get_rev_switch_dir(char dir) {
	return dir == 's' ? 'c' : 's';
}

void Track::track_server() {
	Name::RegisterAs(TRACK_SERVER_NAME);
	Courier::CourierPool<TrackCourierReq, 32> courier_pool = Courier::CourierPool<TrackCourierReq, 32>(&track_courier, Priority::HIGH_PRIORITY);
	AddressBook addr = getAddressBook();

	track_node track[TRACK_MAX];
	etl::unordered_set<int, TRACK_MAX> train_wanted_nodes[Train::NUM_TRAINS];
	init_tracka(track);
	Dijkstra dijkstra = Dijkstra(track);
	char switch_state[NUM_SWITCHES];
	for (int i = 0; i < NUM_SWITCHES; i++) {
		switch_state[i] = '\0';
	}
	etl::queue<int, 4> switch_subscriber;

	/**
	 * 3 responsibilities
	 * 1. initialize the track as needed (on demand if neccessary)
	 * 2. provide routing through dijkstra
	 * 3. provide reservation for global routing
	 */

	TrackCourierReq req_to_courier = {};
	auto pipe_sw = [&](char id, char dir) {
		if (switch_state[get_switch_id_to_index(id)] != dir) {
			switch_state[get_switch_id_to_index(id)] = dir;
			req_to_courier.header = RequestHeader::TRACK_COUR_SWITCH;
			req_to_courier.body.command.id = id;
			req_to_courier.body.command.action = dir;
			courier_pool.request(&req_to_courier);
			if (153 <= id && id <= 156) {
				char rev_id = get_rev_switch_id(id);
				char rev_dir = get_rev_switch_dir(dir);
				switch_state[get_switch_id_to_index(rev_id)] = rev_dir;
				req_to_courier.header = RequestHeader::TRACK_COUR_SWITCH;
				req_to_courier.body.command.id = rev_id;
				req_to_courier.body.command.action = rev_dir;
				courier_pool.request(&req_to_courier);
			}
			return true;
		} else {
			return false;
		}
	};

	// this is all hard coded, changes based on the condition of the track
	auto tracka_initialization_sequence = [&]() {
		init_tracka(track); // default configuration is part a
		dijkstra = Dijkstra(track);
		char new_switch_state[NUM_SWITCHES];

		for (int i = 0; i < NUM_SWITCHES; i++) {
			new_switch_state[i] = 'c';
		}
		new_switch_state[18] = 's';
		new_switch_state[20] = 's';
		for (int i = 0; i < NUM_SWITCHES; i++) {
			pipe_sw(get_switch_index_to_id(i), new_switch_state[i]);
		}
	};

	auto trackb_initialization_sequence = [&]() {
		init_trackb(track); // default configuration is part a
		dijkstra = Dijkstra(track);
		char new_switch_state[NUM_SWITCHES];
		for (int i = 0; i < NUM_SWITCHES; i++) {
			new_switch_state[i] = 's';
		}
		new_switch_state[4] = 'c';
		new_switch_state[6] = 'c';
		new_switch_state[18] = 'c';
		new_switch_state[20] = 'c';
		for (int i = 0; i < NUM_SWITCHES; i++) {
			pipe_sw(get_switch_index_to_id(i), new_switch_state[i]);
		}
	};

	auto reply_to_switch_subs = [&]() {
		while (!switch_subscriber.empty()) {
			Message::Reply::Reply(switch_subscriber.front(), switch_state, sizeof(switch_state));
			switch_subscriber.pop();
		}
	};

	auto can_reserve = [&](track_node* node, int reserver_id) {
		if (node->reserved_by == RESERVED_BY_NO_ONE || node->reserved_by == reserver_id) {
			return true;
		} else {
			return false;
		}
	};

	auto reserve = [&](track_node& node, int reserver_id) {
		node.reserved_by = reserver_id;
		node.reserve_dir = DIRECT_RESERVE;
		node.reverse->reserved_by = reserver_id;
		node.reverse->reserve_dir = REVERSE_RESERVE;
	};

	auto cancel_reserve = [&](track_node& node, int reserver_id) {
		if (node.reserved_by == RESERVED_BY_NO_ONE || node.reserved_by != reserver_id) {
			Task::_KernelCrash("try to un-reserve a path that doesn't belone to you %d %s\r\n", reserver_id, node.name);
		}
		node.reserved_by = RESERVED_BY_NO_ONE;
		node.reserve_dir = RESERVED_BY_NO_ONE;
		node.reverse->reserved_by = RESERVED_BY_NO_ONE;
		node.reverse->reserve_dir = RESERVED_BY_NO_ONE;
	};

	auto branch_safety = [&](track_node* node, int id) {
		if (!can_reserve(node->edge[DIR_CURVED].dest, id)) {
			return node->edge[DIR_CURVED].dest->index;
		} else if (!can_reserve(node->edge[DIR_STRAIGHT].dest, id)) {
			return node->edge[DIR_STRAIGHT].dest->index;
		}
		return -1;
	};

	// check all 4 branches !!!!
	auto central_branch_safety = [&](int id) {
		int result = -1;
		result = branch_safety(&track[116], id);
		if (result != -1) {
			return result;
		}
		result = branch_safety(&track[118], id);
		if (result != -1) {
			return result;
		}
		result = branch_safety(&track[120], id);
		if (result != -1) {
			return result;
		}
		result = branch_safety(&track[122], id);
		if (result != -1) {
			return result;
		}
		return -1;
	};

	auto detect_deadlock = [&](track_node* node, int id) {
		if (node->reserved_by == RESERVED_BY_NO_ONE) {
			_KernelCrash("A node reserve by no one is causing deadlock %s", node->name);
		}
		int current_owner = node->reserved_by;
		debug_print(addr.term_trans_tid, "trying to detect deadlock for %d, current_owener %d ", id, current_owner);

		for (auto it = train_wanted_nodes[Train::train_num_to_index(current_owner)].begin();
			 it != train_wanted_nodes[Train::train_num_to_index(current_owner)].end();
			 it++) {
			debug_print(addr.term_trans_tid, "%s : %d, ",track[*it].name, track[*it].reserved_by);
		}
		debug_print(addr.term_trans_tid, "\r\n");

		for (auto it = train_wanted_nodes[Train::train_num_to_index(current_owner)].begin();
			 it != train_wanted_nodes[Train::train_num_to_index(current_owner)].end();
			 it++) {
			if (track[*it].reserved_by == id || track[*it].reverse->reserved_by == id) {
				debug_print(addr.term_trans_tid, "\r\n");
				return current_owner;
			}
		}
		return -1;
	};

	auto evaluate_robustness_failed = [&](ReservationStatus& res, track_node* node, int id) {
		train_wanted_nodes[Train::train_num_to_index(id)].insert(node->index);
		if (!can_reserve(node, id)) {
			int deadlock_other_id = detect_deadlock(node, id);
			if (deadlock_other_id != -1) {
				res.dead_lock_detected = true;
			}
			res.successful = false;
			return true;
		}
		int node_index = -1;
		// in the case of branching, you need to check both branch as well, yes, you will have one more redundent check, but that is fine
		if (node->type == node_type::NODE_BRANCH) {
			node_index = branch_safety(node, id);
		}
		// similarly, if it is a merge, it need to check if the reverse is fine
		else if (node->type == node_type::NODE_MERGE) {
			node_index = branch_safety(node->reverse, id);
		}

		if (node_index != -1) {
			int deadlock_other_id = detect_deadlock(&track[node_index], id);
			if (deadlock_other_id != -1) {
				res.dead_lock_detected = true;
			}
			res.successful = false;
			return true;
		}
		// robustness, central rail must be all clear (too much edge cases, so easy solution is only let 1 train go through)
		if (node->num == 154 || node->num == 153 || node->num == 155 || node->num == 156) {
			node_index = central_branch_safety(id);
			if (node_index != -1) {
				train_wanted_nodes[Train::train_num_to_index(id)].insert(116);
				train_wanted_nodes[Train::train_num_to_index(id)].insert(118);
				train_wanted_nodes[Train::train_num_to_index(id)].insert(120);
				train_wanted_nodes[Train::train_num_to_index(id)].insert(122);

				int deadlock_other_id = detect_deadlock(&track[node_index], id);
				if (deadlock_other_id != -1) {
					res.dead_lock_detected = true;
				}
				res.successful = false;
				return true;
			}
		}
		return false;
	};

	int from;
	TrackServerReq req = {};
	while (true) {
		Message::Receive::Receive(&from, (char*)&req, sizeof(TrackServerReq));
		switch (req.header) {
		case RequestHeader::TRACK_INIT: {
			if (req.body.info == TRACK_A_ID) {
				tracka_initialization_sequence();
			} else if (req.body.info == TRACK_B_ID) {
				trackb_initialization_sequence();
			} else {
				_KernelCrash("trying to set the state of the track into impossible setting %d", req.body.info);
			}
			reply_to_switch_subs();
			Reply::EmptyReply(from);
			break;
		}
		case RequestHeader::TRACK_GET_SWITCH_STATE: {
			Message::Reply::Reply(from, switch_state, sizeof(switch_state));
			break;
		}
		case RequestHeader::TRACK_RNG: {
			int source = req.body.start_and_end.start;
			int dest = dijkstra.random_sensor_dest(source);
			PathRespond res;
			res.source = source;
			res.dest = dest;
			Reply::Reply(from, (const char*)&res, sizeof(res));
			break;
		}
		case RequestHeader::TRACK_SWITCH: {
			char id = req.body.command.id;
			char dir = req.body.command.action;
			Reply::EmptyReply(from);

			if (pipe_sw(id, dir)) {
				reply_to_switch_subs();
			}
			break;
		}
		case RequestHeader::TRACK_GET_PATH: {
			/**
			 * Return a somewhat optimized path,
			 * right not just basic dijkstra, but should be better in the future
			 * This should be a NON-BLOCKING call the respond right away
			 */
			int source = req.body.start_and_end.start;
			int dest = req.body.start_and_end.end;
			bool reverse_allowed = req.body.start_and_end.allow_reverse;
			etl::unordered_set<int, TRACK_MAX> banned_node;
			for (int i = 0; i < req.body.start_and_end.banned_len; i++) {
				banned_node.insert(req.body.start_and_end.banned[i]);
			}

			if (reverse_allowed) {
				PathRespond res;
				res.reverse = false;
				res.successful = dijkstra.weighted_path_with_ban(res.path, banned_node, &res.reverse, &res.rev_offset, &dest, source, dest);
				res.path_len = dijkstra.get_dist(dest);
				res.source = source;
				res.dest = dest;
				Reply::Reply(from, (const char*)&res, sizeof(res));
			} else {
				PathRespond res;
				res.reverse = false;
				res.successful = dijkstra.path(res.path, source, dest);
				res.path_len = dijkstra.get_dist(dest);
				res.source = source;
				res.dest = dest;
				Reply::Reply(from, (const char*)&res, sizeof(res));
			}
			break;
		}

		case RequestHeader::TRACK_UNRESERVE: {
			int len = req.body.reservation.len;
			int* path = req.body.reservation.path;
			int id = req.body.reservation.train_id;
			debug_print(addr.term_trans_tid, "%d trying to unreserve: ", id);
			for (int i = 0; i < len; i++) {
				cancel_reserve(track[path[i]], id);
				debug_print(addr.term_trans_tid, "%s ", track[path[i]].name);
			}
			debug_print(addr.term_trans_tid, "\r\n");

			Reply::EmptyReply(from);
			break;
		}

		case RequestHeader::TRACK_TRY_RESERVE: {
			int len = req.body.reservation.len;
			int* path = req.body.reservation.path;
			int id = req.body.reservation.train_id;
			ReservationStatus res;
			res.successful = true;
			res.dead_lock_detected = false;
			res.res_dist = 0;
			// try to reserve
			train_wanted_nodes[Train::train_num_to_index(id)].clear();
			debug_print(addr.term_trans_tid, "%d trying to reserve: ", id);
			for (int i = 0; i < len; i++) {
				debug_print(addr.term_trans_tid, "%s ", track[path[i]].name);
			}
			debug_print(addr.term_trans_tid, "\r\n");

			for (int i = 0; i < len; i++) {
				if (evaluate_robustness_failed(res, &track[path[i]], id))
					break;
			}
			debug_print(addr.term_trans_tid, "\r\n");

			// also check safe distance ahead
			uint64_t safety_distance = 0;
			track_node* next_node = &track[path[len - 1]];

			while (res.successful && safety_distance < SAFETY_DISTANCE) {

				if (next_node->type == node_type::NODE_MERGE || next_node->type == node_type::NODE_SENSOR) {
					// merge node simply ignore and add the length
					safety_distance += next_node->edge[DIR_AHEAD].dist;
					next_node = next_node->edge[DIR_AHEAD].dest;
				} else if (next_node->type == node_type::NODE_BRANCH) {
					int switch_index = get_switch_id_to_index(next_node->num);
					if (switch_state[switch_index] == 's') {
						safety_distance += next_node->edge[DIR_STRAIGHT].dist;
						next_node = next_node->edge[DIR_STRAIGHT].dest;
					} else if (switch_state[switch_index] == 'c') {
						safety_distance += next_node->edge[DIR_CURVED].dist;
						next_node = next_node->edge[DIR_CURVED].dest;
					} else {
						Task::_KernelCrash(
							"impossible path passed from try reserve %d %s %d\r\n", switch_index, next_node->name, switch_state[switch_index]);
					}
				} else {
					// you have to be node end, break
					break;
				}

				if (evaluate_robustness_failed(res, next_node, id))
					break;
			}

			debug_print(addr.term_trans_tid, "%d reserve is successful: %d\r\n", id, res.successful);
			// if you can reserve, then reserve
			if (res.successful) {
				train_wanted_nodes[Train::train_num_to_index(id)].clear();
				for (int i = 0; i < len; i++) {
					track_node* node = &track[path[i]];
					reserve(track[path[i]], id);
					if (node->type == node_type::NODE_BRANCH) {
						track_node* next_node = &track[path[i + 1]];
						if (next_node == node->edge[DIR_STRAIGHT].dest) {
							pipe_sw(node->num, 's');
							res.res_dist += node->edge[DIR_STRAIGHT].dist;
						} else if (next_node == node->edge[DIR_CURVED].dest) {
							pipe_sw(node->num, 'c');
							res.res_dist += node->edge[DIR_CURVED].dist;
						} else {
							Task::_KernelCrash("impossible condition met, somehow there is no next node to inspect in track_server\r\n");
						}
					} else if (i != (len - 1)) {
						res.res_dist += node->edge[DIR_AHEAD].dist;
					}
				}
				reply_to_switch_subs();
			}
			if (res.dead_lock_detected) {
				debug_print(addr.term_trans_tid, "detected deadlock for train %d ! \r\n", id);
			}
			// return the reservation result
			Reply::Reply(from, (const char*)&res, sizeof(res));
			break;
		}
		case RequestHeader::TRACK_COURIER_COMPLETE: {
			courier_pool.receive(from);
			break;
		}
		case RequestHeader::TRACK_SWITCH_SUBSCRIBE: {
			switch_subscriber.push(from);
			break;
		}
		default: {
			Task::_KernelCrash("Track Server illegal type: [%d]\r\n", req.header);
		}
		} // switch
	}

	Exit();
}

void Track::track_courier() {
	AddressBook addr = getAddressBook();

	int from;
	TrackCourierReq req;
	TrackServerReq req_to_admin;
	Train::TrainAdminReq req_to_train;

	while (true) {
		Message::Receive::Receive(&from, (char*)&req, sizeof(req));
		Message::Reply::EmptyReply(from); // unblock caller right away
		switch (req.header) {
		case RequestHeader::TRACK_COUR_SWITCH: {
			req_to_admin.header = RequestHeader::TRACK_COURIER_COMPLETE;
			req_to_train.header = RequestHeader::TRAIN_SWITCH;
			req_to_train.body.command.id = req.body.command.id;
			req_to_train.body.command.action = req.body.command.action;
			Send::SendNoReply(addr.train_admin_tid, reinterpret_cast<char*>(&req_to_train), sizeof(req_to_train));
			Send::SendNoReply(addr.track_server_tid, (const char*)&req_to_admin, sizeof(req_to_admin));
			break;
		}
		default:
			Task::_KernelCrash("Track Courier illegal type: [%d]\r\n", req.header);
		} // switch
	}
}