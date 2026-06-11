# Motor Drive Frontend GUI Manual

This manual explains how to use the Motor Drive Frontend GUI and how to make
basic, safe edits to the JSON configuration files that control telemetry
parsing, plotting, gauges, indicators, and tuning controls.

## 1. Files You Will Use

The main files in the repository root are:

- `telemetry_config.json`: active runtime configuration for telemetry packets,
  display fields, indicators, gauges, space vector plots, and tuning controls.
- `config_example.jsonc`: commented reference configuration. Use it for
  guidance, but do not load it directly as the runtime config unless comments
  are removed and the file is saved as strict JSON.
- `sync_protocol.json`: reference schema for synchronized simulation messages.
  Most GUI users do not need to edit this.
- `simulation_example.csv`: sample input format for simulation playback.

At runtime, the application searches for `telemetry_config.json` beside the
executable, in the current working directory, and in a `config` subfolder of
those locations.

## 2. Starting The Application

1. Start the executable.
2. Confirm the firmware is connected over a USB virtual COM port.
3. In `Serial Port Settings`, choose the COM port and baud rate. The default
   baud rate is `115200`.
4. Press the serial start button. It changes from play to stop while the port is
   open.
5. Use the refresh button if the expected COM port is missing.

Incoming text responses and local command echoes appear in the receive console.
Local echoes are prefixed with `>>`.

## 3. Sending Commands

The `Basic Command` area sends common firmware commands:

- `FOC`: sends `start foc`.
- `VVVF`: sends `start vvvf`.
- `Sixstep`: sends `sixstep`.
- `Align`: sends `align`.
- `Aln Reset`: sends `align reset`.
- `Stop`: sends `stop`.
- `Audible`: sends `audible`.
- `Reset`: sends `reset`.

For custom commands, type the command in the `Sending` input box and press the
send button or Enter. The GUI automatically appends the serial newline.

## 4. Setting Speed Or Torque Targets

Use the `Set Target` area to send speed or torque commands.

1. Select `Speed` or `Torque`.
2. Move the target slider or enter an exact value.
3. Optionally set a time value. A time of `0` sends an immediate target.
4. Press the send button.

Ranges enforced by the GUI:

- Speed: `-9000` to `9000`.
- Torque: `0.000` to `0.150`.
- Time: `0` to `60` seconds.

Command format sent by the GUI:

```text
speed <target>
speed <target> <time>
torque <target>
torque <target> <time>
```

## 5. Runtime Tuning

The `Tuning` area is built from the `tuning` section of
`telemetry_config.json`.

Typical workflow:

1. Choose a subsystem, such as `Speed`, `Id`, `Iq`, or `Gain`.
2. Choose a parameter, such as `kp`, `ki`, `Ia`, or `Vbatt`.
3. Press enquire to request the current value from firmware.
4. Enter a new value and press send, or use increment/decrement with the step
   slider.
5. Use undo to return to the previous value tracked by the GUI.

Command formats sent by the GUI:

```text
tune <subsystem-command> <parameter-command> ?
tune <subsystem-command> <parameter-command> <value>
increment <subsystem-command> <parameter-command> <step>
```

The subsystem and parameter command names come from the JSON configuration, so
they must match the firmware command interface.

## 6. Firmware Logging Controls

The `Logging` buttons send log configuration commands to the firmware:

- `Preset 1` to `Preset 4`: sends `log preset 1` through `log preset 4`.
- `Clear`: sends `log rm all`.
- `Bin`: sends `log bin`.
- `UTF8`: sends `log utf8`.
- `ADC`: toggles ADC packet logging with `log add adc` or `log rm adc`.

The telemetry field list in the plotting area also controls firmware logging.
Checking a field sends `log add <field-command>`. Unchecking it sends
`log rm <field-command>`.

## 7. Saving CSV Data

The GUI has two different save modes:

- `Save CSV -> Telemetry`: starts or stops continuous telemetry CSV logging.
- `Save CSV -> ADC`: starts or stops continuous ADC sample CSV logging.
- `Quick Save`: records telemetry, ADC, or both for the selected duration.

Quick Save workflow:

1. Tick `Telemetry`, `ADC`, or both.
2. Optionally enter a filename suffix.
3. Enter a positive duration in seconds.
4. Press the save button.

Files are written in the current working directory. Names include a timestamp,
for example:

```text
2026-06-11-14.30.00_log_data.csv
2026-06-11-14.30.00_adc_sample.csv
```

Quick Save filenames use the same timestamp pattern and include the optional
suffix before `.csv`.

## 8. Status, Gauges, And Display Controls

The `Status` area shows configured indicators such as FOC, VVVF, Protection,
overcurrent, undervoltage, and configuration errors. Indicator behavior comes
from the `indicators` section of `telemetry_config.json`.

