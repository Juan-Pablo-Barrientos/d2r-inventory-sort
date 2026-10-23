#pragma once

// Plans how to sort an inventory with the moves a player has: pick an item up,
// drop the held item on free cells, or drop it onto exactly one item, which
// then becomes the held item. The plan is complete before anything moves, so a
// layout that cannot be reached leaves the inventory untouched.
//
// No game or SDK types here, so tools/planner_test.cpp can test it natively.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <vector>

namespace InventoryPlanner {

inline constexpr int MaxSide  = 16;
inline constexpr int MaxCells = MaxSide * MaxSide;
inline constexpr int MaxItems = 160;

struct Item {
	uint32_t id;
	uint32_t code;
	int      group; // kind of item (weapons, armor, potions, ...); lower sorts first
	int      quality;
	int      x;
	int      y;
	int      width;
	int      height;
};

enum class MoveKind : uint8_t {
	Pick  = 0, // item goes from the grid to the cursor
	Place = 1, // the held item goes to free cells at x,y
	Swap  = 2, // the held item goes to x,y and other goes to the cursor
};

struct Move {
	MoveKind kind;
	uint8_t  item;
	uint8_t  other;
	uint8_t  x;
	uint8_t  y;
};

enum class PackOrder : uint32_t {
	TallFirst  = 0,
	LargeFirst = 1,
	WideFirst  = 2,
	KindFirst  = 3, // kind of item, then tall first
};

// Contiguous keeps each item at or after the previous one, so items that sort
// together stay together. Backfill lets small items fill gaps left behind.
enum class PackFill : uint32_t {
	Contiguous = 0,
	Backfill   = 1,
};

// ---- Target layout ---------------------------------------------------------

// Big items first so they pack well. Equal sizes keep the same kind of item
// together, then the same code, then quality, so potions, gems and runes end up
// side by side.
inline auto ComesFirst(const Item& a, const Item& b, PackOrder order) noexcept -> bool {
	int primaryA = 0, primaryB = 0, secondaryA = 0, secondaryB = 0;
	if (order == PackOrder::KindFirst && a.group != b.group) {
		return a.group < b.group;
	}
	switch (order) {
	case PackOrder::KindFirst:
	case PackOrder::TallFirst:
		primaryA = a.height, primaryB = b.height, secondaryA = a.width, secondaryB = b.width;
		break;
	case PackOrder::LargeFirst:
		primaryA = a.width * a.height, primaryB = b.width * b.height, secondaryA = a.height, secondaryB = b.height;
		break;
	case PackOrder::WideFirst:
		primaryA = a.width, primaryB = b.width, secondaryA = a.height, secondaryB = b.height;
		break;
	}
	if (primaryA != primaryB) {
		return primaryA > primaryB;
	}
	if (secondaryA != secondaryB) {
		return secondaryA > secondaryB;
	}
	if (a.group != b.group) {
		return a.group < b.group;
	}
	if (a.code != b.code) {
		// Codes are four ASCII bytes; compare them as text.
		return std::memcmp(&a.code, &b.code, sizeof(a.code)) < 0;
	}
	if (a.quality != b.quality) {
		return a.quality < b.quality;
	}
	return a.id < b.id;
}

// First fit, filling each column top to bottom before moving right. With
// smallFromRight, single-cell items instead fill from the right edge inward,
// the way players keep potions and scrolls apart from their gear. Writes each
// item's target top-left cell.
inline auto Pack(const Item* items, int count, int columns, int rows, PackOrder order, PackFill fill, bool smallFromRight, uint8_t* targetX, uint8_t* targetY) noexcept -> bool {
	int sorted[MaxItems];
	for (int index = 0; index < count; ++index) {
		sorted[index] = index;
	}
	std::sort(sorted, sorted + count, [&](int a, int b) { return ComesFirst(items[a], items[b], order); });

	bool used[MaxSide][MaxSide] {};
	int  lastX = 0, lastY = 0;
	for (int rank = 0; rank < count; ++rank) {
		const Item& item  = items[sorted[rank]];
		bool        found = false;
		if (smallFromRight && item.width == 1 && item.height == 1) {
			continue;
		}
		for (int x = fill == PackFill::Contiguous ? lastX : 0; x + item.width <= columns && !found; ++x) {
			for (int y = fill == PackFill::Contiguous && x == lastX ? lastY : 0; y + item.height <= rows && !found; ++y) {
				bool free = true;
				for (int cx = x; cx < x + item.width && free; ++cx) {
					for (int cy = y; cy < y + item.height && free; ++cy) {
						free = !used[cx][cy];
					}
				}
				if (!free) {
					continue;
				}
				for (int cx = x; cx < x + item.width; ++cx) {
					for (int cy = y; cy < y + item.height; ++cy) {
						used[cx][cy] = true;
					}
				}
				targetX[sorted[rank]] = static_cast<uint8_t>(x);
				targetY[sorted[rank]] = static_cast<uint8_t>(y);
				lastX                 = x;
				lastY                 = y;
				found                 = true;
			}
		}
		if (!found) {
			return false;
		}
	}
	if (!smallFromRight) {
		return true;
	}
	int x = columns - 1, y = 0;
	for (int rank = 0; rank < count; ++rank) {
		const Item& item = items[sorted[rank]];
		if (item.width != 1 || item.height != 1) {
			continue;
		}
		while (x >= 0 && used[x][y]) {
			if (++y == rows) {
				y = 0;
				--x;
			}
		}
		if (x < 0) {
			return false;
		}
		used[x][y]            = true;
		targetX[sorted[rank]] = static_cast<uint8_t>(x);
		targetY[sorted[rank]] = static_cast<uint8_t>(y);
	}
	return true;
}

struct Layout {
	PackOrder order;
	PackFill  fill;
	bool      smallFromRight;
};

// Tried in this order until one packs and can be reached: gear by kind on the
// left with single-cell items on the right, then tighter packings.
inline constexpr Layout Layouts[] = {
	{ PackOrder::KindFirst, PackFill::Contiguous, true },  { PackOrder::KindFirst, PackFill::Backfill, true },
	{ PackOrder::TallFirst, PackFill::Backfill, true },    { PackOrder::TallFirst, PackFill::Contiguous, false },
	{ PackOrder::TallFirst, PackFill::Backfill, false },   { PackOrder::LargeFirst, PackFill::Backfill, false },
	{ PackOrder::WideFirst, PackFill::Backfill, false },
};

// ---- Search ----------------------------------------------------------------

struct Limits {
	int beamWidth = 48;
	int maxDepth  = 0; // drops; 0 picks a default from the item count
};

namespace Detail {

inline constexpr int16_t Empty = -1;

// Score weights: items away from their target cost their area, cells of their
// target still covered by other items cost a little, holding an item costs a
// little. Zero means sorted.
inline constexpr int WeightAway    = 10;
inline constexpr int WeightBlocked = 3;
inline constexpr int WeightHand    = 5;

struct Node {
	int32_t  parent;
	int32_t  score;
	uint64_t hash;
	int16_t  hand;
	Move     moves[2];
	uint8_t  moveCount;
};

struct Candidate {
	int32_t  score;
	int32_t  parent;
	uint64_t hash;
	uint8_t  item;
	uint8_t  x;
	uint8_t  y;
	int16_t  hit;
};

struct Problem {
	int      count;
	int      columns;
	int      rows;
	uint8_t  width[MaxItems];
	uint8_t  height[MaxItems];
	uint8_t  targetX[MaxItems];
	uint8_t  targetY[MaxItems];
	int32_t  base[MaxItems];
	int16_t  targetOwner[MaxCells];
	uint64_t cellKey[MaxItems][MaxCells];
	uint64_t handKey[MaxItems + 1];
};

inline auto Cell(int x, int y) noexcept -> int {
	return y * MaxSide + x;
}

inline auto Inside(int cx, int cy, int x, int y, int w, int h) noexcept -> bool {
	return cx >= x && cx < x + w && cy >= y && cy < y + h;
}

struct View {
	const Problem& p;
	const uint8_t* pos; // packed x | y << 4
	int16_t        hand;
	int16_t        owner[MaxCells];

