// Native test for src/inventory_planner.h: random inventories, planned and then
// replayed against the server rules (pick needs an empty cursor; place needs
// free cells; swap needs exactly one overlapped item).
//
// usage: planner_test [cases-per-fill] [beam-width max-drops]
// Without a beam width it runs Plan(), the same call the plugin makes.

#include "../src/inventory_planner.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <random>

using namespace InventoryPlanner;

static constexpr int Columns = 10;
static constexpr int Rows    = 4;

struct Size {
	int w, h;
};

static constexpr Size Sizes[] = { { 2, 4 }, { 2, 3 }, { 2, 2 }, { 1, 3 }, { 1, 2 }, { 2, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 } };

static auto Scatter(std::mt19937& rng, double fraction, Item* items) -> int {
	bool used[Columns][Rows] {};
	int  count = 0, area = 0;
	const int goal = static_cast<int>(Columns * Rows * fraction);
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

// Replays moves under the server rules. Returns an error or nullptr.
static auto Replay(const Item* items, int count, const uint8_t* tx, const uint8_t* ty, const std::vector<Move>& moves) -> const char* {
	int x[MaxItems], y[MaxItems];
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
			x[hand] = m.x, y[hand] = m.y, inGrid[hand] = true;
			hand = -1;
			if (m.kind == MoveKind::Swap) {
				hand = hit, inGrid[hit] = false;
			}
			break;
		}
		}
	}
	if (hand >= 0) {
		return "ends with an item on the cursor";
	}
	for (int k = 0; k < count; ++k) {
		if (x[k] != tx[k] || y[k] != ty[k]) {
			return "an item is not at its target";
		}
	}
	return nullptr;
}

// Prints every candidate layout for one inventory with its score.
static void Show(const Item* items, int count) {
	constexpr int LayoutCount = static_cast<int>(sizeof(Layouts) / sizeof(Layouts[0]));
	static const char* const Names[] = { "tall", "large", "wide", "kind" };
	for (int index = 0; index < LayoutCount; ++index) {
		uint8_t tx[MaxItems], ty[MaxItems];
		if (!Pack(items, count, Columns, Rows, Layouts[index].order, Layouts[index].fill, Layouts[index].smallFromRight, tx, ty)) {
			std::printf("%s %s%s: does not pack\n", Names[static_cast<int>(Layouts[index].order)], Layouts[index].fill == PackFill::Contiguous ? "contiguous" : "backfill", Layouts[index].smallFromRight ? ", small items right" : "");
			continue;
		}
		char grid[Rows][Columns + 1];
		for (int y = 0; y < Rows; ++y) {
			for (int x = 0; x < Columns; ++x) {
				grid[y][x] = '.';
			}
			grid[y][Columns] = 0;
		}
		for (int k = 0; k < count; ++k) {
			for (int x = tx[k]; x < tx[k] + items[k].width; ++x) {
				for (int y = ty[k]; y < ty[k] + items[k].height; ++y) {
					grid[y][x] = static_cast<char>('A' + k);
				}
			}
		}
		std::printf("%s %s%s:\n", Names[static_cast<int>(Layouts[index].order)], Layouts[index].fill == PackFill::Contiguous ? "contiguous" : "backfill", Layouts[index].smallFromRight ? ", small items right" : "");
		for (int y = 0; y < Rows; ++y) {
			std::printf("  %s\n", grid[y]);
		}
	}
}

static auto Code(const char* text) -> uint32_t {
	uint32_t code = 0x20202020U;
	std::memcpy(&code, text, std::strlen(text));
	return code;
}

