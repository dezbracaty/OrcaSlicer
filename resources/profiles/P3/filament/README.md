# P3 material overrides

The `CFSYS *.json` files in this directory retain the corresponding CFSYS
material definitions and preset IDs. Most copies only enable
`reduce_fan_stop_start_freq`; `CFSYS PLA.json` overrides its flow ratio to 0.99,
matching Generic PLA @P3. P3.json selects these files so Alpha500 retains its
own material parameters. Other materials without P3 overrides remain shared.

Keep temperatures, fan speeds and initial no-cooling layers aligned with the
original CFSYS files when updating these copies. A zero minimum fan speed still
allows the fan to stop on long layers; this switch does not raise that minimum.
