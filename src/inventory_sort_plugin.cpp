#include <D2RLPlugin/console.h>
#include <D2RLPlugin/context.h>
#include <D2RLPlugin/data_tables.h>
#include <D2RLPlugin/inventory.h>
#include <D2RLPlugin/item.h>
#include <D2RLPlugin/lifecycle_events.h>
#include <D2RLPlugin/panels.h>
#include <D2RLPlugin/resources.h>
#include <D2RLPlugin/shared_events.h>
#include <D2RLPlugin/threads.h>
#include <D2RLPlugin/version.h>
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <thread>
#include <vector>

#include "inventory_planner.h"

// Adds a Sort button to the player inventory. Sorting plans a tidy layout and
// every move to reach it first (inventory_planner.h), then moves items one at
// a time with the same client requests as a manual drag: grid to cursor,
// cursor to grid, or cursor onto one item, which swaps it onto the cursor. The
// game server checks every move, so nothing is created or destroyed, and it
// works in single player and on a TCP/IP client alike.

static constexpr D2RL::PluginInfo InventorySortPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "inventory-sort",
	.name        = "Inventory Sort",
	.version     = "0.1.0",
	.author      = "Juan",
	.description = "Adds a button to the inventory that sorts its items.",
	.flags       = D2RL::PluginFlags::Client,
};

static constexpr char DefaultConfig[] = R"(# Inventory Sort
# Adds a button to the inventory that sorts its items.

[inventory-sort]
# Show the Sort button in the inventory. The inventory-sort console command
# works either way.
show_button = true

# Inventory grid size. 0 reads it from the game data (10 x 4 in the base game).
columns = 0
rows = 0

# Write every move to the plugin log.
log_moves = false
)";

// The child layout hangs off the stock inventory panel, so its coordinates are
// the panel's: the button sits in the empty strip right of the gold display.
static constexpr char ButtonLayout[] = R"json({
	"type": "Widget",
	"name": "inventory-sort/SortButton",
	"fields": {
		"rect": { "x": 898, "y": 1340, "width": 64, "height": 64 }
	},
	"children": [
		{
			"type": "ButtonWidget",
			"name": "inventory-sort/Button",
			"fields": {
				"rect": { "x": 0, "y": 0 },
				"filename": "d2rloader/inventory-sort/sort_button",
				"hoveredFrame": 3,
				"disabledFrame": 2,
				"onClickMessage": "PanelManager:TogglePanel:inventory-sort/Sort",
				"pressLabelOffset": { "x": 0, "y": 2 },
				"tooltipString": "Sort inventory"
			}
		}
	]
})json";

// The button reuses a stock UI message so the layout parser accepts it. The
// plugin consumes it before PanelManager sees it.
static constexpr char ClickTarget[]  = "PanelManager";
static constexpr char ClickCommand[] = "TogglePanel";
static constexpr char ClickText[]    = "inventory-sort/Sort";

static constexpr int ButtonSpriteResource       = 101;
static constexpr int ButtonLowEndSpriteResource = 102;

// D2RCore wraps D2R's client-to-server send with this function. It is a private
// export, so its prologue is checked before use and sorting stays off when a
// different D2RLoader build changes it.
using SendClientGameplayPacketFn = void(__cdecl*)(const uint8_t* data, uint32_t size) noexcept;

static constexpr uint8_t SendClientGameplayPacketPrologue[] = { 0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x50 };

// D2R client packets, checked against D2Game's handler table in this build.
static constexpr uint8_t  PacketCursorToGrid = 0x18; // [id][itemId][x][y][page], 17 bytes
static constexpr uint8_t  PacketGridToCursor = 0x19; // [id][itemId][x][y][page], 17 bytes
// Dropping onto exactly one item swaps it onto the cursor; 0x18 refuses that.
// Positions are two u16 cells (x, y). The rectangles must overlap.
static constexpr uint8_t  PacketCursorSwap   = 0x1F; // [id][cursorItemId][gridItemId][page][gridItemCell][dropCell], 21 bytes
static constexpr uint32_t InventoryGridPage  = 0;

static constexpr int MaxGridSide  = 16;
static constexpr int MaxItems     = MaxGridSide * MaxGridSide;
static constexpr int MaxItemSizes = 8'192;

// Inventory.txt rows: gridX and gridY are bytes 16 and 17 of a 240-byte row.
static constexpr uint32_t InventoryRowSize   = 240;
static constexpr uint32_t InventoryGridXByte = 16;
static constexpr uint32_t InventoryGridYByte = 17;

// In the compiled item rows of this build, invwidth and invheight follow the
// item code by 158 and 159 bytes. They are checked against known items before
// use, and searched for if a different build moves them.
static constexpr uint32_t ItemSizeAfterCode = 158;

static constexpr ULONGLONG MoveTimeoutMs = 2'000;

struct Config {
	bool showButton = true;
	int  columns    = 0;
	int  rows       = 0;
	bool logMoves   = false;
};

struct ItemSize {
	uint32_t code;
	uint8_t  width;
	uint8_t  height;
	uint8_t  group;
};

struct GridItem {
	uint32_t runtimeId = 0;
	uint32_t code      = 0;
	int      group     = 0;
	int      quality   = 0;
	int      x         = 0;
	int      y         = 0;
	int      width     = 1;
	int      height    = 1;
};

struct Snapshot {
	GridItem items[MaxItems] {};
	int      count        = 0;
	bool     cursorUsed   = false;
	uint32_t cursorItemId = 0;
};

// Planning runs on a worker thread. The UI thread then sends the moves one at
// a time and waits until the inventory matches the plan after each one.
enum class JobState : uint32_t {
	Idle     = 0,
	Planning = 1,
	Moving   = 2,
};

struct SortJob {
	JobState                            state   = JobState::Idle;
	D2RL::PlayerHandle                  player  = D2RL::InvalidPlayerHandle;
	int                                 columns = 0;
	int                                 rows    = 0;
	int                                 count   = 0;
	InventoryPlanner::Item              items[InventoryPlanner::MaxItems] {};
	std::vector<InventoryPlanner::Move> moves;
	size_t                              next = 0;
	// Where the plan puts every item after the moves sent so far.
	int                                 x[InventoryPlanner::MaxItems] {};
	int                                 y[InventoryPlanner::MaxItems] {};
	int                                 hand    = -1;
	bool                                waiting = false;
	ULONGLONG                           sentAt  = 0;
};

static constexpr int PlanPending = 0;
static constexpr int PlanFound   = 1;
static constexpr int PlanNone    = 2;

static const D2RL::PluginContext*     plugin;
static const D2RL::InventoryService*  inventory;
static const D2RL::ItemService*       items;
static const D2RL::ThreadService*     threads;
static const D2RL::DataTableService*  dataTables;
static SendClientGameplayPacketFn     sendPacket;
static Config                         config;

static ItemSize          itemSizes[MaxItemSizes];
static int               itemSizeCount;
static int               tableColumns;
static int               tableRows;
static std::atomic<bool> tablesReady;

static SortJob          job;
static Snapshot         scratch;
static std::thread      planner;
static std::atomic<int> planResult;

static void Logf(bool warning, const char* format, ...) noexcept {
	char    message[256] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	if (warning) {
		plugin->LogWarn(message);
	} else {
		plugin->LogInfo(message);
	}
}

// Tells the player in the D2RLoader console and keeps a copy in the log.
static void Tellf(bool warning, const char* format, ...) noexcept {
	char    message[256] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	plugin->WriteConsoleMessage(message, warning ? D2RL::ConsoleMessageKind::Warning : D2RL::ConsoleMessageKind::Output);
	if (warning) {
		plugin->LogWarn(message);
	} else {
		plugin->LogInfo(message);
	}
}

static auto CodeText(uint32_t code, char (&text)[5]) noexcept -> const char* {
	std::memcpy(text, &code, 4);
	text[4] = 0;
	return text;
}

