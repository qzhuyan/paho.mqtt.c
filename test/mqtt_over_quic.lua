-- Wireshark LUA script to add MQTT over QUIC support
-- run:
--   wireshark -X lua_script:mqtt_over_quic.lua
--   OR put it in
--      Unix-like:  ~/.local/lib/wireshark/plugins
--      Windows: %APPDATA%\Wireshark\plugins
local dissector_table = DissectorTable.get("quic.proto")
local mqtt_handle = Dissector.get("mqtt")
dissector_table:add("mqtt", mqtt_handle)
