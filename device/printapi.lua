-- Shared helpers for the print API CGI scripts.
-- Installed to /usr/lib/lua/printapi.lua on the printer (Lua 5.1, OpenWrt 15.05).

local nixio = require "nixio"
local nfs = require "nixio.fs"

local M = {}

M.STORAGE = "/tmp/run/mountd/mmcblk0p1"
M.PRINT_FILES = {
	print       = "/usr/sbin/print.json",
	local_print = "/usr/sbin/local_print.json",
	msg         = "/usr/sbin/print_msg.txt",
}
M.EXTENSIONS = { pm5s = true, pwmb = true, pmx2 = true }

local REASONS = {
	[200] = "OK", [201] = "Created", [204] = "No Content",
	[400] = "Bad Request", [404] = "Not Found", [405] = "Method Not Allowed",
	[409] = "Conflict", [411] = "Length Required", [500] = "Internal Server Error",
	[503] = "Service Unavailable", [507] = "Insufficient Storage",
}

---------------------------------------------------------------------------
-- JSON encoding
---------------------------------------------------------------------------

local ARRAY_MT = {}
-- Mark a table so it always encodes as a JSON array (even when empty).
function M.array(t) return setmetatable(t or {}, ARRAY_MT) end

-- Wrap a pre-encoded JSON string so the encoder emits it verbatim.
local RAW_MT = {}
local function raw(s) return setmetatable({ s }, RAW_MT) end

-- Explicit JSON null (a plain nil would drop the key from an object).
M.null = raw("null")

local function esc(s)
	return '"' .. s:gsub('[%c"\\]', function(c)
		if c == '"' then return '\\"'
		elseif c == "\\" then return "\\\\"
		elseif c == "\n" then return "\\n"
		elseif c == "\r" then return "\\r"
		elseif c == "\t" then return "\\t"
		else return string.format("\\u%04x", c:byte()) end
	end) .. '"'
end