The gauge area shows configured live gauges such as voltage, speed, and
modulation index. Gauge behavior comes from the `gauges` section of
`telemetry_config.json`.

Useful display controls:

- Gauge mode button: toggles between circular and vertical bar gauges.
- Gauge test button: runs a visual gauge sweep.
- History file button: selects a historic telemetry CSV.
- History play button: replays the selected historic telemetry CSV into the
  display.

## 9. Plotting And Oscilloscope Use

The plotting area is driven by the telemetry fields configured in
`telemetry_config.json`.

Common actions:

- Check or uncheck fields in the field list to request firmware log fields.
- Double-click a field to create a new oscilloscope for that field.
- Drag fields into an oscilloscope to plot them.
- Use the oscilloscope gear button to choose plotted fields and colors.
- Use `+` and `-` on a scope to add or remove scopes.
- Use up and down controls to reorder scopes.
- Use the lock button to disable Y-axis auto-scaling.
- Use the display points slider to change how many samples are shown.
- Use pause to freeze or resume the plots.
- Use the trigger controls to freeze a capture when a selected field crosses a
  configured threshold.
- Use the config file button in the plotting area to load another JSON
  telemetry configuration.

The scope view supports vertical dragging and zooming.

## 10. Space Vector View

The space vector area uses the `space_vector` section of
`telemetry_config.json`.

The default configuration defines:

- `Alpha-Beta`: uses `VALPHA`, `VBETA`, and `VBATT`.
- `ABC`: uses `VA_EST`, `VB_EST`, `VC_EST`, and `VBATT`.

Use the space vector toggle to show or hide this area. Use the source selector
buttons to switch between configured plots. The arrow button toggles display of
the latest vector arrow, and the test button runs a visual sweep.

## 11. Simulation Mode

Open simulation mode from the `Simulation` area.

Simulation can coordinate two frontend instances:

- One instance runs as the server/coordinator.
- Another instance connects as the client.
- UDP discovery can find listening servers.
- TCP JSON-line messages synchronize prepare, ready, fire, finish, and abort
  events.

Basic workflow:

1. Open the simulation dialog.
2. On the server instance, choose the listen address mode and port, then press
   `Start`.
3. On the client instance, enter the server IP and port or use discovery, then
   press `Connect`.
4. Open a simulation CSV file.
5. Use the options dialog to map CSV columns for server target, client target,
   time, time unit, and starting row.
6. Start or pause the simulation from the control tab.
7. Use stop to abort the synchronized run.

The expected CSV shape is similar to:

```csv
Time,Speed,Torque
0,0,0
0.1,0.972895,6.18757E-05
```

## 12. Basic JSON Editing Rules

`telemetry_config.json` must be strict JSON:

- Use double quotes around strings and property names.
- Do not use comments.
- Do not leave trailing commas.
- Keep braces, brackets, and commas balanced.
- Use numbers for numeric values, not quoted strings.
- Make a backup before editing a working configuration.

Recommended editing workflow:

1. Copy `telemetry_config.json` to a backup file.
2. Use `config_example.jsonc` as a reference while editing.
3. Make one small change at a time.
4. Validate the JSON syntax in your editor.
5. Load the edited file with the config file button in the plotting area, or
   restart the application.
6. If the GUI reports a configuration error, fix the reported section and load
   again.

The parser only replaces the active configuration after the new file fully
validates, so a failed reload does not corrupt the currently running parser
state.

## 13. Editing Telemetry Fields

Telemetry fields are defined mainly in `fields` and `fields2`.

Example field:

```json
{ "name": "RPM", "size": 4, "format": "f", "bit": 0, "command": "rpm" }
```

Properties:

- `name`: display name used by the GUI.
- `size`: number of bytes in the binary payload.
- `format`: parser format. Supported values are `B`, `H`, and `f`.
- `bit`: mask bit used by the firmware to indicate that this field is present.
- `command`: firmware log command name used by `log add` and `log rm`.

Supported formats:

```text
B = unsigned 8-bit value
H = unsigned 16-bit little-endian value
f = 32-bit little-endian float
```

When adding a field:

1. Confirm the firmware sends the field.
2. Choose `fields` for the first telemetry mask or `fields2` for the second
   telemetry mask.
3. Use a unique `bit` within that list.
4. Set `size` and `format` to match the firmware payload exactly.
5. Set `command` to the firmware log name.
6. Put fields in the same payload order used by the firmware.

If the mask bit, size, format, or order is wrong, later fields in the packet may
be decoded incorrectly.

## 14. Editing Custom Fields

Custom fields are calculated locally by the GUI after telemetry parsing.

Example:

```json
{
  "name": "VA_EST",
  "expression": "VBATT / 3 * (2 * DUTY_A - DUTY_B - DUTY_C)"
}
```

