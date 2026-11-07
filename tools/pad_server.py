# pad_server.py <command file>: keeps one virtual Xbox 360 pad plugged in (ViGEmBus) and runs the commands
# appended to the command file, one per line:
#   A 200            press a button for 200 ms (A B X Y LB RB START BACK UP DOWN LEFT RIGHT LS RS GUIDE)
#   RT 0.8 3000      hold a trigger at 0.8 for 3000 ms (LT RT)
#   LX -1 500        hold the left stick's x at -1 for 500 ms (LX LY RX RY)
#   WAIT 1000
#   QUIT
import sys, time, os
import vgamepad as vg

B = vg.XUSB_BUTTON
BUTTONS = {
    'A': B.XUSB_GAMEPAD_A, 'B': B.XUSB_GAMEPAD_B, 'X': B.XUSB_GAMEPAD_X, 'Y': B.XUSB_GAMEPAD_Y,
    'LB': B.XUSB_GAMEPAD_LEFT_SHOULDER, 'RB': B.XUSB_GAMEPAD_RIGHT_SHOULDER,
    'START': B.XUSB_GAMEPAD_START, 'BACK': B.XUSB_GAMEPAD_BACK, 'GUIDE': B.XUSB_GAMEPAD_GUIDE,
    'UP': B.XUSB_GAMEPAD_DPAD_UP, 'DOWN': B.XUSB_GAMEPAD_DPAD_DOWN,
    'LEFT': B.XUSB_GAMEPAD_DPAD_LEFT, 'RIGHT': B.XUSB_GAMEPAD_DPAD_RIGHT,
    'LS': B.XUSB_GAMEPAD_LEFT_THUMB, 'RS': B.XUSB_GAMEPAD_RIGHT_THUMB,
}

path = sys.argv[1]
pad = vg.VX360Gamepad()
pad.reset(); pad.update()
open(path, 'w').close()
print('pad ready', flush=True)
done = 0
while True:
    time.sleep(0.05)
    try:
        lines = open(path).read().splitlines()
    except OSError:
        continue
    for line in lines[done:]:
        done += 1
        p = line.split()
        if not p:
            continue
        c = p[0].upper()
        print('>', line, flush=True)
        if c == 'QUIT':
            pad.reset(); pad.update(); sys.exit(0)
        if c == 'WAIT':
            time.sleep(int(p[1]) / 1000)
        elif c in BUTTONS:
            pad.press_button(BUTTONS[c]); pad.update()
            time.sleep(int(p[1]) / 1000 if len(p) > 1 else 0.15)
            pad.release_button(BUTTONS[c]); pad.update()
        elif c in ('LT', 'RT'):
            v = float(p[1])
            (pad.left_trigger_float if c == 'LT' else pad.right_trigger_float)(v); pad.update()
            time.sleep(int(p[2]) / 1000)
            (pad.left_trigger_float if c == 'LT' else pad.right_trigger_float)(0.0); pad.update()
        elif c in ('LX', 'LY', 'RX', 'RY'):
            v = float(p[1])
            if c[0] == 'L':
                pad.left_joystick_float(v if c == 'LX' else 0.0, v if c == 'LY' else 0.0)
            else:
                pad.right_joystick_float(v if c == 'RX' else 0.0, v if c == 'RY' else 0.0)
            pad.update()
            time.sleep(int(p[2]) / 1000)
            pad.left_joystick_float(0.0, 0.0); pad.right_joystick_float(0.0, 0.0); pad.update()
