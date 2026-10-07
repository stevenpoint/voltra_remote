#pragma once

/** Build the LVGL user interface. Call once with the LVGL mutex held. */
void ui_init();

/**
 * Serial commands for screenshots and design work (tools/screenshot.py). Takes the LVGL
 * mutex itself. Returns false if the command is not one of these.
 *   shot                     send a screenshot: "SHOT w h swap bytes\n", raw RGB565, "\nEND\n"
 *   show main|settings|ecc|chains|attach|connect
 *   demo off|idle|loaded|set|ecc|twin   stand-in Voltra state (off: back to the real one)
 *   demo devs                two stand-in Voltras in range, for the connect screen
 *   ping                     just OK, once the UI is up
 * The stand-in lapses two minutes after the last command.
 *   set ecc|chains|attach|weight|reps N  adjust the stand-in state
 *   set pulley 1|2|0.5       the pulley ratio, until "demo off"
 */
bool ui_command(const char *cmd);