// ---- Configuration --------------------------------------------------------

static auto TrimLeft(const char* text) noexcept -> const char* {
	while (*text == ' ' || *text == '\t') {
		++text;
	}
	return text;
}

static auto ReadValue(const char* line, const char* key) noexcept -> const char* {
	const size_t length = std::strlen(key);
	if (std::strncmp(line, key, length) != 0) {
		return nullptr;
	}
	const char* rest = TrimLeft(line + length);
	return *rest == '=' ? TrimLeft(rest + 1) : nullptr;
}

static auto ReadGridSide(const char* value) noexcept -> int {
	const int side = std::atoi(value);
	return side >= 1 && side <= MaxGridSide ? side : 0;
}

static void LoadConfig() noexcept {
	config = {};
	char text[4'096] {};
	if (!plugin->EnsureConfig(DefaultConfig)) {
		plugin->LogWarn("Could not create the default config. Using defaults.");
		return;
	}
	if (!plugin->ReadConfig(text, sizeof(text))) {
		plugin->LogWarn("Could not read the config. Using defaults.");
		return;
	}
	char* context = nullptr;
	for (char* line = strtok_s(text, "\r\n", &context); line != nullptr; line = strtok_s(nullptr, "\r\n", &context)) {
		line = const_cast<char*>(TrimLeft(line));
		if (const char* showButton = ReadValue(line, "show_button")) {
			config.showButton = std::strncmp(showButton, "false", 5) != 0;
		} else if (const char* columns = ReadValue(line, "columns")) {
			config.columns = ReadGridSide(columns);
		} else if (const char* rows = ReadValue(line, "rows")) {
			config.rows = ReadGridSide(rows);
		} else if (const char* logMoves = ReadValue(line, "log_moves")) {
			config.logMoves = std::strncmp(logMoves, "true", 4) == 0;
		}
	}
}

// ---- Item sizes from the game data ----------------------------------------

struct KnownSize {
	uint32_t code;
	uint8_t  width;
	uint8_t  height;
};

// Base-game items whose inventory size never changes, used to check where the
// compiled rows keep invwidth and invheight.
static constexpr KnownSize KnownSizes[] = {
	{ D2RL::Items::MakeItemCode("hp1"), 1, 1 },
	{ D2RL::Items::MakeItemCode("isc"), 1, 1 },
	{ D2RL::Items::MakeItemCode("tbk"), 1, 2 },
	{ D2RL::Items::MakeItemCode("dgr"), 1, 2 },
	{ D2RL::Items::MakeItemCode("hax"), 1, 3 },
	{ D2RL::Items::MakeItemCode("aqv"), 1, 3 },
	{ D2RL::Items::MakeItemCode("cap"), 2, 2 },
	{ D2RL::Items::MakeItemCode("buc"), 2, 2 },
	{ D2RL::Items::MakeItemCode("lbl"), 2, 1 },
	{ D2RL::Items::MakeItemCode("axe"), 2, 3 },
	{ D2RL::Items::MakeItemCode("qui"), 2, 3 },
	{ D2RL::Items::MakeItemCode("lbw"), 2, 4 },
	{ D2RL::Items::MakeItemCode("spr"), 2, 4 },
};

static constexpr int KnownSizeCount = static_cast<int>(sizeof(KnownSizes) / sizeof(KnownSizes[0]));

static auto ReadU32(const uint8_t* bytes) noexcept -> uint32_t {
	uint32_t value = 0;
	std::memcpy(&value, bytes, 4);
	return value;
}

static auto CompareItemSize(const void* lhs, const void* rhs) noexcept -> int {
	const uint32_t a = static_cast<const ItemSize*>(lhs)->code;
	const uint32_t b = static_cast<const ItemSize*>(rhs)->code;
	return a < b ? -1 : (a > b ? 1 : 0);
}

static auto FindItemSize(uint32_t code) noexcept -> const ItemSize* {
	const ItemSize key { code, 0, 0, 0 };
	return static_cast<const ItemSize*>(std::bsearch(&key, itemSizes, static_cast<size_t>(itemSizeCount), sizeof(ItemSize), CompareItemSize));
}

// ---- Item groups ----------------------------------------------------------

// Kinds of item, in the order they sort within one size.
enum ItemGroup : uint8_t {
	GroupWeapons      = 0,
	GroupArmor        = 1,
	GroupCharms       = 2,
	GroupJewelry      = 3,
	GroupGemsAndRunes = 4,
	GroupPotions      = 5,
	GroupScrolls      = 6,
	GroupKeys         = 7,
	GroupOther        = 8,
};

struct GroupRoot {
	char      code[5];
	ItemGroup group;
};

// An item type joins the first group listed here whose code is the type itself
// or one of its parents (Equiv1 and Equiv2 in ItemTypes.txt).
static constexpr GroupRoot GroupRoots[] = {
	{ "char", GroupCharms },       { "ring", GroupJewelry },      { "amul", GroupJewelry }, { "jewl", GroupJewelry },
	{ "gem", GroupGemsAndRunes },  { "rune", GroupGemsAndRunes }, { "poti", GroupPotions }, { "elix", GroupPotions },
	{ "scro", GroupScrolls },      { "book", GroupScrolls },      { "key", GroupKeys },     { "weap", GroupWeapons },
	{ "misl", GroupWeapons },      { "armo", GroupArmor },
};

struct KnownType {
	uint32_t item;
	char     type[5];
};

// Base-game items and their types, used to check the compiled row layouts.
static constexpr KnownType KnownTypes[] = {
	{ D2RL::Items::MakeItemCode("hp1"), "hpot" }, { D2RL::Items::MakeItemCode("isc"), "scro" }, { D2RL::Items::MakeItemCode("cap"), "helm" },
	{ D2RL::Items::MakeItemCode("buc"), "shie" }, { D2RL::Items::MakeItemCode("qui"), "tors" }, { D2RL::Items::MakeItemCode("hax"), "axe" },
	{ D2RL::Items::MakeItemCode("lbw"), "bow" },
};

// In this build an ItemTypes row keeps its two parent types as u16 row numbers
// right after its code, and an item row keeps its type's row number 174 bytes
// after the item code.
static constexpr uint32_t ParentsAfterTypeCode = 4;
static constexpr uint32_t TypeAfterItemCode    = 174;
static constexpr int      MaxItemTypes         = 4'096;

static uint8_t  typeGroups[MaxItemTypes];
static uint32_t typeGroupCount;

// ItemTypes codes are padded with zeros or spaces; compare them without padding.
static auto TypeCode(uint32_t raw) noexcept -> uint32_t {
	uint32_t code = 0;
	for (int index = 0; index < 4; ++index) {
		const uint32_t byte = (raw >> (index * 8)) & 0xFFU;
		if (byte != 0 && byte != ' ') {
			code |= byte << (index * 8);
		}
	}
	return code;
}

static auto TypeCode(const char* text) noexcept -> uint32_t {
	uint32_t code = 0;
	for (int index = 0; index < 4 && text[index] != 0; ++index) {
		code |= static_cast<uint32_t>(static_cast<uint8_t>(text[index])) << (index * 8);
	}
	return code;
}

// Short type codes are stored padded with spaces ("axe "); try zeros as well.
static auto FindTypeRow(const D2RL::PluginContext* context, D2RL::DataTables::Bank bank, const char* type, uint32_t rowSize) noexcept -> const uint8_t* {
	using namespace D2RL::DataTables;
	for (const char pad : { ' ', '\0' }) {
		char padded[4] {};
		for (int index = 0; index < 4; ++index) {
			padded[index] = index < static_cast<int>(std::strlen(type)) ? type[index] : pad;
		}
		RowView row { .structSize = RowViewSize, .bank = bank };
		if (dataTables->findRowByCode(context, bank, TableId::ItemTypes, MakeFourCC(padded[0], padded[1], padded[2], padded[3]), &row) == Result::Success && row.rowSize == rowSize) {
			return static_cast<const uint8_t*>(row.row);
		}
	}
	return nullptr;
}

// Works out every item type's group. Returns false, leaving every type in
// GroupOther, when the table does not look as expected.
static auto ReadTypeGroups(const D2RL::PluginContext* context, D2RL::DataTables::Bank bank) noexcept -> bool {
	using namespace D2RL::DataTables;
	std::memset(typeGroups, GroupOther, sizeof(typeGroups));
	typeGroupCount = 0;

	TableView table { .structSize = TableViewSize, .bank = bank };
	if (dataTables->getTable(context, bank, TableId::ItemTypes, &table) != Result::Success || table.rows == nullptr || table.rowCount == 0 || table.rowCount > MaxItemTypes || table.rowSize < 8) {
		return false;
	}
	const uint8_t* shield = FindTypeRow(context, bank, "shie", table.rowSize);
	const uint8_t* armor  = FindTypeRow(context, bank, "tors", table.rowSize);
	if (shield == nullptr || armor == nullptr) {
		plugin->LogWarn("Item types: could not look up shie or tors.");
		return false;
	}
	int codeOffset = -1;
	for (uint32_t offset = 0; offset + 8 <= table.rowSize && codeOffset < 0; ++offset) {
		if (TypeCode(ReadU32(shield + offset)) == TypeCode("shie") && TypeCode(ReadU32(armor + offset)) == TypeCode("tors")) {
			codeOffset = static_cast<int>(offset);
		}
	}
	if (codeOffset < 0 || static_cast<uint32_t>(codeOffset) + ParentsAfterTypeCode + 4 > table.rowSize) {
		plugin->LogWarn("Item types: could not find the type code in the rows.");
		return false;
	}

	const auto row = [&](uint32_t index) noexcept { return static_cast<const uint8_t*>(table.rows) + static_cast<size_t>(index) * table.rowSize; };
	const auto parent = [&](uint32_t index, int which) noexcept -> uint32_t {
		uint16_t value = 0;
		std::memcpy(&value, row(index) + codeOffset + ParentsAfterTypeCode + which * 2, 2);
		return value;
	};
	const auto code = [&](uint32_t index) noexcept { return TypeCode(ReadU32(row(index) + codeOffset)); };
	const auto shieldIndex = static_cast<uint32_t>((shield - row(0)) / table.rowSize);
	const auto armorIndex  = static_cast<uint32_t>((armor - row(0)) / table.rowSize);
	// Shields are a kind of "shld", body armor a kind of "armo".
	if (parent(shieldIndex, 0) >= table.rowCount || code(parent(shieldIndex, 0)) != TypeCode("shld") || parent(armorIndex, 0) >= table.rowCount || code(parent(armorIndex, 0)) != TypeCode("armo")) {
		plugin->LogWarn("Item types: the parent types are not where this build keeps them.");
		return false;
	}

	for (uint32_t type = 0; type < table.rowCount; ++type) {
		// Walk the type and its parents; row 0 ("Any") ends a branch.
		uint32_t stack[32] { type };
		int      depth = 1;
		int      best  = static_cast<int>(sizeof(GroupRoots) / sizeof(GroupRoots[0]));
		for (int visited = 0; depth > 0 && visited < 64; ++visited) {
			const uint32_t current = stack[--depth];
			const uint32_t value   = code(current);
			for (int index = 0; index < best; ++index) {
				if (value == TypeCode(GroupRoots[index].code)) {
					best = index;
				}
			}
			for (int which = 0; which < 2 && depth < 32; ++which) {
				const uint32_t next = parent(current, which);
				if (next != 0 && next < table.rowCount) {
					stack[depth++] = next;
				}
			}
		}
		typeGroups[type] = best < static_cast<int>(sizeof(GroupRoots) / sizeof(GroupRoots[0])) ? GroupRoots[best].group : GroupOther;
	}
	typeGroupCount = table.rowCount;
	return true;
}

// Finds where item rows keep their type and checks it against known items.
static auto FindTypeOffset(const D2RL::PluginContext* context, D2RL::DataTables::Bank bank, int codeOffset, uint32_t rowSize, const uint8_t* const* knownRows) noexcept -> int {
	using namespace D2RL::DataTables;
	TableView types { .structSize = TableViewSize, .bank = bank };
	if (typeGroupCount == 0 || dataTables->getTable(context, bank, TableId::ItemTypes, &types) != Result::Success) {
		return -1;
	}
	const uint32_t offset = static_cast<uint32_t>(codeOffset) + TypeAfterItemCode;
	if (offset + 2 > rowSize) {
		return -1;
	}
	for (const KnownType& known : KnownTypes) {
		const uint8_t* item = nullptr;
		for (int index = 0; index < KnownSizeCount; ++index) {
			if (KnownSizes[index].code == known.item) {
				item = knownRows[index];
			}
		}
		const uint8_t* type = FindTypeRow(context, bank, known.type, types.rowSize);
		if (item == nullptr || type == nullptr) {
			Logf(true, "Item types: could not look up type %s.", known.type);
			return -1;
		}
		uint16_t value = 0;
		std::memcpy(&value, item + offset, 2);
		if (static_cast<const uint8_t*>(types.rows) + static_cast<size_t>(value) * types.rowSize != type) {
			Logf(true, "Item types: an item row does not name type %s where expected.", known.type);
			return -1;
		}
	}
	return static_cast<int>(offset);
}

static auto ReadItemSizes(const D2RL::PluginContext* context, D2RL::DataTables::Bank bank) noexcept -> bool {
	using namespace D2RL::DataTables;

	TableView table { .structSize = TableViewSize, .bank = bank };
	if (dataTables->getTable(context, bank, TableId::Items, &table) != Result::Success || table.rows == nullptr || table.rowCount == 0 || table.rowSize < 8) {
		return false;
	}

	// Look up the known rows and find the code inside one of them.
	const uint8_t* knownRows[KnownSizeCount] {};
	int            knownCount = 0;
	for (int index = 0; index < KnownSizeCount; ++index) {
		RowView row { .structSize = RowViewSize, .bank = bank };
		if (dataTables->findRowByCode(context, bank, TableId::Items, KnownSizes[index].code, &row) == Result::Success && row.row != nullptr && row.rowSize == table.rowSize) {
			knownRows[index] = static_cast<const uint8_t*>(row.row);
			++knownCount;
		}
	}
	if (knownCount < KnownSizeCount / 2) {
		Logf(true, "Only %d of %d known items were found in the item table.", knownCount, KnownSizeCount);
		return false;
	}

	int codeOffset = -1;
	for (uint32_t offset = 0; offset + 4 <= table.rowSize && codeOffset < 0; ++offset) {
		bool all = true;
		for (int index = 0; index < KnownSizeCount && all; ++index) {
			all = knownRows[index] == nullptr || ReadU32(knownRows[index] + offset) == KnownSizes[index].code;
		}
		if (all) {
			codeOffset = static_cast<int>(offset);
		}
	}
	if (codeOffset < 0) {
		plugin->LogWarn("Could not find the item code in the item table rows.");
		return false;
	}

	const auto sizesMatch = [&](uint32_t offset) noexcept {
		if (offset + 2 > table.rowSize) {
			return false;
		}
		for (int index = 0; index < KnownSizeCount; ++index) {
			if (knownRows[index] != nullptr && (knownRows[index][offset] != KnownSizes[index].width || knownRows[index][offset + 1] != KnownSizes[index].height)) {
				return false;
			}
		}
		return true;
	};
	int sizeOffset = sizesMatch(static_cast<uint32_t>(codeOffset) + ItemSizeAfterCode) ? codeOffset + static_cast<int>(ItemSizeAfterCode) : -1;
	for (uint32_t offset = 0; offset + 2 <= table.rowSize && sizeOffset < 0; ++offset) {
		if (sizesMatch(offset)) {
			sizeOffset = static_cast<int>(offset);
		}
	}
	if (sizeOffset < 0) {
		plugin->LogWarn("Could not find the item sizes in the item table rows.");
		return false;
	}

	const bool groups     = ReadTypeGroups(context, bank);
	const int  typeOffset = groups ? FindTypeOffset(context, bank, codeOffset, table.rowSize, knownRows) : -1;
	if (typeOffset < 0) {
		plugin->LogWarn("Could not read item types. Items of one size will not be grouped by kind.");
	}

	int count = 0;
	for (uint32_t index = 0; index < table.rowCount && count < MaxItemSizes; ++index) {
		const uint8_t* row    = static_cast<const uint8_t*>(table.rows) + static_cast<size_t>(index) * table.rowSize;
		const uint32_t code   = ReadU32(row + codeOffset);
		const uint8_t  width  = row[sizeOffset];
		const uint8_t  height = row[sizeOffset + 1];
		uint8_t        group  = GroupOther;
		if (typeOffset >= 0) {
			uint16_t type = 0;
			std::memcpy(&type, row + typeOffset, 2);
			group = type < typeGroupCount ? typeGroups[type] : static_cast<uint8_t>(GroupOther);
		}
		if (code != 0 && width >= 1 && width <= MaxGridSide && height >= 1 && height <= MaxGridSide) {
			itemSizes[count++] = { code, width, height, group };
		}
	}
	std::qsort(itemSizes, static_cast<size_t>(count), sizeof(ItemSize), CompareItemSize);
	itemSizeCount = count;
	Logf(false, "Read %d item sizes (rows of %u bytes, code at %d, size at %d, type at %d).", count, table.rowSize, codeOffset, sizeOffset, typeOffset);
	return true;
}

// Inventory.txt has the classic rows first and the expansion rows second. The
// first expansion row is the Amazon, whose grid all player classes share in the
// base game.
static void ReadGridSize(const D2RL::PluginContext* context, D2RL::DataTables::Bank bank) noexcept {
	using namespace D2RL::DataTables;

	tableColumns = 10;
	tableRows    = 4;
	TableView table { .structSize = TableViewSize, .bank = bank };
	if (dataTables->getTable(context, bank, TableId::Inventory, &table) != Result::Success || table.rows == nullptr || table.rowSize != InventoryRowSize || table.rowCount == 0) {
		plugin->LogWarn("Could not read the inventory table. Assuming a 10 x 4 inventory.");
		return;
	}
	const uint32_t candidates[] = { table.rowCount / 2, 0 };
	for (const uint32_t index : candidates) {
		const uint8_t* row     = static_cast<const uint8_t*>(table.rows) + static_cast<size_t>(index) * table.rowSize;
		const int      columns = row[InventoryGridXByte];
		const int      rows    = row[InventoryGridYByte];
		if (columns >= 1 && columns <= MaxGridSide && rows >= 1 && rows <= MaxGridSide) {
			tableColumns = columns;
			tableRows    = rows;
			return;
		}
	}
	plugin->LogWarn("The inventory table has no usable grid size. Assuming a 10 x 4 inventory.");
}

static void __cdecl OnDataTablesLoaded(const D2RL::PluginContext* context, const D2RL::Lifecycle::DataTablesLoadedEvent*, void*) noexcept {
	tablesReady = false;
	for (const auto bank : { D2RL::DataTables::Bank::Rotw, D2RL::DataTables::Bank::Lod }) {
		if (ReadItemSizes(context, bank)) {
			ReadGridSize(context, bank);
			tablesReady = true;
			Logf(false, "Inventory grid from the game data: %d x %d.", tableColumns, tableRows);
			return;
		}
	}
	plugin->LogError("Could not read item sizes from the game data. Sorting is disabled.");
}

// ---- Packets --------------------------------------------------------------

static void SendGridPacket(uint8_t packetId, uint32_t itemId, uint32_t x, uint32_t y) noexcept {
	uint8_t packet[17] { packetId };
	std::memcpy(packet + 1, &itemId, 4);
	std::memcpy(packet + 5, &x, 4);
	std::memcpy(packet + 9, &y, 4);
	std::memcpy(packet + 13, &InventoryGridPage, 4);
	sendPacket(packet, sizeof(packet));
}

static void SendSwapPacket(uint32_t cursorItemId, uint32_t gridItemId, int gridX, int gridY, int dropX, int dropY) noexcept {
	const uint16_t cells[4] { static_cast<uint16_t>(gridX), static_cast<uint16_t>(gridY), static_cast<uint16_t>(dropX), static_cast<uint16_t>(dropY) };
	uint8_t        packet[21] { PacketCursorSwap };
	std::memcpy(packet + 1, &cursorItemId, 4);
	std::memcpy(packet + 5, &gridItemId, 4);
	std::memcpy(packet + 9, &InventoryGridPage, 4);
	std::memcpy(packet + 13, cells, sizeof(cells));
	sendPacket(packet, sizeof(packet));
}

static auto ResolveSendPacket() noexcept -> SendClientGameplayPacketFn {
	const HMODULE core = GetModuleHandleW(L"D2RCore.dll");
	if (core == nullptr) {
		plugin->LogError("D2RCore.dll is not loaded. Sorting is disabled.");
		return nullptr;
	}
	const auto address = reinterpret_cast<const uint8_t*>(GetProcAddress(core, "SendClientGameplayPacket"));
	if (address == nullptr) {
		plugin->LogError("D2RCore.dll has no SendClientGameplayPacket export. Sorting is disabled.");
		return nullptr;
	}
	if (std::memcmp(address, SendClientGameplayPacketPrologue, sizeof(SendClientGameplayPacketPrologue)) != 0) {
		plugin->LogError("SendClientGameplayPacket changed in this D2RLoader build. Sorting is disabled until the plugin is updated.");
		return nullptr;
	}
	return reinterpret_cast<SendClientGameplayPacketFn>(address);
}

// ---- Snapshots ------------------------------------------------------------

struct CaptureState {
	Snapshot* snapshot;
	bool      unknownSize;
	uint32_t  unknownCode;
};

static auto __cdecl CaptureItem(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* userData) noexcept -> D2RL::Inventory::IterationAction {
	auto* state = static_cast<CaptureState*>(userData);
	if (state == nullptr || item == nullptr || item->structSize < D2RL::Items::ItemInfoRequiredSize || item->container != D2RL::Items::ItemContainer::Inventory) {
		return D2RL::Inventory::IterationAction::Continue;
	}
	Snapshot* snapshot = state->snapshot;
	if (snapshot->count == MaxItems) {
		return D2RL::Inventory::IterationAction::Stop;
	}
	const ItemSize* size = FindItemSize(item->code);
	if (size == nullptr) {
		state->unknownSize = true;
		state->unknownCode = item->code;
		return D2RL::Inventory::IterationAction::Stop;
	}
	snapshot->items[snapshot->count++] = {
		.runtimeId = item->runtimeId,
		.code      = item->code,
		.group     = size->group,
		.quality   = static_cast<int>(item->quality),
		.x         = item->x,
		.y         = item->y,
		.width     = size->width,
		.height    = size->height,
	};
	return D2RL::Inventory::IterationAction::Continue;
}

static auto Capture(D2RL::PlayerHandle player, Snapshot* snapshot, uint32_t* unknownCode) noexcept -> bool {
	snapshot->count        = 0;
	snapshot->cursorUsed   = false;
	snapshot->cursorItemId = 0;
	CaptureState state { snapshot, false, 0 };

	const D2RL::Inventory::ItemFilter filter {
		.structSize    = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory),
	};
	if (inventory->forEachInventoryItem(plugin, player, &filter, CaptureItem, &state) != D2RL::Inventory::Result::Success) {
		return false;
	}
	if (state.unknownSize) {
		*unknownCode = state.unknownCode;
		return false;
	}

	D2RL::ItemHandle cursor = D2RL::InvalidItemHandle;
	if (inventory->getCursorItem(plugin, player, &cursor) == D2RL::Inventory::Result::Success && cursor != D2RL::InvalidItemHandle) {
		D2RL::Items::ItemInfo info { .structSize = D2RL::Items::ItemInfoSize };
		snapshot->cursorUsed = true;
		if (items->getItemInfo(plugin, cursor, &info) == D2RL::Items::Result::Success) {
			snapshot->cursorItemId = info.runtimeId;
		}
	}
	return true;
}

