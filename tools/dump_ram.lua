--[[
dump_ram.lua -- dump a running GBA's RAM from mgba-headless.

    mgba-headless -t state.ss1 --script dump_ram.lua chip_gate_homebrew.gba

Writes ewram.bin (256 KB at 0x02000000) and iwram.bin (32 KB at 0x03000000).
The Chip Gate homebrew builds its 14-byte chip records in RAM at startup, so a
savestate taken after picking a game in its menu contains the table that maps
chip index to the word the gate puts on the wire.

Environment:
    MGBA_FRAMES  frames to run before dumping (default 120)
    MGBA_OUT     output prefix (default "dump")
    MGBA_KEYS    optional blind key macro, see below

Loading a savestate is strongly preferred over the key macro: mgba-headless has
no display, so driving the menu blind means guessing at cursor positions.
--]]

local FRAMES = tonumber(os.getenv("MGBA_FRAMES") or "120")
local PREFIX = os.getenv("MGBA_OUT") or "dump"
local MACRO  = os.getenv("MGBA_KEYS")

local REGIONS = {
	{ name = "ewram", base = 0x02000000, size = 0x40000 },
	{ name = "iwram", base = 0x03000000, size = 0x08000 },
}

-- Optional macro: "30:A,60:DOWN,90:A" -> press that key on that frame.
-- Each entry is held for 4 frames, which the homebrew's menu reliably samples.
local schedule = {}
if MACRO then
	for pair in string.gmatch(MACRO, "[^,]+") do
		local f, k = string.match(pair, "^%s*(%d+)%s*:%s*(%w+)%s*$")
		if f then
			schedule[tonumber(f)] = string.upper(k)
		end
	end
end

local KEYS = {
	A = 0, B = 1, SELECT = 2, START = 3,
	RIGHT = 4, LEFT = 5, UP = 6, DOWN = 7, R = 8, L = 9,
}

local frame = 0
local holding = nil
local holdUntil = 0
local done = false

local function writeRegion(r)
	local data = emu:readRange(r.base, r.size)
	local path = string.format("%s_%s.bin", PREFIX, r.name)
	local f, err = io.open(path, "wb")
	if not f then
		console:error(string.format("cannot open %s: %s", path, tostring(err)))
		return false
	end
	f:write(data)
	f:close()
	console:log(string.format("%s  0x%08X  %d bytes -> %s", r.name, r.base, #data, path))
	return true
end

local function onFrame()
	frame = frame + 1

	local key = schedule[frame]
	if key and KEYS[key] then
		emu:setKeys(1 << KEYS[key])
		holding = key
		holdUntil = frame + 4
		console:log(string.format("frame %d: press %s", frame, key))
	elseif holding and frame >= holdUntil then
		emu:setKeys(0)
		holding = nil
	end

	if frame < FRAMES or done then
		return
	end
	done = true

	local ok = true
	for _, r in ipairs(REGIONS) do
		ok = writeRegion(r) and ok
	end
	console:log(ok and "dump complete" or "dump finished with errors")
	os.exit(ok and 0 or 1)
end

console:log(string.format("dumping at frame %d, prefix '%s'", FRAMES, PREFIX))
callbacks:add("frame", onFrame)
