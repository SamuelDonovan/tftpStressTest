"""Drive Tftpd64's GUI TFTP client from outside the process.

Tftpd64's client is not a CLI -- its transfer state machine lives in the GUI
dialog (src/_gui/tftp_cli.c). This runs under Wine as a Windows process and
drives the real dialog through Win32 messages: fill the fields, trigger the
Get/Put handler the same way the source does (WM_COMMAND to the client dialog),
then watch the controls for the end of the transfer.

Exit code 0 = the client reported success, 1 = it reported failure (the message
box text goes to stderr), 2 = harness/driver problem.
"""

import argparse
import ctypes
import sys
import time
from ctypes import wintypes

u32 = ctypes.windll.user32
EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

WM_COMMAND = 0x0111
WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D
BM_CLICK = 0x00F5
CB_FINDSTRINGEXACT = 0x0158
CB_SETCURSEL = 0x014E

ID_HOST, ID_LOCAL, ID_REMOTE, ID_PORT = 2005, 2006, 2017, 2011
ID_GET, ID_PUT, ID_BREAK, ID_BLKSIZE = 2003, 2004, 2008, 2013

CLIENT_CLASS = "Ttftpd32ClientBackGround"


SMTO_ABORTIFHUNG = 0x0002


def window_text(hwnd):
    # SendMessage blocks forever if the GUI thread is wedged; a bounded send
    # keeps the driver's own deadline meaningful.
    buf = ctypes.create_unicode_buffer(4096)
    result = ctypes.c_ulong()
    ok = u32.SendMessageTimeoutW(hwnd, WM_GETTEXT, 4096, buf, SMTO_ABORTIFHUNG,
                                 1000, ctypes.byref(result))
    return buf.value if ok else ""