// ---- Sorting --------------------------------------------------------------

#ifdef INVENTORY_SORT_TEST
static void __cdecl DumpGrid(const D2RL::PluginContext* context, void*) noexcept;
#endif

static auto PlanIndex(uint32_t runtimeId) noexcept -> int {
	for (int index = 0; index < job.count; ++index) {
		if (job.items[index].id == runtimeId) {
			return index;
		}
	}
	return -1;
}

// True when the inventory is exactly where the plan expects it now.
static auto MatchesPlan(const Snapshot& now) noexcept -> bool {
	if (now.count != job.count - (job.hand >= 0 ? 1 : 0)) {
		return false;
	}
	if (job.hand >= 0 ? !now.cursorUsed || now.cursorItemId != job.items[job.hand].id : now.cursorUsed) {
		return false;
	}
	for (int index = 0; index < now.count; ++index) {
		const int k = PlanIndex(now.items[index].runtimeId);
		if (k < 0 || k == job.hand || job.x[k] != now.items[index].x || job.y[k] != now.items[index].y) {
			return false;
		}
	}
	return true;
}

static void Finish(bool success, const char* reason = nullptr) noexcept {
	if (success) {
		Tellf(false, "Inventory sorted (%zu moves).", job.moves.size());
	} else {
		Tellf(true, "Inventory sort stopped: %s", reason);
	}
	job.state = JobState::Idle;
#ifdef INVENTORY_SORT_TEST
	DumpGrid(plugin, nullptr);
#endif
}

