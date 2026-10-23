// Native test for src/inventory_planner.h: random inventories, planned and then
// replayed against the server rules (pick needs an empty cursor; place needs
// free cells; swap needs exactly one overlapped item).
//
// usage: planner_test [sort|compact][+locked] [cases-per-fill] [columns rows]   success rates and move counts
//        planner_test show                             example layouts for both modes

#include "../src/inventory_planner.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

using namespace InventoryPlanner;

// Grid size; the base game has 10 x 4. Arguments 3 and 4 change it.
static int Columns = 10;
static int Rows    = 4;

struct Size {
	int w, h;
};

static constexpr Size Sizes[] = { { 2, 4 }, { 2, 3 }, { 2, 2 }, { 1, 3 }, { 1, 2 }, { 2, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 } };

static auto Scatter(std::mt19937& rng, double fraction, Item* items) -> int {
	bool      used[MaxSide][MaxSide] {};
	int       count = 0, area = 0;
	const int goal  = static_cast<int>(Columns * Rows * fraction);
	for (int tries = 0; tries < 4'000 && area < goal; ++tries) {
		const Size s = Sizes[rng() % (sizeof(Sizes) / sizeof(Sizes[0]))];
		const int  x = static_cast<int>(rng() % (Columns - s.w + 1)), y = static_cast<int>(rng() % (Rows - s.h + 1));
		if (area + s.w * s.h > goal) {
			continue;
		}
		bool free = true;
		for (int cx = x; cx < x + s.w; ++cx) {
			for (int cy = y; cy < y + s.h; ++cy) {
				free = free && !used[cx][cy];
			}
		}
		if (!free) {
			continue;
		}
		for (int cx = x; cx < x + s.w; ++cx) {
			for (int cy = y; cy < y + s.h; ++cy) {
				used[cx][cy] = true;
			}
		}
		items[count] = { static_cast<uint32_t>(count + 1), static_cast<uint32_t>(rng() % 8), static_cast<int>(rng() % 4), 0, x, y, s.w, s.h };
		++count;
		area += s.w * s.h;
	}
	return count;
}

// Replays moves under the server rules and writes where the items end up.
// Returns an error or nullptr.
static auto Replay(const Item* items, int count, const std::vector<Move>& moves, int* x, int* y) -> const char* {
	bool inGrid[MaxItems];
	for (int k = 0; k < count; ++k) {
		x[k] = items[k].x, y[k] = items[k].y, inGrid[k] = true;
	}
	int        hand    = -1;
	const auto overlap = [&](int a, int ax, int ay, int b) {
		return ax < x[b] + items[b].width && x[b] < ax + items[a].width && ay < y[b] + items[b].height && y[b] < ay + items[a].height;
	};
	for (const Move& m : moves) {
		switch (m.kind) {
		case MoveKind::Pick:
			if (hand >= 0 || !inGrid[m.item]) {
				return "pick with a busy cursor or an item not in the grid";
			}
			if (items[m.item].locked) {
				return "pick of a locked item";
			}
			hand = m.item, inGrid[m.item] = false;
			break;
		case MoveKind::Place:
		case MoveKind::Swap: {
			if (hand != m.item) {
				return "drop of an item that is not held";
			}
			if (m.x + items[hand].width > Columns || m.y + items[hand].height > Rows) {
				return "drop out of bounds";
			}
			int hits = 0, hit = -1;
			for (int k = 0; k < count; ++k) {
				if (inGrid[k] && overlap(hand, m.x, m.y, k)) {
					++hits, hit = k;
				}
			}
			if (m.kind == MoveKind::Place && hits != 0) {
				return "place onto occupied cells";
			}
			if (m.kind == MoveKind::Swap && (hits != 1 || hit != m.other)) {
				return "swap without exactly the named item";
			}
			if (m.kind == MoveKind::Swap && items[hit].locked) {
				return "swap that picks up a locked item";
			}
			if (m.kind == MoveKind::Swap && !CanSwapOnto(items[hand], items[hit])) {
				return "drop onto an item the game doesn't swap (cube, tome, stack)";
			}
			x[hand] = m.x, y[hand] = m.y, inGrid[hand] = true;
			hand = -1;
			if (m.kind == MoveKind::Swap) {
				hand = hit, inGrid[hit] = false;
			}
			break;
		}
		}
	}
	return hand >= 0 ? "ends with an item on the cursor" : nullptr;
}

