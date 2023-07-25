-- Wireshark LUA script to add MQTT over QUIC support
-- run:
--   wireshark -X lua_script:mqtt_over_quic.lua
local dissector_table = DissectorTable.get("quic.proto")
local mqtt_handle = Dissector.get("mqtt")
dissector_table:add("mqtt", mqtt_handle)