static void SendNextMove() noexcept {
	using InventoryPlanner::MoveKind;
	const InventoryPlanner::Move& move = job.moves[job.next++];
	const int                     k    = move.item;
	const uint32_t                id   = job.items[k].id;
	switch (move.kind) {
	case MoveKind::Pick:
		if (config.logMoves) {
			Logf(false, "Pick up id=%u at %d,%d.", id, job.x[k], job.y[k]);
		}
		SendGridPacket(PacketGridToCursor, id, static_cast<uint32_t>(job.x[k]), static_cast<uint32_t>(job.y[k]));
		job.hand = k;
		break;
	case MoveKind::Place:
		if (config.logMoves) {
			Logf(false, "Place id=%u at %d,%d.", id, move.x, move.y);
		}
		SendGridPacket(PacketCursorToGrid, id, move.x, move.y);
		job.x[k] = move.x, job.y[k] = move.y, job.hand = -1;
		break;
	case MoveKind::Swap: {
		const int other = move.other;
		if (config.logMoves) {
			Logf(false, "Swap id=%u into %d,%d for id=%u.", id, move.x, move.y, job.items[other].id);
		}
		SendSwapPacket(id, job.items[other].id, job.x[other], job.y[other], move.x, move.y);
		job.x[k] = move.x, job.y[k] = move.y, job.hand = other;
		break;
	}
	}
	job.waiting = true;
	job.sentAt  = GetTickCount64();
}