def class_name(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def title(hwnd):
    buf = ctypes.create_unicode_buffer(512)
    u32.GetWindowTextW(hwnd, buf, 512)
    return buf.value


def top_level():
    found = []
    u32.EnumWindows(EnumProc(lambda h, _: (found.append(h), True)[1]), 0)
    return found


def children(hwnd):
    found = []
    u32.EnumChildWindows(hwnd, EnumProc(lambda c, _: (found.append(c), True)[1]), 0)
    return found


def find_main(deadline):
    while time.time() < deadline:
        for hwnd in top_level():
            if title(hwnd).startswith("Tftpd64") and u32.IsWindowVisible(hwnd):
                return hwnd
        time.sleep(0.05)
    return None


def find_client_dialog(main):
    for child in children(main):
        if class_name(child) == CLIENT_CLASS:
            return child
    return None


# Any modal dialog that is not the main window is the client reporting an error
# (Tftpd64 surfaces transfer failures as a message box).
def find_message_box(main):
    for hwnd in top_level():
        if hwnd == main or not u32.IsWindowVisible(hwnd):
            continue
        if class_name(hwnd) != "#32770":
            continue
        kids = children(hwnd)
        classes = [class_name(k) for k in kids]
        # Tftpd64's transfer-progress window is not an error: it carries a
        # progress bar. A real error box is text plus an OK button.
        if any(k.startswith("msctls_progress") for k in classes):
            continue
        body = " ".join(t for t in (window_text(k) for k in kids) if t)
        return hwnd, body
    return None, ""


# Tftpd64 reports the end of a transfer with a message box either way: a
# success reads "N blocks transferred ... MD5: ...", a failure carries an
# "Error #N" or says the server stopped the transfer. Classify on the text.
FAILURE_MARKERS = ("error #", "stops the transfer", "timeout", "timed out",
                   "cancel", "unable", "denied", "cannot")


def classify(body):
    lowered = body.lower()
    if any(marker in lowered for marker in FAILURE_MARKERS):
        return False
    return "transferred" in lowered


def error_code(body):
    marker = body.lower().find("error #")
    if marker < 0:
        return None
    digits = ""
    for character in body[marker + 7:]:
        if character.isdigit():
            digits += character
        else:
            break
    return digits or None


def dismiss(hwnd):
    for child in children(hwnd):
        if class_name(child) == "Button" and window_text(child).strip("&") in (
            "OK", "Ok", "Yes", "Cancel"):
            u32.SendMessageW(child, BM_CLICK, 0, 0)
            return
    u32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--op", choices=["get", "put"], required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", required=True)
    parser.add_argument("--local", required=True)   # Windows path
    parser.add_argument("--remote", required=True)
    parser.add_argument("--blksize", default="Default")
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    deadline = time.time() + args.timeout
    main_window = find_main(deadline)
    if main_window is None:
        print("driver: Tftpd64 main window not found", file=sys.stderr)
        return 2
    client = find_client_dialog(main_window)
    if client is None:
        print("driver: client dialog not found", file=sys.stderr)
        return 2

    # A previous run may have been killed mid-transfer, leaving a message box up
    # or a transfer still going. Clear both before starting a new one, or this
    # transfer inherits that state and the result is meaningless.
    box, _ = find_message_box(main_window)
    if box:
        dismiss(box)
        time.sleep(0.1)
    stale_break = u32.GetDlgItem(main_window, ID_BREAK)
    if stale_break and u32.IsWindowEnabled(stale_break):
        client_dialog = find_client_dialog(main_window)
        u32.SendMessageW(client_dialog, WM_COMMAND, ID_BREAK, 0)
        for _ in range(40):
            time.sleep(0.05)
            box, _ = find_message_box(main_window)
            if box:
                dismiss(box)
                break
            if not u32.IsWindowEnabled(stale_break):
                break

    u32.SetDlgItemTextW(main_window, ID_HOST, args.host)
    u32.SetDlgItemTextW(main_window, ID_PORT, str(args.port))
    u32.SetDlgItemTextW(main_window, ID_LOCAL, args.local)
    u32.SetDlgItemTextW(main_window, ID_REMOTE, args.remote)

    # The block size is a fixed dropdown list (Default/128/512/1024/...), not a
    # free-text field: a value it does not offer cannot be requested at all.
    combo = u32.GetDlgItem(main_window, ID_BLKSIZE)
    if not combo:
        print("driver: block size combo not found", file=sys.stderr)
        return 2
    index = u32.SendMessageW(combo, CB_FINDSTRINGEXACT, -1,
                             ctypes.c_wchar_p(str(args.blksize)))
    if index < 0:
        print(f"driver: blksize {args.blksize} is not offered by the client "
              f"(fixed dropdown)", file=sys.stderr)
        return 3
    u32.SendMessageW(combo, CB_SETCURSEL, index, 0)

    break_button = u32.GetDlgItem(main_window, ID_BREAK)
    button_id = ID_GET if args.op == "get" else ID_PUT

    # Trigger exactly as tftp_cli.c does internally.
    u32.SendMessageW(client, WM_COMMAND, button_id, 0)

    # The transfer runs on Tftpd64's own timer/socket loop. It is over when the
    # Break button goes back to disabled; a message box means it failed.
    while time.time() < deadline:
        box, body = find_message_box(main_window)
        # An empty body means the read itself failed (the GUI thread was busy and
        # the bounded send gave up), not that the client said nothing. Treating
        # that as a verdict would invent a failure, so keep polling instead.
        if box and body.strip():
            dismiss(box)
            ok = classify(body)
            code = error_code(body)
            summary = " ".join(body.split())
            print(f"driver: client reported: {summary}", file=sys.stderr)
            if code:
                print(f"driver: tftp-error-code={code}", file=sys.stderr)
            return 0 if ok else 1
        time.sleep(0.03)

    # No verdict box within the watchdog: the transfer is stuck. Cancel it and
    # report the hang rather than letting the dialog stay wedged for the next
    # test.
    u32.SendMessageW(client, WM_COMMAND, ID_BREAK, 0)
    time.sleep(0.3)
    box, _ = find_message_box(main_window)
    if box:
        dismiss(box)
    print("driver: transfer did not finish within the watchdog", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