int main(int argc, char** argv) {
	if (argc > 1 && std::strcmp(argv[1], "show") == 0) {
		// The inventory from the in-game test: A-D weapons, E-G armor, H-I potions, J-K scrolls.
		const Item mixed[] = {
			{ 1, Code("lbw"), 0, 0, 0, 0, 2, 4 }, { 2, Code("spr"), 0, 0, 0, 0, 2, 4 }, { 3, Code("hax"), 0, 0, 0, 0, 1, 3 }, { 4, Code("dgr"), 0, 0, 0, 0, 1, 2 },
			{ 5, Code("qui"), 1, 0, 0, 0, 2, 3 }, { 6, Code("cap"), 1, 0, 0, 0, 2, 2 }, { 7, Code("lbl"), 1, 0, 0, 0, 2, 1 }, { 8, Code("hp1"), 5, 0, 0, 0, 1, 1 },
			{ 9, Code("hp1"), 5, 0, 0, 0, 1, 1 }, { 10, Code("isc"), 6, 0, 0, 0, 1, 1 }, { 11, Code("tsc"), 6, 0, 0, 0, 1, 1 },
		};
		Show(mixed, static_cast<int>(sizeof(mixed) / sizeof(mixed[0])));
		// A lighter inventory with charms, jewelry, gems, runes, potions and keys.
		const Item small[] = {
			{ 1, Code("axe"), 0, 0, 0, 0, 2, 3 }, { 2, Code("buc"), 1, 0, 0, 0, 2, 2 }, { 3, Code("cm2"), 2, 0, 0, 0, 1, 2 }, { 4, Code("cm1"), 2, 0, 0, 0, 1, 1 },
			{ 5, Code("rin"), 3, 0, 0, 0, 1, 1 }, { 6, Code("amu"), 3, 0, 0, 0, 1, 1 }, { 7, Code("gcv"), 4, 0, 0, 0, 1, 1 }, { 8, Code("gcv"), 4, 0, 0, 0, 1, 1 },
			{ 9, Code("r01"), 4, 0, 0, 0, 1, 1 }, { 10, Code("hp1"), 5, 0, 0, 0, 1, 1 }, { 11, Code("hp1"), 5, 0, 0, 0, 1, 1 }, { 12, Code("hp1"), 5, 0, 0, 0, 1, 1 },
			{ 13, Code("mp1"), 5, 0, 0, 0, 1, 1 }, { 14, Code("mp1"), 5, 0, 0, 0, 1, 1 }, { 15, Code("key"), 7, 0, 0, 0, 1, 1 }, { 16, Code("tbk"), 6, 0, 0, 0, 1, 2 },
		};
		std::printf("\n");
		Show(small, static_cast<int>(sizeof(small) / sizeof(small[0])));
		return 0;
	}
	const int cases = argc > 1 ? std::atoi(argv[1]) : 500;
	Limits    limits;
	if (argc > 2) {
		limits.beamWidth = std::atoi(argv[2]);
	}
	if (argc > 3) {
		limits.maxDepth = std::atoi(argv[3]);
	}
	int failures = 0;
	for (const double fraction : { 0.3, 0.5, 0.7, 0.85, 0.95, 1.0 }) {
		int    ok = 0, fail = 0, nopack = 0, illegal = 0, worstMoves = 0, totalMoves = 0;
		double slowest = 0, total = 0;
		for (int seed = 0; seed < cases; ++seed) {
			std::mt19937 rng(static_cast<uint32_t>(seed * 1'000 + fraction * 100));
			Item         items[MaxItems];
			const int    count = Scatter(rng, fraction, items);
			uint8_t           tx[MaxItems], ty[MaxItems];
			std::vector<Move> moves;
			const auto        t0     = std::chrono::steady_clock::now();
			const bool        packed = Pack(items, count, Columns, Rows, PackOrder::WideFirst, PackFill::Backfill, false, tx, ty) || Pack(items, count, Columns, Rows, PackOrder::TallFirst, PackFill::Backfill, false, tx, ty);
			const bool        found  = argc > 2 ? [&] {
				for (const Layout& layout : Layouts) {
					if (Pack(items, count, Columns, Rows, layout.order, layout.fill, layout.smallFromRight, tx, ty) && Search(items, count, Columns, Rows, tx, ty, moves, limits)) {
						return true;
					}
				}
				return false;
			}() : Plan(items, count, Columns, Rows, moves);
			if (found) {
				// Recover the target the plan reached, for Replay.
				int  x[MaxItems], y[MaxItems], hand = -1;
				for (int k = 0; k < count; ++k) {
					x[k] = items[k].x, y[k] = items[k].y;
				}
				for (const Move& m : moves) {
					if (m.kind == MoveKind::Pick) {
						hand = m.item;
					} else {
						x[m.item] = m.x, y[m.item] = m.y, hand = m.kind == MoveKind::Swap ? m.other : -1;
					}
				}
				for (int k = 0; k < count; ++k) {
					tx[k] = static_cast<uint8_t>(x[k]), ty[k] = static_cast<uint8_t>(y[k]);
				}
			}
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			slowest         = ms > slowest ? ms : slowest;
			total += ms;
			if (!packed) {
				++nopack;
				continue;
			}
			if (!found) {
				++fail;
				continue;
			}
			if (const char* error = Replay(items, count, tx, ty, moves)) {
				++illegal;
				if (illegal <= 3) {
					std::printf("  illegal plan (fill %.2f, seed %d): %s\n", fraction, seed, error);
				}
				continue;
			}
			++ok;
			totalMoves += static_cast<int>(moves.size());
			worstMoves = static_cast<int>(moves.size()) > worstMoves ? static_cast<int>(moves.size()) : worstMoves;
		}
		failures += illegal;
		std::printf("fill %.2f: ok %d, no plan %d, cannot pack %d, illegal %d | avg %d, worst %d moves | avg %.1f ms, slowest %.1f ms\n", fraction, ok, fail, nopack, illegal, ok != 0 ? totalMoves / ok : 0, worstMoves, total / cases, slowest);
	}
	return failures == 0 ? 0 : 1;
}