static void PlanOnWorker() noexcept {
	int result = PlanNone;
	try {
		result = InventoryPlanner::Plan(job.items, job.count, job.columns, job.rows, job.moves) ? PlanFound : PlanNone;
	} catch (...) {
		result = PlanNone;
	}
	planResult.store(result, std::memory_order_release);
}

static void __cdecl Tick(const D2RL::PluginContext* context, void*) noexcept {
	if (job.state == JobState::Idle) {
		return;
	}
	if (job.state == JobState::Planning) {
		const int result = planResult.load(std::memory_order_acquire);
		if (result == PlanPending) {
			(void)threads->runOnUiThread(context, Tick, nullptr);
			return;
		}
		planner.join();
		if (result != PlanFound) {
			job.state = JobState::Idle;
			Tellf(true, "There is not enough free space to sort this inventory. Free a few cells and try again.");
			return;
		}
		if (job.moves.empty()) {
			job.state = JobState::Idle;
			Tellf(false, "The inventory is already sorted.");
			return;
		}
		if (config.logMoves) {
			Logf(false, "Planned %zu moves for %d items.", job.moves.size(), job.count);
		}
		job.state = JobState::Moving;
	}

	D2RL::PlayerHandle player      = D2RL::InvalidPlayerHandle;
	uint32_t           unknownCode = 0;
	if (inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success || player != job.player || !Capture(player, &scratch, &unknownCode)) {
		Finish(false, "the character is no longer available.");
		return;
	}
	if (!MatchesPlan(scratch)) {
		if (!job.waiting) {
			Finish(false, scratch.cursorUsed ? "the inventory changed while sorting. Put down the item on the cursor." : "the inventory changed while sorting.");
			return;
		}
		if (GetTickCount64() - job.sentAt > MoveTimeoutMs) {
			Finish(false, scratch.cursorUsed ? "the game did not accept a move. Put down the item on the cursor." : "the game did not accept a move.");
			return;
		}
	} else {
		job.waiting = false;
		if (job.next == job.moves.size()) {
			Finish(true);
			return;
		}
		SendNextMove();
	}
	if (threads->runOnUiThread(context, Tick, nullptr) != D2RL::Threads::Result::Success) {
		job.state = JobState::Idle;
		plugin->LogError("Could not schedule the next sort step.");
	}
}

static void StartSort(const D2RL::PluginContext* context) noexcept {
	if (job.state != JobState::Idle) {
		return;
	}
	if (sendPacket == nullptr) {
		Tellf(true, "Inventory sort is unavailable in this D2RLoader build. See inventory-sort.log.");
		return;
	}
	if (!tablesReady) {
		Tellf(true, "Inventory sort is unavailable: item sizes could not be read from the game data.");
		return;
	}

	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success || player == D2RL::InvalidPlayerHandle) {
		Tellf(true, "Inventory sort needs a character in a game.");
		return;
	}
	uint32_t unknownCode = 0;
	if (!Capture(player, &scratch, &unknownCode)) {
		char code[5] {};
		Tellf(true, unknownCode != 0 ? "Inventory sort does not know the size of item '%s'." : "Inventory sort could not read the inventory.", CodeText(unknownCode, code));
		return;
	}
	if (scratch.cursorUsed) {
		Tellf(true, "Put down the item on the cursor before sorting.");
		return;
	}
	if (scratch.count > InventoryPlanner::MaxItems) {
		Tellf(true, "Inventory sort supports up to %d items.", InventoryPlanner::MaxItems);
		return;
	}

	const int columns = config.columns != 0 ? config.columns : tableColumns;
	const int rows    = config.rows != 0 ? config.rows : tableRows;
	for (int index = 0; index < scratch.count; ++index) {
		const GridItem& item = scratch.items[index];
		if (item.x < 0 || item.y < 0 || item.x + item.width > columns || item.y + item.height > rows) {
			Tellf(true, "Inventory sort expected a %d x %d inventory but found an item at %d,%d. Set columns and rows in inventory-sort.toml.", columns, rows, item.x, item.y);
			return;
		}
	}

	job.player  = player;
	job.columns = columns;
	job.rows    = rows;
	job.count   = scratch.count;
	job.next    = 0;
	job.hand    = -1;
	job.waiting = false;
	job.moves.clear();
	for (int index = 0; index < scratch.count; ++index) {
		const GridItem& item = scratch.items[index];
		job.items[index]     = { item.runtimeId, item.code, item.group, item.quality, item.x, item.y, item.width, item.height };
		job.x[index]         = item.x;
		job.y[index]         = item.y;
	}

	planResult.store(PlanPending, std::memory_order_relaxed);
	try {
		planner = std::thread(PlanOnWorker);
	} catch (...) {
		Tellf(true, "Inventory sort could not start its planner.");
		return;
	}
	job.state = JobState::Planning;
	if (threads->runOnUiThread(context, Tick, nullptr) != D2RL::Threads::Result::Success) {
		plugin->LogError("Could not schedule the sort.");
		planner.join();
		job.state = JobState::Idle;
	}
}