// Area of the biggest item shape (from FreeShapes) that fits in the free cells.
static auto BiggestFreeShape(const Item* items, int count, const int* x, const int* y) -> int {
	bool used[MaxSide][MaxSide] {};
	for (int k = 0; k < count; ++k) {
		for (int cx = x[k]; cx < x[k] + items[k].width; ++cx) {
			for (int cy = y[k]; cy < y[k] + items[k].height; ++cy) {
				used[cx][cy] = true;
			}
		}
	}
	int best = 0;
	for (const Shape& shape : FreeShapes) {
		for (int sx = 0; sx + shape.width <= Columns; ++sx) {
			for (int sy = 0; sy + shape.height <= Rows; ++sy) {
				bool free = true;
				for (int cx = sx; cx < sx + shape.width && free; ++cx) {
					for (int cy = sy; cy < sy + shape.height && free; ++cy) {
						free = !used[cx][cy];
					}
				}
				if (free && shape.width * shape.height > best) {
					best = shape.width * shape.height;
				}
			}
		}
	}
	return best;
}

static void PrintGrid(const Item* items, int count, const int* x, const int* y) {
	char grid[MaxSide][MaxSide + 1];
	for (int row = 0; row < Rows; ++row) {
		std::memset(grid[row], '.', Columns);
		grid[row][Columns] = 0;
	}
	for (int k = 0; k < count; ++k) {
		for (int cx = x[k]; cx < x[k] + items[k].width; ++cx) {
			for (int cy = y[k]; cy < y[k] + items[k].height; ++cy) {
				grid[cy][cx] = static_cast<char>('A' + k);
			}
		}
	}
	for (int row = 0; row < Rows; ++row) {
		std::printf("  %s\n", grid[row]);
	}
}

static auto Code(const char* text) -> uint32_t {
	uint32_t code = 0x20202020U;
	std::memcpy(&code, text, std::strlen(text));
	return code;
}

static void Show(const char* title, const Item* items, int count) {
	int x[MaxItems], y[MaxItems];
	std::printf("%s, as found:\n", title);
	for (int k = 0; k < count; ++k) {
		x[k] = items[k].x, y[k] = items[k].y;
	}
	PrintGrid(items, count, x, y);
	for (const bool compact : { false, true }) {
		std::vector<Move> moves;
		const bool        found = compact ? PlanCompact(items, count, Columns, Rows, moves) : Plan(items, count, Columns, Rows, moves);
		if (!found) {
			std::printf("%s: no plan\n", compact ? "compact" : "sort");
			continue;
		}
		const char* error = Replay(items, count, moves, x, y);
		std::printf("%s: %zu moves%s%s, biggest free block %d cells\n", compact ? "compact" : "sort", moves.size(), error ? ", ILLEGAL: " : "", error ? error : "", BiggestFreeShape(items, count, x, y));
		PrintGrid(items, count, x, y);
		// Pressing the button again should find nothing to do.
		Item again[MaxItems];
		for (int k = 0; k < count; ++k) {
			again[k] = items[k], again[k].x = x[k], again[k].y = y[k];
		}
		std::vector<Move> repeat;
		const bool        repeated = compact ? PlanCompact(again, count, Columns, Rows, repeat) : Plan(again, count, Columns, Rows, repeat);
		std::printf("  pressed again: %s\n", repeated ? (repeat.empty() ? "nothing to do" : "moves again") : "no plan");
	}
	std::printf("\n");
}

