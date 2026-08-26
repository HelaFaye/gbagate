--[[
mgba_tester.lua -- run the custom tester ROM against the firmware gate, and
report a verdict.

    python3 qemu/broker.py --mgba 7000                     # first: it listens
    mgba-headless --script qemu/mgba_tester.lua rom/tester.gba

This is two things in one script, because mGBA takes a single --script:

  1. A *correct* bridge between the ROM and the firmware gate. It differs from
     mgba_link.lua in the one way that matters against a real gate: the firmware
     gate consumes link_proto WORD packets and emits RESULT packets (the gate's
     answer is RESULT word[1] -- see src/link_bridge.c and src/link_source.c).
     So this script SENDS WORD (the ROM's staged word) and RECEIVES RESULT (the
     gate's answer), where mgba_link.lua had them the other way round and would
     deadlock against the firmware through the relay.

  2. A checker. The ROM publishes a verdict to a fixed EWRAM address
     (rom/tester.c); this reads it, logs "ROM-TEST: PASS" or "ROM-TEST: FAIL",
     saves a screenshot, writes a small result file, and exits. qemu/test_rom.sh
     watches for those.

Environment:
    LINK_HOST / LINK_PORT   where broker.py --mgba listens (default 127.0.0.1:7000)
    ROMTEST_SHOT            screenshot path      (default rom_test.png)
    ROMTEST_RESULT          result file path     (default rom_test.result)
    ROMTEST_MAX_FRAMES      give up after N frames (default 1500, ~25 s)
--]]

local HOST = os.getenv("LINK_HOST") or "127.0.0.1"
local PORT = tonumber(os.getenv("LINK_PORT") or "7000")
local SHOT   = os.getenv("ROMTEST_SHOT")   or "rom_test.png"
local RESULT = os.getenv("ROMTEST_RESULT") or "rom_test.result"
local MAX_FRAMES = tonumber(os.getenv("ROMTEST_MAX_FRAMES") or "1500")

-- link_proto constants, mirroring src/link_proto.h
local T_HELLO, T_HELLO_ACK = 0x01, 0x02
local T_WORD, T_RESULT     = 0x10, 0x11
local T_PING, T_PONG       = 0x30, 0x31
local ROLE_MASTER          = 1     -- the game is the parent
local VERSION              = 1

local SLIP_END, SLIP_ESC         = 0xC0, 0xDB
local SLIP_ESC_END, SLIP_ESC_ESC = 0xDC, 0xDD

-- GBA serial registers (our contract with tester.c)

-- tester.c result signature in EWRAM
local RESULT_ADDR  = 0x0203FF00
-- Mailbox, laid out by result_t in rom/tester.c. Offsets follow the 9-entry
-- rotation array (18 bytes) and its padding halfword.
local MB_TO_GATE   = RESULT_ADDR + 44   -- ROM -> gate, u16
local MB_SEQ_TO    = RESULT_ADDR + 46
local MB_FROM_GATE = RESULT_ADDR + 48   -- gate -> ROM, u16
local MB_SEQ_FROM  = RESULT_ADDR + 50
local RESULT_MAGIC = 0x47544247
local ST_RUNNING, ST_PASS, ST_FAIL = 0, 1, 2

local sock, rxbuf, escaped = nil, "", false
local last_seq_to, seq_from = 0, 0
local frame_cb = nil
local seq, linked, frames = 0, false, 0
local done = false

local function crc16(s)
  local crc = 0xFFFF
  for i = 1, #s do
    crc = crc ~ (string.byte(s, i) << 8)
    crc = crc & 0xFFFF
    for _ = 1, 8 do
      if crc & 0x8000 ~= 0 then crc = ((crc << 1) ~ 0x1021) & 0xFFFF
      else crc = (crc << 1) & 0xFFFF end
    end
  end
  return crc
end

local function slip(raw)
  local out = string.char(SLIP_END)
  for i = 1, #raw do
    local b = string.byte(raw, i)
    if b == SLIP_END then out = out .. string.char(SLIP_ESC, SLIP_ESC_END)
    elseif b == SLIP_ESC then out = out .. string.char(SLIP_ESC, SLIP_ESC_ESC)
    else out = out .. string.char(b) end
  end
  return out .. string.char(SLIP_END)
end

local function send(ptype, s, payload)
  payload = payload or ""
  local body = string.char(ptype, s & 0xFF, (s >> 8) & 0xFF) .. payload
  local c = crc16(body)
  sock:send(slip(body .. string.char(c & 0xFF, (c >> 8) & 0xFF)))
end

local function u16(v) return string.char(v & 0xFF, (v >> 8) & 0xFF) end

local function finish(status, note)
  if done then return end
  done = true

  -- Tear the callbacks and the socket down *before* anything else. Leaving
  -- the frame callback registered while the process exits, or exiting with a
  -- live socket callback, segfaults mGBA -- which showed up here as an empty
  -- log and exit 139, with no Lua error to go on.
  if frame_cb then
    pcall(function() callbacks:remove(frame_cb) end)
    frame_cb = nil
  end
  if sock then
    pcall(function() sock:close() end)
    sock = nil
  end
  local pass = (status == ST_PASS)
  local verdict = pass and "PASS" or "FAIL"
  console:log(string.format("ROM-TEST: %s (%s)", verdict, note or ""))

  -- No screenshot here. emu:screenshot() from inside a frame callback that is
  -- also tearing itself down crashes headless mGBA, and a picture is not worth
  -- losing the verdict for. qemu/fbgrab.py takes one from the firmware side.

  -- Result file, best effort, so test_rom.sh has something unambiguous to read.
  pcall(function()
    local f = io.open(RESULT, "w")
    if f then
      f:write(verdict .. "\n")
      if emu and status ~= nil then
        f:write(string.format("status=%d\n", status))
        f:write(string.format("frames=%d\n", emu:read32(RESULT_ADDR + 8)))
        f:write(string.format("linked=%d\n", emu:read32(RESULT_ADDR + 12)))
        f:write(string.format("gate_id=%04X\n", emu:read32(RESULT_ADDR + 16) & 0xFFFF))
      end
      f:close()
    end
  end)

  -- Deliberately no os.exit(). Exiting the process from inside the scripting
  -- engine, mid-callback, is what segfaulted mGBA (exit 139, empty log, no Lua
  -- error to go on). The result file is the verdict; the harness's timeout
  -- ends the run.
  console:log("ROM-TEST: done, verdict written to " .. RESULT)
end

local function handle(raw)
  if #raw < 5 then return end
  local ptype = string.byte(raw, 1)
  local s = string.byte(raw, 2) | (string.byte(raw, 3) << 8)
  local plen = #raw - 5
  if crc16(raw:sub(1, 3 + plen)) ~=
     (string.byte(raw, 4 + plen) | (string.byte(raw, 5 + plen) << 8)) then
    return  -- corrupt; drop it
  end

  if ptype == T_HELLO then
    -- Bring the bridge up: acknowledge the firmware's HELLO.
    send(T_HELLO_ACK, s, string.char(VERSION, ROLE_MASTER, 2))
    seq = s
    if not linked then
      linked = true
      console:log(string.format("link up at seq %d (acked firmware HELLO)", s))
    end

  elseif ptype == T_HELLO_ACK then
    seq = s
    if not linked then
      linked = true
      console:log(string.format("link up at seq %d (firmware acked our HELLO)", s))
    end

  elseif ptype == T_RESULT then
    -- The gate's answer is RESULT word[1]: raw bytes 6..7 (after type+seq).
    local answer = string.byte(raw, 6) | (string.byte(raw, 7) << 8)
    -- Into the mailbox, not SIOMULTI0: mGBA refuses a script's writes to the
    -- multiplayer data registers ("GPIO write: Unhandled SIOMULTI0"), so the
    -- ROM would never see the answer. EWRAM writes always land.
    emu:write16(MB_FROM_GATE, answer)
    seq_from = (seq_from + 1) & 0xFFFF
    emu:write16(MB_SEQ_FROM, seq_from)

  elseif ptype == T_PING then
    send(T_PONG, s, raw:sub(4, 7))
  end
end

local function pump()
  if not sock then return end
  local data = sock:receive(4096)
  if not data or #data == 0 then return end
  for i = 1, #data do
    local b = string.byte(data, i)
    if b == SLIP_END then
      if #rxbuf > 0 then handle(rxbuf) end
      rxbuf, escaped = "", false
    elseif b == SLIP_ESC then
      escaped = true
    else
      if escaped then
        escaped = false
        if b == SLIP_ESC_END then b = SLIP_END
        elseif b == SLIP_ESC_ESC then b = SLIP_ESC end
      end
      if #rxbuf < 24 then rxbuf = rxbuf .. string.char(b) end
    end
  end
end

local function check_result()
  if emu:read32(RESULT_ADDR) ~= RESULT_MAGIC then
    return
  end
  local status = emu:read32(RESULT_ADDR + 4)
  if status == ST_PASS then
    finish(ST_PASS, string.format("gate %04X, %d linked frames",
      emu:read32(RESULT_ADDR + 16) & 0xFFFF, emu:read32(RESULT_ADDR + 12)))
  elseif status == ST_FAIL then
    finish(ST_FAIL, "ROM reported failure (never linked / rotation wrong)")
  end
end

local function onFrame()
  if not sock or done then return end
  pump()

  frames = frames + 1

  if linked then
    -- Forward a word only when the ROM has staged a new one. Sending every
    -- frame would resend stale words whenever the round trip outlasts a
    -- frame, which through a socket it usually does.
    local seq_to = emu:read16(MB_SEQ_TO)
    if seq_to ~= last_seq_to then
      last_seq_to = seq_to
      seq = (seq + 1) & 0xFFFF
      send(T_WORD, seq, u16(emu:read16(MB_TO_GATE)))
    end
  end

  check_result()

  if not done and frames >= MAX_FRAMES then
    -- Timed out. Read whatever the ROM published, if anything.
    local status = ST_FAIL
    local note = "timed out"
    if emu:read32(RESULT_ADDR) == RESULT_MAGIC then
      status = emu:read32(RESULT_ADDR + 4)
      if status == ST_RUNNING then
        status = ST_FAIL
        note = "timed out while still running"
      end
    else
      note = "timed out; ROM published no result"
    end
    finish(status, note)
  end
end

sock = socket.tcp()
local ok = sock:connect(HOST, PORT)
if not ok then
  console:error(string.format("cannot reach the broker at %s:%d", HOST, PORT))
  console:error("start it first:  python3 qemu/broker.py --mgba " .. PORT)
  pcall(function() os.exit(2) end)
else
  console:log(string.format("connected to the broker at %s:%d", HOST, PORT))
  -- Prompt the handshake from our side too; the firmware also sends HELLO.
  send(T_HELLO, 0, string.char(VERSION, ROLE_MASTER, 2))
  -- Deliberately no sock:add("received", ...): pump() is called from onFrame
  -- and nowhere else. Registering it as a socket callback as well made it
  -- reentrant -- the callback could fire while onFrame was mid-pump, and
  -- during teardown it could fire on a closing socket.
  frame_cb = callbacks:add("frame", onFrame)
end