// ---- Test harness ---------------------------------------------------------

#ifdef INVENTORY_SORT_TEST
// Test builds only (build.ps1 -Test). Drives the plugin from a command file,
// <game>\d2rloader\inventory-sort-test.txt, so a test can run without clicks:
//   fill <seed>   create test items at scattered cells (local games only)
//   sort          press the Sort button
//   dump          log the inventory grid
//   clear         destroy the items fill created
//   purge         destroy every inventory item with a test item code

static constexpr ULONGLONG TestPollMs = 300;

// Large items first so the scatter can place most of them.
static constexpr uint32_t TestItemCodes[] = {
	D2RL::Items::MakeItemCode("spr"), D2RL::Items::MakeItemCode("qui"), D2RL::Items::MakeItemCode("axe"), D2RL::Items::MakeItemCode("cap"),
	D2RL::Items::MakeItemCode("buc"), D2RL::Items::MakeItemCode("hax"), D2RL::Items::MakeItemCode("dgr"), D2RL::Items::MakeItemCode("tbk"),
	D2RL::Items::MakeItemCode("cm2"), D2RL::Items::MakeItemCode("lbl"), D2RL::Items::MakeItemCode("hp1"), D2RL::Items::MakeItemCode("mp1"),
	D2RL::Items::MakeItemCode("hp1"), D2RL::Items::MakeItemCode("gcv"), D2RL::Items::MakeItemCode("r01"), D2RL::Items::MakeItemCode("rin"),
	D2RL::Items::MakeItemCode("amu"), D2RL::Items::MakeItemCode("cm1"), D2RL::Items::MakeItemCode("mp1"), D2RL::Items::MakeItemCode("key"),
	D2RL::Items::MakeItemCode("hp1"), D2RL::Items::MakeItemCode("gcv"), D2RL::Items::MakeItemCode("r01"),
};

static ULONGLONG          testLastPoll;
static bool               testPolling;
static D2RL::PlayerHandle testPlayer = D2RL::InvalidPlayerHandle;
static uint32_t           testSeed;
static D2RL::ItemHandle   testItems[64];
static int                testItemCount;
static Snapshot           testScratch;

static void __cdecl DumpGrid(const D2RL::PluginContext* context, void*) noexcept {
	D2RL::PlayerHandle player      = D2RL::InvalidPlayerHandle;
	uint32_t           unknownCode = 0;
	if (inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success || !Capture(player, &testScratch, &unknownCode)) {
		plugin->LogWarn("test: could not read the inventory.");
		return;
	}
	const int columns = config.columns != 0 ? config.columns : tableColumns;
	const int rows    = config.rows != 0 ? config.rows : tableRows;
	char      grid[MaxGridSide][MaxGridSide + 1] {};
	for (int y = 0; y < rows; ++y) {
		std::memset(grid[y], '.', static_cast<size_t>(columns));
	}
	bool overlap = false;
	for (int index = 0; index < testScratch.count; ++index) {
		const GridItem& item = testScratch.items[index];
		char            code[5] {};
		Logf(false, "test: %c = %s %dx%d group %d id=%u at %d,%d", 'A' + index % 26, CodeText(item.code, code), item.width, item.height, item.group, item.runtimeId, item.x, item.y);
		for (int y = item.y; y < item.y + item.height && y < rows; ++y) {
			for (int x = item.x; x < item.x + item.width && x < columns; ++x) {
				overlap    = overlap || grid[y][x] != '.';
				grid[y][x] = static_cast<char>('A' + index % 26);
			}
		}
	}
	for (int y = 0; y < rows; ++y) {
		Logf(false, "test: |%s|", grid[y]);
	}
	Logf(overlap, "test: %d items, cursor=%s%s", testScratch.count, testScratch.cursorUsed ? "busy" : "empty", overlap ? ", OVERLAP" : "");
}

static auto NextRandom() noexcept -> uint32_t {
	testSeed = testSeed * 1'664'525U + 1'013'904'223U;
	return testSeed >> 8U;
}

static void __cdecl FillOnGameThread(const D2RL::PluginContext* context, void*) noexcept {
	const D2RL::Inventory::ItemFilter filter {
		.structSize    = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory),
	};
	testScratch.count = 0;
	CaptureState state { &testScratch, false, 0 };
	if (inventory->forEachInventoryItem(context, testPlayer, &filter, CaptureItem, &state) != D2RL::Inventory::Result::Success || state.unknownSize) {
		plugin->LogWarn("test: fill could not read the inventory.");
		return;
	}
	const int columns = tableColumns;
	const int rows    = tableRows;
	bool      used[MaxGridSide][MaxGridSide] {};
	for (int index = 0; index < testScratch.count; ++index) {
		const GridItem& item = testScratch.items[index];
		for (int x = item.x; x < item.x + item.width; ++x) {
			for (int y = item.y; y < item.y + item.height; ++y) {
				used[x][y] = true;
			}
		}
	}

	int created = 0;
	for (const uint32_t code : TestItemCodes) {
		const ItemSize* size = FindItemSize(code);
		if (size == nullptr || testItemCount == 64) {
			continue;
		}
		for (int attempt = 0; attempt < 400; ++attempt) {
			const int x    = static_cast<int>(NextRandom() % static_cast<uint32_t>(columns - size->width + 1));
			const int y    = static_cast<int>(NextRandom() % static_cast<uint32_t>(rows - size->height + 1));
			bool      free = true;
			for (int cx = x; cx < x + size->width && free; ++cx) {
				for (int cy = y; cy < y + size->height && free; ++cy) {
					free = !used[cx][cy];
				}
			}
			if (!free) {
				continue;
			}
			D2RL::Items::ItemCreateSpec spec {
				.structSize      = D2RL::Items::ItemCreateSpecSize,
				.code            = code,
				.quality         = D2RL::Items::Quality::Normal,
				.qualityRecordId = D2RL::Items::RandomQualityRecord,
				.itemLevel       = 1,
				.seedMode        = D2RL::Items::SeedMode::Random,
				.quantity        = D2RL::Items::DefaultValue,
				.durability      = D2RL::Items::DefaultValue,
				.stateFlags      = D2RL::Items::ItemStateIdentified,
				.destination     = {
					.structSize = D2RL::Items::ItemDestinationSize,
					.container  = D2RL::Items::ItemContainer::Inventory,
					.placement  = D2RL::Items::Placement::Exact,
					.x          = static_cast<uint32_t>(x),
					.y          = static_cast<uint32_t>(y),
				},
			};
			D2RL::ItemHandle handle = D2RL::InvalidItemHandle;
			const auto       result = items->createItem(context, testPlayer, &spec, &handle);
			if (result != D2RL::Items::Result::Success) {
				char text[5] {};
				Logf(true, "test: could not create %s at %d,%d (result %u).", CodeText(code, text), x, y, static_cast<uint32_t>(result));
				break;
			}
			testItems[testItemCount++] = handle;
			++created;
			for (int cx = x; cx < x + size->width; ++cx) {
				for (int cy = y; cy < y + size->height; ++cy) {
					used[cx][cy] = true;
				}
			}
			break;
		}
	}
	Logf(false, "test: created %d items.", created);
}

static void __cdecl ClearOnGameThread(const D2RL::PluginContext* context, void*) noexcept {
	int destroyed = 0;
	for (int index = 0; index < testItemCount; ++index) {
		if (items->destroyItem(context, testPlayer, testItems[index], D2RL::Items::SocketedItemPolicy::RejectIfNotEmpty) == D2RL::Items::Result::Success) {
			++destroyed;
		}
	}
	testItemCount = 0;
	Logf(false, "test: destroyed %d items.", destroyed);
}