	View(const Problem& problem, const uint8_t* positions, int16_t held) noexcept : p(problem), pos(positions), hand(held) {
		std::fill(owner, owner + MaxCells, Empty);
		for (int k = 0; k < p.count; ++k) {
			if (k == hand) {
				continue;
			}
			const int x = pos[k] & 15, y = pos[k] >> 4;
			for (int cx = x; cx < x + p.width[k]; ++cx) {
				for (int cy = y; cy < y + p.height[k]; ++cy) {
					owner[Cell(cx, cy)] = static_cast<int16_t>(k);
				}
			}
		}
	}

	auto Away(int k) const noexcept -> bool {
		return k == hand || (pos[k] & 15) != p.targetX[k] || (pos[k] >> 4) != p.targetY[k];
	}
};

inline auto FullScore(const View& v) noexcept -> int32_t {
	const Problem& p     = v.p;
	int32_t        score = v.hand >= 0 ? WeightHand : 0;
	for (int k = 0; k < p.count; ++k) {
		if (!v.Away(k)) {
			continue;
		}
		score += p.base[k];
		for (int cx = p.targetX[k]; cx < p.targetX[k] + p.width[k]; ++cx) {
			for (int cy = p.targetY[k]; cy < p.targetY[k] + p.height[k]; ++cy) {
				const int16_t o = v.owner[Cell(cx, cy)];
				score += (o != Empty && o != k) ? WeightBlocked : 0;
			}
		}
	}
	return score;
}

// Score after moving item k to x,y (k is held, or is picked up first) and
// putting hit (or nothing) on the cursor. Only the touched cells and the two
// moved items are re-evaluated.
inline auto ScoreAfter(const View& v, int32_t score, int k, int x, int y, int hit) noexcept -> int32_t {
	const Problem& p        = v.p;
	const bool     kInGrid  = k != v.hand;
	const int      kx       = v.pos[k] & 15, ky = v.pos[k] >> 4;
	const int      hx       = hit >= 0 ? v.pos[hit] & 15 : 0, hy = hit >= 0 ? v.pos[hit] >> 4 : 0;
	const auto     ownerNow = [&](int cx, int cy) noexcept -> int16_t {
		if (Inside(cx, cy, x, y, p.width[k], p.height[k])) {
			return static_cast<int16_t>(k);
		}
		if (kInGrid && Inside(cx, cy, kx, ky, p.width[k], p.height[k])) {
			return Empty;
		}
		if (hit >= 0 && Inside(cx, cy, hx, hy, p.width[hit], p.height[hit])) {
			return Empty;
		}
		return v.owner[Cell(cx, cy)];
	};
	const auto awayNow = [&](int j) noexcept -> bool {
		if (j == k) {
			return x != p.targetX[k] || y != p.targetY[k];
		}
		if (j == hit) {
			return true;
		}
		return v.Away(j);
	};
	const auto contribution = [&](int j, bool after) noexcept -> int32_t {
		if (!(after ? awayNow(j) : v.Away(j))) {
			return 0;
		}
		int32_t value = p.base[j];
		for (int cx = p.targetX[j]; cx < p.targetX[j] + p.width[j]; ++cx) {
			for (int cy = p.targetY[j]; cy < p.targetY[j] + p.height[j]; ++cy) {
				const int16_t o = after ? ownerNow(cx, cy) : v.owner[Cell(cx, cy)];
				value += (o != Empty && o != j) ? WeightBlocked : 0;
			}
		}
		return value;
	};

	score += contribution(k, true) - contribution(k, false);
	if (hit >= 0) {
		score += contribution(hit, true) - contribution(hit, false);
	}
	score += (hit >= 0 ? WeightHand : 0) - (v.hand >= 0 ? WeightHand : 0);

	// Target cells of other items whose cover changed.
	int  touched[3 * MaxCells];
	int  touchedCount = 0;
	bool seen[MaxCells] {};
	const auto addRect = [&](int rx, int ry, int rw, int rh) noexcept {
		for (int cx = rx; cx < rx + rw; ++cx) {
			for (int cy = ry; cy < ry + rh; ++cy) {
				const int c = Cell(cx, cy);
				if (!seen[c]) {
					seen[c]                  = true;
					touched[touchedCount++] = c;
				}
			}
		}
	};
	addRect(x, y, p.width[k], p.height[k]);
	if (kInGrid) {
		addRect(kx, ky, p.width[k], p.height[k]);
	}
	if (hit >= 0) {
		addRect(hx, hy, p.width[hit], p.height[hit]);
	}
	for (int index = 0; index < touchedCount; ++index) {
		const int     c = touched[index];
		const int16_t j = p.targetOwner[c];
		if (j == Empty || j == k || j == hit || !v.Away(j)) {
			continue;
		}
		const int16_t before = v.owner[c];
		const int16_t after  = ownerNow(c % MaxSide, c / MaxSide);
		score += ((after != Empty && after != j) ? WeightBlocked : 0) - ((before != Empty && before != j) ? WeightBlocked : 0);
	}
	return score;
}

} // namespace Detail

// Finds moves that put every item at its target with the cursor empty.
inline auto Search(const Item* items, int count, int columns, int rows, const uint8_t* targetX, const uint8_t* targetY, std::vector<Move>& moves, const Limits& limits = {}) -> bool {
	using namespace Detail;
	moves.clear();
	if (count <= 0) {
		return true;
	}
	if (count > MaxItems || columns > MaxSide || rows > MaxSide) {
		return false;
	}

	auto problem = std::make_unique<Problem>();
	Problem& p   = *problem;
	p.count      = count;
	p.columns    = columns;
	p.rows       = rows;
	std::fill(p.targetOwner, p.targetOwner + MaxCells, Empty);
	uint64_t seed = 0x9E3779B97F4A7C15ULL;
	const auto next = [&]() noexcept {
		seed ^= seed << 13U;
		seed ^= seed >> 7U;
		seed ^= seed << 17U;
		return seed;
	};
	for (int k = 0; k < count; ++k) {
		p.width[k]   = static_cast<uint8_t>(items[k].width);
		p.height[k]  = static_cast<uint8_t>(items[k].height);
		p.targetX[k] = targetX[k];
		p.targetY[k] = targetY[k];
		p.base[k]    = items[k].width * items[k].height * WeightAway + 1;
		for (int cx = targetX[k]; cx < targetX[k] + items[k].width; ++cx) {
			for (int cy = targetY[k]; cy < targetY[k] + items[k].height; ++cy) {
				p.targetOwner[Cell(cx, cy)] = static_cast<int16_t>(k);
			}
		}
		for (int c = 0; c < MaxCells; ++c) {
			p.cellKey[k][c] = next();
		}
	}
	for (int k = 0; k <= count; ++k) {
		p.handKey[k] = next();
	}

	std::vector<uint8_t> positions; // count bytes per node
	std::vector<Node>    nodes;
	positions.reserve(static_cast<size_t>(count) * 4'096);
	nodes.reserve(4'096);

	std::vector<uint8_t> start(static_cast<size_t>(count));
	uint64_t             startHash = p.handKey[count];
	for (int k = 0; k < count; ++k) {
		start[k] = static_cast<uint8_t>(items[k].x | (items[k].y << 4));
		startHash ^= p.cellKey[k][Cell(items[k].x, items[k].y)];
	}
	const View startView(p, start.data(), Empty);
	nodes.push_back({ -1, FullScore(startView), startHash, Empty, {}, 0 });
	positions.insert(positions.end(), start.begin(), start.end());

	int goal = nodes[0].score == 0 ? 0 : -1;
	std::vector<int32_t>         beam { 0 };
	std::vector<Candidate>       candidates;
	std::unordered_set<uint64_t> seen { startHash };
	const int                    maxDepth = limits.maxDepth > 0 ? limits.maxDepth : count * 8 + 20;

	for (int depth = 0; depth < maxDepth && goal < 0 && !beam.empty(); ++depth) {
		candidates.clear();
		for (const int32_t index : beam) {
			const Node     node = nodes[index];
			const uint8_t* pos  = &positions[static_cast<size_t>(index) * count];
			const View     v(p, pos, node.hand);
			for (int k = 0; k < count; ++k) {
				if (node.hand >= 0 ? k != node.hand : !v.Away(k)) {
					continue;
				}
				const int  kx      = pos[k] & 15, ky = pos[k] >> 4;
				const bool kInGrid = k != node.hand;
				for (int x = 0; x + p.width[k] <= columns; ++x) {
					for (int y = 0; y + p.height[k] <= rows; ++y) {
						if (kInGrid && x == kx && y == ky) {
							continue;
						}
						int16_t hit   = Empty;
						bool    valid = true;
						for (int cx = x; cx < x + p.width[k] && valid; ++cx) {
							for (int cy = y; cy < y + p.height[k] && valid; ++cy) {
								const int16_t o = v.owner[Cell(cx, cy)];
								if (o == Empty || o == k) {
									continue;
								}
								if (hit == Empty) {
									hit = o;
								} else if (hit != o) {
									valid = false;
								}
							}
						}
						if (!valid) {
							continue;
						}
						// The hash covers grid items and which item is held, not
						// where the held item used to be.
						uint64_t hash = node.hash ^ p.handKey[node.hand >= 0 ? node.hand : count] ^ p.handKey[hit >= 0 ? hit : count];
						hash ^= p.cellKey[k][Cell(x, y)];
						if (kInGrid) {
							hash ^= p.cellKey[k][Cell(kx, ky)];
						}
						if (hit >= 0) {
							hash ^= p.cellKey[hit][Cell(pos[hit] & 15, pos[hit] >> 4)];
						}
						if (seen.count(hash) != 0) {
							continue;
						}
						const int32_t score = ScoreAfter(v, node.score, k, x, y, hit);
						candidates.push_back({ score, index, hash, static_cast<uint8_t>(k), static_cast<uint8_t>(x), static_cast<uint8_t>(y), hit });
					}
				}
			}
		}
		if (candidates.empty()) {
			break;
		}
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score < b.score; });

		beam.clear();
		for (const Candidate& c : candidates) {
			if (static_cast<int>(beam.size()) == limits.beamWidth) {
				break;
			}
			if (!seen.insert(c.hash).second) {
				continue;
			}
			const Node& parent = nodes[c.parent];
			Node        node { c.parent, c.score, c.hash, c.hit, {}, 0 };
			if (parent.hand < 0) {
				node.moves[node.moveCount++] = { MoveKind::Pick, c.item, 0, 0, 0 };
			}
			node.moves[node.moveCount++] = { c.hit >= 0 ? MoveKind::Swap : MoveKind::Place, c.item, static_cast<uint8_t>(c.hit >= 0 ? c.hit : 0), c.x, c.y };
			const size_t from = static_cast<size_t>(c.parent) * count;
			positions.insert(positions.end(), positions.begin() + static_cast<ptrdiff_t>(from), positions.begin() + static_cast<ptrdiff_t>(from + count));
			positions[positions.size() - count + c.item] = static_cast<uint8_t>(c.x | (c.y << 4));
			nodes.push_back(node);
			beam.push_back(static_cast<int32_t>(nodes.size() - 1));
			if (c.score == 0) {
				goal = static_cast<int>(nodes.size() - 1);
				break;
			}
		}
	}
	if (goal < 0) {
		return false;
	}

	std::vector<Move> reversed;
	for (int index = goal; index > 0; index = nodes[index].parent) {
		for (int m = nodes[index].moveCount - 1; m >= 0; --m) {
			reversed.push_back(nodes[index].moves[m]);
		}
	}
	moves.assign(reversed.rbegin(), reversed.rend());
	return true;
}

// Tries each layout in order with a narrow beam, then again with a wide one.
// Writes the moves to the first one that can be reached.
inline auto Plan(const Item* items, int count, int columns, int rows, std::vector<Move>& moves) -> bool {
	uint8_t targetX[MaxItems] {};
	uint8_t targetY[MaxItems] {};
	for (const int beamWidth : { 48, 256 }) {
		const Limits limits { .beamWidth = beamWidth };
		for (const Layout& layout : Layouts) {
			if (Pack(items, count, columns, rows, layout.order, layout.fill, layout.smallFromRight, targetX, targetY) && Search(items, count, columns, rows, targetX, targetY, moves, limits)) {
				return true;
			}
		}
	}
	return false;
}

} // namespace InventoryPlanner