int main(int argc, char** argv) {
	const bool showOnly = argc > 1 && std::strcmp(argv[1], "show") == 0;
	if (showOnly) {
		// Gear and small items scattered over the grid with 6 free cells, so
		// compact should leave a 2 x 3 block (body armor size).
		const Item six[] = {
			{ 1, Code("axe"), 0, 0, 3, 1, 2, 3 }, { 2, Code("buc"), 1, 0, 0, 0, 2, 2 }, { 3, Code("qui"), 1, 0, 7, 0, 2, 3 }, { 4, Code("hax"), 0, 0, 2, 0, 1, 3 },
			{ 5, Code("cap"), 1, 0, 5, 2, 2, 2 }, { 6, Code("dgr"), 0, 0, 9, 2, 1, 2 }, { 7, Code("tbk"), 6, 0, 0, 2, 1, 2 }, { 8, Code("hp1"), 5, 0, 1, 2, 1, 1 },
			{ 9, Code("hp1"), 5, 0, 3, 0, 1, 1 }, { 10, Code("mp1"), 5, 0, 5, 0, 1, 1 }, { 11, Code("gcv"), 4, 0, 6, 0, 1, 1 }, { 12, Code("r01"), 4, 0, 1, 3, 1, 1 },
			{ 13, Code("key"), 7, 0, 9, 0, 1, 1 }, { 14, Code("amu"), 3, 0, 7, 3, 1, 1 },
		};
		Show("Six free cells", six, static_cast<int>(sizeof(six) / sizeof(six[0])));
		// The same gear without the small items: 13 free cells, room for a bow (2 x 4)
		// and a helm (2 x 2) at best.
		Show("Thirteen free cells", six, 7);
		return 0;
	}

	// A mode ending in "+locked" (sort+locked, compact+locked) locks the first
	// three items of each inventory, the way the cube and tomes are locked.
	const bool compact = argc > 1 && std::strncmp(argv[1], "compact", 7) == 0;
	const bool locked  = argc > 1 && std::strstr(argv[1], "+locked") != nullptr;
	const int  cases   = argc > 2 ? std::atoi(argv[2]) : 500;
	if (argc > 4) {
		Columns = std::atoi(argv[3]);
		Rows    = std::atoi(argv[4]);
	}
	int        failures = 0;
	std::printf("%s mode%s, %d x %d\n", compact ? "compact" : "sort", locked ? " with locked items" : "", Columns, Rows);
	for (const double fraction : { 0.3, 0.5, 0.7, 0.85, 0.95, 1.0 }) {
		int    ok = 0, fail = 0, illegal = 0, worstMoves = 0, totalMoves = 0, freeBefore = 0, freeAfter = 0;
		double slowest = 0, total = 0;
		for (int seed = 0; seed < cases; ++seed) {
			std::mt19937      rng(static_cast<uint32_t>(seed * 1'000 + fraction * 100));
			Item              items[MaxItems];
			const int         count = Scatter(rng, fraction, items);
			for (int k = 0; locked && k < count && k < 3; ++k) {
				items[k].locked = true;
			}
			// An unlocked cube: the game refuses drops onto it.
			if (locked && count > 3) {
				items[3].refusesDrops = true;
			}
			std::vector<Move> moves;
			const auto        t0    = std::chrono::steady_clock::now();
			const bool        found = compact ? PlanCompact(items, count, Columns, Rows, moves) : Plan(items, count, Columns, Rows, moves);
			const double      ms    = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			slowest                 = ms > slowest ? ms : slowest;
			total += ms;
			if (!found) {
				++fail;
				continue;
			}
			int x[MaxItems], y[MaxItems];
			for (int k = 0; k < count; ++k) {
				x[k] = items[k].x, y[k] = items[k].y;
			}
			freeBefore += BiggestFreeShape(items, count, x, y);
			if (const char* error = Replay(items, count, moves, x, y)) {
				++illegal;
				if (illegal <= 3) {
					std::printf("  illegal plan (fill %.2f, seed %d): %s\n", fraction, seed, error);
				}
				continue;
			}
			freeAfter += BiggestFreeShape(items, count, x, y);
			++ok;
			totalMoves += static_cast<int>(moves.size());
			worstMoves = static_cast<int>(moves.size()) > worstMoves ? static_cast<int>(moves.size()) : worstMoves;
		}
		failures += illegal;
		std::printf("fill %.2f: ok %d, no plan %d, illegal %d | avg %d, worst %d moves | biggest free block %.1f -> %.1f cells | avg %.1f ms, slowest %.1f ms\n", fraction, ok, fail, illegal, ok != 0 ? totalMoves / ok : 0, worstMoves,
		            ok != 0 ? static_cast<double>(freeBefore) / ok : 0.0, ok != 0 ? static_cast<double>(freeAfter) / ok : 0.0, total / cases, slowest);
	}
	return failures == 0 ? 0 : 1;
}