// Destroys every inventory item whose code is on the test list, including
// items fill made in an earlier session.
static auto __cdecl PurgeItem(const D2RL::PluginContext* context, const D2RL::Items::ItemInfo* item, void*) noexcept -> D2RL::Inventory::IterationAction {
	if (item == nullptr || item->container != D2RL::Items::ItemContainer::Inventory) {
		return D2RL::Inventory::IterationAction::Continue;
	}
	for (const uint32_t code : TestItemCodes) {
		if (item->code == code || item->code == D2RL::Items::MakeItemCode("lbw") || item->code == D2RL::Items::MakeItemCode("aqv")) {
			if (testItemCount < 64) {
				testItems[testItemCount++] = item->handle;
			}
			break;
		}
	}
	(void)context;
	return D2RL::Inventory::IterationAction::Continue;
}

static void __cdecl PurgeOnGameThread(const D2RL::PluginContext* context, void*) noexcept {
	testItemCount = 0;
	const D2RL::Inventory::ItemFilter filter {
		.structSize    = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory),
	};
	(void)inventory->forEachInventoryItem(context, testPlayer, &filter, PurgeItem, nullptr);
	ClearOnGameThread(context, nullptr);
}

static void RunTestCommand(const D2RL::PluginContext* context, char* command) noexcept {
	Logf(false, "test: command '%s'", command);
	if (inventory->getLocalPlayer(context, &testPlayer) != D2RL::Inventory::Result::Success || testPlayer == D2RL::InvalidPlayerHandle) {
		plugin->LogWarn("test: no character in a game.");
		return;
	}
	if (std::strncmp(command, "fill", 4) == 0) {
		testSeed = static_cast<uint32_t>(std::atoi(TrimLeft(command + 4))) * 2'654'435'761U + 1U;
		if (threads->runOnGameThread(context, FillOnGameThread, nullptr) != D2RL::Threads::Result::Success) {
			plugin->LogWarn("test: fill needs a local game.");
		}
	} else if (std::strcmp(command, "sort") == 0) {
		StartSort(context);
	} else if (std::strcmp(command, "dump") == 0) {
		DumpGrid(context, nullptr);
	} else if (std::strncmp(command, "swap ", 5) == 0) {
		// swap gx gy dx dy: drop the cursor item at dx,dy onto the item at gx,gy.
		char*      end   = nullptr;
		const long gridX = std::strtol(command + 5, &end, 10);
		const long gridY = std::strtol(end, &end, 10);
		const long dropX = std::strtol(end, &end, 10);
		const long dropY = std::strtol(end, nullptr, 10);
		uint32_t   unknownCode = 0;
		if (!Capture(testPlayer, &testScratch, &unknownCode) || !testScratch.cursorUsed) {
			plugin->LogWarn("test: swap needs an item on the cursor.");
			return;
		}
		for (int index = 0; index < testScratch.count; ++index) {
			const GridItem& item = testScratch.items[index];
			if (item.x == gridX && item.y == gridY) {
				Logf(false, "test: sending swap cursor=%u grid=%u at %ld,%ld drop %ld,%ld", testScratch.cursorItemId, item.runtimeId, gridX, gridY, dropX, dropY);
				SendSwapPacket(testScratch.cursorItemId, item.runtimeId, item.x, item.y, static_cast<int>(dropX), static_cast<int>(dropY));
				return;
			}
		}
		plugin->LogWarn("test: no item has its top-left cell there.");
	} else if (std::strncmp(command, "log ", 4) == 0) {
		config.logMoves = command[4] == '1';
	} else if (std::strncmp(command, "pick ", 5) == 0 || std::strncmp(command, "place ", 6) == 0) {
		// pick x y: the item covering cell x,y goes to the cursor, sending x,y as given.
		// place x y: the cursor item is dropped with x,y as given.
		const bool  pick = command[1] == 'i';
		char*       end  = nullptr;
		const long  x    = std::strtol(command + (pick ? 5 : 6), &end, 10);
		const long  y    = std::strtol(end, nullptr, 10);
		uint32_t    unknownCode = 0;
		if (!Capture(testPlayer, &testScratch, &unknownCode)) {
			plugin->LogWarn("test: could not read the inventory.");
			return;
		}
		uint32_t itemId = testScratch.cursorUsed ? testScratch.cursorItemId : 0;
		if (pick) {
			itemId = 0;
			for (int index = 0; index < testScratch.count; ++index) {
				const GridItem& item = testScratch.items[index];
				if (x >= item.x && x < item.x + item.width && y >= item.y && y < item.y + item.height) {
					itemId = item.runtimeId;
				}
			}
		}
		if (itemId == 0) {
			plugin->LogWarn("test: no item for that command.");
			return;
		}
		Logf(false, "test: sending %s id=%u x=%ld y=%ld", pick ? "pick" : "place", itemId, x, y);
		SendGridPacket(pick ? PacketGridToCursor : PacketCursorToGrid, itemId, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
	} else if (std::strcmp(command, "purge") == 0) {
		if (threads->runOnGameThread(context, PurgeOnGameThread, nullptr) != D2RL::Threads::Result::Success) {
			plugin->LogWarn("test: purge needs a local game.");
		}
	} else if (std::strcmp(command, "clear") == 0) {
		if (threads->runOnGameThread(context, ClearOnGameThread, nullptr) != D2RL::Threads::Result::Success) {
			plugin->LogWarn("test: clear needs a local game.");
		}
	}
}

static void __cdecl PollTestCommands(const D2RL::PluginContext* context, void*) noexcept {
	const ULONGLONG time = GetTickCount64();
	if (time - testLastPoll >= TestPollMs) {
		testLastPoll = time;
		wchar_t path[MAX_PATH] {};
		if (GetModuleFileNameW(nullptr, path, MAX_PATH) != 0) {
			if (wchar_t* slash = std::wcsrchr(path, L'\\')) {
				wcscpy_s(slash + 1, static_cast<size_t>(MAX_PATH - (slash + 1 - path)), L"d2rloader\\inventory-sort-test.txt");
				const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file != INVALID_HANDLE_VALUE) {
					char  command[64] {};
					DWORD read = 0;
					(void)ReadFile(file, command, sizeof(command) - 1, &read, nullptr);
					CloseHandle(file);
					DeleteFileW(path);
					command[read] = 0;
					for (DWORD index = 0; index < read; ++index) {
						if (command[index] == '\r' || command[index] == '\n') {
							command[index] = 0;
						}
					}
					RunTestCommand(context, command);
				}
			}
		}
	}
	testPolling = threads->runOnUiThread(context, PollTestCommands, nullptr) == D2RL::Threads::Result::Success;
}

// Work queued during plugin load can be dropped before the UI exists, so the
// poll also starts once a character is ready.
static void StartTestPolling(const D2RL::PluginContext* context) noexcept {
	if (!testPolling) {
		testPolling = threads->runOnUiThread(context, PollTestCommands, nullptr) == D2RL::Threads::Result::Success;
	}
}

static void __cdecl OnTestPlayerReady(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent*, void*) noexcept {
	StartTestPolling(context);
}
#endif

// ---- UI and console -------------------------------------------------------

static auto __cdecl OnUiMessage(const D2RL::PluginContext* context, const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept -> D2RL::SharedEvents::UiMessageAction {
	if (event == nullptr || event->structSize < D2RL::SharedEvents::UiMessageEventRequiredSize || event->target == nullptr || event->command == nullptr || event->text == nullptr) {
		return D2RL::SharedEvents::UiMessageAction::Continue;
	}
	if (std::strcmp(event->target, ClickTarget) != 0 || std::strcmp(event->command, ClickCommand) != 0 || std::strcmp(event->text, ClickText) != 0) {
		return D2RL::SharedEvents::UiMessageAction::Continue;
	}
	StartSort(context);
	return D2RL::SharedEvents::UiMessageAction::Consume;
}

static void __cdecl StartSortOnUiThread(const D2RL::PluginContext* context, void*) noexcept {
	StartSort(context);
}

static auto SortCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	return threads->runOnUiThread(command->plugin, StartSortOnUiThread, nullptr) == D2RL::Threads::Result::Success ? D2RL::ConsoleCommandResult::Handled : D2RL::ConsoleCommandResult::Failed;
}