local function encode(v)
	local t = type(v)
	if v == nil then return "null"
	elseif t == "boolean" then return tostring(v)
	elseif t == "number" then
		if v ~= v or v == math.huge or v == -math.huge then return "null" end
		-- "%d" truncates to a 32-bit long on this MIPS32 target; use %.0f for integers.
		if v == math.floor(v) then return string.format("%.0f", v) end
		return string.format("%.14g", v)
	elseif t == "string" then return esc(v)
	elseif t == "table" then
		local mt = getmetatable(v)
		if mt == RAW_MT then return v[1] end
		local out = {}
		if mt == ARRAY_MT or #v > 0 then
			for i = 1, #v do out[i] = encode(v[i]) end
			return "[" .. table.concat(out, ",") .. "]"
		end
		local keys = {}
		for k in pairs(v) do keys[#keys + 1] = tostring(k) end
		table.sort(keys)
		for _, k in ipairs(keys) do
			out[#out + 1] = esc(k) .. ":" .. encode(v[k])
		end
		return "{" .. table.concat(out, ",") .. "}"
	end
	return "null"
end
M.encode = encode

---------------------------------------------------------------------------
-- CGI request / response
---------------------------------------------------------------------------

function M.method() return (os.getenv("REQUEST_METHOD") or "GET"):upper() end

local function urldecode(s)
	s = s:gsub("%+", " ")
	return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end

function M.query()
	local q = {}
	for pair in (os.getenv("QUERY_STRING") or ""):gmatch("[^&]+") do
		local k, v = pair:match("^([^=]*)=?(.*)$")
		if k and k ~= "" then q[urldecode(k)] = urldecode(v) end
	end
	return q
end

-- All values of a repeated query parameter, e.g. ?name=a&name=b.
function M.query_all(key)
	local out = {}
	for pair in (os.getenv("QUERY_STRING") or ""):gmatch("[^&]+") do
		local k, v = pair:match("^([^=]*)=?(.*)$")
		if k and urldecode(k) == key then out[#out + 1] = urldecode(v) end
	end
	return out
end

local function headers(status, ctype, allow)
	io.stdout:write(
		"Status: ", status, " ", REASONS[status] or "", "\r\n",
		"Content-Type: ", ctype, "\r\n",
		"Cache-Control: no-store\r\n",
		"Access-Control-Allow-Origin: *\r\n",
		"Access-Control-Allow-Methods: ", allow or "GET", ", OPTIONS\r\n",
		"Access-Control-Allow-Headers: Content-Type\r\n",
		"\r\n")
end

M.allow = "GET"

function M.respond(status, body)
	headers(status, "application/json", M.allow)
	io.stdout:write(encode(body), "\n")
	io.stdout:flush()
	os.exit(0)
end

-- Respond with an already-encoded JSON string.
function M.respond_raw(status, json)
	headers(status, "application/json", M.allow)
	io.stdout:write(json, "\n")
	io.stdout:flush()
	os.exit(0)
end

function M.fail(status, msg)
	M.respond(status, { ok = false, error = msg })
end

-- Answer CORS preflight requests; call at the top of each script.
function M.handle_options(allow)
	M.allow = allow
	if M.method() == "OPTIONS" then
		headers(204, "text/plain", allow)
		os.exit(0)
	end
end

---------------------------------------------------------------------------
-- Files / storage
---------------------------------------------------------------------------

-- Returns the validated filename, or nil plus a reason.
function M.safe_name(n)
	if type(n) ~= "string" or n == "" then return nil, "missing name" end
	if #n > 200 then return nil, "name too long" end
	if n:find("[^%w%._%- ()%[%]+,]") then return nil, "name contains invalid characters" end
	if n:sub(1, 1) == "." then return nil, "name may not start with '.'" end
	local ext = n:match("%.([^%.]+)$")
	if not ext or not M.EXTENSIONS[ext:lower()] then
		return nil, "unsupported file extension"
	end
	return n
end

function M.has_ext(n)
	local ext = n:match("%.([^%.]+)$")
	return ext ~= nil and M.EXTENSIONS[ext:lower()] == true
end

function M.read_file(path)
	local f = io.open(path, "rb")
	if not f then return nil end
	local s = f:read("*a")
	f:close()
	return s
end

-- Embed file contents that are already JSON verbatim; otherwise as a string.
function M.raw_json(text)
	if text == nil then return nil end
	local trimmed = text:match("^%s*(.-)%s*$")
	if trimmed == "" then return nil end
	local first, last = trimmed:sub(1, 1), trimmed:sub(-1)
	if (first == "{" and last == "}") or (first == "[" and last == "]") then
		return raw(trimmed)
	end
	return trimmed
end

function M.storage_mounted()
	local mounts = M.read_file("/proc/mounts") or ""
	for line in mounts:gmatch("[^\n]+") do
		local mp = line:match("^%S+%s+(%S+)")
		if mp == M.STORAGE then return true end
	end
	return false
end

-- Returns free, total (bytes) for the storage mount, or nil.
-- Only call while attached: touching STORAGE otherwise triggers a mountd automount attempt.
function M.storage_space()
	if not M.storage_mounted() then return nil end
	local st = nfs.statvfs(M.STORAGE)
	if not st then return nil end
	local bs = (st.frsize and st.frsize > 0) and st.frsize or st.bsize
	return st.bavail * bs, st.blocks * bs
end

function M.stat(path) return nfs.stat(path) end
function M.dir(path) return nfs.dir(path) end

---------------------------------------------------------------------------
-- eMMC handover (see docs/serial_interface.md § eMMC handover)
--
-- The "Local" eMMC is muxed between the mainboard and this board by GPIO0.
-- The mainboard must agree (serial cmd 0x23) before GPIO0 may be raised;
-- raising it while the mainboard is using the eMMC (e.g. printing) would
-- yank its storage. We therefore pause comm, send the request ourselves,
-- read the mainboard's answer, and only switch when it says "released".
---------------------------------------------------------------------------

local GPIO_VALUE = "/sys/class/gpio/gpio0/value"
local TTY = "/dev/ttyS1"
local DEVICE = "/dev/mmcblk0p1"
local LOCK = "/tmp/printapi.lock"            -- directory; mkdir is atomic
local LOCK_STALE_SECS = 1200                  -- longer than uhttpd script_timeout (900)
M.CACHE = "/tmp/printapi_files.json"

local OWNER_LINUX, OWNER_MAINBOARD = 0, 1

local function sleep(secs, nsec) nixio.nanosleep(secs, nsec or 0) end

local function frame(owner)
	return "DM" .. string.char(0x23, 0x00, 0x00, 0x00, 0x0B, owner) .. string.rep("\0", 12)
end

local function hex(s)
	return (s:gsub(".", function(c) return string.format("%02x ", c:byte()) end))
end

local function gpio_get()
	return tonumber((M.read_file(GPIO_VALUE) or ""):match("%d"))
end

local function gpio_set(v)
	local f = io.open(GPIO_VALUE, "w")
	if not f then return false end
	f:write(v and "1" or "0")
	f:close()
	return true
end

local function comm_pid()
	local p = io.popen("pidof comm")
	local out = p and p:read("*a") or ""
	if p then p:close() end
	return tonumber(out:match("%d+"))
end

local function comm_stopped(pid)
	local st = M.read_file("/proc/" .. pid .. "/status") or ""
	return st:match("State:%s*T") ~= nil
end

-- Make sure comm is not left paused (e.g. if a request died mid-exchange).
function M.ensure_comm_running()
	local pid = comm_pid()
	if pid and comm_stopped(pid) then nixio.kill(pid, nixio.const.SIGCONT) end
end

-- Send an eMMC change request and wait for the mainboard's answer.
-- Returns status byte [7], owner byte [8] of the answer, or nil + error.
local function exchange(owner)
	local pid = comm_pid()
	if pid then nixio.kill(pid, nixio.const.SIGSTOP) end
	local ok, r7, r8 = pcall(function()
		local f = assert(nixio.open(TTY, "r+"))
		f:setblocking(false)
		repeat local d = f:read(256) until not d or #d == 0   -- drop stale bytes
		f:write(frame(owner))
		local buf = ""
		for _ = 1, 15 do                                      -- up to ~3 s
			local ready = nixio.poll({ { fd = f, events = nixio.poll_flags("in") } }, 200)
			if ready and ready > 0 then
				local d = f:read(256)
				if d then buf = buf .. d end
			end
			local i = 1
			while true do
				local s = buf:find("DM", i, true)
				if not s or #buf < s + 8 then break end
				local cmd = buf:byte(s + 2) + buf:byte(s + 3) * 256
				if cmd == 0x23 and buf:byte(s + 4) ~= 0 then
					f:close()
					return buf:byte(s + 7), buf:byte(s + 8)
				end
				i = s + 1
			end
		end
		f:close()
		error("no answer from mainboard" .. (#buf > 0 and (" (got " .. hex(buf) .. ")") or ""))
	end)
	if pid then nixio.kill(pid, nixio.const.SIGCONT) end
	if not ok then return nil, tostring(r7):gsub("^.-:%d+: ", "") end
	return r7, r8
end

-- Download progress shown on the touchscreen: serial cmd 0x26, 12 bytes, fire-and-forget
-- (comm 0x414cf8; its reporter thread 0x42d204 sends state 1 every 5 s while downloading,
-- and the job handler sends (100, state 2) on success before handing the eMMC back).
--   44 4D 26 00 00 00 03 PP SS EE 00 00     PP = percent, (SS,EE) from the state below
M.PROGRESS_DOWNLOADING, M.PROGRESS_DONE, M.PROGRESS_FAILED = 1, 2, 7
local PROGRESS_STATE_BYTES = {
	[1] = { 1, 0 }, [2] = { 3, 0 }, [3] = { 3, 1 }, [4] = { 2, 0 },
	[5] = { 4, 0 }, [6] = { 5, 0 }, [7] = { 6, 0 }, [8] = { 7, 0 },
}

function M.report_progress(pct, state)
	pct = math.max(0, math.min(100, math.floor(pct or 0)))
	local sb = PROGRESS_STATE_BYTES[state] or { 0, 0 }
	local fr = "DM" .. string.char(0x26, 0x00, 0x00, 0x00, 0x03, pct, sb[1], sb[2], 0x00, 0x00)
	-- Pause comm for the instant of the write so our frame can't interleave with one of its own
	-- (a small tty write() is atomic, so stopping between comm's syscalls is enough).
	local pid = comm_pid()
	if pid then nixio.kill(pid, nixio.const.SIGSTOP) end
	pcall(function()
		local f = assert(nixio.open(TTY, "r+"))
		f:write(fr)
		f:close()
	end)
	if pid then nixio.kill(pid, nixio.const.SIGCONT) end
end

local function lock_age()
	local st = nfs.stat(LOCK)
	return st and (os.time() - st.mtime) or nil
end

local function unlock() nfs.rmdir(LOCK) end

-- Hand the eMMC back to the mainboard. Safe to call repeatedly.
local function release_storage()
	if M.storage_mounted() then
		os.execute("sync")
		if os.execute("umount " .. M.STORAGE .. " 2>/dev/null") ~= 0 then
			os.execute("umount -l " .. M.STORAGE .. " 2>/dev/null")
		end
	end
	if gpio_get() == 1 then
		gpio_set(false)
		sleep(2)                                   -- comm waits 1-2 s here too
	end
	exchange(OWNER_MAINBOARD)
end

-- Recover from a request that died while holding the storage.
function M.recover_stale()
	local age = lock_age()
	if age and age > LOCK_STALE_SECS then
		release_storage()
		unlock()
	end
	M.ensure_comm_running()
end

local function busy_reason()
	if (M.read_file(M.PRINT_FILES.msg) or ""):match("%S") then
		return "printer is busy with a cloud job"
	end
	if gpio_get() == 1 then return "storage is in use by the printer's cloud service" end
	return nil
end

-- Attach the eMMC to this board and mount it. Returns true or nil, err.
local function attach()
	M.recover_stale()
	local why = busy_reason()
	if why then return nil, why end

	-- one storage user at a time; wait briefly for a concurrent request
	local got = false
	for _ = 1, 30 do
		if nfs.mkdir(LOCK) then got = true break end
		sleep(1)
	end
	if not got then return nil, "storage is busy with another request" end

	local r7, r8 = exchange(OWNER_LINUX)
	if r7 ~= 0 or r8 ~= OWNER_LINUX then
		unlock()
		if r7 == nil then return nil, r8 end
		return nil, string.format("mainboard refused to release storage (printing?) [%d,%d]", r7, r8)
	end

	sleep(1)
	gpio_set(true)
	for _ = 1, 20 do                               -- eMMC enumerates in ~1 s
		if nfs.stat(DEVICE) then break end
		sleep(0, 500000000)
	end
	if not nfs.stat(DEVICE) then
		release_storage(); unlock()
		return nil, "storage did not appear after switching"
	end
	for _ = 1, 10 do                               -- mountd usually mounts it for us
		if M.storage_mounted() then break end
		sleep(0, 500000000)
	end
	if not M.storage_mounted() then
		os.execute("mount -t vfat -o iocharset=cp936 " .. DEVICE .. " " .. M.STORAGE)
	end
	if not M.storage_mounted() then
		release_storage(); unlock()
		return nil, "could not mount storage"
	end
	-- Let the kernel finish probing the eMMC (boot0/boot1 partition scans) before we
	-- use it; switching away mid-probe logs I/O errors. comm waits 3 s here.
	sleep(2)
	return true
end

-- Snapshot the print-file listing into the cache (call while attached).
local function refresh_cache()
	local files = M.array()
	local iter = nfs.dir(M.STORAGE)
	if iter then
		for n in iter do
			if n:sub(1, 1) ~= "." and M.has_ext(n) then
				local st = nfs.stat(M.STORAGE .. "/" .. n)
				if st and st.type == "reg" then
					files[#files + 1] = { name = n, size = st.size, mtime = st.mtime }
				end
			end
		end
	end
	table.sort(files, function(a, b) return a.mtime > b.mtime end)
	local free, total = M.storage_space()
	local snap = { files = files, free = free or M.null, total = total or M.null, updated = os.time() }
	local tmp = M.CACHE .. ".tmp"
	local f = io.open(tmp, "w")
	if f then
		f:write(encode(snap))
		f:close()
		os.rename(tmp, M.CACHE)
	end
end

-- Run fn() with the eMMC attached; always refreshes the cache and releases.
-- fn must not call respond()/fail() (they exit without cleanup); return values instead.
-- Returns true, fn's results... | nil, err.
function M.with_storage(fn)
	local ok, err = attach()
	if not ok then return nil, err end
	local res = { pcall(fn) }
	pcall(refresh_cache)
	release_storage()
	unlock()
	if not res[1] then return nil, tostring(res[2]) end
	return true, unpack(res, 2)
end

-- Cached listing as raw JSON (or nil if never listed).
function M.cached_listing() return M.read_file(M.CACHE) end

function M.cached_free()
	return tonumber((M.cached_listing() or ""):match('"free":(%d+)'))
end

function M.storage_state()
	return {
		owner = gpio_get() == 1 and "linux" or "mainboard",
		busy = nfs.stat(LOCK) ~= nil,
		mounted = M.storage_mounted(),
	}
end

return M