Use custom fields when you want a derived value without changing the firmware
packet. Expressions can use configured field names, numeric constants,
arithmetic operators, parentheses, and common functions such as `sqrt`, `sin`,
`cos`, `abs`, `pow`, `min`, and `max`.

Custom field names can be used by plots, indicators, gauges, and space vector
views.

## 15. Editing Indicators

Indicators live in the `indicators` array.

Example condition indicator:

```json
{
  "name": "FW",
  "type": "condition",
  "indicator": 4,
  "dataSource": "FW",
  "status": [
    { "lowerBound": 0, "upperBound": 0, "displayText": "FW", "color": "off" },
    { "lowerBound": 1, "upperBound": 1, "displayText": "FW", "color": "yellow" },
    { "displayText": "FW", "color": "off" }
  ]
}
```

Indicator types:

- `mode`: compares the current control mode with `status.value`.
- `condition`: compares a field value with `lowerBound` and `upperBound`.
- `bitwise`: checks a bit in `errors` or another numeric data source.

The `indicator` number maps to the fixed status labels in the GUI. Colors may
be named colors such as `green`, `yellow`, `red`, `lightblue`, `off`, or a Qt
color string such as `#33aa66`.

## 16. Editing Gauges

Gauges live in the `gauges` array.

Example:

```json
{
  "name": "Voltage",
  "gauge": 0,
  "dataSource": "VBATT",
  "secondaryDataSource": "MAXVAL",
  "topDisplayUnit": "V",
  "valueDecimals": 2,
  "min": 0.0,
  "max": 30.0,
  "divisions": 6,
  "thresholds": [
    { "upperBound": 14.0, "color": "green" },
    { "lowerBound": 14.0, "upperBound": 26.0, "color": "yellow" },
    { "lowerBound": 26.0, "color": "red" }
  ],
  "hysteresis": 2.0
}
```

Useful properties:

- `gauge`: display order.
- `dataSource`: telemetry or custom field driving the gauge.
- `secondaryDataSource`: marker source. `MAXVAL` keeps rolling maximum behavior.
- `topDisplayUnit`: unit label.
- `valueDecimals`: number of decimal places.
- `min` and `max`: display range. `max` must be greater than `min`.
- `divisions`: number of scale divisions.
- `thresholds`: colored operating regions.
- `hysteresis`: reduces rapid threshold flicker.

## 17. Editing Tuning Controls

Tuning controls live in the `tuning` array.

Example:

```json
{
  "subsystem": "Speed",
  "command": "speed",
  "parameters": [
    { "name": "kp", "command": "p" },
    { "name": "ki", "command": "i" }
  ]
}
```

Properties:

- `subsystem`: label shown in the GUI.
- `command`: firmware subsystem command.
- `parameters`: parameter labels and firmware command names.

Add tuning entries only for parameters that the firmware supports. The GUI will
send commands such as:

```text
tune speed p ?
tune speed p 0.120
increment speed p 0.010000
```

## 18. Editing Packet Layouts

The `telemetry_fields`, `telemetry_structure`, `adc_sample_fields`, and
`adc_sample_structure` sections define the binary packet layout.

Only edit these sections when the firmware packet structure changes. These
sections control header bytes, version fields, masks, variable payloads, ADC
sample fields, timestamps, and optional CRC fields.

High-risk changes:

- Changing `header` values.
- Adding or removing fields from `telemetry_structure`.
- Changing field lengths.
- Moving `payload` or `payload2`.
- Enabling or changing CRC fields.
- Changing ADC sample payload format or timing fields.

After packet layout changes, test with known telemetry before relying on plots
or logs.

## 19. Troubleshooting

Serial port missing:

- Press refresh.
- Check the USB connection and firmware power.
- Close other tools that may be using the COM port.

No telemetry fields appear:

- Confirm `telemetry_config.json` loaded successfully.
- Confirm the firmware is sending binary telemetry that matches the configured
  packet layout.

Field checkbox does not start data:

- Confirm the field has a valid `command` in the JSON.
- Confirm the firmware supports that `log add <command>` name.

Plots are flat or nonsensical:

- Check field `size`, `format`, `bit`, and order.
- Confirm the correct first or second mask list is used.
- Check that firmware and GUI are using the same configuration version.

Configuration file fails to load:

- Remove comments and trailing commas.
- Check that the root is a JSON object.
- Check that required arrays such as `fields`, `fields2`, `errors`, `modes`,
  `telemetry_structure`, and `adc_sample_structure` are present.
- Use the error message shown by the GUI to find the failing section.

CSV files are not created:

- Check write permission in the current working directory.
- Stop an existing save mode before starting another conflicting capture.
- For Quick Save, select `Telemetry`, `ADC`, or both and enter a positive
  duration.