static auto RegisterBytes(const D2RL::ResourceService* resources, const char* path, const void* bytes, uint64_t size) noexcept -> bool {
	const D2RL::Resources::ResourceRegistration resource {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = path,
		.bytes      = bytes,
		.byteCount  = size,
	};
	D2RL::Resources::RegistrationHandle handle = D2RL::Resources::InvalidHandle;
	const auto                          result = resources->registerResource(plugin, &resource, &handle);
	if (result != D2RL::Resources::Result::Success) {
		Logf(true, "Could not register %s (result %u).", path, static_cast<uint32_t>(result));
		return false;
	}
	return true;
}

static auto RegisterSprite(const D2RL::ResourceService* resources, HMODULE module, int resourceId, const char* path) noexcept -> bool {
	const HRSRC   info = FindResourceW(module, MAKEINTRESOURCEW(resourceId), MAKEINTRESOURCEW(10)); // RT_RCDATA
	const HGLOBAL data = info != nullptr ? LoadResource(module, info) : nullptr;
	const void*   bytes = data != nullptr ? LockResource(data) : nullptr;
	if (bytes == nullptr) {
		Logf(true, "The %s sprite is missing from the DLL.", path);
		return false;
	}
	return RegisterBytes(resources, path, bytes, SizeofResource(module, info));
}

static auto AddButton(const D2RL::PluginContext* context) noexcept -> bool {
	const D2RL::ResourceService* resources = nullptr;
	const D2RL::PanelService*    panels    = nullptr;
	if (context->QueryService(&resources) != D2RL::ServiceQueryResult::Success || !D2RL::HasResourceServiceField(resources, D2RL::ResourceServiceRequiredSize)) {
		plugin->LogError("The resource service is unavailable, so the Sort button cannot be added.");
		return false;
	}
	if (context->QueryService(&panels) != D2RL::ServiceQueryResult::Success || !D2RL::HasPanelServiceField(panels, D2RL::PanelServiceRegisterChildLayoutFieldEnd)) {
		plugin->LogError("This D2RLoader version cannot add buttons to the inventory.");
		return false;
	}

	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&AddButton), &module);
	if (!RegisterSprite(resources, module, ButtonSpriteResource, "data/hd/global/ui/d2rloader/inventory-sort/sort_button.sprite")
	    || !RegisterSprite(resources, module, ButtonLowEndSpriteResource, "data/hd/global/ui/d2rloader/inventory-sort/sort_button.lowend.sprite")
	    || !RegisterBytes(resources, "data/global/ui/layouts/inventory-sort/SortButtonhd.json", ButtonLayout, sizeof(ButtonLayout) - 1)) {
		return false;
	}

	const D2RL::Panels::ChildLayoutRegistration registration {
		.structSize = D2RL::Panels::ChildLayoutRegistrationSize,
		.flags      = D2RL::Panels::ChildLayoutFlags::KeyboardMouseOnly,
		.stockPanel = D2RL::Panels::StockPanel::PlayerInventory,
		.localId    = "SortButton",
	};
	D2RL::Panels::ChildLayoutHandle handle = D2RL::Panels::InvalidChildLayoutHandle;
	const auto                      result = panels->registerChildLayout(context, &registration, &handle);
	if (result != D2RL::Panels::Result::Success) {
		Logf(true, "Could not add the Sort button to the inventory (result %u).", static_cast<uint32_t>(result));
		return false;
	}
	return true;
}

static auto ListenForClicks(const D2RL::PluginContext* context) noexcept -> bool {
	const D2RL::SharedEventService* events = nullptr;
	if (context->QueryService(&events) != D2RL::ServiceQueryResult::Success || !D2RL::HasSharedEventServiceField(events, D2RL::SharedEventServiceRequiredSize)) {
		plugin->LogError("The shared event service is unavailable, so the Sort button cannot be used.");
		return false;
	}
	const D2RL::SharedEvents::UiMessageListener listener {
		.structSize = D2RL::SharedEvents::UiMessageListenerSize,
		.callback   = OnUiMessage,
	};
	D2RL::SharedEvents::ListenerHandle handle = D2RL::SharedEvents::InvalidHandle;
	const auto                         result = events->registerUiMessageListener(context, &listener, &handle);
	if (result != D2RL::SharedEvents::Result::Success) {
		Logf(true, "Could not listen for the Sort button (result %u).", static_cast<uint32_t>(result));
		return false;
	}
	return true;
}

// ---- Plugin entry points --------------------------------------------------

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &InventorySortPluginInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	plugin = context;

	if (context->QueryService(&inventory) != D2RL::ServiceQueryResult::Success || !D2RL::HasInventoryServiceField(inventory, D2RL::InventoryServiceRequiredSize)) {
		context->LogError("The inventory service is unavailable.");
		return false;
	}
	if (context->QueryService(&items) != D2RL::ServiceQueryResult::Success || !D2RL::HasItemServiceField(items, D2RL::ItemServiceRequiredSize)) {
		context->LogError("The item service is unavailable.");
		return false;
	}
	if (context->QueryService(&threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(threads, D2RL::ThreadServiceRequiredSize)) {
		context->LogError("The thread service is unavailable.");
		return false;
	}
	if (context->QueryService(&dataTables) != D2RL::ServiceQueryResult::Success || !D2RL::HasDataTableServiceField(dataTables, D2RL::DataTableServiceRequiredSize)) {
		context->LogError("The data table service is unavailable.");
		return false;
	}

	const D2RL::LifecycleService* lifecycle = nullptr;
	if (context->QueryService(&lifecycle) != D2RL::ServiceQueryResult::Success || !D2RL::HasLifecycleServiceField(lifecycle, D2RL::LifecycleServiceRequiredSize)) {
		context->LogError("The lifecycle service is unavailable.");
		return false;
	}
	const D2RL::Lifecycle::DataTablesLoadedListener tablesListener {
		.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize,
		.callback   = OnDataTablesLoaded,
	};
	D2RL::Lifecycle::ListenerHandle tablesHandle = D2RL::Lifecycle::InvalidHandle;
	if (lifecycle->registerDataTablesLoadedListener(context, &tablesListener, &tablesHandle) != D2RL::Lifecycle::Result::Success) {
		context->LogError("Could not listen for data-table loads.");
		return false;
	}

	LoadConfig();
	sendPacket = ResolveSendPacket();

	bool buttonReady = false;
	if (config.showButton) {
		buttonReady = ListenForClicks(context) && AddButton(context);
	}
	(void)context->RegisterConsoleCommand("inventory-sort", SortCommand, "Sort the items in your inventory.");
#ifdef INVENTORY_SORT_TEST
	const D2RL::Lifecycle::GameplayEventListener testListener {
		.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
		.kind       = D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady,
		.callback   = OnTestPlayerReady,
	};
	D2RL::Lifecycle::ListenerHandle testHandle = D2RL::Lifecycle::InvalidHandle;
	(void)lifecycle->registerGameplayEventListener(context, &testListener, &testHandle);
	StartTestPolling(context);
	plugin->LogWarn("This is a test build. It reads commands from d2rloader\\inventory-sort-test.txt.");
#endif
	Logf(false, "Inventory Sort loaded: button=%s send=%s.", config.showButton ? (buttonReady ? "ok" : "failed") : "off", sendPacket != nullptr ? "ok" : "missing");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	if (planner.joinable()) {
		planner.join();
	}
	job.state = JobState::Idle;
}
