--[[
mgba_link.lua -- connect a game in mGBA to firmware running in QEMU.

    mgba-headless --script qemu/mgba_link.lua game.gba
    python3 qemu/broker.py --mgba 7000

The broker listens; this script connects to it and exchanges link_proto
packets, feeding what the firmware sends into the game's SIO registers and
reporting back what the game sent.

An honest caveat
----------------
mGBA has no external SIO driver, so this script drives the registers from Lua
at frame granularity. That is enough to exercise the firmware's protocol
handling -- handshake, sequencing, pacing, chip words -- and it is not a
substitute for real timing, which only a scope on real hardware settles.

Set LINK_PORT / LINK_HOST in the environment to override.
--]]

local HOST = os.getenv("LINK_HOST") or "127.0.0.1"
local PORT = tonumber(os.getenv("LINK_PORT") or "7000")

-- link_proto constants, mirroring src/link_proto.h
local T_HELLO, T_HELLO_ACK = 0x01, 0x02
local T_WORD, T_RESULT     = 0x10, 0x11
local T_PING, T_PONG       = 0x30, 0x31
local ROLE_SLAVE           = 0
local VERSION              = 1

local SLIP_END, SLIP_ESC       = 0xC0, 0xDB
local SLIP_ESC_END, SLIP_ESC_ESC = 0xDC, 0xDD

-- GBA serial registers
local REG_SIOMULTI0 = 0x04000120
local REG_SIOMLT_SEND = 0x0400012A

local sock, rxbuf, escaped = nil, "", false
local seq, linked, frames = 0, false, 0

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

local function handle(raw)
  if #raw < 5 then return end
  local ptype = string.byte(raw, 1)
  local s = string.byte(raw, 2) | (string.byte(raw, 3) << 8)
  local plen = #raw - 5
  if crc16(raw:sub(1, 3 + plen)) ~=
     (string.byte(raw, 4 + plen) | (string.byte(raw, 5 + plen) << 8)) then
    return  -- corrupt; the firmware will underrun, which is the honest outcome
  end

  if ptype == T_HELLO then
    send(T_HELLO_ACK, s, string.char(VERSION, ROLE_SLAVE, 2))
    seq, linked = s, true
    console:log(string.format("link up at seq %d", s))

  elseif ptype == T_WORD then
    -- What the firmware wants the game to see this frame.
    emu:write16(REG_SIOMULTI0, string.byte(raw, 4) | (string.byte(raw, 5) << 8))

  elseif ptype == T_PING then
    send(T_PONG, s, raw:sub(4, 7))
  end
end

local function pump()
  local data = sock:receive(1024)
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

local function onFrame()
  if not sock then return end
  pump()
  if not linked then return end

  frames = frames + 1
  -- One transfer per frame, which is what a frame-paced game does.
  local word = emu:read16(REG_SIOMLT_SEND)
  seq = (seq + 1) & 0xFFFF
  send(T_RESULT, seq,
       u16(word) .. u16(0xFFFF) .. u16(0xFFFF) .. u16(0xFFFF) ..
       string.char(2, 1))
end

sock = socket.tcp()
local ok = sock:connect(HOST, PORT)
if not ok then
  console:error(string.format("cannot reach the broker at %s:%d", HOST, PORT))
  console:error("start it first:  python3 qemu/broker.py --mgba " .. PORT)
else
  console:log(string.format("connected to the broker at %s:%d", HOST, PORT))
  sock:add("received", pump)
  callbacks:add("frame", onFrame)
end
